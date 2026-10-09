#include "abi_bridge.h"
#include "fatal.h"
#include "ios_host.h"
#include "disc_image.h"
#include "asset_cache.h"
#include "guest_call.h"
#include "load_progress.h"
#include "net/ios_net.h"
#include "scope_exit.h"
#include "runtime/native/adapters/model_catalog_bridge.h"
#include <aurora/thread_role.hpp>
#include <cstring>
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <exception>
#include <mutex>
#include <pthread.h>
#include <thread>
#include <filesystem>
#include <fstream>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#ifdef __vita__
#include <psp2/io/fcntl.h>
#include <psp2/io/dirent.h>
#include <psp2/io/stat.h>
#else
#include <sys/xattr.h>
#endif
#include <memory>
#include <iostream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>

namespace {
struct File {
    int host;
    uint32_t mode;
};

std::map<uint32_t, File> files;
std::map<uint32_t, std::string> devices;
struct EventHook {
    uint32_t fd, output, callback, argument;
};

std::optional<EventHook> eventHook;
struct HidState {
    bool changed = true;
    std::optional<EventHook> hook;
};

std::map<uint32_t, HidState> hidDevices;
std::deque<IosCompletion> completions;
std::array<uint8_t, 20> ledSettings{}, dimmingSettings{};
uint32_t nextHandle = 100;
uint32_t lastDiscPosition = 0;
DiscImage& Disc() {
    return GameDiscImage();
}

// The DVD request a disc read belongs to (DiscRequestOf).
struct DiscRequest {
    uint32_t block = 0; // the executing DVDCommandBlock; 0: none or not a read
    uint64_t end = 0;   // where the request ends on the disc
    uint32_t caller = 0; // development: its callback, for the disc statistics
};

// Disc reads the DVD library sends with IOS_IoctlAsync (DI 0x71) run on a
// reader thread, as a Wii drive reads while the game runs, and their replies
// reach the library's callback at a later pump. The game's file loader issues
// a read and polls for its reply once a frame, so its loading screens keep
// animating while the reader works. With load-time conversion (asset_cache.h)
// a read is answered only once the textures and models of the files it
// touches are converted: nothing that looks them up later (draw-table
// preparation, model and texture binding) waits for them.
//
// The DVD library reads a request in chunks of at most 512 KiB, each sent once
// the last one's reply has reached its callback. A chunk tells where its
// request ends (DiscRequest), so while no read waits the reader reads the rest
// of the request ahead, and the next chunk is answered from memory.
class DiscReader {
    using Clock = std::chrono::steady_clock;

  public:
    // Game thread. A read (length != 0) is answered once the reader has it,
    // anything else at once; replies keep the order of their requests.
    void Submit(uint32_t callback, uint32_t argument, int32_t result, uint32_t output = 0, uint64_t offset = 0, uint32_t length = 0, const DiscRequest& request = {}) {
        std::lock_guard guard(lock);
        replies.push_back({callback, argument, output, length, offset, result, length == 0});
        replies.back().request = request;
#ifdef MH3_DEVELOPMENT
        replies.back().submitted = Clock::now();

        // The caller's first read after its last reply: how long it was idle.
        if (const auto last = delivered.find(request.caller); length && last != delivered.end()) {
            CallerReads& c = callers[request.caller];
            c.idle += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(replies.back().submitted - last->second).count());
            ++c.idled;
            delivered.erase(last);
        }
#endif
        if (length && !started) {
            started = true;
            pthread_attr_t attributes;
            pthread_attr_init(&attributes);
            pthread_attr_setstacksize(&attributes, 1u << 20);
            const int failed = pthread_create(
                &thread, &attributes,
                [](void* self) -> void* {
                    static_cast<DiscReader*>(self)->Run();
                    return nullptr;
                },
                this);
            pthread_attr_destroy(&attributes);

            if (failed)
                MH3_THROW(std::runtime_error("Cannot start the disc reader"));
        }

        changed.notify_all();
    }

    // Game thread: hands the replies that are ready, in order, to the IOS
    // completions, writing each read into guest memory first.
    void Deliver() {
        std::unique_lock guard(lock);

        while (!replies.empty() && replies.front().ready) {
            Reply reply = std::move(replies.front());
            replies.pop_front();

            if (reply.broken)
                continue; // answered by Break
#ifdef MH3_DEVELOPMENT
            if (reply.length) {
                const auto now = Clock::now();
                callers[reply.request.caller].deliver += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(now - reply.readyAt).count());
                delivered[reply.request.caller] = now;
            }
#endif
            guard.unlock();

            if (!reply.failure.empty())
                MH3_THROW(std::runtime_error(reply.failure));

            if (reply.length) {
                std::memcpy(Memory::GetPointer(reply.output, reply.length), reply.data.data(), reply.length);
                RecordNativeDiscWrite(reply.output, reply.offset, reply.length);
                LoadProgress::DiscDelivered(reply.offset, reply.length);
            }

            completions.push_back({reply.callback, reply.result, reply.argument});
            guard.lock();
        }
    }

    // Game thread: the DVD library cancels the read in flight (DVDCancelAsync).
    // Its reply is answered now, as the drive's at the end of the transfer,
    // but leaves guest memory alone: the canceled block's data goes unused.
    // The reader finishes a transfer it started and drops it.
    void Break() {
        Deliver();
        std::lock_guard guard(lock);

        if (replies.empty() || replies.front().ready || !replies.front().length)
            return;

        Reply& reply = replies.front();
        reply.broken = true;
        reply.ready = !reply.taken;
        completions.push_back({reply.callback, reply.result, reply.argument});
#ifdef MH3_DEVELOPMENT
        std::fprintf(stderr, "MH3 disc: canceled read of %u bytes at 0x%llx (%s)\n", reply.length, (unsigned long long)reply.offset,
                     reply.taken ? "in transfer" : "queued");
#endif
    }

    // Any thread: whether a read is not answered yet.
    bool Waiting() {
        std::lock_guard guard(lock);
        return std::any_of(replies.begin(), replies.end(), [](const Reply& reply) { return reply.length && !reply.ready; });
    }

    // Game thread: waits up to `limit` for the next reply to be ready; false
    // when there is none in flight.
    bool Await(std::chrono::milliseconds limit) {
        std::unique_lock guard(lock);

        if (replies.empty())
            return false;

        if (!replies.front().ready)
            changed.wait_for(guard, limit);

        return true;
    }

    ~DiscReader() {
        {
            std::lock_guard guard(lock);
            stopping = true;
        }

        changed.notify_all();

        if (started)
            pthread_join(thread, nullptr);
    }

  private:
    struct Reply {
        uint32_t callback, argument, output, length;
        uint64_t offset;
        int32_t result;
        bool ready, taken = false, broken = false;
        std::vector<uint8_t> data;
        std::string failure; // why the read failed
        DiscRequest request;
#ifdef MH3_DEVELOPMENT
        Clock::time_point submitted, readyAt;
        bool ahead = false; // answered from the read-ahead
#endif
    };

    // Reader thread: the rest of a DVD request read ahead. data[head...] holds
    // the disc bytes from `start`; the request ends at `end`.
    struct Ahead {
        uint32_t block = 0;
        uint64_t start = 0, end = 0, used = 0;
        size_t head = 0;
        std::vector<uint8_t> data;
        size_t Size() const { return data.size() - head; }
        void Drop(size_t bytes) {
            head += bytes;
            start += bytes;

            if (head == data.size()) {
                data.clear();
                head = 0;
            }
        }
    };

    // One RVZ chunk: a read that comes in waits for at most one piece.
    static constexpr size_t kAheadPiece = 128u << 10;

    // Read at most this far past the last chunk answered.
    static constexpr size_t kAheadWindow = 4u << 20;
    std::array<Ahead, 2> aheads;
    uint64_t aheadUses = 0;

    // Reader thread: a chunk, from the read-ahead of its request as far as
    // that has it; sets up the read-ahead of the rest of the request.
    Status Fetch(const Reply& reply, bool& hit, std::vector<uint8_t>& data) {
        const uint64_t offset = reply.offset, stop = offset + reply.length;
        Ahead* ahead = nullptr;

        for (auto& a : aheads)
            if (a.block && a.block == reply.request.block) {
                if (a.start == offset)
                    ahead = &a;
                else
                    a = {}; // the block's earlier request
            }
        data.clear();
        hit = ahead && ahead->Size() >= reply.length;

        if (ahead) {
            const size_t have = std::min<size_t>(ahead->Size(), reply.length);
            data.assign(ahead->data.begin() + ahead->head, ahead->data.begin() + ahead->head + have);
            ahead->Drop(have);
        }

        if (data.size() < reply.length) {
            std::vector<uint8_t> rest;
            MH3_RETURN_IF_ERROR(Disc().TryRead(offset + data.size(), uint32_t(reply.length - data.size()), rest));
            data.insert(data.end(), rest.begin(), rest.end());

            if (ahead)
                ahead->start = stop; // empty: the rest was read here
        }

        if (!reply.request.block || reply.request.end <= stop) {
            if (ahead)
                *ahead = {};

            return {};
        }

        if (!ahead) {
            ahead = &*std::min_element(aheads.begin(), aheads.end(), [](const Ahead& a, const Ahead& b) { return a.used < b.used; });
            *ahead = {};
            ahead->block = reply.request.block;
            ahead->start = stop;
            ahead->end = reply.request.end;
        }

        ahead->used = ++aheadUses;
        return {};
    }

    // Reader thread, unlocked: reads the next piece of a request's rest; false
    // when there is nothing to read ahead, or the read failed: the read-ahead
    // is dropped then, and the chunk's own read reports the failure.
    bool ReadAhead() {
        for (auto& a : aheads) {
            const uint64_t at = a.start + a.Size();

            if (!a.block || at >= a.end || a.Size() >= kAheadWindow)
                continue;

            std::vector<uint8_t> piece;

            if (!Disc().TryRead(at, uint32_t(std::min<uint64_t>(kAheadPiece, a.end - at)), piece).ok()) {
                aheads = {};
                return false;
            }

            if (a.head && a.head >= a.data.size() / 2) {
                a.data.erase(a.data.begin(), a.data.begin() + a.head);
                a.head = 0;
            }

            a.data.insert(a.data.end(), piece.begin(), piece.end());
#ifdef MH3_DEVELOPMENT
            std::lock_guard guard(lock);
            aheadBytes += piece.size();
#endif
            return true;
        }

        return false;
    }

    void Run() {
        // Core 0 (Vita): the game and render threads keep cores 2 and 1. On
        // core 3 (ThreadRole::Spare) the village load took 4.28 s instead
        // of 3.82 s on the devkit.
        aurora::pin_current_thread(aurora::ThreadRole::Worker);
        std::unique_lock guard(lock);

        for (;;) {
            Reply* next = nullptr;
            const auto pending = [&] {
                for (auto& reply : replies)
                    if (!reply.ready && !reply.taken)
                        return next = &reply, true;
                return false;
            };

            // Timed: VitaSDK's condition variable can lose a notification.
            while (!stopping && !pending()) {
                guard.unlock();
                const bool read = ReadAhead();
                guard.lock();

                if (!read && !stopping && !pending())
                    changed.wait_for(guard, std::chrono::milliseconds(20));
            }

            if (stopping)
                return;
            // Earlier replies stay put while this one is read (deque references
            // survive push_back, and only ready replies are removed).
            next->taken = true;
            const uint64_t offset = next->offset;
            const uint32_t length = next->length;
            guard.unlock();
            std::vector<uint8_t> data;
            const auto start = Clock::now();
            auto read = start;
            bool hit = false;
            const Status fetched = Fetch(*next, hit, data);

            if (fetched.ok()) {
                read = Clock::now();

                if (auto* cache = GameAssetCache())
                    while (!cache->Converted(offset, length, std::chrono::milliseconds(20)) && !stopping) {
                    }
            }

            guard.lock();
#ifdef MH3_DEVELOPMENT
            next->ahead = hit;
            next->readyAt = Clock::now();
            Count(*next, start, read, next->readyAt);
#else
            (void)hit;
#endif
            next->data = std::move(data);
            next->failure = fetched.message();
            next->ready = true;
            changed.notify_all();
        }
    }
#ifdef MH3_DEVELOPMENT
  public:
    // Development: each second's reads per caller for the "MH3 disc" lines:
    // the time on the disc, waiting for the asset cache's conversion, and
    // from the request to the reply; how long a ready reply waited for the
    // game thread to deliver it, and how long the caller then took to ask for
    // its next read (idle). Reads over 50 ms are listed with their file.
    void Report(double seconds) {
        std::vector<std::pair<uint32_t, CallerReads>> rows;
        uint64_t ahead;
        {
            std::lock_guard guard(lock);
            rows.assign(callers.begin(), callers.end());
            callers.clear();
            ahead = std::exchange(aheadBytes, 0);
        }

        if (ahead)
            std::fprintf(stderr, "MH3 disc: read ahead %.2f MiB/s\n", ahead / (1048576.0 * seconds));

        for (const auto& [caller, c] : rows)
            std::fprintf(stderr,
                         "MH3 disc: %s %.1f reads/s %.2f MiB/s (%llu from the read-ahead); disc %.2f ms avg %.2f max, conversion wait %.2f ms avg, reply %.2f ms avg %.2f max, "
                         "delivery %.2f ms avg, idle %.2f ms avg (%llu)\n",
                         CallerName(caller).c_str(), c.reads / seconds, c.bytes / (1048576.0 * seconds), static_cast<unsigned long long>(c.ahead),
                         c.disc / (1000.0 * std::max<uint64_t>(c.reads, 1)), c.discMax / 1000.0, c.convert / (1000.0 * std::max<uint64_t>(c.reads, 1)),
                         c.reply / (1000.0 * std::max<uint64_t>(c.reads, 1)), c.replyMax / 1000.0, c.deliver / (1000.0 * std::max<uint64_t>(c.reads, 1)),
                         c.idle / (1000.0 * std::max<uint64_t>(c.idled, 1)), static_cast<unsigned long long>(c.idled));
    }

  private:
    struct CallerReads {
        uint64_t reads = 0, bytes = 0, ahead = 0, disc = 0, discMax = 0, convert = 0, reply = 0, replyMax = 0; // us
        uint64_t deliver = 0, idle = 0, idled = 0;                                                         // us, us, count
    };

    using Callers = std::map<uint32_t, CallerReads>;
    static std::string CallerName(uint32_t caller) {
        switch (caller) {
        case 0x80434CDC:
            return "movie";
        case 0x800CE6E4:
            return "loader task";
        case 0x800EA6E4:
        case 0x800EA848:
        case 0x800EA9C8:
            return "stream";
        }

        char name[16];
        std::snprintf(name, sizeof name, "0x%08X", caller);
        return name;
    }

    // Reader thread, locked.
    void Count(const Reply& reply, Clock::time_point start, Clock::time_point read, Clock::time_point end) {
        const auto us = [](Clock::duration d) { return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(d).count()); };
        CallerReads& c = callers[reply.request.caller];
        const uint64_t disc = us(read - start), total = us(end - reply.submitted);
        ++c.reads;
        c.ahead += reply.ahead;
        c.bytes += reply.length;
        c.disc += disc;
        c.discMax = std::max(c.discMax, disc);
        c.convert += us(end - read);
        c.reply += total;
        c.replyMax = std::max(c.replyMax, total);

        if (total > 50'000)
            std::fprintf(stderr, "MH3 disc: %s read of %u bytes at 0x%llx (%s) took %.1f ms (disc %.1f ms)\n", CallerName(reply.request.caller).c_str(), reply.length,
                         static_cast<unsigned long long>(reply.offset), FileAt(reply.offset).c_str(), total / 1000.0, disc / 1000.0);
    }

    static std::string FileAt(uint64_t offset) {
        static const auto files = [] {
            std::vector<std::pair<uint64_t, std::string>> sorted;

            for (const auto& name : Disc().FileNames())
                sorted.emplace_back(Disc().FileOffset(name), name);

            std::sort(sorted.begin(), sorted.end());
            return sorted;
        }();
        auto it = std::upper_bound(files.begin(), files.end(), offset, [](uint64_t value, const auto& file) { return value < file.first; });
        return it == files.begin() ? "?" : std::prev(it)->second;
    }

    Callers callers;
    std::map<uint32_t, Clock::time_point> delivered; // each caller's last reply, until its next read
    uint64_t aheadBytes = 0;
#endif
    std::mutex lock;
    std::condition_variable changed;
    std::deque<Reply> replies;
    std::atomic<bool> stopping{false};
    bool started = false;
    pthread_t thread{};
};

DiscReader& Reader() {
    // After the disc and its asset cache, so destroyed before them at exit.
    (void)Disc();
    static DiscReader reader;
    return reader;
}

// The DVD request behind the disc read of [offset, offset + length): the
// executing command block (0x804A9B60, decompiled with Ghidra, reads its
// word offset +0x10, length +0x14 and bytes transferred +0x20) when the read
// is its next chunk. The caller is the block's callback, or for DVDReadAsync
// (whose block is a DVDFileInfo) the reader's own.
DiscRequest DiscRequestOf(const CpuContext* ctx, uint64_t offset, uint32_t length) {
    enum : int32_t { kExecuting = -0x3CF0 }; // DVDCommandBlock* executing
    const uint32_t block = Memory::Read32(ctx->gpr[13] + uint32_t(kExecuting));

    if (!block)
        return {};

    const uint64_t start = uint64_t(Memory::Read32(block + 0x10)) * 4, end = start + Memory::Read32(block + 0x14);
    DiscRequest request;

    if (start + Memory::Read32(block + 0x20) == offset && offset + length <= end)
        request = {block, end};
#ifdef MH3_DEVELOPMENT
    constexpr uint32_t kReadAsyncCallback = 0x804A6A70;
    const uint32_t callback = Memory::Read32(block + 0x28);
    request.caller = callback == kReadAsyncCallback ? Memory::Read32(block + 0x38) : callback;
#endif
    return request;
}

int32_t Error() {
    switch (errno) {
    case ENOENT:
        return -106;
    case EEXIST:
        return -105;
    case EACCES:
    case EPERM:
    case ELOOP:
        return -102;
    case ENOSPC:
        return -108;
    default:
        return -101;
    }
}

std::filesystem::path NandRoot() {
    const char* user = std::getenv("MH3_USER_DIR");
    return (user ? std::filesystem::path(user) : std::filesystem::path("saves")) / "nand";
}

bool ValidNandPath(const std::string& path, bool allowRoot = false) {
    if (path.empty() || path.size() >= 64 || path[0] != '/' || path.find("//") != std::string::npos || (path.size() == 1 && !allowRoot))
        return false;

    for (const auto& part : std::filesystem::path(path.substr(1)))
        if (part == ".." || part == "." || part.empty())
            return false;
    return true;
}

// Persist Wii metadata on the inode, so native renames preserve it and NAND
// cluster accounting does not include host-only sidecar files. This port has
// one title: UID 0x1000, group 0x3038 (verified in RMHP08's TMD).
using NandMetadata = std::array<uint8_t, 10>;
constexpr NandMetadata defaultMetadata{0, 0, 0x10, 0, 0x30, 0x38, 3, 3, 3, 0};
#ifndef __vita__
#ifdef __APPLE__
constexpr const char* metadataName = "org.mh3.nand";
#else
constexpr const char* metadataName = "user.mh3.nand";
#endif
int SetMetadata(int fd, const NandMetadata& metadata) {
#ifdef __EMSCRIPTEN__
    // WasmFS has no extended attributes: on the web every entry keeps the
    // title's defaults (docs/web-build.md).
    (void)fd;
    (void)metadata;
    const int result = 0;
#elif defined(__APPLE__)
    const int result = ::fsetxattr(fd, metadataName, metadata.data(), metadata.size(), 0, 0);
#else
    const int result = ::fsetxattr(fd, metadataName, metadata.data(), metadata.size(), 0);
#endif
    return result == 0 ? 0 : Error();
}

int GetMetadata(int fd, NandMetadata& metadata) {
#ifdef __EMSCRIPTEN__
    (void)fd;
    metadata = defaultMetadata;
    const auto size = ssize_t(metadata.size());
#elif defined(__APPLE__)
    const auto size = ::fgetxattr(fd, metadataName, metadata.data(), metadata.size(), 0, 0);

    if (size < 0 && errno == ENOATTR) {
        metadata = defaultMetadata;
        return 0;
    }
#else
    const auto size = ::fgetxattr(fd, metadataName, metadata.data(), metadata.size());

    if (size < 0 && errno == ENODATA) {
        metadata = defaultMetadata;
        return 0;
    }
#endif
    return size == metadata.size() ? 0 : size < 0 ? Error() : -101;
}
#endif
std::string GuestPath(uint32_t address) {
    std::string path;

    for (unsigned i = 0; i < 64; ++i) {
        const char c = Memory::Read8(address + i);

        if (!c)
            return path;

        path += c;
    }

    return {};
}

// IOS_Open does not create files. ISFS creation will be handled independently.
// Resolve every component relative to the private NAND root without following
// symlinks, and reject traversal before opening any host file.
#ifdef __vita__
#include "../vita/nand.inc"
#else
int CloseNandPath(int fd) {
    return ::close(fd);
}

int SyncNandFile(int fd) {
    return ::fsync(fd) == 0 ? 0 : Error();
}

int OpenNandPath(const std::string& path, int finalFlags, bool allowRoot = false) {
    if (!ValidNandPath(path, allowRoot))
        return -4;

    std::filesystem::path relative = path.substr(1);

    for (const auto& part : relative)
        if (part == ".." || part == "." || part.empty())
            return -4;
    const auto root = NandRoot();
    int dir = ::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);

    if (dir < 0)
        return Error();

    auto end = relative.end();

    for (auto it = relative.begin(); it != end; ++it) {
        auto next = it;
        ++next;
        const bool last = next == end;
        int flags = last ? finalFlags : O_RDONLY | O_DIRECTORY;
        int child = ::openat(dir, it->c_str(), flags | O_NOFOLLOW);
        int saved = errno;
        ::close(dir);
        errno = saved;

        if (child < 0)
            return Error();

        dir = child;
    }

    return dir;
}

int InitializeNand() {
    const auto root = NandRoot();
    std::filesystem::create_directories(root.parent_path());

    if (::mkdir(root.c_str(), 0700) != 0 && errno != EEXIST)
        return Error();
    // IOS and title installation normally provide these parents. No game
    // settings or save contents are synthesized, and existing files survive.
    constexpr const char* directories[]{"/tmp", "/sys", "/shared2", "/title", "/title/00010000", "/title/00010001", "/title/00010004", "/title/00010000/524d4850", "/title/00010000/524d4850/data"};

    for (const auto* path : directories) {
        const std::filesystem::path entry(path);
        const int parent = OpenNandPath(entry.parent_path().string(), O_RDONLY | O_DIRECTORY, true);

        if (parent < 0)
            return parent;

        const auto name = entry.filename().string();
        const bool created = ::mkdirat(parent, name.c_str(), 0700) == 0;

        if (!created && errno != EEXIST) {
            const int error = Error();
            ::close(parent);
            return error;
        }

        const int child = ::openat(parent, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
        const int error = child < 0 ? Error() : created ? SetMetadata(child, defaultMetadata) : 0;

        if (child >= 0)
            ::close(child);

        ::close(parent);

        if (error)
            return error;
    }

    return 0;
}

int CreateNandEntry(const std::string& path, bool directory, const NandMetadata& metadata) {
    if (!ValidNandPath(path))
        return -101;

    const std::filesystem::path entry(path);
    const int parent = OpenNandPath(entry.parent_path().string(), O_RDONLY | O_DIRECTORY, true);

    if (parent < 0)
        return parent;

    const auto name = entry.filename().string();
    int child;

    if (directory) {
        if (::mkdirat(parent, name.c_str(), 0700) != 0) {
            const int error = Error();
            ::close(parent);
            return error;
        }

        child = ::openat(parent, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    } else
        child = ::openat(parent, name.c_str(), O_CREAT | O_EXCL | O_RDWR | O_NOFOLLOW, 0600);

    const int result = child < 0 ? Error() : SetMetadata(child, metadata);

    if (child >= 0)
        ::close(child);

    ::close(parent);
    return result;
}

int RenameNandEntry(const std::string& from, const std::string& to) {
    if (!ValidNandPath(from) || !ValidNandPath(to))
        return -101;

    const std::filesystem::path source(from), target(to);
    const int sourceDir = OpenNandPath(source.parent_path().string(), O_RDONLY | O_DIRECTORY, true);

    if (sourceDir < 0)
        return sourceDir;

    const int targetDir = OpenNandPath(target.parent_path().string(), O_RDONLY | O_DIRECTORY, true);

    if (targetDir < 0) {
        ::close(sourceDir);
        return targetDir;
    }

    struct stat info {};
    int result = 0;

    if (::fstatat(sourceDir, source.filename().c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0)
        result = Error();
    else if (!S_ISREG(info.st_mode) && !S_ISDIR(info.st_mode))
        result = -102;

    if (!result) {
        if (::fstatat(targetDir, target.filename().c_str(), &info, AT_SYMLINK_NOFOLLOW) == 0) {
            if (!S_ISREG(info.st_mode))
                result = -102;
        } else if (errno != ENOENT)
            result = Error();
    }

    if (!result && ::renameat(sourceDir, source.filename().c_str(), targetDir, target.filename().c_str()) != 0)
        result = Error();

    ::close(sourceDir);
    ::close(targetDir);
    return result;
}

int RemoveNandTree(int parent, const char* name, bool execute, unsigned depth = 0) {
    if (depth > 64)
        return -101;

    struct stat info {};

    if (::fstatat(parent, name, &info, AT_SYMLINK_NOFOLLOW) != 0)
        return Error();

    const bool directory = S_ISDIR(info.st_mode);

    if (!directory && !S_ISREG(info.st_mode))
        return -102;

    for (const auto& [handle, file] : files) {
        struct stat opened {};

        if (::fstat(file.host, &opened) == 0 && opened.st_dev == info.st_dev && opened.st_ino == info.st_ino)
            return -111; // ISFS InUse
    }

    if (directory) {
        const int fd = ::openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);

        if (fd < 0)
            return Error();

        DIR* raw = ::fdopendir(fd);

        if (!raw) {
            const int error = Error();
            ::close(fd);
            return error;
        }

        std::unique_ptr<DIR, decltype(&::closedir)> entries(raw, ::closedir);

        while (true) {
            errno = 0;
            const auto* entry = ::readdir(raw);

            if (!entry) {
                if (errno)
                    return Error();

                break;
            }

            const std::string_view child(entry->d_name);

            if (child == "." || child == "..")
                continue;

            const int result = RemoveNandTree(fd, entry->d_name, execute, depth + 1);

            if (result)
                return result;
        }
    }

    if (execute && ::unlinkat(parent, name, directory ? AT_REMOVEDIR : 0) != 0)
        return Error();

    return 0;
}

int DeleteNandEntry(const std::string& path) {
    if (!ValidNandPath(path))
        return -101;

    const std::filesystem::path entry(path);
    const int parent = OpenNandPath(entry.parent_path().string(), O_RDONLY | O_DIRECTORY, true);

    if (parent < 0)
        return parent;
    // Validate the entire subtree before removing any file (including InUse).
    int result = RemoveNandTree(parent, entry.filename().c_str(), false);

    if (!result)
        result = RemoveNandTree(parent, entry.filename().c_str(), true);

    ::close(parent);
    return result;
}

int OpenFile(const std::string& path, uint32_t mode) {
    if (mode < 1 || mode > 3)
        return -4;

    const int dir = OpenNandPath(path, mode == 1 ? O_RDONLY : mode == 2 ? O_WRONLY : O_RDWR);

    if (dir < 0)
        return dir;

    struct stat info {};

    if (::fstat(dir, &info) != 0) {
        const int error = Error();
        ::close(dir);
        return error;
    }

    if (!S_ISREG(info.st_mode)) {
        ::close(dir);
        return -101;
    }

    NandMetadata metadata;
    const int metadataResult = GetMetadata(dir, metadata);

    if (metadataResult) {
        ::close(dir);
        return metadataResult;
    }

    const uint32_t owner = (uint32_t(metadata[0]) << 24) | (uint32_t(metadata[1]) << 16) | (uint32_t(metadata[2]) << 8) | metadata[3];
    const uint16_t group = (uint16_t(metadata[4]) << 8) | metadata[5];
    const uint8_t allowed = owner == 0x1000 ? metadata[6] : group == 0x3038 ? metadata[7] : metadata[8];

    if ((allowed & mode) != mode) {
        ::close(dir);
        return -102;
    }

    const uint32_t handle = nextHandle++;
    files.emplace(handle, File{dir, mode});
    return handle;
}

int DirectoryUsage(int fd, uint64_t& clusters, uint64_t& inodes, unsigned depth = 0) {
    // Own each descriptor even on error, and never follow a host symlink.
    DIR* raw = ::fdopendir(fd);

    if (!raw) {
        const int error = Error();
        ::close(fd);
        return error;
    }

    std::unique_ptr<DIR, decltype(&::closedir)> directory(raw, ::closedir);

    if (depth > 64)
        return -4;

    ++inodes;

    while (true) {
        errno = 0;
        const auto* entry = ::readdir(raw);

        if (!entry)
            return errno ? Error() : 0;

        const std::string_view name(entry->d_name);

        if (name == "." || name == "..")
            continue;

        struct stat info {};

        if (::fstatat(fd, entry->d_name, &info, AT_SYMLINK_NOFOLLOW) != 0)
            return Error();

        if (S_ISREG(info.st_mode)) {
            ++inodes;
            clusters += (uint64_t(info.st_size) + 0x3FFF) / 0x4000;
        } else if (S_ISDIR(info.st_mode)) {
            const int child = ::openat(fd, entry->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);

            if (child < 0)
                return Error();

            const auto result = DirectoryUsage(child, clusters, inodes, depth + 1);

            if (result)
                return result;
        } else
            return -102;
    }
}
#endif
} // namespace

#ifdef MH3_DEVELOPMENT
void ReportDiscStatistics(double seconds) {
    Reader().Report(seconds);
}
#endif

bool DiscReadWaiting() {
    return Reader().Waiting();
}

std::optional<IosCompletion> TakeIosCompletion() {
    if (completions.empty())
        return std::nullopt;

    auto result = completions.front();
    completions.pop_front();
    return result;
}

void QueueIosCompletion(IosCompletion completion) {
    if (completion.callback)
        completions.push_back(completion);
}

void TriggerStmEvent(uint32_t event) {
    if (!eventHook)
        return;

    Memory::Write32(eventHook->output, event);
    completions.push_back({eventHook->callback, 0, eventHook->argument});
    eventHook.reset();
}

// IPCWriteReg's initialization acknowledges stale status and enables replies.
// Actual requests are delivered through the IOS HLE entry points below.
extern "C" void MH3_IPCWriteReg(CpuContext* ctx) {
    if (ctx->gpr[3] != 1 || ctx->gpr[4] != 0x38)
        MH3_THROW(std::runtime_error("Unimplemented direct IPC register operation"));
}
REGISTER_NATIVE_FUNCTION(0x804BB950, MH3_IPCWriteReg);

extern "C" void MH3_IOSOpen(CpuContext* ctx) {
    const std::string path = GuestPath(ctx->gpr[3]);

    // Online play: sockets, SSL, the network configuration and, once online
    // play started, KD (net/ios_net.h).
    if (IosNet::IsDevice(path)) {
        const int32_t result = IosNet::Open(path);

        if (result) {
            ctx->gpr[3] = result;
            return;
        }

        const uint32_t handle = nextHandle++;
        devices.emplace(handle, path);
        ctx->gpr[3] = handle;
        return;
    }

    // No WiiConnect24 background-download daemon is running on the host.
    // Let the SDK's existing unavailable-service path handle offline startup.
    if (path == "/dev/net/kd/request" || path == "/dev/net/kd/time") {
        ctx->gpr[3] = -6;
        return;
    }

    if (path == "/dev/stm/immediate" || path == "/dev/stm/eventhook" || path == "/dev/fs" || path == "/dev/es" || path == "/dev/di" || path == "/dev/usb/hid") {
        if (path == "/dev/di")
            (void)Disc();

        if (path == "/dev/fs") {
            const int result = InitializeNand();

            if (result) {
                std::fprintf(stderr, "MH3 NAND initialization failed: %d (errno %d), root %s\n", result, errno, NandRoot().c_str());
                ctx->gpr[3] = result;
                return;
            }
        }

        uint32_t handle = nextHandle++;
        devices.emplace(handle, path);

        if (path == "/dev/usb/hid")
            hidDevices.emplace(handle, HidState{});

        ctx->gpr[3] = handle;
        return;
    }

    // No USB host controller: the Wii Speak library (PMIC) finds no microphone.
    if (path.starts_with("/dev/usb/oh")) {
        ctx->gpr[3] = -6;
        return;
    }

    if (path.starts_with("/dev/"))
        MH3_THROW(std::runtime_error("IOS device needs a host implementation: " + path));

    ctx->gpr[3] = OpenFile(path, ctx->gpr[4]);
}
REGISTER_NATIVE_FUNCTION(0x804BC500, MH3_IOSOpen);
extern "C" void MH3_IOSClose(CpuContext* ctx) {
    if (devices.erase(ctx->gpr[3])) {
        if (eventHook && eventHook->fd == ctx->gpr[3])
            eventHook.reset();

        if (const auto hid = hidDevices.find(ctx->gpr[3]); hid != hidDevices.end()) {
            if (hid->second.hook) {
                const auto hook = *hid->second.hook;
                Memory::Write32(hook.output, 0xFFFFFFFF);
                completions.push_back({hook.callback, -1, hook.argument});
            }

            hidDevices.erase(hid);
        }

        ctx->gpr[3] = 0;
        return;
    }

    auto it = files.find(ctx->gpr[3]);

    if (it == files.end()) {
        ctx->gpr[3] = -4;
        return;
    }

    const int syncError = (it->second.mode & 2) ? SyncNandFile(it->second.host) : 0;
    int result = CloseNandPath(it->second.host);
    files.erase(it);
    ctx->gpr[3] = syncError ? syncError : result == 0 ? 0 : Error();
}
REGISTER_NATIVE_FUNCTION(0x804BC810, MH3_IOSClose);

// The USB library's IOS_Open and IOS_Close with a two-second timeout
// (0x804BC630, 0x804BC8C0; decompiled with Ghidra): they queue the request
// as __ios_Ipc2 does and give up with -3 when no reply came in time. Here the
// request runs at once, so they are IOS_Open(path, mode) and IOS_Close(fd).
extern "C" void MH3_IOSOpenWithTimeout(CpuContext* ctx) {
    MH3_IOSOpen(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x804BC630, MH3_IOSOpenWithTimeout, "IOSOpenWithTimeout");

extern "C" void MH3_IOSCloseWithTimeout(CpuContext* ctx) {
    MH3_IOSClose(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x804BC8C0, MH3_IOSCloseWithTimeout, "IOSCloseWithTimeout");
extern "C" void MH3_IOSRead(CpuContext* ctx) {
    auto it = files.find(ctx->gpr[3]);

    if (it == files.end()) {
        ctx->gpr[3] = -4;
        return;
    }

    if (!(it->second.mode & 1)) {
        ctx->gpr[3] = -102;
        return;
    }

    uint32_t address = ctx->gpr[4], size = ctx->gpr[5];

    if (!Memory::Contains(address, size)) {
        ctx->gpr[3] = -4;
        return;
    }

    // Use guest writes so executable-memory checks cannot be bypassed.
    std::vector<uint8_t> buffer(size);
    ssize_t count = ::read(it->second.host, buffer.data(), size);

    if (count < 0) {
        ctx->gpr[3] = Error();
        return;
    }

    for (ssize_t i = 0; i < count; ++i)
        Memory::Write8(address + i, buffer[i]);

    ctx->gpr[3] = count;
}
REGISTER_NATIVE_FUNCTION(0x804BCA70, MH3_IOSRead);
extern "C" void MH3_IOSWrite(CpuContext* ctx) {
    auto it = files.find(ctx->gpr[3]);

    if (it == files.end()) {
        ctx->gpr[3] = -4;
        return;
    }

    if (!(it->second.mode & 2)) {
        ctx->gpr[3] = -102;
        return;
    }

    ssize_t count = ::write(it->second.host, Memory::GetPointer(ctx->gpr[4], ctx->gpr[5]), ctx->gpr[5]);
    ctx->gpr[3] = count < 0 ? Error() : count;
}
REGISTER_NATIVE_FUNCTION(0x804BCC80, MH3_IOSWrite);
void SeekFile(CpuContext* ctx) {
    const auto it = files.find(ctx->gpr[3]);

    if (it == files.end() || ctx->gpr[5] > 2) {
        ctx->gpr[3] = -101;
        return;
    }

    struct stat info {};
    const auto current = ::lseek(it->second.host, 0, SEEK_CUR);

    if (current < 0 || ::fstat(it->second.host, &info) != 0) {
        ctx->gpr[3] = Error();
        return;
    }

    // IOS uses 32-bit offsets, including modular relative/end offsets, and
    // rejects seeks beyond EOF (unlike POSIX).
    const uint32_t base = ctx->gpr[5] == 0 ? 0 : ctx->gpr[5] == 1 ? current : info.st_size;
    const uint32_t position = base + ctx->gpr[4];

    if (position > uint64_t(info.st_size)) {
        ctx->gpr[3] = -101;
        return;
    }

    const auto result = ::lseek(it->second.host, position, SEEK_SET);
    ctx->gpr[3] = result < 0 ? Error() : result;
}

// STM event hooks remain pending until a real host reset/power event. In
// particular, installing the hook must not immediately call the guest handler.
// Protocol reference: Dolphin Core/IOS/STM/STM.cpp, IOCTL_STM_EVENTHOOK.
extern "C" void MH3_IOSIoctl(CpuContext*);
extern "C" void MH3_IOSIoctlAsync(CpuContext* ctx) {
    auto it = devices.find(ctx->gpr[3]);

    if (it != devices.end() && it->second == "/dev/usb/hid" && ctx->gpr[4] == 0) {
        // HID v4 GETDEVICECHANGE: enumerate once, then retain the subscription
        // until a change or shutdown. There are no Wii USB HID peripherals;
        // keyboard/gamepad input is supplied separately through WPAD.
        if (!ctx->gpr[7] || ctx->gpr[8] != 0x600 || !Memory::Contains(ctx->gpr[7], 0x600) || !ctx->gpr[9]) {
            ctx->gpr[3] = -4;
            return;
        }

        auto& state = hidDevices.at(ctx->gpr[3]);

        if (state.hook) {
            ctx->gpr[3] = -8;
            return;
        }

        if (state.changed) {
            Memory::Write32(ctx->gpr[7], 0xFFFFFFFF); // end of device list
            completions.push_back({ctx->gpr[9], 0, ctx->gpr[10]});
            state.changed = false;
        } else {
            state.hook = EventHook{ctx->gpr[3], ctx->gpr[7], ctx->gpr[9], ctx->gpr[10]};
        }

        ctx->gpr[3] = 0;
        return;
    }

    if (it != devices.end() && IosNet::IsDevice(it->second)) {
        IosNet::IoctlAsync(it->second, ctx->gpr[4], ctx->gpr[5], ctx->gpr[6], ctx->gpr[7], ctx->gpr[8], ctx->gpr[9], ctx->gpr[10]);
        ctx->gpr[3] = 0;
        return;
    }

    if (it != devices.end() && it->second == "/dev/di") {
        const uint32_t callback = ctx->gpr[9], argument = ctx->gpr[10];

        if (!callback) {
            ctx->gpr[3] = -4;
            return;
        }

        const uint32_t input = ctx->gpr[5], output = ctx->gpr[7], capacity = ctx->gpr[8];

        if (ctx->gpr[4] == 0x71 && ctx->gpr[6] == 0x20 && Memory::Contains(input, 0x20)) {
            // Command buffer: byte length at +4, word-addressed offset at +8.
            const uint32_t length = Memory::Read32(input + 4), position = Memory::Read32(input + 8);

            if (length > capacity || !Memory::Contains(output, length))
                Reader().Submit(callback, argument, 32);
            else if (length) {
                lastDiscPosition = position;
                const DiscRequest request = DiscRequestOf(ctx, uint64_t(position) * 4, length);
                LoadProgress::DiscRead(uint64_t(position) * 4, length, request.end);
                Reader().Submit(callback, argument, 1, output, uint64_t(position) * 4, length, request);
            } else
                Reader().Submit(callback, argument, 1);
        } else {
            MH3_IOSIoctl(ctx);
            Reader().Submit(callback, argument, static_cast<int32_t>(ctx->gpr[3]));
        }

        ctx->gpr[3] = 0;
        return;
    }

    if (files.contains(ctx->gpr[3]) || (it != devices.end() && (it->second == "/dev/stm/immediate" || it->second == "/dev/fs" || it->second == "/dev/usb/hid"))) {
        const uint32_t callback = ctx->gpr[9], argument = ctx->gpr[10];

        if (!callback) {
            ctx->gpr[3] = -4;
            return;
        }

        MH3_IOSIoctl(ctx);
        completions.push_back({callback, static_cast<int32_t>(ctx->gpr[3]), argument});
        ctx->gpr[3] = 0;
        return;
    }

    if (it != devices.end() && it->second == "/dev/stm/eventhook" && ctx->gpr[4] == 0x1000) {
        if (ctx->gpr[8] < 4 || !Memory::Contains(ctx->gpr[7], ctx->gpr[8]) || !ctx->gpr[9]) {
            ctx->gpr[3] = -4;
            return;
        }

        if (eventHook) {
            completions.push_back({ctx->gpr[9], -8, ctx->gpr[10]});
            ctx->gpr[3] = 0;
            return;
        }

        eventHook = EventHook{ctx->gpr[3], ctx->gpr[7], ctx->gpr[9], ctx->gpr[10]};
        ctx->gpr[3] = 0;
        return;
    }

    MH3_THROW(std::runtime_error("IOS async ioctl needs a host implementation: " + (it == devices.end() ? std::string("invalid descriptor") : it->second) + " command " + std::to_string(ctx->gpr[4])));
}
REGISTER_NATIVE_FUNCTION(0x804BCE70, MH3_IOSIoctlAsync);

extern "C" void MH3_IOSIoctl(CpuContext* ctx) {
    if (const auto file = files.find(ctx->gpr[3]); file != files.end()) {
        if (ctx->gpr[4] != 11)
            MH3_THROW(std::runtime_error("IOS file ioctl not implemented: " + std::to_string(ctx->gpr[4])));

        if (ctx->gpr[8] < 8 || !Memory::Contains(ctx->gpr[7], 8)) {
            ctx->gpr[3] = -101;
            return;
        }

        struct stat info {};
        const auto offset = ::lseek(file->second.host, 0, SEEK_CUR);

        if (offset < 0 || ::fstat(file->second.host, &info) != 0) {
            ctx->gpr[3] = Error();
            return;
        }

        Memory::Write32(ctx->gpr[7], info.st_size);
        Memory::Write32(ctx->gpr[7] + 4, offset);
        ctx->gpr[3] = 0;
        return;
    }

    const auto device = devices.find(ctx->gpr[3]);

    if (device == devices.end()) {
        ctx->gpr[3] = -4;
        return;
    }

    const uint32_t command = ctx->gpr[4];

    // A network request may wait while other guest threads open and close
    // devices: keep the path.
    if (IosNet::IsDevice(device->second)) {
        const std::string path = device->second;
        ctx->gpr[3] = IosNet::Ioctl(ctx, path, command, ctx->gpr[5], ctx->gpr[6], ctx->gpr[7], ctx->gpr[8]);
        return;
    }

    if (device->second == "/dev/usb/hid") {
        // IOS55 HID v4 protocol: Dolphin Core/IOS/USB/USB_HID/HIDv4.cpp
        // and RMHP08 HID initialization at 0x80528AF0.
        if (command == 6) {
            ctx->gpr[3] = 0x40001;
            return;
        }

        if (command == 1) {
            ctx->gpr[3] = 0;
            return;
        } // SET_SUSPEND is an IOS no-op
        if (command == 7) {
            auto& state = hidDevices.at(ctx->gpr[3]);

            if (state.hook) {
                const auto hook = *state.hook;
                Memory::Write32(hook.output, 0xFFFFFFFF);
                completions.push_back({hook.callback, -1, hook.argument});
                state.hook.reset();
                state.changed = true;
            }

            ctx->gpr[3] = 0;
            return;
        }

        if ((command >= 2 && command <= 5) || command == 8) {
            const uint32_t expected = command == 8 ? 8 : 32;
            ctx->gpr[3] = !ctx->gpr[5] || ctx->gpr[6] != expected || !Memory::Contains(ctx->gpr[5], expected) ? -4 : -6;
            return; // No attached device matches any IOS ID.
        }

        MH3_THROW(std::runtime_error("USB HID ioctl needs a host implementation: " + std::to_string(command)));
    }

    if (device->second == "/dev/fs") {
        const uint32_t input = ctx->gpr[5], inputSize = ctx->gpr[6], output = ctx->gpr[7], outputSize = ctx->gpr[8];

        if (command == 7) {
            if (inputSize < 64 || !Memory::Contains(input, 64)) {
                ctx->gpr[3] = -101;
                return;
            }

            const auto path = GuestPath(input);
            ctx->gpr[3] = DeleteNandEntry(path);
            return;
        }

        if (command == 8) {
            if (inputSize < 128 || !Memory::Contains(input, 128)) {
                ctx->gpr[3] = -101;
                return;
            }

            const auto from = GuestPath(input), to = GuestPath(input + 64);
            ctx->gpr[3] = RenameNandEntry(from, to);
            return;
        }

        if (command == 3 || command == 9 || command == 5) {
            if (inputSize < 74 || !Memory::Contains(input, inputSize)) {
                ctx->gpr[3] = -101;
                return;
            }

            auto metadata = defaultMetadata;

            for (unsigned i = 0; i < 4; ++i)
                metadata[6 + i] = Memory::Read8(input + 70 + i);

            if (metadata[6] > 3 || metadata[7] > 3 || metadata[8] > 3) {
                ctx->gpr[3] = -101;
                return;
            }

            const auto path = GuestPath(input + 6);
            int result;

            if (command == 5) {
                for (unsigned i = 0; i < 6; ++i)
                    metadata[i] = Memory::Read8(input + i);

                const int fd = OpenNandPath(path, O_RDONLY);
                result = fd < 0 ? fd : SetMetadata(fd, metadata);

                if (fd >= 0)
                    CloseNandPath(fd);
            } else
                result = CreateNandEntry(path, command == 3, metadata);

            ctx->gpr[3] = result;
            return;
        }

        if (command == 6) {
            if (inputSize < 64 || outputSize < 74 || !Memory::Contains(input, 64) || !Memory::Contains(output, outputSize)) {
                ctx->gpr[3] = -101;
                return;
            }

            const auto path = GuestPath(input);
            const int fd = OpenNandPath(path, O_RDONLY, true);

            if (fd < 0) {
                ctx->gpr[3] = fd;
                return;
            }

            NandMetadata metadata;
            const int result = GetMetadata(fd, metadata);
            CloseNandPath(fd);

            if (result) {
                ctx->gpr[3] = result;
                return;
            }

            for (unsigned i = 0; i < 74; ++i)
                Memory::Write8(output + i, 0);

            for (unsigned i = 0; i < 6; ++i)
                Memory::Write8(output + i, metadata[i]);

            for (size_t i = 0; i < path.size(); ++i)
                Memory::Write8(output + 6 + i, path[i]);

            for (unsigned i = 0; i < 4; ++i)
                Memory::Write8(output + 70 + i, metadata[6 + i]);

            ctx->gpr[3] = 0;
            return;
        }
    }

    if (device->second == "/dev/di") {
        // IOS DI command buffer contains byte length and word-addressed offset.
        // DI uses success=1 (unlike ordinary IOS success=0).
        const uint32_t input = ctx->gpr[5], output = ctx->gpr[7], capacity = ctx->gpr[8];

        if (ctx->gpr[6] != 0x20 || !Memory::Contains(input, 0x20)) {
            ctx->gpr[3] = 32;
            return;
        }

        if (command == 0x12) {
            if (capacity < 0x20 || !Memory::Contains(output, 0x20)) {
                ctx->gpr[3] = 32;
                return;
            }

            for (uint32_t i = 0; i < 0x20; ++i)
                Memory::Write8(output + i, 0);
            // Retail Wii inquiry values recorded in Dolphin DVDInterface.cpp.
            Memory::Write32(output, 2);
            Memory::Write32(output + 4, 0x20060526);
            Memory::Write32(output + 8, 0x41000000);
            ctx->gpr[3] = 1;
            return;
        }

        if (command == 0x70 || command == 0x71) {
            const uint32_t length = command == 0x70 ? 0x20 : Memory::Read32(input + 4);
            const uint32_t position = command == 0x70 ? 0 : Memory::Read32(input + 8);

            if (length > capacity || !Memory::Contains(output, length)) {
                ctx->gpr[3] = 32;
                return;
            }

            // Straight into guest RAM (bytes are bytes; no per-byte checks).
            const auto bytes = Disc().Read(uint64_t(position) * 4, length);
            std::memcpy(Memory::GetPointer(output, length), bytes.data(), length);
            RecordNativeDiscWrite(output, uint64_t(position) * 4, length);
            lastDiscPosition = position;
            ctx->gpr[3] = 1;
            return;
        }

        if (command == 0x7A || command == 0x88 || command == 0x83 || command == 0xE0) {
            if (capacity < 4 || !Memory::Contains(output, 4)) {
                ctx->gpr[3] = 32;
                return;
            }

            Memory::Write32(output, command == 0x88 ? 2 : command == 0x83 ? lastDiscPosition : 0);
            ctx->gpr[3] = 1;
            return;
        }

        // The host-mounted disc is present and has no physical cover interrupt.
        if (command == 0x86 || command == 0x85 || command == 0x87 || command == 0x89 || command == 0x7E) {
            ctx->gpr[3] = 1;
            return;
        }
    }

    if (device->second == "/dev/stm/immediate") {
        // Hardware presentation settings, retained without driving Wii LEDs.
        // IDs/layout: Dolphin Core/IOS/STM/STM.h and MH3 0x804D6230.
        if (command == 0x6002 || command == 0x5001) {
            if (ctx->gpr[6] < 20 || !Memory::Contains(ctx->gpr[5], 20)) {
                ctx->gpr[3] = -4;
                return;
            }

            auto& settings = command == 0x6002 ? ledSettings : dimmingSettings;

            for (uint32_t i = 0; i < 20; ++i)
                settings[i] = Memory::Read8(ctx->gpr[5] + i);

            ctx->gpr[3] = 0;
            return;
        }

        if (command == 0x3002) {
            if (!eventHook) {
                ctx->gpr[3] = -6;
                return;
            }

            TriggerStmEvent(0);
            ctx->gpr[3] = 0;
            return;
        }
    }

    MH3_THROW(std::runtime_error("IOS ioctl needs a host implementation: " + device->second + " command " + std::to_string(command)));
}
REGISTER_NATIVE_FUNCTION(0x804BCFB0, MH3_IOSIoctl);

// ES_GetTitleID and ES_GetDataDir, verified at MH3's ESP wrappers
// 0x804B0060 and 0x804AFFC0. Title ID comes from this disc's signed TMD.
extern "C" void MH3_IOSIoctlv(CpuContext* ctx) {
    const auto device = devices.find(ctx->gpr[3]);

    if (device == devices.end()) {
        ctx->gpr[3] = -4;
        return;
    }

    const uint32_t command = ctx->gpr[4], inputs = ctx->gpr[5], outputs = ctx->gpr[6];
    const uint32_t vectors = ctx->gpr[7];

    if (inputs > 16 || outputs > 16 || !Memory::Contains(vectors, (inputs + outputs) * 8)) {
        ctx->gpr[3] = -4;
        return;
    }

    if (IosNet::IsDevice(device->second)) {
        const std::string path = device->second;
        ctx->gpr[3] = IosNet::Ioctlv(ctx, path, command, inputs, outputs, vectors);
        return;
    }

    if (device->second == "/dev/fs" && command == 12) {
        if (inputs != 1 || outputs != 2 || Memory::Read32(vectors + 4) != 64 || Memory::Read32(vectors + 12) != 4 || Memory::Read32(vectors + 20) != 4) {
            ctx->gpr[3] = -4;
            return;
        }

        const uint32_t pathAddress = Memory::Read32(vectors);
        const uint32_t clusterOut = Memory::Read32(vectors + 8), inodeOut = Memory::Read32(vectors + 16);

        if (!Memory::Contains(pathAddress, 64) || !Memory::Contains(clusterOut, 4) || !Memory::Contains(inodeOut, 4)) {
            ctx->gpr[3] = -4;
            return;
        }

        const auto path = GuestPath(pathAddress);
        const int directory = OpenNandPath(path, O_RDONLY | O_DIRECTORY, true);

        if (directory < 0) {
            ctx->gpr[3] = directory;
            return;
        }

        uint64_t clusters = 0, inodes = 0;
        const auto result = DirectoryUsage(directory, clusters, inodes);

        if (result) {
            ctx->gpr[3] = result;
            return;
        }

        if (clusters > UINT32_MAX || inodes > UINT32_MAX) {
            ctx->gpr[3] = -101;
            return;
        }

        Memory::Write32(clusterOut, clusters);
        Memory::Write32(inodeOut, inodes);
        ctx->gpr[3] = 0;
        return;
    }

    if (device->second == "/dev/es" && command == 0x16 && inputs == 1 && outputs == 2) {
        // New host NAND has no ticket-consumption records. The original SDK
        // still checks the ticket's own time-limit fields after this query.
        const uint32_t countOut = Memory::Read32(vectors + 16);

        if (Memory::Read32(vectors + 4) != 8 || !Memory::Contains(Memory::Read32(vectors), 8) || Memory::Read32(vectors + 20) != 4 || !Memory::Contains(countOut, 4)) {
            ctx->gpr[3] = -4;
            return;
        }

        Memory::Write32(countOut, 0);
        ctx->gpr[3] = 0;
        return;
    }

    if (device->second == "/dev/es" && command == 0x1B && inputs == 1 && outputs == 1) {
        const uint32_t input = Memory::Read32(vectors), inputSize = Memory::Read32(vectors + 4);
        const uint32_t output = Memory::Read32(vectors + 8), outputSize = Memory::Read32(vectors + 12);

        if (outputSize != 0xD8 || !Memory::Contains(output, outputSize)) {
            ctx->gpr[3] = -4;
            return;
        }

        std::array<uint8_t, 0x2A4> ticket{};

        if (!input && !inputSize) {
            const char* root = std::getenv("MH3_DISC_DIR");
            std::ifstream file((root ? std::filesystem::path(root) : "Assets/DATA") / "ticket.bin", std::ios::binary);
            file.read(reinterpret_cast<char*>(ticket.data()), ticket.size());

            if (!file)
                MH3_THROW(std::runtime_error("Unable to read disc ticket for ES view"));
        } else {
            if (inputSize != ticket.size() || !Memory::Contains(input, inputSize)) {
                ctx->gpr[3] = -4;
                return;
            }

            for (uint32_t i = 0; i < ticket.size(); ++i)
                ticket[i] = Memory::Read8(input + i);
        }

        // RSA2048 ticket v0 view: version, 3 padding bytes, then ticket ID
        // through time limits. Layout verified against Dolphin ES/Formats.h.
        if (ticket[0] || ticket[1] != 1 || ticket[2] || ticket[3] != 1 || ticket[0x1BC] != 0) {
            ctx->gpr[3] = -4;
            return;
        }

        Memory::Write32(output, 0);

        for (uint32_t i = 0; i < 0xD4; ++i)
            Memory::Write8(output + 4 + i, ticket[0x1D0 + i]);

        ctx->gpr[3] = 0;
        return;
    }

    if (device->second == "/dev/es" && outputs == 1 && ((command == 0x20 && inputs == 0) || (command == 0x1D && inputs == 1))) {
        const uint32_t out = Memory::Read32(vectors + inputs * 8);
        const uint32_t size = Memory::Read32(vectors + inputs * 8 + 4);
        constexpr uint64_t title = 0x00010000524D4850ull;
        constexpr char directory[] = "/title/00010000/524d4850/data";
        const uint32_t required = command == 0x20 ? 8 : sizeof(directory);

        if (size < required || !Memory::Contains(out, size)) {
            ctx->gpr[3] = -4;
            return;
        }

        if (command == 0x20)
            Memory::Write64(out, title);
        else {
            const uint32_t in = Memory::Read32(vectors);

            if (Memory::Read32(vectors + 4) != 8 || !Memory::Contains(in, 8)) {
                ctx->gpr[3] = -4;
                return;
            }

            if (Memory::Read64(in) != title) {
                ctx->gpr[3] = -106;
                return;
            }

            for (uint32_t i = 0; i < sizeof(directory); ++i)
                Memory::Write8(out + i, directory[i]);
        }

        ctx->gpr[3] = 0;
        return;
    }

    MH3_THROW(std::runtime_error("IOS ioctlv needs a host implementation: " + device->second + " command " + std::to_string(command)));
}
REGISTER_NATIVE_FUNCTION(0x804BD310, MH3_IOSIoctlv);

namespace {
void AsyncFile(CpuContext* ctx, void (*operation)(CpuContext*), uint32_t callback, uint32_t argument) {
    if (!callback) {
        ctx->gpr[3] = -4;
        return;
    }

    operation(ctx);
    completions.push_back({callback, static_cast<int32_t>(ctx->gpr[3]), argument});
    ctx->gpr[3] = 0;
}
} // namespace

extern "C" void MH3_IOSOpenAsync(CpuContext* ctx) {
    AsyncFile(ctx, MH3_IOSOpen, ctx->gpr[5], ctx->gpr[6]);
}
REGISTER_NATIVE_FUNCTION(0x804BC3E0, MH3_IOSOpenAsync);
extern "C" void MH3_IOSReadAsync(CpuContext* ctx) {
    AsyncFile(ctx, MH3_IOSRead, ctx->gpr[6], ctx->gpr[7]);
}
REGISTER_NATIVE_FUNCTION(0x804BC970, MH3_IOSReadAsync);
extern "C" void MH3_IOSWriteAsync(CpuContext* ctx) {
    AsyncFile(ctx, MH3_IOSWrite, ctx->gpr[6], ctx->gpr[7]);
}
REGISTER_NATIVE_FUNCTION(0x804BCB80, MH3_IOSWriteAsync);
extern "C" void MH3_IOSSeekAsync(CpuContext* ctx) {
    AsyncFile(ctx, SeekFile, ctx->gpr[6], ctx->gpr[7]);
}
REGISTER_NATIVE_FUNCTION(0x804BCD90, MH3_IOSSeekAsync);
extern "C" void MH3_IOSCloseAsync(CpuContext* ctx) {
    AsyncFile(ctx, MH3_IOSClose, ctx->gpr[4], ctx->gpr[5]);
}
REGISTER_NATIVE_FUNCTION(0x804BC750, MH3_IOSCloseAsync);
extern "C" void MH3_IOSIoctlvAsync(CpuContext* ctx) {
    if (const auto device = devices.find(ctx->gpr[3]); device != devices.end() && IosNet::IsDevice(device->second)) {
        if (!ctx->gpr[8] || ctx->gpr[5] > 16 || ctx->gpr[6] > 16 || !Memory::Contains(ctx->gpr[7], (ctx->gpr[5] + ctx->gpr[6]) * 8)) {
            ctx->gpr[3] = -4;
            return;
        }

        IosNet::IoctlvAsync(device->second, ctx->gpr[4], ctx->gpr[5], ctx->gpr[6], ctx->gpr[7], ctx->gpr[8], ctx->gpr[9]);
        ctx->gpr[3] = 0;
        return;
    }

    AsyncFile(ctx, MH3_IOSIoctlv, ctx->gpr[8], ctx->gpr[9]);
}
REGISTER_NATIVE_FUNCTION(0x804BD220, MH3_IOSIoctlvAsync);

// Boot's SCCheckStatus loop is a safe event-pump boundary before OS scheduling
// starts. Preserve the polling caller's full CPU state around each callback.
// Process a bounded batch so callbacks that resubmit cannot starve the caller.
void PumpIosCompletions(CpuContext* ctx) {
    static bool pumping = false;

    if (pumping)
        return;

    pumping = true;
    struct Reset {
        bool& flag;
        ~Reset() { flag = false; }
    } reset{pumping};
    Reader().Deliver();

    for (unsigned i = 0; i < 32; ++i) {
        auto completion = TakeIosCompletion();

        if (!completion)
            break;

        const CpuContext saved = *ctx;
        ScopeExit restore([&] { *ctx = saved; });
        ctx->gpr[3] = completion->result;
        ctx->gpr[4] = completion->argument;
        InvokeIndirectCpu(completion->callback, ctx);
    }
}

extern "C" void func_804DB770(CpuContext*);
extern "C" void MH3_SCCheckStatus(CpuContext* ctx) {
    PumpIosCompletions(ctx);
    func_804DB770(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x804DB770, MH3_SCCheckStatus, "MH3_SCCheckStatus");

// MH3 polls DVD command status while waiting for module/asset reads. Deliver
// queued IOS completions before querying the original SDK state machine.
extern "C" void func_804AACC0(CpuContext*);
extern "C" void MH3_DVDGetCommandBlockStatus(CpuContext* ctx) {
    PumpIosCompletions(ctx);
    func_804AACC0(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x804AACC0, MH3_DVDGetCommandBlockStatus, "MH3_DVDGetCommandBlockStatus");

// DVDCancelAsync (0x804AAE90), decompiled with Ghidra. For the block whose
// read is in flight it sets Canceling, the break flag and the cancel callback,
// and the drive's reply finishes the cancel: cbForStateBusy (0x804AA110) marks
// the block canceled, calls its callback with -3 and then the cancel callback,
// and starts the next command. DVDCancel (0x804AB200) sleeps on the DVD thread
// queue until then; this runtime switches no guest threads, so that sleep
// never reaches a pump to deliver the reply (a movie skipped while its read
// was in flight hung the Vita). The disc reader breaks the read instead, and
// its reply arrives before DVDCancelAsync returns, as the drive's interrupt
// could right after it.
extern "C" void func_804AAE90(CpuContext*);
extern "C" void MH3_DVDCancelAsync(CpuContext* ctx) {
    enum : int32_t {
        kBreaking = -0x3D60,  // s32: stop the read in flight
        kExecuting = -0x3CF0, // DVDCommandBlock* executing
    };

    const uint32_t block = ctx->gpr[3], sda = ctx->gpr[13];
#ifdef MH3_DEVELOPMENT
    const uint32_t state = Memory::Read32(block + 0xC), executing = Memory::Read32(sda + uint32_t(kExecuting));
#endif
    func_804AAE90(ctx);
#ifdef MH3_DEVELOPMENT
    if (state == 1 || state == 2) // busy or waiting; DVDClose cancels every finished block too
        std::fprintf(stderr, "MH3 disc: DVDCancelAsync of block 0x%08X (state %d, %s) returned %u, breaking %u\n", block, int32_t(state),
                     executing == block ? "executing" : "not executing", ctx->gpr[3], Memory::Read32(sda + uint32_t(kBreaking)));
#endif
    if (!ctx->gpr[3] || !Memory::Read32(sda + uint32_t(kBreaking)) || Memory::Read32(sda + uint32_t(kExecuting)) != block)
        return;

    Reader().Break();
    const CpuContext saved = *ctx;
    PumpIosCompletions(ctx);
    *ctx = saved;
}
REGISTER_NATIVE_FUNCTION_AS(0x804AAE90, MH3_DVDCancelAsync, "DVDCancelAsync");

// DVDGetDriveStatus (0x804AAD90), decompiled with Ghidra. The frame counter's
// retrace callbacks sample it every retrace.
extern "C" void MH3_DVDGetDriveStatus(CpuContext* ctx) {
    enum : int32_t {
        kFatalError = -0x3D74, // s32 FatalErrorFlag
        kPausing = -0x3D78,    // s32 PausingFlag
        kExecuting = -0x3CF0,  // DVDCommandBlock* executing
    };

    constexpr uint32_t kDummyCommandBlock = 0x80748A20;
    const CpuContext saved = *ctx;
    const uint32_t sda = saved.gpr[13];
    ctx->msr &= ~0x8000u; // OSDisableInterrupts (inlined)
    uint32_t status;

    if (Memory::Read32(sda + uint32_t(kFatalError)))
        status = 0xFFFFFFFF;
    else if (Memory::Read32(sda + uint32_t(kPausing)))
        status = 8; // DVD_STATE_PAUSING
    else {
        const uint32_t executing = Memory::Read32(sda + uint32_t(kExecuting));

        if (!executing || executing == kDummyCommandBlock)
            status = 0;
        else {
            ctx->gpr[3] = executing;
            InvokeIndirectCpu(0x804AACC0, ctx); // DVDGetCommandBlockStatus
            status = ctx->gpr[3];
        }
    }

    *ctx = saved; // OSRestoreInterrupts
    ctx->gpr[3] = status;
}
REGISTER_NATIVE_FUNCTION_AS(0x804AAD90, MH3_DVDGetDriveStatus, "DVDGetDriveStatus");

// WaitDvd (0x80435EA8), decompiled with Ghidra: the game's synchronous file
// loads (load_file 0x800CECC8 and its siblings) call it to wait until the DVD
// command block `block` is done, or with 0 until the drive is idle as the
// retrace callback last sampled it (DVDGetDriveStatus). While the drive
// reports an error it shows the disc error screen, drawing frames itself;
// a reset or power press stops the sound and restarts or shuts down.
//
// On the Wii the retrace interrupt and the drive's reply arrive during the
// spin. Here they arrive at pump points and disc reads finish on the reader
// thread, so while the drive is busy the wait delivers device work and
// sleeps until the reader's next reply (or 1 ms): otherwise a busy status
// sampled before the spin would never change.
void PumpDevices(CpuContext* cpu); // os_sleep.cpp
namespace {
enum : uint32_t {
    WPADGetStatus = 0x804EDCA0, // the WPAD library state, interrupts disabled
    WPADDisconnect = 0x804EEF40,
    DVDGetCommandBlockStatus = 0x804AACC0,
    SetMasterVolume = 0x800E874C, // AXSetMasterVolume, interrupts disabled
    SePause = 0x800E8338,         // SeManager state 2
    SeResume = 0x800E8378,        // SeManager state 4
    SeStop = 0x800D849C,          // SeManager state 1
    StreamPause = 0x800E4B30,     // StreamManager state 2
    StreamResume = 0x800E4B70,    // StreamManager state 4
    VIEnableDimming = 0x804E96A0,
    func_8045607C = 0x8045607C,
    RunDrawHook = 0x80040890, // the installed draw callback (0x8065B360), if any
    DrawDiscErrorMessage = 0x80435DBC,
    FontFlush = 0x8005C174,
    NativeFrameEnd = 0x8003FFFC,
    FramePacer = 0x800401A8,
    FontStackReset = 0x8005B52C, // flfntStackReset
    RumbleStopAll = 0x80044AB8,
    BgmStopAll = 0x800F6398,
    NativeMovieCleanup = 0x804360B0,
    OSShutdownSystem = 0x804D2BA0,
    OSRestart = 0x804D2CC0,
    QuestResetTimeBase = 0x803A91F0,
};

constexpr uint32_t kWaiting = 0x8065B2D0;   // u8: inside WaitDvd
constexpr uint32_t kErrorSeen = 0x8065B2D1; // u8: the drive reported a fatal error
constexpr uint32_t kWait = 0x806D6D40;      // s32 +4 error screen kind (-1 none), u8 +9 dimming, u8 +11 outer kWaiting
constexpr int32_t kDriveStatus = -0x4688;   // r13: s32 DVDGetDriveStatus at the last retrace
constexpr int32_t kPowerPressed = -0x467A;  // r13: u8
constexpr int32_t kResetPressed = -0x467B;  // r13: u8

void WaitDvd(GuestCall& game, uint32_t block, uint32_t sda) {
    Memory::Write8(kWait + 11, Memory::Read8(kWaiting) != 0);
    Memory::Write8(kWaiting, 1);
    int32_t status;

    do {
        if (int32_t(game.Call<WPADGetStatus>()) == 3) {
            game.Call<WPADDisconnect>({2});
            game.Call<WPADDisconnect>({3});
        }

        status = block ? int32_t(game.Call<DVDGetCommandBlockStatus>({block})) : int32_t(Memory::Read32(sda + uint32_t(kDriveStatus)));
        int32_t screen = -1;

        switch (status) {
        case -1: // fatal error
            screen = 3;
            Memory::Write8(kErrorSeen, 1);
            break;
        case 4: // no disc
            screen = 0;
            break;
        case 6: // wrong disc
            screen = 1;
            break;
        case 11: // retry
            screen = 2;
            break;
        }

        Memory::Write32(kWait + 4, uint32_t(screen));

        if (screen != -1) {
            game.Call<SetMasterVolume>({0});
            game.Call<SePause>();
            game.Call<StreamPause>();
            Memory::Write8(kWait + 9, game.Call<VIEnableDimming>({1}) != 0);
            game.Call<func_8045607C>();
            game.Call<RunDrawHook>();
            game.Call<DrawDiscErrorMessage>();
            game.Call<FontFlush>();
            game.Call<NativeFrameEnd>();
            game.Call<FramePacer>();
            game.Call<FontStackReset>();
        }

        if (status != -1 && (Memory::Read8(sda + uint32_t(kPowerPressed)) == 1 || Memory::Read8(sda + uint32_t(kResetPressed)) == 1)) {
            if (int32_t(game.Call<WPADGetStatus>()) == 3)
                game.Call<RumbleStopAll>();

            game.Call<SeStop>();
            game.Call<BgmStopAll>();
            game.Call<NativeMovieCleanup>();

            if (Memory::Read8(sda + uint32_t(kPowerPressed)))
                game.Call<OSShutdownSystem>();

            if (Memory::Read8(sda + uint32_t(kResetPressed)))
                game.Call<OSRestart>({0});
        }

        if (status != 0 && screen == -1) {
            // Busy: let the reply and the retrace that report the drive idle arrive.
            const CpuContext saved = *game.cpu;
            PumpDevices(game.cpu);
            *game.cpu = saved;

            if (!Reader().Await(std::chrono::milliseconds(1)))
                std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
    } while (status != 0);
    game.Call<SetMasterVolume>({0x8000});
    game.Call<SeResume>();
    game.Call<StreamResume>();
    Memory::Write8(kWaiting, Memory::Read8(kWait + 11));

    if (int32_t(Memory::Read32(kWait + 4)) != -1) {
        game.Call<VIEnableDimming>({Memory::Read8(kWait + 9)});
        game.Call<QuestResetTimeBase>();
    }
}
} // namespace

extern "C" void MH3_WaitDvd(CpuContext* ctx) {
    GuestFrame frame(ctx, 48);
    GuestCall game{ctx};
    WaitDvd(game, frame.Arg(3), frame.Arg(13));
}
REGISTER_NATIVE_FUNCTION_AS(0x80435EA8, MH3_WaitDvd, "WaitDvd");
