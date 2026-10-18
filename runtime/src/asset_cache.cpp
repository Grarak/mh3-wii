#include "asset_cache.h"
#include "fatal.h"
#include "fs_status.h"
#include "disc_image.h"
#include "launcher_convert.h"
#include "load_progress.h"
#include <aurora/thread_role.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <thread>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <pthread.h>
#include <stdexcept>
#ifdef __vita__
#include <psp2/io/devctl.h>
#include <psp2/io/fcntl.h>
#endif

namespace {
using namespace VitaLauncher;

// Bump when converted pack contents change: the whole cache is rebuilt.
constexpr const char* Version = "mh3-asset-cache 1\n";

// The converted models' own format, so a model converter change reconverts
// only the models (2: GPU skinning, docs/optimizations/rendering/gpu-skinning.md).
constexpr const char* ModelFormat = "mh3-model-packs 2\n";

// The same for the texture packs (2: exact small formats,
// docs/optimizations/assets/compressed-textures.md; 3: the Vita's buttons,
// button_glyphs.cpp; 4: the buttons centred).
constexpr const char* TextureFormat = "mh3-texture-packs 4\n";
constexpr uint32_t RecordMagic = 0x5243334d; // "M3CR"
constexpr unsigned Workers = 3;
#ifdef __vita__
// A model workspace is ~30 MiB of the 112 MiB newlib heap: one at a time.
constexpr unsigned ModelWorkers = 1;

// The game thread waits once this much indexed file data awaits conversion.
constexpr size_t MaxPendingBytes = 16u << 20;
#else
constexpr unsigned ModelWorkers = Workers;
constexpr size_t MaxPendingBytes = 48u << 20;
#endif
// Extracted stream files (cache/files): the disc's audio streams, copied out
// as plain files at their first read so that later reads skip the image's
// decompression and decryption. Least recently used files go beyond
// AssetCache::StreamCacheBytes, or beyond what the storage holds at startup
// less a reserve for saves (the Vita's launcher warns then).
constexpr uint64_t StorageReserve = 32ull << 20;

// Small enough that game reads waiting on the image's lock are not delayed long.
constexpr size_t ExtractChunk = 256u << 10;

// Extracted files open at once, the least recently read closing first: the
// Vita gives a process 50 descriptors, shared with the texture packs and
// online play's sockets, and 64 MiB of streams were dozens of files.
constexpr size_t MaxOpenCopies = 6;

uint32_t Le32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

uint64_t Le64(const uint8_t* p) {
    return Le32(p) | uint64_t(Le32(p + 4)) << 32;
}

void Put32(std::vector<uint8_t>& out, uint32_t v) {
    for (unsigned i = 0; i < 4; ++i)
        out.push_back(uint8_t(v >> (8 * i)));
}

void Put64(std::vector<uint8_t>& out, uint64_t v) {
    Put32(out, uint32_t(v));
    Put32(out, uint32_t(v >> 32));
}

uint32_t Fnv(const uint8_t* p, size_t n) {
    uint32_t h = 2166136261u;

    for (size_t i = 0; i < n; ++i)
        h = (h ^ p[i]) * 16777619u;

    return h;
}

std::string Hex16(uint64_t value) {
    char text[17];
    std::snprintf(text, sizeof text, "%016llx", static_cast<unsigned long long>(value));
    return text;
}

std::vector<uint8_t> Load(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

// Free bytes on the storage holding `path`; UINT64_MAX when unknown.
uint64_t FreeStorage(const std::filesystem::path& path) {
#ifdef __vita__
    const std::string device = path.string().substr(0, path.string().find(':') + 1);
    SceIoDevInfo storage{};

    if (device.empty() || sceIoDevctl(device.c_str(), 0x3001, nullptr, 0, &storage, sizeof storage) < 0)
        return UINT64_MAX;

    return uint64_t(storage.free_size);
#else
    std::error_code error;
    const auto space = std::filesystem::space(path, error);
    return error ? UINT64_MAX : uint64_t(space.available);
#endif
}

// Bytes of the regular files directly under `folder`.
uint64_t FolderBytes(const std::filesystem::path& folder) {
    uint64_t bytes = 0;
    std::error_code error;
    std::filesystem::directory_iterator entries(folder, error);

    for (; !error && entries != std::filesystem::directory_iterator(); entries.increment(error)) {
        std::error_code sizeError;
        const uint64_t size = entries->file_size(sizeError);
        bytes += sizeError ? 0 : size;
    }

    return bytes;
}

// The converted packs, stream copies and index under `cache`, in bytes.
uint64_t CacheUse(const std::filesystem::path& cache) {
    return FolderBytes(cache) + FolderBytes(cache / "textures") + FolderBytes(cache / "models") + FolderBytes(cache / "files");
}

std::string MiB(uint64_t bytes) {
    return std::to_string((bytes + (1u << 19)) >> 20) + " MiB";
}

// Write to a temporary name, then publish: a file under its final name is
// complete. A failed write is written again once: on the Vita a suspend
// invalidates the descriptors of files open under ux0:data, a write it cut
// among them (VitaIo::PreadReopening).
Status Publish(const std::filesystem::path& path, std::span<const uint8_t> bytes) {
    const auto temporary = std::filesystem::path(path.string() + ".tmp");
    const auto write = [&] {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        out.close();
        return bool(out);
    };

    if (!write() && !write())
        return Status::Error("Cannot write " + path.string() + " (not enough space?)");

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    return FsStatus::Rename(temporary, path);
}

bool Exists(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::exists(path, error);
}
#ifdef MH3_DEVELOPMENT
// Development log of conversion jobs and of the game thread's waits for them:
// a first load of a map converts its packs on the workers while the game
// thread may wait, which is otherwise silent.
double Seconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
#endif
} // namespace

struct AssetCache::Impl {
    struct Source {
        SourceFile file;
        bool indexed = false, checked = false, indexing = false;
        uint32_t firstTexture = 0, textureCount = 0, firstModel = 0, modelCount = 0;
        std::vector<uint64_t> patches; // texture i's tag offset; its ID is firstTexture + i
        bool texturesDone = false, modelsDone = false, modelsUsable = false;
        bool announced = false;       // its texture pack went to packReady
        bool modelsAnnounced = false; // its model pack went to modelReady
        std::string failure;

        // The whole file while its conversion is pending; game reads are served from it.
        std::shared_ptr<const std::vector<uint8_t>> data;
    };

    enum class Kind { Textures, Models, Extract, TextureHelp };

    // A disc file served from a plain copy once extracted.
    struct Extract {
        SourceFile file;
        enum class State { None, Queued, Ready } state = State::None;
        std::FILE* copy = nullptr; // open while Ready and recently read (OpenCopy)
        uint64_t used = 0;         // last read, for eviction
        bool failed = false;       // no retry this launch
    };

    // One file's textures converted by several workers (ConvertTextures):
    // the owner job places every payload in the pack beforehand, then it and
    // the helper jobs it queues claim the textures in order, one at a time,
    // and put each at its place. The pack is the serial one, byte for byte;
    // a worker holds one texture's buffers at a time.
    struct TextureWork {
        const Source* source = nullptr;
        std::shared_ptr<const std::vector<uint8_t>> data;
        std::vector<TextureInventory::Texture> textures;
        TexturePackWriter writer;
        std::atomic<uint32_t> next{0};
        std::mutex lock;
        std::condition_variable idle;
        unsigned active = 0; // workers converting
        Status failure;
        std::atomic<uint32_t> helped{0}; // textures the helpers converted

        // Converts textures until none is left to claim. A helper that comes
        // after the last claim converts nothing.
        void Run(bool helper) {
            {
                std::lock_guard guard(lock);
                ++active;
            }

            for (uint32_t i; (i = next++) < textures.size();) {
                helped += helper;
                TextureConversion converted;
                Status status = ConvertTexture(textures[i], *data, source->file.offset, source->file.path, converted);

                if (status.ok())
                    status = writer.Put(i, std::move(converted));

                if (!status.ok()) {
                    std::lock_guard guard(lock);

                    if (failure.ok())
                        failure = std::move(status);

                    next = uint32_t(textures.size());
                }
            }

            std::lock_guard guard(lock);
            --active;
            idle.notify_all();
        }
    };

    struct Job {
        Source* source;
        Kind kind;
        Extract* extract = nullptr;
        std::shared_ptr<TextureWork> textures; // Kind::TextureHelp
    };

    std::filesystem::path root, cache;
    RawRead read;
    PackReady packReady;
    ModelPackReady modelReady;
    std::vector<Source> sources;        // by partition offset
    std::vector<Extract> extracts;      // by partition offset
    uint64_t extractedBytes = 0, reads = 0;
    uint64_t extractBudget = AssetCache::StreamCacheBytes;
    size_t openCopies = 0;

    // Bytes that writes granted by Room are putting on the storage.
    uint64_t writing = 0;

    // Learned prefetch: the streams each map read (cache/files/streams.log,
    // "map-name offset" lines), queued again when that map loads.
    std::string streamContext;
    std::map<std::string, std::vector<uint64_t>> streamsByMap;
    std::map<uint32_t, Source*> byTexture; // first texture ID -> source
    uint32_t nextTexture = 1, nextModel = 1;
    std::mutex lock;
    std::condition_variable changed;
    std::deque<Job> jobs;
    size_t pendingBytes = 0;
    unsigned modelJobs = 0;

    // Model workspaces not in use (~30 MiB each): shared by the workers, so no
    // more exist than model jobs run at once, and kept once made, so a later
    // load never waits for that much memory.
    std::vector<std::unique_ptr<ModelConverter>> spareConverters;
    bool stopping = false;
    std::vector<pthread_t> threads;
#ifdef MH3_DEVELOPMENT
    // Watchdog view of the game's waits and the workers' steps (no lock
    // needed to read): the file a wait or a job concerns stays valid while
    // the cache lives (sources and extracts never move).
    struct Step {
        std::atomic<const char*> what{nullptr}, file{nullptr};
        std::atomic<double> since{0};
        void Set(const char* w, const char* f) {
            file = f;
            since = Seconds();
            what = w;
        }
    };

    Step gameStep;
    std::atomic<const Source*> gameSource{nullptr};

    // The disc reader's wait for a file that is still converting (Converted).
    Step readerStep;
    std::atomic<const Source*> readerSource{nullptr};
    std::array<Step, Workers> workerSteps;
    std::atomic<unsigned> nextWorker{0};
    pthread_t watchdog{};
    void Watch();
#endif

    // VitaSDK's pthread condition variable (pthread-embedded) sometimes loses
    // a broadcast: a waiter whose condition already holds stays blocked
    // (a cold load of Moga Woods stalled on a published model pack). Every
    // wait here re-checks its condition at least every 20 ms.
    template <class Ready>
    void Wait(std::unique_lock<std::mutex>& guard, Ready ready) {
        while (!ready())
            changed.wait_for(guard, std::chrono::milliseconds(20));
    }

    std::filesystem::path TexturePath(const Source& s) const { return cache / "textures" / (Hex16(s.file.offset) + "-" + std::to_string(s.firstTexture) + ".bin"); }
    std::filesystem::path ModelPath(const Source& s) const { return cache / "models" / (Hex16(s.file.offset) + "-" + std::to_string(s.firstModel) + ".bin"); }
    std::filesystem::path SkipPath(const Source& s) const { return cache / "models" / (Hex16(s.file.offset) + "-" + std::to_string(s.firstModel) + ".skip"); }
    std::filesystem::path ExtractPath(const Extract& e) const { return cache / "files" / (Hex16(e.file.offset) + "-" + std::to_string(e.file.bytes) + ".bin"); }

    Impl(const std::filesystem::path& r, std::vector<SourceFile> files, std::vector<SourceFile> streams, RawRead raw) : root(r), cache(r / "cache"), read(std::move(raw)) {
        std::sort(files.begin(), files.end(), [](const SourceFile& a, const SourceFile& b) { return a.offset < b.offset; });
        sources.resize(files.size());

        for (size_t i = 0; i < files.size(); ++i)
            sources[i].file = std::move(files[i]);

        const auto version = Load(cache / "version.txt");

        if (std::string(version.begin(), version.end()) != Version) {
            std::filesystem::remove_all(cache);
            std::filesystem::create_directories(cache / "textures");
            std::filesystem::create_directories(cache / "models");
            CheckStatus(Publish(cache / "version.txt", {reinterpret_cast<const uint8_t*>(Version), std::strlen(Version)}));
        }

        if (const auto format = Load(cache / "textures" / "format.txt"); std::string(format.begin(), format.end()) != TextureFormat) {
            std::filesystem::remove_all(cache / "textures");
            std::filesystem::create_directories(cache / "textures");
            CheckStatus(Publish(cache / "textures" / "format.txt", {reinterpret_cast<const uint8_t*>(TextureFormat), std::strlen(TextureFormat)}));
        }

        if (const auto format = Load(cache / "models" / "format.txt"); std::string(format.begin(), format.end()) != ModelFormat) {
            std::filesystem::remove_all(cache / "models");
            std::filesystem::create_directories(cache / "models");
            CheckStatus(Publish(cache / "models" / "format.txt", {reinterpret_cast<const uint8_t*>(ModelFormat), std::strlen(ModelFormat)}));
        }

        RemoveLeftovers();
        LoadIndex();
        std::sort(streams.begin(), streams.end(), [](const SourceFile& a, const SourceFile& b) { return a.offset < b.offset; });
        std::filesystem::create_directories(cache / "files");
        extracts.resize(streams.size());

        for (size_t i = 0; i < streams.size(); ++i) {
            extracts[i].file = std::move(streams[i]);

            // A file under its final name is complete (Publish order).
            std::error_code error;
            const auto size = std::filesystem::file_size(ExtractPath(extracts[i]), error);

            if (!error && size == extracts[i].file.bytes) {
                extracts[i].state = Extract::State::Ready;
                extractedBytes += size;
            }
        }

        if (const uint64_t free = FreeStorage(root); free != UINT64_MAX) {
            const uint64_t room = extractedBytes + free;
            extractBudget = std::min(extractBudget, room > StorageReserve ? room - StorageReserve : 0);
            std::fprintf(stderr, "MH3 asset cache: %s free on the storage, the cache holds %s (%s of stream copies); it wants %s in all\n", MiB(free).c_str(), MiB(CacheUse(cache)).c_str(),
                         MiB(extractedBytes).c_str(), MiB(AssetCache::StorageWanted).c_str());
        }

        LoadStreamLog();
        threads.resize(Workers);

        for (auto& thread : threads) {
            pthread_attr_t attributes;
            pthread_attr_init(&attributes);
            pthread_attr_setstacksize(&attributes, 1u << 20);
            const int result = pthread_create(
                &thread, &attributes,
                [](void* self) -> void* {
                    static_cast<Impl*>(self)->Work();
                    return nullptr;
                },
                this);
            pthread_attr_destroy(&attributes);

            if (result)
                MH3_THROW(std::runtime_error("Cannot start asset conversion workers"));
        }
#ifdef MH3_DEVELOPMENT
        pthread_create(&watchdog, nullptr, [](void* self) -> void* { static_cast<Impl*>(self)->Watch(); return nullptr; }, this);
#endif
    }

    ~Impl() {
        {
            std::lock_guard guard(lock);
            stopping = true;
        }

        changed.notify_all();

        for (auto thread : threads)
            pthread_join(thread, nullptr);
#ifdef MH3_DEVELOPMENT
        pthread_join(watchdog, nullptr);
#endif
        for (auto& e : extracts)
            CloseCopy(e);
    }

    // Lock held. Opens a Ready file's copy, closing the least recently read
    // open one beyond MaxOpenCopies.
    bool OpenCopy(Extract& e) {
        if (e.copy)
            return true;

        while (openCopies >= MaxOpenCopies) {
            Extract* oldest = nullptr;

            for (auto& other : extracts)
                if (other.copy && (!oldest || other.used < oldest->used))
                    oldest = &other;

            if (!oldest)
                break;

            CloseCopy(*oldest);
        }

        e.copy = std::fopen(ExtractPath(e).string().c_str(), "rb");
        openCopies += e.copy != nullptr;

        // Unbuffered: newlib reads each served range in one call (a buffered
        // fread took 6.6 s for a 1.5 MiB model payload on the Vita).
        if (e.copy)
            std::setvbuf(e.copy, nullptr, _IONBF, 0);

        return e.copy;
    }

    // Lock held.
    void CloseCopy(Extract& e) {
        if (!e.copy)
            return;

        std::fclose(e.copy);
        e.copy = nullptr;
        --openCopies;
    }

    void LoadStreamLog() {
        std::ifstream log(cache / "files" / "streams.log");
        std::string map;
        uint64_t offset = 0;

        while (log >> map >> offset)
            streamsByMap[map].push_back(offset);
    }

    // Lock held. Records that the active map read this stream.
    void NoteStream(const Extract& e) {
        if (streamContext.empty())
            return;

        auto& known = streamsByMap[streamContext];

        if (std::find(known.begin(), known.end(), e.file.offset) != known.end())
            return;

        known.push_back(e.file.offset);
        std::ofstream log(cache / "files" / "streams.log", std::ios::app);
        log << streamContext << ' ' << e.file.offset << '\n';
    }

    // Lock held. Queues the extraction of a stream not yet copied.
    void QueueExtract(Extract& e) {
        if (e.state != Extract::State::None || e.failed)
            return;

        e.state = Extract::State::Queued;
        jobs.push_back({nullptr, Kind::Extract, &e});
        changed.notify_all();
    }

    Extract* FindExtract(uint64_t at) {
        auto it = std::upper_bound(extracts.begin(), extracts.end(), at, [](uint64_t value, const Extract& e) { return value < e.file.offset; });

        if (it == extracts.begin())
            return nullptr;

        --it;
        return at - it->file.offset < it->file.bytes ? &*it : nullptr;
    }

    // Lock held. Serves [at, stop) of an extracted file; `served` is false when
    // it is not extracted (and its extraction is queued).
    Status ReadExtracted(Extract& e, uint64_t at, uint64_t stop, uint8_t* out, bool& served) {
        e.used = ++reads;
        NoteStream(e);
        QueueExtract(e);
        served = e.state == Extract::State::Ready;

        if (!served)
            return {};

        const size_t count = size_t(stop - at);
        const auto read = [&] { return OpenCopy(e) && !std::fseek(e.copy, long(at - e.file.offset), SEEK_SET) && std::fread(out, 1, count, e.copy) == count; };

        // A suspend (the PS button) invalidates descriptors under ux0:data
        // (ENODEV after the resume): open the copy again once and retry.
        if (!read()) {
            CloseCopy(e);

            if (!read())
                return Status::Error("Cannot read the extracted " + e.file.path);
        }

        return {};
    }

    // Worker: copies the file out of the image, making room by evicting the
    // least recently used copies, and publishes it.
    void RunExtract(Extract& e, std::unique_lock<std::mutex>& guard) {
        while (extractedBytes + e.file.bytes > extractBudget) {
            Extract* oldest = nullptr;

            for (auto& other : extracts)
                if (other.state == Extract::State::Ready && (!oldest || other.used < oldest->used))
                    oldest = &other;
            if (!oldest)
                break;

            Evict(*oldest);
        }

        // Larger than the whole budget, or the storage is short (the packs
        // written since the launch took it): keep reading it from the image.
        if (extractedBytes + e.file.bytes > extractBudget || !Room(e.file.bytes, false)) {
            e.state = Extract::State::None;
            e.failed = true;
            return;
        }

        extractedBytes += e.file.bytes; // reserved while copying
        guard.unlock();
#ifdef MH3_DEVELOPMENT
        const double since = Seconds();
#endif
        const auto path = ExtractPath(e), temporary = std::filesystem::path(path.string() + ".tmp");
        bool published = false;
        bool written = false;
        Status failure;
        {
            std::vector<uint8_t> chunk(ExtractChunk);

            if (std::FILE* out = std::fopen(temporary.string().c_str(), "wb")) {
                written = true;

                for (uint64_t done = 0; done < e.file.bytes && written;) {
                    const size_t count = size_t(std::min<uint64_t>(ExtractChunk, e.file.bytes - done));
                    failure = read(e.file.offset + done, chunk.data(), count);
                    written = failure.ok() && std::fwrite(chunk.data(), 1, count, out) == count;
                    done += count;
                }

                written = std::fclose(out) == 0 && written;
            }

            if (written) {
                std::error_code ignored;
                std::filesystem::remove(path, ignored);
                failure = FsStatus::Rename(temporary, path);

                published = failure.ok();
            }
        }

        if (!failure.ok()) {
            std::fprintf(stderr, "MH3 asset cache: extracting %s failed: %s\n", e.file.path.c_str(), failure.message().c_str());
            written = false;
        }

        if (!published) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
        }
#ifdef MH3_DEVELOPMENT
        if (Seconds() - since > 0.5 || !published)
            std::fprintf(stderr, "MH3 asset cache: extracted %s (%llu bytes) in %.2f s%s\n", e.file.path.c_str(), static_cast<unsigned long long>(e.file.bytes), Seconds() - since, published ? "" : ", failed");
#endif
        guard.lock();
        writing -= e.file.bytes;

        // Opened at its first read (OpenCopy).
        if (published) {
            e.state = Extract::State::Ready;
        } else {
            // Not enough space or a read error: keep reading from the image.
            extractedBytes -= e.file.bytes;
            e.state = Extract::State::None;
            e.failed = true;
        }
    }

    // Lock held. Deletes a Ready stream copy.
    void Evict(Extract& e) {
        CloseCopy(e);
        e.state = Extract::State::None;
        extractedBytes -= e.file.bytes;
        std::error_code ignored;
        std::filesystem::remove(ExtractPath(e), ignored);
    }

    // Temporary files of writes that a crash or a full storage cut short.
    void RemoveLeftovers() {
        for (const auto& folder : {cache, cache / "textures", cache / "models", cache / "files"}) {
            std::vector<std::filesystem::path> leftovers;
            std::error_code error;
            std::filesystem::directory_iterator entries(folder, error);

            for (; !error && entries != std::filesystem::directory_iterator(); entries.increment(error))
                if (entries->path().extension() == ".tmp")
                    leftovers.push_back(entries->path());

            for (const auto& path : leftovers)
                std::filesystem::remove(path, error);
        }
    }

    // Lock held. The converted packs of files this launch has not loaded,
    // one group per file and folder (a model pack with its identity), the
    // least recently written first.
    std::vector<std::vector<std::filesystem::path>> StalePacks() {
        std::map<std::string, std::pair<std::filesystem::file_time_type, std::vector<std::filesystem::path>>> groups;

        for (const char* folder : {"textures", "models"}) {
            std::error_code error;
            std::filesystem::directory_iterator entries(cache / folder, error);

            for (; !error && entries != std::filesystem::directory_iterator(); entries.increment(error)) {
                const auto& path = entries->path();
                const std::string name = path.filename().string();

                // "<offset>-<first ID>..." names; format.txt and partial writes stay.
                if (name.size() < 17 || name[16] != '-' || path.extension() == ".tmp")
                    continue;

                const uint64_t offset = std::strtoull(name.substr(0, 16).c_str(), nullptr, 16);
                const Source* s = Find(offset);

                if (s && s->file.offset == offset && (s->checked || s->indexing || s->data))
                    continue;

                auto& group = groups[std::string(folder) + "/" + name.substr(0, name.find('.'))];
                std::error_code timeError;
                group.first = std::max(group.first, std::filesystem::last_write_time(path, timeError));
                group.second.push_back(path);
            }
        }

        std::vector<std::pair<std::filesystem::file_time_type, std::vector<std::filesystem::path>>> ordered;

        for (auto& [name, group] : groups)
            ordered.push_back(std::move(group));

        std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        std::vector<std::vector<std::filesystem::path>> stale;

        for (auto& [time, paths] : ordered)
            stale.push_back(std::move(paths));

        return stale;
    }

    // Lock held. Makes room on the storage for a write of `bytes`, keeping
    // StorageReserve free beside the other writes in flight. Deletes, as
    // needed: the stream copies this launch has not read, then (with
    // `packs`) the packs of files it has not loaded, then the least recently
    // read stream copies. Deleted files are made again at their next use.
    // On success the bytes count as in flight until the writer subtracts
    // them from `writing`.
    bool Room(uint64_t bytes, bool packs) {
        const uint64_t before = FreeStorage(cache);
        uint64_t free = before, streamBytes = 0, packBytes = 0;
        unsigned streamFiles = 0, packFiles = 0;
        const auto enough = [&] { return free == UINT64_MAX || free >= bytes + writing + StorageReserve; };
        const auto evictStreams = [&](bool read) {
            while (!enough()) {
                Extract* oldest = nullptr;

                for (auto& e : extracts)
                    if (e.state == Extract::State::Ready && (read || !e.used) && (!oldest || e.used < oldest->used))
                        oldest = &e;

                if (!oldest)
                    return;

                free += oldest->file.bytes;
                streamBytes += oldest->file.bytes;
                ++streamFiles;
                Evict(*oldest);
            }
        };

        evictStreams(false);

        if (packs && !enough())
            for (const auto& group : StalePacks()) {
                for (const auto& path : group) {
                    std::error_code error;
                    const uint64_t size = std::filesystem::file_size(path, error);

                    if (!error && std::filesystem::remove(path, error)) {
                        free += size;
                        packBytes += size;
                        ++packFiles;
                    }
                }

                if (enough())
                    break;
            }

        evictStreams(true);
#ifdef MH3_DEVELOPMENT
        if (streamFiles || packFiles || !enough())
            std::fprintf(stderr, "MH3 asset cache: %s room for a %s write (%s free, %s in flight): deleted %u stream copies (%s) and %u pack files (%s)\n", enough() ? "made" : "found no", MiB(bytes).c_str(),
                         MiB(before).c_str(), MiB(writing).c_str(), streamFiles, MiB(streamBytes).c_str(), packFiles, MiB(packBytes).c_str());
#endif
        if (!enough())
            return false;

        writing += bytes;
        return true;
    }

    // Unlocked: Room for a pack, or the error that stops the game.
    Status PackRoom(const Source& s, const char* what, uint64_t bytes) {
        std::lock_guard guard(lock);

        if (Room(bytes, true))
            return {};

        const uint64_t free = FreeStorage(cache);
        return Status::Error("Not enough storage space for the converted " + std::string(what) + " of " + s.file.path + ": " + MiB(bytes) + " needed beside a " + MiB(StorageReserve) +
                             " reserve, " + (free == UINT64_MAX ? std::string("unknown") : MiB(free)) + " free. Free up space on " + cache.string());
    }

    // Unlocked: the end of a write that PackRoom granted.
    void Wrote(uint64_t bytes) {
        std::lock_guard guard(lock);
        writing -= bytes;
    }

    Source* Find(uint64_t at) {
        auto it = std::upper_bound(sources.begin(), sources.end(), at, [](uint64_t value, const Source& s) { return value < s.file.offset; });

        if (it == sources.begin())
            return nullptr;

        --it;
        return at - it->file.offset < it->file.bytes ? &*it : nullptr;
    }

    // index.log: one record per indexed source file. A torn final record is dropped.
    void LoadIndex() {
        const auto log = Load(cache / "index.log");
        size_t at = 0;

        while (log.size() - at >= 40) {
            const uint8_t* r = log.data() + at;
            const uint32_t bytes = Le32(r + 4), count = Le32(r + 20);

            if (Le32(r) != RecordMagic || count > TextureInventory::MaxAssets || bytes != 40 + uint64_t(count) * 8 || bytes > log.size() - at || Fnv(r, bytes - 4) != Le32(r + bytes - 4))
                break;

            const uint64_t offset = Le64(r + 8);
            const uint32_t fileBytes = Le32(r + 16), firstTexture = Le32(r + 24), firstModel = Le32(r + 28), models = Le32(r + 32);
            nextTexture = std::max(nextTexture, firstTexture + count);
            nextModel = std::max(nextModel, firstModel + models);
            auto* s = Find(offset);

            if (s && s->file.offset == offset && s->file.bytes == fileBytes && !s->indexed) {
                s->indexed = true;
                s->firstTexture = firstTexture;
                s->textureCount = count;
                s->firstModel = firstModel;
                s->modelCount = models;

                for (uint32_t i = 0; i < count; ++i)
                    s->patches.push_back(Le64(r + 36 + size_t(i) * 8));

                if (count)
                    byTexture[firstTexture] = s;
            }

            at += bytes;
        }

        if (at != log.size()) {
            std::fprintf(stderr, "MH3 asset cache: dropping %lu torn index bytes\n", static_cast<unsigned long>(log.size() - at));
            CheckStatus(Publish(cache / "index.log", {log.data(), at}));
        }
    }

    Status Append(const Source& s) {
        std::vector<uint8_t> record;
        Put32(record, RecordMagic);
        Put32(record, 40 + s.textureCount * 8);
        Put64(record, s.file.offset);
        Put32(record, s.file.bytes);
        Put32(record, s.textureCount);
        Put32(record, s.firstTexture);
        Put32(record, s.firstModel);
        Put32(record, s.modelCount);

        for (auto patch : s.patches)
            Put64(record, patch);

        Put32(record, Fnv(record.data(), record.size()));

        // Appended again once when a suspend cut the write (Publish).
        const auto append = [&] {
            std::ofstream out(cache / "index.log", std::ios::binary | std::ios::app);
            out.write(reinterpret_cast<const char*>(record.data()), std::streamsize(record.size()));
            out.close();
            return bool(out);
        };

        if (!append() && !append())
            return Status::Error("Cannot append the asset cache index (not enough space?)");

        return {};
    }

    Status ReadFile(const Source& s, std::shared_ptr<const std::vector<uint8_t>>& file) {
        auto data = std::make_shared<std::vector<uint8_t>>(s.file.bytes);
        MH3_RETURN_IF_ERROR(read(s.file.offset, data->data(), data->size()));
        file = std::move(data);
        return {};
    }

    void Queue(Source& s, Kind kind) {
        if (!s.data)
            MH3_THROW(std::logic_error("Asset conversion without source data"));

        LoadProgress::ConversionQueued(s.data->size());
        jobs.push_back({&s, kind});
        changed.notify_all();
    }

    // Lock held. Whether the source's converted packs are all published.
    bool Converted(Source& s) {
        s.texturesDone = !s.textureCount || Exists(TexturePath(s));
        s.modelsUsable = s.modelCount && Exists(ModelPath(s));
        s.modelsDone = !s.modelCount || s.modelsUsable || Exists(SkipPath(s));
        return s.texturesDone && s.modelsDone;
    }

    // Lock held. Hands a source's written texture and model packs to
    // packReady and modelReady, each once.
    void Announce(Source& s) {
        if (!s.failure.empty())
            return;

        if (!s.announced && s.texturesDone && s.textureCount && packReady) {
            s.announced = true;
            packReady(TexturePath(s).string());
        }

        if (!s.modelsAnnounced && s.modelsDone && s.modelsUsable && modelReady) {
            s.modelsAnnounced = true;
            modelReady(uint32_t(&s - sources.data()), ModelPath(s).string());
        }
    }

    // Lock held. Makes sure the source is indexed and its conversion either
    // published or queued. Released while reading and parsing the file.
    Status Ensure(Source& s, std::unique_lock<std::mutex>& guard) {
        Wait(guard, [&] { return !s.indexing; });

        if (s.checked)
            return {};

        if (s.indexed && Converted(s)) {
            s.checked = true;
            Announce(s);
            return {};
        }

        s.indexing = true;
        guard.unlock();
        std::shared_ptr<const std::vector<uint8_t>> data;
        std::vector<TextureInventory::Texture> textures;
        std::vector<ModelSource> models;
        Status indexed = ReadFile(s, data);

        if (indexed.ok() && !s.indexed) {
            indexed = TextureInventory::Parse(*data, s.file.offset, textures);

            if (indexed.ok())
                indexed = ListModels(*data, models);
        }

        guard.lock();
        s.indexing = false;
        changed.notify_all();
        MH3_RETURN_IF_ERROR(indexed);

        if (!s.indexed) {
            if (nextTexture + textures.size() > TextureInventory::MaxAssets + 1 || nextModel + models.size() > 65536)
                return Status::Error("Asset cache ID space exhausted; delete " + cache.string());

            s.firstTexture = nextTexture;
            s.textureCount = uint32_t(textures.size());
            s.firstModel = nextModel;
            s.modelCount = uint32_t(models.size());

            for (const auto& t : textures)
                s.patches.push_back(t.patch);

            MH3_RETURN_IF_ERROR(Append(s));
            nextTexture += s.textureCount;
            nextModel += s.modelCount;
            s.indexed = true;

            if (s.textureCount)
                byTexture[s.firstTexture] = &s;
        }

        s.checked = true;

        if (Converted(s)) {
            Announce(s);
            return {};
        }

        s.data = std::move(data);
        pendingBytes += s.data->size();

        if (!s.texturesDone)
            Queue(s, Kind::Textures);

        if (!s.modelsDone)
            Queue(s, Kind::Models);
#ifdef MH3_DEVELOPMENT
        if (pendingBytes > MaxPendingBytes) {
            const double since = Seconds();
            gameSource = &s;
            gameStep.Set("waiting for pending conversion", s.file.path.c_str());
            Wait(guard, [&] { return pendingBytes <= MaxPendingBytes || stopping; });
            gameStep.Set(nullptr, nullptr);

            if (Seconds() - since > 0.5)
                std::fprintf(stderr, "MH3 asset cache: game waited %.2f s for %.1f MiB of pending conversion\n", Seconds() - since, pendingBytes / 1048576.0);
        }
#endif
        Wait(guard, [&] { return pendingBytes <= MaxPendingBytes || stopping; });
        return {};
    }

    void Work() {
        aurora::pin_current_thread(aurora::ThreadRole::Any);
#ifdef MH3_DEVELOPMENT
        Step& step = workerSteps[nextWorker++ % Workers];
        step.Set("idle", nullptr);
#endif
        std::unique_lock guard(lock);

        for (;;) {
            auto next = jobs.end();
            Wait(guard, [&] {
                next = std::find_if(jobs.begin(), jobs.end(), [&](const Job& j) { return j.kind != Kind::Models || modelJobs < ModelWorkers; });
                return stopping || next != jobs.end();
            });

            if (stopping)
                return;

            const Job job = *next;
            jobs.erase(next);

            if (job.kind == Kind::Extract) {
#ifdef MH3_DEVELOPMENT
                step.Set("extracting", job.extract->file.path.c_str());
#endif
                RunExtract(*job.extract, guard);
#ifdef MH3_DEVELOPMENT
                step.Set("idle", nullptr);
#endif
                changed.notify_all();
                continue;
            }

            if (job.kind == Kind::TextureHelp) {
                guard.unlock();
#ifdef MH3_DEVELOPMENT
                step.Set("helping to convert textures", job.textures->source->file.path.c_str());
#endif
                job.textures->Run(true);
#ifdef MH3_DEVELOPMENT
                step.Set("relocking after helping", job.textures->source->file.path.c_str());
#endif
                guard.lock();
#ifdef MH3_DEVELOPMENT
                step.Set("idle", nullptr);
#endif
                continue;
            }

            std::unique_ptr<ModelConverter> converter;

            if (job.kind == Kind::Models) {
                ++modelJobs;

                if (!spareConverters.empty()) {
                    converter = std::move(spareConverters.back());
                    spareConverters.pop_back();
                }
            }

            auto& s = *job.source;
            const auto data = s.data;
            guard.unlock();
            std::string failure;
            bool usable = false;
#ifdef MH3_DEVELOPMENT
            const double since = Seconds();
            const char* kind = job.kind == Kind::Textures ? "textures" : "models";
            step.Set(job.kind == Kind::Textures ? "converting textures" : "converting models", s.file.path.c_str());
#endif
            const auto convert = [&] {
                if (job.kind == Kind::Textures)
                    return ConvertTextures(s, data);

                if (!converter)
                    converter = std::make_unique<ModelConverter>();

                return ConvertModels(s, *data, *converter, usable);
            };

            // A failed conversion runs again once: a suspend that cut the
            // writes of its pack fails them (Publish), and the pack is
            // written again whole.
            Status converted = convert();

            if (!converted.ok())
                converted = convert();

            if (!converted.ok())
                failure = s.file.path + ": " + converted.message();

            LoadProgress::ConversionDone(data->size());
#ifdef MH3_DEVELOPMENT
            if (Seconds() - since > 0.5 || !failure.empty())
                std::fprintf(stderr, "MH3 asset cache: converted %s of %s (%lu bytes) in %.2f s%s%s\n", kind, s.file.path.c_str(), static_cast<unsigned long>(data ? data->size() : 0), Seconds() - since,
                             failure.empty() ? "" : ", failed: ", failure.c_str());
            step.Set("relocking after converting", s.file.path.c_str());
#endif
            guard.lock();
#ifdef MH3_DEVELOPMENT
            step.Set("idle", nullptr);
#endif
            if (!failure.empty() && s.failure.empty())
                s.failure = failure;

            if (job.kind == Kind::Textures) {
                s.texturesDone = true;
                Announce(s);
            } else {
                s.modelsDone = true;
                s.modelsUsable = usable;
                --modelJobs;

                if (converter)
                    spareConverters.push_back(std::move(converter));

                Announce(s);
            }

            if (s.texturesDone && s.modelsDone && s.data) {
                pendingBytes -= s.data->size();
                s.data.reset();
            }

            changed.notify_all();
        }
    }

    // Unlocked: converts the file's textures with whichever workers are idle
    // (TextureWork, TextureHelp jobs at the front of the queue).
    Status ConvertTextures(const Source& s, const std::shared_ptr<const std::vector<uint8_t>>& data) {
        auto work = std::make_shared<TextureWork>();
        work->source = &s;
        work->data = data;

        // IDs follow the parse order recorded in the index.
        MH3_RETURN_IF_ERROR(TextureInventory::Parse(*data, s.file.offset, work->textures, s.firstTexture));

        if (work->textures.size() != s.textureCount)
            return Status::Error("texture inventory differs from the cache index");

        std::vector<uint64_t> bytes(work->textures.size());

        for (size_t i = 0; i < bytes.size(); ++i)
            MH3_RETURN_IF_ERROR(TexturePayloadBytes(work->textures[i], *data, s.file.offset, bytes[i]));

        // The payloads, their 64-byte alignment and the records.
        uint64_t packBytes = 4096 + uint64_t(bytes.size()) * 128;

        for (const uint64_t b : bytes)
            packBytes += b;

        MH3_RETURN_IF_ERROR(PackRoom(s, "textures", packBytes));
        const struct Written {
            Impl& owner;
            uint64_t bytes;
            ~Written() { owner.Wrote(bytes); }
        } written{*this, packBytes};
        const auto path = TexturePath(s), temporary = std::filesystem::path(path.string() + ".tmp");
        MH3_RETURN_IF_ERROR(work->writer.Open(temporary, s.textureCount, s.firstTexture));
        work->writer.Plan(bytes);

        if (work->textures.size() > 1) {
            std::lock_guard guard(lock);

            for (size_t i = 1; i < std::min<size_t>(Workers, work->textures.size()); ++i)
                jobs.push_front({nullptr, Kind::TextureHelp, nullptr, work});

            changed.notify_all();
        }

        work->Run(false);
        {
            // Timed: VitaSDK's condition variable can lose a notification.
            std::unique_lock guard(work->lock);

            while (work->active)
                work->idle.wait_for(guard, std::chrono::milliseconds(20));

            MH3_RETURN_IF_ERROR(work->failure);
        }

        MH3_RETURN_IF_ERROR(work->writer.Finish());
#ifdef MH3_DEVELOPMENT
        if (work->helped)
            std::fprintf(stderr, "MH3 asset cache: helping workers converted %u of the %lu textures of %s\n", work->helped.load(), static_cast<unsigned long>(work->textures.size()), s.file.path.c_str());
#endif
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        return FsStatus::Rename(temporary, path);
    }

    // `usable`: the source has a model pack (left false when it has none).
    Status ConvertModels(const Source& s, const std::vector<uint8_t>& data, ModelConverter& converter, bool& usable) {
        std::vector<ModelSource> models;
        MH3_RETURN_IF_ERROR(ListModels(data, models));

        if (models.size() != s.modelCount)
            return Status::Error("model inventory differs from the cache index");

        for (uint32_t i = 0; i < models.size(); ++i)
            models[i].id = s.firstModel + i;

        std::vector<TextureRow> rows;
        std::vector<TextureInventory::Texture> textures;
        MH3_RETURN_IF_ERROR(TextureInventory::Parse(data, s.file.offset, textures, s.firstTexture ? s.firstTexture : 1));

        for (const auto& t : textures)
            rows.emplace_back(s.file.offset, t);

        ModelConversion result;

        if (!converter.Convert(s.file.path, s.file.offset, data, models, std::move(rows), result))
            return Publish(SkipPath(s), {});

        auto identity = ModelPath(s);
        identity.replace_extension(".identity.bin");

        // The pack is published last: its presence marks a complete pair.
        const uint64_t bytes = result.identity.size() + result.pack.size();
        MH3_RETURN_IF_ERROR(PackRoom(s, "models", bytes));
        Status published = Publish(identity, result.identity);

        if (published.ok())
            published = Publish(ModelPath(s), result.pack);

        Wrote(bytes);
        MH3_RETURN_IF_ERROR(published);
        usable = true;
        return {};
    }

    Status Failure(const Source& s) const {
        if (!s.failure.empty())
            return Status::Error("Asset conversion failed: " + s.failure);

        return {};
    }
};

#ifdef MH3_DEVELOPMENT
// Development watchdog: while the game has waited on the cache for over 5 s,
// every 5 s it reports what the game waits for, each worker's step and
// whether the cache lock can be taken, to the TTY and to cache/watchdog.log
// (the data check prints that file, in case the TTY itself is stuck).
void AssetCache::Impl::Watch() {
    for (;;) {
        for (int i = 0; i < 50; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            if (lock.try_lock()) {
                const bool stop = stopping;
                lock.unlock();

                if (stop)
                    return;
            }
        }

        const bool reader = !gameStep.what && readerStep.what;
        const Step& step = reader ? readerStep : gameStep;
        const char* what = step.what;
        const double waited = Seconds() - step.since;

        if (!what || waited < 5)
            continue;

        char text[1024];
        int n = std::snprintf(text, sizeof text, "MH3 asset cache watchdog: the %s has been %s %s for %.0f s;", reader ? "disc reader" : "game", what, step.file.load() ? step.file.load() : "", waited);
        bool locked = false;

        for (int i = 0; i < 20 && !locked; ++i) {
            locked = lock.try_lock();

            if (!locked)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        if (locked) {
            n += std::snprintf(text + n, sizeof text - n, " lock free, %.1f MiB pending, %lu jobs queued, %u model jobs;", pendingBytes / 1048576.0, static_cast<unsigned long>(jobs.size()), modelJobs);

            if (const Source* waited = reader ? readerSource.load() : gameSource.load())
                n += std::snprintf(text + n, sizeof text - n, " source indexed %d checked %d indexing %d textures %u done %d, models %u done %d usable %d, data %d, failure '%s';", waited->indexed, waited->checked,
                                   waited->indexing, waited->textureCount, waited->texturesDone, waited->modelCount, waited->modelsDone, waited->modelsUsable, waited->data != nullptr, waited->failure.c_str());
            lock.unlock();
        } else
            n += std::snprintf(text + n, sizeof text - n, " lock held for 200 ms;");

        for (unsigned w = 0; w < Workers && n < int(sizeof text) - 64; ++w) {
            const char* step = workerSteps[w].what;
            const char* file = workerSteps[w].file;
            n += std::snprintf(text + n, sizeof text - n, " worker %u %s %s (%.0f s)", w, step ? step : "not started", file ? file : "", Seconds() - workerSteps[w].since);
        }

        std::fprintf(stderr, "%s\n", text);

        if (std::FILE* out = std::fopen((cache / "watchdog.log").string().c_str(), "a")) {
            std::fprintf(out, "%s\n", text);
            std::fclose(out);
        }
    }
}
#endif

std::string AssetCache::StorageWarning(const std::filesystem::path& root) {
    const uint64_t free = FreeStorage(root);

    if (free == UINT64_MAX)
        return {};

    const uint64_t used = CacheUse(root / "cache");

    if (free + used >= StorageWanted)
        return {};

    const auto text = root.string();
    const auto colon = text.find(':');
    const std::string storage = colon != std::string::npos ? text.substr(0, colon + 1) : text;
    return storage + " has " + MiB(free) + " free and Monster Hunter Tri's asset cache holds " + MiB(used) + "; the cache wants " + MiB(StorageWanted) +
           " in all. Free up space: until then maps convert again at more of their loads and fewer sound streams are cached, and a map whose converted assets do not fit stops the game "
           "with an error.";
}

AssetCache::AssetCache(const std::filesystem::path& root, std::vector<SourceFile> sources, std::vector<SourceFile> streams, RawRead read)
    : impl(std::make_unique<Impl>(root, std::move(sources), std::move(streams), std::move(read))) {}
AssetCache::~AssetCache() = default;

Status AssetCache::Read(uint64_t offset, uint8_t* out, size_t bytes) {
    auto& m = *impl;
    const uint64_t end = offset + bytes;
    std::unique_lock guard(m.lock);

    for (uint64_t at = offset; at < end;) {
        auto* s = m.Find(at);

        if (!s) {
            if (auto* e = m.FindExtract(at)) {
                const uint64_t stop = std::min(end, e->file.offset + e->file.bytes);

                bool served = false;
                MH3_RETURN_IF_ERROR(m.ReadExtracted(*e, at, stop, out + (at - offset), served));

                if (!served) {
                    guard.unlock();
                    MH3_RETURN_IF_ERROR(m.read(at, out + (at - offset), stop - at));
                    guard.lock();
                }

                at = stop;
                continue;
            }

            // Not a source file: read up to the next one.
            auto next = std::upper_bound(m.sources.begin(), m.sources.end(), at, [](uint64_t value, const Impl::Source& source) { return value < source.file.offset; });
            uint64_t stop = next == m.sources.end() ? end : std::min(end, next->file.offset);
            auto stream = std::upper_bound(m.extracts.begin(), m.extracts.end(), at, [](uint64_t value, const Impl::Extract& e) { return value < e.file.offset; });

            if (stream != m.extracts.end())
                stop = std::min(stop, stream->file.offset);

            guard.unlock();
            MH3_RETURN_IF_ERROR(m.read(at, out + (at - offset), stop - at));
            guard.lock();
            at = stop;
            continue;
        }

        MH3_RETURN_IF_ERROR(m.Ensure(*s, guard));
        const uint64_t stop = std::min(end, s->file.offset + s->file.bytes);

        if (auto data = s->data) {
            std::memcpy(out + (at - offset), data->data() + (at - s->file.offset), stop - at);
        } else {
            guard.unlock();
            MH3_RETURN_IF_ERROR(m.read(at, out + (at - offset), stop - at));
            guard.lock();
        }

        // Native texture tags: the converted asset ID replaces TEX0's last word.
        for (size_t i = 0; i < s->patches.size(); ++i) {
            const uint64_t patch = s->patches[i];

            if (patch + 4 <= at || patch >= stop)
                continue;

            const uint32_t tag = 0xa0000000u | (s->firstTexture + uint32_t(i));

            for (unsigned b = 0; b < 4; ++b)
                if (patch + b >= at && patch + b < stop)
                    out[patch + b - offset] = uint8_t(tag >> (24 - 8 * b));
        }

        at = stop;
    }

    return {};
}

bool AssetCache::Converted(uint64_t offset, size_t bytes, std::chrono::milliseconds limit) {
    auto& m = *impl;
    const auto deadline = std::chrono::steady_clock::now() + limit;
    const uint64_t end = offset + bytes;
    std::unique_lock guard(m.lock);

    for (uint64_t at = offset; at < end;) {
        auto next = std::upper_bound(m.sources.begin(), m.sources.end(), at, [](uint64_t value, const Impl::Source& source) { return value < source.file.offset; });

        if (next == m.sources.begin() || at - std::prev(next)->file.offset >= std::prev(next)->file.bytes) {
            at = next == m.sources.end() ? end : next->file.offset;
            continue;
        }

        auto& s = *std::prev(next);

        // Timed: a lost wakeup (Impl::Wait) only delays the next check.
        while (s.checked && !(s.texturesDone && s.modelsDone)) {
#ifdef MH3_DEVELOPMENT
            // The reader retries every limit: keep the first wait's start.
            if (m.readerSource != &s) {
                m.readerSource = &s;
                m.readerStep.Set("reading a file that is converting", s.file.path.c_str());
            }
#endif
            if (m.changed.wait_until(guard, deadline) == std::cv_status::timeout && !(s.texturesDone && s.modelsDone))
                return false;
        }

        at = s.file.offset + s.file.bytes;
    }
#ifdef MH3_DEVELOPMENT
    if (m.readerSource) {
        m.readerSource = nullptr;
        m.readerStep.Set(nullptr, nullptr);
    }
#endif
    return true;
}

void AssetCache::PrefetchStreams(const std::string& map) {
    auto& m = *impl;
    std::lock_guard guard(m.lock);
    m.streamContext = map;
    const auto known = m.streamsByMap.find(map);

    if (known == m.streamsByMap.end())
        return;

    for (const uint64_t offset : known->second)
        if (auto* e = m.FindExtract(offset); e && e->file.offset == offset) {
            e->used = ++m.reads; // keep this map's copies over older ones
            m.QueueExtract(*e);
        }
}

void AssetCache::SetTexturePackReady(PackReady ready) {
    std::lock_guard guard(impl->lock);
    impl->packReady = std::move(ready);
}

void AssetCache::SetModelPackReady(ModelPackReady ready) {
    std::lock_guard guard(impl->lock);
    impl->modelReady = std::move(ready);
}

Status AssetCache::TexturePack(uint32_t id, std::string& pack) {
    auto& m = *impl;
    std::unique_lock guard(m.lock);
    auto it = m.byTexture.upper_bound(id);

    if (it == m.byTexture.begin() || id - std::prev(it)->first >= std::prev(it)->second->textureCount)
        return Status::Error("Native texture asset " + std::to_string(id) + " belongs to no indexed source");

    auto& s = *std::prev(it)->second;
    MH3_RETURN_IF_ERROR(m.Ensure(s, guard));
#ifdef MH3_DEVELOPMENT
    if (!s.texturesDone) {
        const double since = Seconds();
        m.gameSource = &s;
        m.gameStep.Set("waiting for textures", s.file.path.c_str());
        m.Wait(guard, [&] { return s.texturesDone; });
        m.gameStep.Set(nullptr, nullptr);

        if (Seconds() - since > 0.5)
            std::fprintf(stderr, "MH3 asset cache: game waited %.2f s for the textures of %s\n", Seconds() - since, s.file.path.c_str());
    }
#endif
    m.Wait(guard, [&] { return s.texturesDone; });
    MH3_RETURN_IF_ERROR(m.Failure(s));
    pack = m.TexturePath(s).string();
    return {};
}

Status AssetCache::ModelPack(uint64_t at, uint32_t& key, std::filesystem::path& pack) {
    auto& m = *impl;
    std::unique_lock guard(m.lock);
    auto* s = m.Find(at);
    pack.clear();

    if (!s)
        return {};

    key = uint32_t(s - m.sources.data());
    MH3_RETURN_IF_ERROR(m.Ensure(*s, guard));
#ifdef MH3_DEVELOPMENT
    if (!s->modelsDone) {
        const double since = Seconds();
        m.gameSource = s;
        m.gameStep.Set("waiting for models", s->file.path.c_str());
        m.Wait(guard, [&] { return s->modelsDone; });
        m.gameStep.Set(nullptr, nullptr);

        if (Seconds() - since > 0.5)
            std::fprintf(stderr, "MH3 asset cache: game waited %.2f s for the models of %s\n", Seconds() - since, s->file.path.c_str());
    }
#endif
    m.Wait(guard, [&] { return s->modelsDone; });
    MH3_RETURN_IF_ERROR(m.Failure(*s));

    if (s->modelsUsable)
        pack = m.ModelPath(*s);

    return {};
}

bool AssetCache::ConvertedModelPack(uint64_t at, uint32_t& key, std::filesystem::path& pack) {
    auto& m = *impl;
    std::lock_guard guard(m.lock);
    const auto* s = m.Find(at);
    pack.clear();

    // A source not checked yet has nothing queued to wait for.
    if (!s || !s->checked)
        return true;

    if (!s->modelsDone)
        return false;

    key = uint32_t(s - m.sources.data());

    if (s->failure.empty() && s->modelsUsable)
        pack = m.ModelPath(*s);

    return true;
}

bool AssetCache::ModelFile(uint64_t at, uint64_t& offset, uint32_t& bytes) {
    auto& m = *impl;
    std::lock_guard guard(m.lock);
    const auto* s = m.Find(at);

    if (!s || !s->indexed || !s->modelCount || (s->modelsDone && !s->modelsUsable))
        return false;

    offset = s->file.offset;
    bytes = s->file.bytes;
    return true;
}

namespace {
struct Game {
    DiscImage image;
    std::unique_ptr<AssetCache> cache;
    explicit Game(const char* root) : image(root) {
        if (!image.Packed() || image.HasNativeTextureMap() || std::getenv("MH3_NATIVE_ASSET_DIR"))
            return;

        std::vector<AssetCache::SourceFile> sources;

        for (const auto& name : TextureInventory::SourceNames(image.FileNames())) {
            const uint32_t bytes = image.FileSize(name);

            if (bytes > TextureInventory::MaxSourceBytes)
                MH3_THROW(std::runtime_error("Oversized texture source: " + name));

            sources.push_back({name, image.FileOffset(name), bytes});
        }

        // The audio streams (16/srt): read in small pieces over minutes of play.
        std::vector<AssetCache::SourceFile> streams;

        for (const auto& name : image.FileNames())
            if (name.rfind("16/srt/", 0) == 0)
                streams.push_back({name, image.FileOffset(name), image.FileSize(name)});
        cache = std::make_unique<AssetCache>(root, std::move(sources), std::move(streams), [this](uint64_t offset, uint8_t* output, size_t length) { return image.ReadRaw(offset, output, length); });
        image.SetReader([this](uint64_t offset, uint8_t* output, size_t length) { return cache->Read(offset, output, length); });
    }

    ~Game() { image.SetReader({}); }
};

Game& GameData() {
    // The launcher's disc preparation completes before this lazy instance.
    const char* path = std::getenv("MH3_DISC_DIR");
    static Game game(path ? path : "Assets/DATA");
    return game;
}
} // namespace

DiscImage& GameDiscImage() {
    return GameData().image;
}

AssetCache* GameAssetCache() {
    return GameData().cache.get();
}
