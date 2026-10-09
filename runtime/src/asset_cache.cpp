#include "asset_cache.h"
#include "disc_image.h"
#include "launcher_convert.h"
#include <aurora/thread_role.hpp>
#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <pthread.h>
#include <stdexcept>
#ifdef __vita__
#include <psp2/io/fcntl.h>
#endif

namespace {
using namespace VitaLauncher;
// Bump when converted pack contents change: the whole cache is rebuilt.
constexpr const char* Version = "mh3-asset-cache 1\n";
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
// decompression and decryption. Least recently used files go beyond this.
#ifdef __vita__
constexpr uint64_t MaxExtractedBytes = 64ull << 20;
#else
constexpr uint64_t MaxExtractedBytes = 512ull << 20;
#endif
// Small enough that game reads waiting on the image's lock are not delayed long.
constexpr size_t ExtractChunk = 256u << 10;

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
// Write to a temporary name, then publish: a file under its final name is complete.
void Publish(const std::filesystem::path& path, std::span<const uint8_t> bytes) {
    const auto temporary = std::filesystem::path(path.string() + ".tmp");
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        out.close();
        if (!out)
            throw std::runtime_error("Cannot write " + path.string() + " (not enough space?)");
    }
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    std::filesystem::rename(temporary, path);
}
bool Exists(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::exists(path, error);
}
} // namespace

struct AssetCache::Impl {
    struct Source {
        SourceFile file;
        bool indexed = false, checked = false, indexing = false;
        uint32_t firstTexture = 0, textureCount = 0, firstModel = 0, modelCount = 0;
        std::vector<uint64_t> patches; // texture i's tag offset; its ID is firstTexture + i
        bool texturesDone = false, modelsDone = false, modelsUsable = false;
        std::string failure;
        // The whole file while its conversion is pending; game reads are served from it.
        std::shared_ptr<const std::vector<uint8_t>> data;
    };
    enum class Kind { Textures, Models, Extract };
    // A disc file served from a plain copy once extracted.
    struct Extract {
        SourceFile file;
        enum class State { None, Queued, Ready } state = State::None;
        std::FILE* copy = nullptr; // open while Ready
        uint64_t used = 0;         // last read, for eviction
        bool failed = false;       // no retry this launch
    };
    struct Job {
        Source* source;
        Kind kind;
        Extract* extract = nullptr;
    };

    std::filesystem::path root, cache;
    RawRead read;
    std::vector<Source> sources;        // by partition offset
    std::vector<Extract> extracts;      // by partition offset
    uint64_t extractedBytes = 0, reads = 0;
    std::map<uint32_t, Source*> byTexture; // first texture ID -> source
    uint32_t nextTexture = 1, nextModel = 1;
    std::mutex lock;
    std::condition_variable changed;
    std::deque<Job> jobs;
    size_t pendingBytes = 0;
    unsigned modelJobs = 0;
    bool stopping = false;
    std::vector<pthread_t> threads;

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
            Publish(cache / "version.txt", {reinterpret_cast<const uint8_t*>(Version), std::strlen(Version)});
        }
        LoadIndex();
        std::sort(streams.begin(), streams.end(), [](const SourceFile& a, const SourceFile& b) { return a.offset < b.offset; });
        std::filesystem::create_directories(cache / "files");
        extracts.resize(streams.size());
        for (size_t i = 0; i < streams.size(); ++i) {
            extracts[i].file = std::move(streams[i]);
            // A file under its final name is complete (Publish order).
            std::error_code error;
            const auto size = std::filesystem::file_size(ExtractPath(extracts[i]), error);
            if (!error && size == extracts[i].file.bytes && (extracts[i].copy = std::fopen(ExtractPath(extracts[i]).string().c_str(), "rb"))) {
                extracts[i].state = Extract::State::Ready;
                extractedBytes += size;
            }
        }
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
                throw std::runtime_error("Cannot start asset conversion workers");
        }
    }
    ~Impl() {
        {
            std::lock_guard guard(lock);
            stopping = true;
        }
        changed.notify_all();
        for (auto thread : threads)
            pthread_join(thread, nullptr);
        for (auto& e : extracts)
            if (e.copy)
                std::fclose(e.copy);
    }
    Extract* FindExtract(uint64_t at) {
        auto it = std::upper_bound(extracts.begin(), extracts.end(), at, [](uint64_t value, const Extract& e) { return value < e.file.offset; });
        if (it == extracts.begin())
            return nullptr;
        --it;
        return at - it->file.offset < it->file.bytes ? &*it : nullptr;
    }
    // Lock held. Serves [at, stop) of an extracted file; false when not extracted
    // (and queues its extraction).
    bool ReadExtracted(Extract& e, uint64_t at, uint64_t stop, uint8_t* out) {
        e.used = ++reads;
        if (e.state == Extract::State::None && !e.failed) {
            e.state = Extract::State::Queued;
            jobs.push_back({nullptr, Kind::Extract, &e});
            changed.notify_all();
        }
        if (e.state != Extract::State::Ready)
            return false;
        const size_t count = size_t(stop - at);
        if (std::fseek(e.copy, long(at - e.file.offset), SEEK_SET) || std::fread(out, 1, count, e.copy) != count)
            throw std::runtime_error("Cannot read the extracted " + e.file.path);
        return true;
    }
    // Worker: copies the file out of the image, making room by evicting the
    // least recently used copies, and publishes it.
    void RunExtract(Extract& e, std::unique_lock<std::mutex>& guard) {
        while (extractedBytes + e.file.bytes > MaxExtractedBytes) {
            Extract* oldest = nullptr;
            for (auto& other : extracts)
                if (other.state == Extract::State::Ready && (!oldest || other.used < oldest->used))
                    oldest = &other;
            if (!oldest)
                break;
            std::fclose(oldest->copy);
            oldest->copy = nullptr;
            oldest->state = Extract::State::None;
            extractedBytes -= oldest->file.bytes;
            std::error_code ignored;
            std::filesystem::remove(ExtractPath(*oldest), ignored);
        }
        if (extractedBytes + e.file.bytes > MaxExtractedBytes) {
            // Larger than the whole budget: keep reading it from the image.
            e.state = Extract::State::None;
            e.failed = true;
            return;
        }
        extractedBytes += e.file.bytes; // reserved while copying
        guard.unlock();
        const auto path = ExtractPath(e), temporary = std::filesystem::path(path.string() + ".tmp");
        std::FILE* copy = nullptr;
        bool written = false;
        try {
            std::vector<uint8_t> chunk(ExtractChunk);
            if (std::FILE* out = std::fopen(temporary.string().c_str(), "wb")) {
                written = true;
                for (uint64_t done = 0; done < e.file.bytes && written;) {
                    const size_t count = size_t(std::min<uint64_t>(ExtractChunk, e.file.bytes - done));
                    read(e.file.offset + done, chunk.data(), count);
                    written = std::fwrite(chunk.data(), 1, count, out) == count;
                    done += count;
                }
                written = std::fclose(out) == 0 && written;
            }
            if (written) {
                std::error_code ignored;
                std::filesystem::remove(path, ignored);
                std::filesystem::rename(temporary, path);
                copy = std::fopen(path.string().c_str(), "rb");
            }
        } catch (const std::exception& error) {
            std::fprintf(stderr, "MH3 asset cache: extracting %s failed: %s\n", e.file.path.c_str(), error.what());
            written = false;
        }
        if (!copy) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
        }
        guard.lock();
        if (copy) {
            e.copy = copy;
            e.state = Extract::State::Ready;
        } else {
            // Not enough space or a read error: keep reading from the image.
            extractedBytes -= e.file.bytes;
            e.state = Extract::State::None;
            e.failed = true;
        }
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
            std::fprintf(stderr, "MH3 asset cache: dropping %zu torn index bytes\n", log.size() - at);
            Publish(cache / "index.log", {log.data(), at});
        }
    }
    void Append(const Source& s) {
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
        std::ofstream out(cache / "index.log", std::ios::binary | std::ios::app);
        out.write(reinterpret_cast<const char*>(record.data()), std::streamsize(record.size()));
        out.close();
        if (!out)
            throw std::runtime_error("Cannot append the asset cache index (not enough space?)");
    }

    std::shared_ptr<const std::vector<uint8_t>> ReadFile(const Source& s) {
        auto data = std::make_shared<std::vector<uint8_t>>(s.file.bytes);
        read(s.file.offset, data->data(), data->size());
        return data;
    }
    void Queue(Source& s, Kind kind) {
        if (!s.data)
            throw std::logic_error("Asset conversion without source data");
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
    // Lock held. Makes sure the source is indexed and its conversion either
    // published or queued. Released while reading and parsing the file.
    void Ensure(Source& s, std::unique_lock<std::mutex>& guard) {
        while (s.indexing)
            changed.wait(guard);
        if (s.checked)
            return;
        if (s.indexed && Converted(s)) {
            s.checked = true;
            return;
        }
        s.indexing = true;
        guard.unlock();
        std::shared_ptr<const std::vector<uint8_t>> data;
        std::vector<TextureInventory::Texture> textures;
        std::vector<ModelSource> models;
        try {
            data = ReadFile(s);
            if (!s.indexed) {
                textures = TextureInventory::Parse(*data, s.file.offset);
                models = ListModels(*data);
            }
        } catch (...) {
            guard.lock();
            s.indexing = false;
            changed.notify_all();
            throw;
        }
        guard.lock();
        s.indexing = false;
        changed.notify_all();
        if (!s.indexed) {
            if (nextTexture + textures.size() > TextureInventory::MaxAssets + 1 || nextModel + models.size() > 65536)
                throw std::runtime_error("Asset cache ID space exhausted; delete " + cache.string());
            s.firstTexture = nextTexture;
            s.textureCount = uint32_t(textures.size());
            s.firstModel = nextModel;
            s.modelCount = uint32_t(models.size());
            for (const auto& t : textures)
                s.patches.push_back(t.patch);
            Append(s);
            nextTexture += s.textureCount;
            nextModel += s.modelCount;
            s.indexed = true;
            if (s.textureCount)
                byTexture[s.firstTexture] = &s;
        }
        s.checked = true;
        if (Converted(s))
            return;
        s.data = std::move(data);
        pendingBytes += s.data->size();
        if (!s.texturesDone)
            Queue(s, Kind::Textures);
        if (!s.modelsDone)
            Queue(s, Kind::Models);
        changed.wait(guard, [&] { return pendingBytes <= MaxPendingBytes || stopping; });
    }

    void Work() {
        aurora::pin_current_thread(aurora::ThreadRole::Any);
        std::unique_ptr<ModelConverter> converter;
        std::unique_lock guard(lock);
        for (;;) {
            if (jobs.empty())
                converter.reset(); // Return the model workspace while idle.
            auto next = jobs.end();
            changed.wait(guard, [&] {
                next = std::find_if(jobs.begin(), jobs.end(), [&](const Job& j) { return j.kind != Kind::Models || modelJobs < ModelWorkers; });
                return stopping || next != jobs.end();
            });
            if (stopping)
                return;
            const Job job = *next;
            jobs.erase(next);
            if (job.kind == Kind::Extract) {
                RunExtract(*job.extract, guard);
                changed.notify_all();
                continue;
            }
            if (job.kind == Kind::Models)
                ++modelJobs;
            auto& s = *job.source;
            const auto data = s.data;
            guard.unlock();
            std::string failure;
            bool usable = false;
            try {
                if (job.kind == Kind::Textures)
                    ConvertTextures(s, *data);
                else {
                    if (!converter)
                        converter = std::make_unique<ModelConverter>();
                    usable = ConvertModels(s, *data, *converter);
                }
            } catch (const std::exception& e) {
                failure = s.file.path + ": " + e.what();
            }
            guard.lock();
            if (!failure.empty() && s.failure.empty())
                s.failure = failure;
            if (job.kind == Kind::Textures)
                s.texturesDone = true;
            else {
                s.modelsDone = true;
                s.modelsUsable = usable;
                --modelJobs;
            }
            if (s.texturesDone && s.modelsDone && s.data) {
                pendingBytes -= s.data->size();
                s.data.reset();
            }
            changed.notify_all();
        }
    }
    void ConvertTextures(const Source& s, const std::vector<uint8_t>& data) {
        // IDs follow the parse order recorded in the index.
        const auto textures = TextureInventory::Parse(data, s.file.offset, s.firstTexture);
        if (textures.size() != s.textureCount)
            throw std::runtime_error("texture inventory differs from the cache index");
        const auto path = TexturePath(s), temporary = std::filesystem::path(path.string() + ".tmp");
        {
            TexturePackWriter writer(temporary, s.textureCount, s.firstTexture);
            for (const auto& t : textures)
                writer.Add(ConvertTexture(t, data, s.file.offset, s.file.path));
            writer.Finish();
        }
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        std::filesystem::rename(temporary, path);
    }
    bool ConvertModels(const Source& s, const std::vector<uint8_t>& data, ModelConverter& converter) {
        auto models = ListModels(data);
        if (models.size() != s.modelCount)
            throw std::runtime_error("model inventory differs from the cache index");
        for (uint32_t i = 0; i < models.size(); ++i)
            models[i].id = s.firstModel + i;
        std::vector<TextureRow> rows;
        for (const auto& t : TextureInventory::Parse(data, s.file.offset, s.firstTexture ? s.firstTexture : 1))
            rows.emplace_back(s.file.offset, t);
        ModelConversion result;
        if (!converter.Convert(s.file.path, s.file.offset, data, models, std::move(rows), result)) {
            Publish(SkipPath(s), {});
            return false;
        }
        auto identity = ModelPath(s);
        identity.replace_extension(".identity.bin");
        // The pack is published last: its presence marks a complete pair.
        Publish(identity, result.identity);
        Publish(ModelPath(s), result.pack);
        return true;
    }
    void Fail(const Source& s) {
        if (!s.failure.empty())
            throw std::runtime_error("Asset conversion failed: " + s.failure);
    }
};

AssetCache::AssetCache(const std::filesystem::path& root, std::vector<SourceFile> sources, std::vector<SourceFile> streams, RawRead read)
    : impl(std::make_unique<Impl>(root, std::move(sources), std::move(streams), std::move(read))) {}
AssetCache::~AssetCache() = default;

void AssetCache::Read(uint64_t offset, uint8_t* out, size_t bytes) {
    auto& m = *impl;
    const uint64_t end = offset + bytes;
    std::unique_lock guard(m.lock);
    for (uint64_t at = offset; at < end;) {
        auto* s = m.Find(at);
        if (!s) {
            if (auto* e = m.FindExtract(at)) {
                const uint64_t stop = std::min(end, e->file.offset + e->file.bytes);
                if (!m.ReadExtracted(*e, at, stop, out + (at - offset))) {
                    guard.unlock();
                    m.read(at, out + (at - offset), stop - at);
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
            m.read(at, out + (at - offset), stop - at);
            guard.lock();
            at = stop;
            continue;
        }
        m.Ensure(*s, guard);
        const uint64_t stop = std::min(end, s->file.offset + s->file.bytes);
        if (auto data = s->data) {
            std::memcpy(out + (at - offset), data->data() + (at - s->file.offset), stop - at);
        } else {
            guard.unlock();
            m.read(at, out + (at - offset), stop - at);
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
}
std::string AssetCache::TexturePack(uint32_t id) {
    auto& m = *impl;
    std::unique_lock guard(m.lock);
    auto it = m.byTexture.upper_bound(id);
    if (it == m.byTexture.begin() || id - std::prev(it)->first >= std::prev(it)->second->textureCount)
        throw std::runtime_error("Native texture asset " + std::to_string(id) + " belongs to no indexed source");
    auto& s = *std::prev(it)->second;
    m.Ensure(s, guard);
    m.changed.wait(guard, [&] { return s.texturesDone; });
    m.Fail(s);
    return m.TexturePath(s).string();
}
std::filesystem::path AssetCache::ModelPack(uint64_t at, uint32_t& key) {
    auto& m = *impl;
    std::unique_lock guard(m.lock);
    auto* s = m.Find(at);
    if (!s)
        return {};
    key = uint32_t(s - m.sources.data());
    m.Ensure(*s, guard);
    m.changed.wait(guard, [&] { return s->modelsDone; });
    m.Fail(*s);
    return s->modelsUsable ? m.ModelPath(*s) : std::filesystem::path{};
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
                throw std::runtime_error("Oversized texture source: " + name);
            sources.push_back({name, image.FileOffset(name), bytes});
        }
        // The audio streams (16/srt): read in small pieces over minutes of play.
        std::vector<AssetCache::SourceFile> streams;
        for (const auto& name : image.FileNames())
            if (name.rfind("16/srt/", 0) == 0)
                streams.push_back({name, image.FileOffset(name), image.FileSize(name)});
        cache = std::make_unique<AssetCache>(root, std::move(sources), std::move(streams), [this](uint64_t offset, uint8_t* output, size_t length) { image.ReadRaw(offset, output, length); });
        image.SetReader([this](uint64_t offset, uint8_t* output, size_t length) { cache->Read(offset, output, length); });
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
