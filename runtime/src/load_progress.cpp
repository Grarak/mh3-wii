// Loading-screen progress (docs/loading-progress.md). A loading screen lasts
// as long as the game's loads: the file loader task's queue (load_file_req
// 0x800CEB14 appends, 0x800CE8E8 pops, the task 0x800CE704 reads the front
// entry), the synchronous loads (load_file 0x800CECC8), and with load-time
// conversion the textures and models of the files they read, which their
// reads wait for (asset_cache.h).
//
// Progress counts bytes read from the disc (delivered to guest memory) and
// source bytes converted (each texture and model job counts its file), alike:
// the Pi 5 converts a load's files faster than it decodes them from the
// image, and an early 32-fold weight made the bar of a converting load jump
// most of the way once its conversions drained. Left are the queued files
// (their FST sizes), the rest of the DVD request being read and the
// conversions queued.
// The game queues a load's files in batches (Alatreon's quest: 34 files, then
// 23 more once the first are in, ...), so the bytes a screen reads in all are
// learned: a screen is known by the files it queues, in their order, and its
// disc bytes are kept (graphics/cache/loading-screens.bin) for the next screen
// that queues the same files.
#include "load_progress.h"
#include "disc_image.h"
#include "fs_status.h"
#include "memory.h"
#ifdef MH3_DEVELOPMENT
#include "dev_options.h"
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

// Loader task work (0x806970E8, decompiled from 0x800CE618..0x800CECC8).
constexpr uint32_t kLoader = 0x806970E8;
enum : uint32_t {
    kState = 0x00,    // u32: 0 idle, 1 opening the front entry, 2 reading it, 3 read
    kCount = 0x04,    // s32: queued entries, the front one included (at most 99)
    kRequests = 0x48, // u32: the entries, 340 bytes each
};

constexpr uint32_t kEntryBytes = 340, kEntryCapacity = 0x00, kEntryPath = 0x08, kPathBytes = 256, kMaxEntries = 99;

// A learned screen: its disc bytes and the first files it queued (FNV-1a of
// each path; a list shorter than kScreenFiles is all it queued).
constexpr uint32_t kScreenFiles = 64, kMaxScreens = 64;
constexpr uint32_t kMagic = 0x504c334d, kVersion = 2; // "M3LP"
struct Learned {
    uint64_t bytes;
    uint32_t count, files[kScreenFiles];
};

// Disc reads (game thread): the bytes delivered, and the DVD request being
// read (the library reads one at a time): [start, end), delivered up to `to`.
// A new request ends the last one (a canceled read leaves its rest unread).
uint64_t delivered = 0;
struct Request {
    uint64_t start = 0, end = 0, to = 0;
    uint64_t Left() const { return end > std::max(start, to) ? end - std::max(start, to) : 0; }
} request;

// Conversions (any thread), in bytes.
std::atomic<uint64_t> conversionQueued{0}, conversionDone{0};

// One loading screen: the counters when it began, the files it queued, the
// learned screen it matches (null: none) and the share shown.
struct Screen {
    bool open = false;
    uint64_t delivered = 0, converted = 0;
    std::vector<std::string> paths;
    std::vector<uint32_t> files;
    const Learned* match = nullptr;
    float shown = 0;
} screen;

std::vector<Learned> learned; // the most recent last
bool learnedLoaded = false;
std::unordered_map<std::string, uint32_t> fileSizes;

std::filesystem::path LearnedPath() {
    const char* user = std::getenv("MH3_USER_DIR");
    return (user ? std::filesystem::path(user) : std::filesystem::path("saves")) / "graphics" / "cache" / "loading-screens.bin";
}

void LoadLearned() {
    learnedLoaded = true;

    std::FILE* in = std::fopen(LearnedPath().string().c_str(), "rb");

    if (!in)
        return;

    uint32_t header[3] = {};

    if (std::fread(header, sizeof header, 1, in) == 1 && header[0] == kMagic && header[1] == kVersion && header[2] <= kMaxScreens) {
        learned.resize(header[2]);

        if (header[2] && std::fread(learned.data(), sizeof(Learned), header[2], in) != header[2])
            learned.clear();
    }

    std::fclose(in);
    learned.erase(std::remove_if(learned.begin(), learned.end(), [](const Learned& l) { return l.count > kScreenFiles; }), learned.end());
}

// Written whole under a temporary name, then renamed: a lost write only
// loses what was learned.
void SaveLearned() {
    const auto path = LearnedPath(), temporary = std::filesystem::path(path.string() + ".tmp");
    std::FILE* out = std::fopen(temporary.string().c_str(), "wb");

    if (!out)
        return;

    const uint32_t header[3] = {kMagic, kVersion, uint32_t(learned.size())};
    bool written = std::fwrite(header, sizeof header, 1, out) == 1 && std::fwrite(learned.data(), sizeof(Learned), learned.size(), out) == learned.size();
    written = std::fclose(out) == 0 && written;
    std::error_code ignored;

    if (written) {
        std::filesystem::remove(path, ignored);
        written = FsStatus::Rename(temporary, path).ok();
    }

    if (!written)
        std::filesystem::remove(temporary, ignored);
}

uint32_t Fnv(const std::string& text) {
    uint32_t hash = 2166136261u;

    for (const char c : text)
        hash = (hash ^ uint8_t(c)) * 16777619u;

    return hash;
}

std::string EntryPath(uint32_t entry) {
    const auto* text = reinterpret_cast<const char*>(Memory::GetPointer(entry + kEntryPath, kPathBytes));
    size_t length = 0;

    while (length < kPathBytes - 1 && text[length])
        ++length;

    size_t first = 0;

    while (first < length && text[first] == '/')
        ++first;

    return std::string(text + first, text + length);
}

// The size of a queued file as the FST has it, no more than its entry's
// buffer: 0 when unknown.
uint32_t QueuedBytes(uint32_t entry, const std::string& path) {
    auto found = fileSizes.find(path);

    if (found == fileSizes.end())
        found = fileSizes.emplace(path, GameDiscImage().FileSizeOrZero(path)).first;

    return std::min(found->second, Memory::Read32(entry + kEntryCapacity));
}

// The most recent learned screen that queued the same files so far.
void Match() {
    const auto agrees = [](const Learned& l) {
        if (screen.files.size() > l.count && l.count < kScreenFiles)
            return false;

        const size_t n = std::min<size_t>(screen.files.size(), l.count);
        return std::equal(screen.files.begin(), screen.files.begin() + n, l.files);
    };

    const auto found = std::find_if(learned.rbegin(), learned.rend(), agrees);
    screen.match = found == learned.rend() ? nullptr : &*found;
}

// The loader queue's files not being read yet; notes the screen's new ones.
uint64_t QueuedLoads(uint32_t& files) {
    const uint32_t state = Memory::Read32(kLoader + kState), base = Memory::Read32(kLoader + kRequests);
    const int32_t count = int32_t(Memory::Read32(kLoader + kCount));
    files = 0;

    if (!base || count <= 0 || uint32_t(count) > kMaxEntries || !Memory::Contains(base, uint32_t(count) * kEntryBytes))
        return 0;

    uint64_t bytes = 0;

    for (uint32_t i = 0; i < uint32_t(count); ++i) {
        const uint32_t entry = base + i * kEntryBytes;
        const std::string path = EntryPath(entry);

        if (screen.files.size() < kScreenFiles && std::find(screen.paths.begin(), screen.paths.end(), path) == screen.paths.end()) {
            screen.paths.push_back(path);
            screen.files.push_back(Fnv(path));
            Match();
#ifdef MH3_DEVELOPMENT
            static const bool trace = TraceOptions().Has("loading");

            if (trace)
                std::fprintf(stderr, "MH3 loading: queued %s (%u bytes), learned %.2f MiB\n", path.c_str(), QueuedBytes(entry, path), screen.match ? screen.match->bytes / 1048576.0 : 0.0);
#endif
        }

        // The front entry is read (state 2-3) by the DVD request in flight.
        if (i || state < 2) {
            bytes += QueuedBytes(entry, path);
            ++files;
        }
    }

    return bytes;
}

// The screen closed: it is learned (again) as the most recent.
void Close() {
    screen.open = false;
    const uint64_t bytes = delivered - screen.delivered;

    if (screen.files.empty() || !bytes)
        return;

    Learned record{bytes, uint32_t(screen.files.size()), {}};
    std::copy(screen.files.begin(), screen.files.end(), record.files);
    screen.match = nullptr;
    learned.erase(std::remove_if(learned.begin(), learned.end(), [&](const Learned& l) { return l.count == record.count && std::equal(l.files, l.files + l.count, record.files); }),
                  learned.end());

    if (learned.size() >= kMaxScreens)
        learned.erase(learned.begin());

    learned.push_back(record);
    SaveLearned();
}

} // namespace

namespace LoadProgress {

void DiscRead(uint64_t offset, uint32_t length, uint64_t end) {
    // The DVD library reads a request in chunks, each starting where the
    // last one ended; the first one tells the whole request.
    if (end <= offset)
        end = offset + length;

    if (end != request.end || offset != std::max(request.start, request.to))
        request = {offset, end, offset};
}

void DiscDelivered(uint64_t offset, uint32_t length) {
    delivered += length;

    if (offset >= request.start && offset + length <= request.end)
        request.to = std::max(request.to, offset + length);
}

void ConversionQueued(uint64_t bytes) {
    conversionQueued.fetch_add(bytes, std::memory_order_relaxed);
}

void ConversionDone(uint64_t bytes) {
    conversionDone.fetch_add(bytes, std::memory_order_relaxed);
}

float Frame() {
    if (!learnedLoaded)
        LoadLearned();

    // A screen lasts until a frame draws none (Idle); stalls of the original's
    // length within it continue it.
    const uint64_t converted = conversionDone.load(std::memory_order_relaxed), queued = conversionQueued.load(std::memory_order_relaxed);

    if (!screen.open) {
        screen = {};
        screen.open = true;
        screen.delivered = delivered;
        screen.converted = converted;
        fileSizes.clear();
    }

    uint32_t files = 0;
    const uint64_t waiting = QueuedLoads(files);
    const uint64_t read = delivered - screen.delivered, conversion = converted - screen.converted;
    const uint64_t converting = queued > converted ? queued - converted : 0;

    // The disc bytes still to come: what the loader has queued or is reading,
    // or what the screen read when last shown, whichever is more. A screen
    // not learned yet assumes as much again as it has read and queued.
    const uint64_t known = read + request.Left() + waiting;
    const uint64_t disc = std::max(known, screen.match ? screen.match->bytes : 2 * known);
    const uint64_t done = read + conversion, total = disc + conversion + converting;
    const float share = total ? float(double(done) / double(total)) : 1.0f;
    screen.shown = std::min(1.0f, std::max(screen.shown, share));
#ifdef MH3_DEVELOPMENT
    static const bool trace = TraceOptions().Has("loading");

    if (trace)
        std::fprintf(stderr, "MH3 loading: %.3f state %u queue %u files %.2f MiB, disc %.2f MiB read %.2f in flight, learned %.2f, conversion %.2f MiB done %.2f queued -> %.3f shown %.3f\n",
                     std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(), Memory::Read32(kLoader + kState), files, waiting / 1048576.0,
                     read / 1048576.0, request.Left() / 1048576.0, screen.match ? screen.match->bytes / 1048576.0 : 0.0, conversion / 1048576.0,
                     converting / 1048576.0, share, screen.shown);
#endif
    return screen.shown;
}

void Idle() {
    if (screen.open)
        Close();
}

} // namespace LoadProgress
