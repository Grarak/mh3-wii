#include "model_assets.h"
#include "fatal.h"
#include "asset_cache.h"
#include "../native/adapters/model_catalog_bridge.h"
#include "../native/adapters/model_gpu_bridge.h"
#include "ios_host.h"
#include "native_frame_packet.h"
#include <aurora/thread_role.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
// Aggregate budgets across every admitted source pack, not per-file quotas.
constexpr size_t MaxIndexBytes = 4 * 1024 * 1024;
constexpr size_t MaxMetadataBytes = 8 * 1024 * 1024;
constexpr size_t MaxIdentityBytes = 4 * 1024 * 1024;
constexpr size_t MaxGPUBytes = 32 * 1024 * 1024;
constexpr uint32_t LegacyPack = UINT32_MAX;
struct Assets {
    std::vector<uint8_t> metadata, identity;
    std::filesystem::path path;
    mh3_mesh_pack pack{};
    mh3_model_catalog catalog{};
    uint64_t generation = 0, lastUse = 0, lastDemandFrame = UINT64_MAX;
    bool gpuRegistered = false, catalogRegistered = false;
    uintptr_t handle() const { return reinterpret_cast<uintptr_t>(this); }
};

// The game thread admits and evicts packs; the render thread makes them GPU
// resident while drawing. The lock covers the maps and use clocks they share.
struct Manager {
    std::mutex lock;
    std::filesystem::path indexPath;
    std::vector<uint8_t> index;
    uint32_t packCount = 0, packOffset = 0, sourceCount = 0, sourceOffset = 0;
    std::map<uint32_t, std::unique_ptr<Assets>> loaded;
    std::map<uintptr_t, Assets*> handles;
    NativeModelAssetStats stats;
    uint64_t clock = 0;
    uint32_t demandFrame = 0;
    bool frameKnown = false;

    // Packs come from the disc image's load-time asset cache, by source file.
    bool converted = false;
};

std::unique_ptr<Manager> manager;
uint64_t generation = 0;
uint32_t Word(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

uint64_t Long(const uint8_t* p) {
    return Word(p) | uint64_t(Word(p + 4)) << 32;
}

Status Failure(const std::filesystem::path& path, const char* reason) {
    return Status::Error("Native model pack " + path.string() + ": " + reason);
}

[[noreturn]] void Error(const std::filesystem::path& path, const char* reason) {
    MH3_THROW(std::runtime_error(Failure(path, reason).message()));
}

Status FileSize(std::ifstream& file, const std::filesystem::path& path, size_t limit, size_t& size) {
    if (!file)
        return Failure(path, "cannot open");

    const auto bytes = file.tellg();

    if (bytes < 0 || uint64_t(bytes) > limit)
        return Failure(path, "file exceeds admission budget");

    file.seekg(0);
    size = size_t(bytes);
    return {};
}

Status Read(std::ifstream& file, const std::filesystem::path& path, void* destination, size_t bytes) {
    if (bytes && !file.read(static_cast<char*>(destination), bytes))
        return Failure(path, "truncated read");

    return {};
}

void RetireCatalog(Assets& loaded) {
    if (loaded.catalogRegistered) {
        UnregisterNativeModelPack(loaded.handle());
        loaded.catalogRegistered = false;
    }
}

void RetireGPU(Assets& loaded) {
    if (loaded.gpuRegistered) {
        UnregisterNativeModelGPU(loaded.handle(), loaded.generation);
        loaded.gpuRegistered = false;
    }
}

void Retire(Assets& loaded) {
    RetireCatalog(loaded);
    RetireGPU(loaded);
}

const uint8_t* PackRecord(const Manager& m, uint32_t index) {
    return m.index.data() + m.packOffset + size_t(index) * 32;
}

const uint8_t* SourceRecord(const Manager& m, uint32_t index) {
    return m.index.data() + m.sourceOffset + size_t(index) * 24;
}

uint32_t SourceIndex(const Manager& m, uint64_t offset, uint32_t bytes) {
    uint32_t low = 0, high = m.sourceCount;

    while (low < high) {
        const uint32_t middle = low + (high - low) / 2;

        if (Long(SourceRecord(m, middle)) <= offset)
            low = middle + 1;
        else
            high = middle;
    }

    if (!low || !bytes)
        return UINT32_MAX;

    const auto* r = SourceRecord(m, low - 1);
    const uint64_t start = Long(r), length = Word(r + 8);

    if (offset - start >= length || bytes > length - (offset - start))
        return UINT32_MAX;

    return low - 1;
}

Status OpenIndex(Manager& m, const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    size_t bytes = 0;
    MH3_RETURN_IF_ERROR(FileSize(file, path, MaxIndexBytes, bytes));
    m.index.resize(bytes);
    MH3_RETURN_IF_ERROR(Read(file, path, m.index.data(), m.index.size()));
    const auto* h = m.index.data();

    if (m.index.size() < 64 || std::memcmp(h, "M3IX", 4) || Word(h + 4) != 1 || Word(h + 8) != m.index.size())
        return Failure(path, "invalid M3IX header");

    m.packCount = Word(h + 12);
    m.packOffset = Word(h + 16);
    m.sourceCount = Word(h + 20);
    m.sourceOffset = Word(h + 24);
    const uint64_t pathOffset = Word(h + 28), pathBytes = Word(h + 32);

    if (!m.packCount || m.packCount > 16384 || !m.sourceCount || m.sourceCount > 65536 || m.packOffset != 64 || m.sourceOffset != 64 + uint64_t(m.packCount) * 32 ||
        pathOffset != m.sourceOffset + uint64_t(m.sourceCount) * 24 || pathOffset + pathBytes != m.index.size())
        return Failure(path, "invalid M3IX table extents");
    for (unsigned at = 36; at < 64; at += 4)
        if (Word(h + at))
            return Failure(path, "M3IX reserved header fields");
    uint64_t cursor = pathOffset;
    std::set<std::string> paths;
    std::vector<uint32_t> references(m.packCount);

    for (uint32_t i = 0; i < m.packCount; ++i) {
        const auto* r = PackRecord(m, i);
        const uint32_t at = Word(r), bytes = Word(r + 4);

        if (at != cursor || !bytes || bytes > 255 || cursor + bytes > m.index.size())
            return Failure(path, "invalid M3IX relative path extent");

        std::string name(reinterpret_cast<const char*>(h + at), bytes);

        if (name.find_first_of("\\:\0", 0, 3) != std::string::npos || name.front() == '/' || name.back() == '/' || std::filesystem::path(name).extension() != ".bin")
            return Failure(path, "unsafe M3IX relative pack path");

        for (const auto& part : std::filesystem::path(name))
            if (part.empty() || part == "." || part == "..")
                return Failure(path, "unsafe M3IX relative pack path");
        if (name.find("//") != std::string::npos || !paths.insert(name).second)
            return Failure(path, "duplicate or unsafe M3IX relative pack path");

        cursor += bytes;
        const uint64_t metadataBytes = Word(r + 8), identityBytes = Word(r + 12), gpuBytes = Word(r + 16), fileBytes = Word(r + 20);

        if (metadataBytes < 128 || metadataBytes > MaxMetadataBytes || identityBytes < 64 || identityBytes > MaxIdentityBytes || !gpuBytes || gpuBytes > MaxGPUBytes ||
            fileBytes < metadataBytes + gpuBytes || fileBytes > MaxMetadataBytes + MaxGPUBytes + 4096 || Word(r + 24) || Word(r + 28))
            return Failure(path, "M3IX pack exceeds admission budget or has invalid fields");
    }

    if (cursor != m.index.size())
        return Failure(path, "unused M3IX path bytes");

    uint64_t end = 0;

    for (uint32_t i = 0; i < m.sourceCount; ++i) {
        const auto* r = SourceRecord(m, i);
        const uint64_t start = Long(r), bytes = Word(r + 8);
        const uint32_t pack = Word(r + 12);

        if (!bytes || start > UINT64_MAX - bytes || (i && start < end) || pack >= m.packCount || Word(r + 16) || Word(r + 20))
            return Failure(path, "overlapping or invalid M3IX source extents");

        end = start + bytes;
        ++references[pack];
    }

    for (uint32_t count : references)
        if (!count)
            return Failure(path, "M3IX pack has no source extent");
    m.indexPath = path;
    m.stats.index_bytes = m.index.size();
    return {};
}

bool EvictOne(Manager& m, uintptr_t keep) {
    std::unique_lock lock(m.lock);
    auto candidate = m.loaded.end();

    for (auto it = m.loaded.begin(); it != m.loaded.end(); ++it) {
        const auto& asset = *it->second;

        if (asset.handle() == keep || NativeModelPackHasLiveSources(asset.handle()))
            continue;

        if (candidate == m.loaded.end() || asset.lastUse < candidate->second->lastUse)
            candidate = it;
    }

    if (candidate == m.loaded.end())
        return false;

    std::shared_ptr<Assets> retired = std::move(candidate->second);
    m.loaded.erase(candidate);
    lock.unlock();
    RetireCatalog(*retired);
    m.stats.metadata_bytes -= retired->metadata.size();
    m.stats.identity_bytes -= retired->identity.size();
    --m.stats.admitted_packs;
    ++m.stats.evictions;

    // Frames already recorded may still draw this pack: the render thread
    // releases its GPU span and memory after them.
    QueueNativeGraphicsTask(NativeTaskOrder::AfterDraws, [&m, retired] {
        std::lock_guard relock(m.lock);
        RetireGPU(*retired);
        m.handles.erase(retired->handle());
    });
    return true;
}

Status ReserveCPU(Manager& m, size_t metadataBytes, size_t identityBytes, const std::filesystem::path& path) {
    while (metadataBytes > MaxMetadataBytes - m.stats.metadata_bytes || identityBytes > MaxIdentityBytes - m.stats.identity_bytes)
        if (!EvictOne(m, 0))
            return Failure(path, "aggregate CPU admission budget occupied by live source owners");
    return {};
}

bool EvictOneGPU(Manager& m, uintptr_t keep) {
    Assets* candidate = nullptr;

    for (auto& item : m.loaded) {
        auto& asset = *item.second;

        if (!asset.gpuRegistered || asset.handle() == keep || (m.frameKnown && asset.lastDemandFrame == m.demandFrame))
            continue;

        if (!candidate || asset.lastUse < candidate->lastUse)
            candidate = &asset;
    }

    if (!candidate)
        return false;
    // Catalog defaults, certified owners and typed CPU mutations keep their
    // stable handle/generation. Prepared CPU meshes retain no GPU span.
    UnregisterNativeModelGPU(candidate->handle(), candidate->generation);
    candidate->gpuRegistered = false;
    ++m.stats.gpu_evictions;
    return true;
}

Assets& Find(Manager& m, uintptr_t handle, uint64_t epoch) {
    const auto found = m.handles.find(handle);

    if (found == m.handles.end() || found->second->generation != epoch)
        MH3_THROW(std::runtime_error("Native model GPU demand refers to a stale resident owner"));

    return *found->second;
}

// Reads a pack and its identities and validates them: file reads and checks of
// host bytes only (no budget, generation or catalog), so the preparation
// workers run it too (PackPreparer). Without an index (`m` null: load-time
// conversion) it reads nothing else.
Status ReadPack(const Manager* m, const std::filesystem::path& path, uint32_t index, std::unique_ptr<Assets>& result) {
    auto next = std::make_unique<Assets>();
    next->path = path;
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    size_t bytes = 0;
    MH3_RETURN_IF_ERROR(FileSize(file, path, MaxMetadataBytes + MaxGPUBytes + 4096, bytes));
    std::array<uint8_t, 128> header{};
    MH3_RETURN_IF_ERROR(Read(file, path, header.data(), header.size()));

    if (std::memcmp(header.data(), "M3MP", 4))
        return Failure(path, "invalid M3MP magic");

    const size_t metadataBytes = Word(header.data() + 8);
    const uint64_t gpuOffset = Word(header.data() + 12), gpuBytes = Word(header.data() + 16);

    if (metadataBytes < header.size() || metadataBytes > MaxMetadataBytes || !gpuBytes || gpuBytes > MaxGPUBytes)
        return Failure(path, "metadata or GPU payload exceeds admission budget");

    if (gpuOffset < metadataBytes || gpuOffset + gpuBytes != bytes)
        return Failure(path, "invalid file extent");

    auto identityPath = path;
    identityPath.replace_extension(".identity.bin");
    std::ifstream identity(identityPath, std::ios::binary | std::ios::ate);
    size_t identityBytes = 0;
    MH3_RETURN_IF_ERROR(FileSize(identity, identityPath, MaxIdentityBytes, identityBytes));

    if (index != LegacyPack && m) {
        const auto* expected = PackRecord(*m, index);

        if (metadataBytes != Word(expected + 8) || identityBytes != Word(expected + 12) || gpuBytes != Word(expected + 16) || bytes != Word(expected + 20))
            return Failure(path, "pack sizes differ from M3IX descriptor");
    }

    next->metadata.resize(metadataBytes);
    next->identity.resize(identityBytes);
    std::memcpy(next->metadata.data(), header.data(), header.size());
    MH3_RETURN_IF_ERROR(Read(file, path, next->metadata.data() + header.size(), metadataBytes - header.size()));
    MH3_RETURN_IF_ERROR(Read(identity, identityPath, next->identity.data(), identityBytes));

    if (mh3_mesh_pack_open_metadata({next->metadata.data(), next->metadata.size()}, &next->pack) != MH3_PACK_OK)
        return Failure(path, "invalid compact metadata");

    if (mh3_model_catalog_open({next->identity.data(), next->identity.size()}, &next->catalog) != MH3_PACK_OK || mh3_model_catalog_validate_pack(&next->catalog, &next->pack) != MH3_PACK_OK)
        return Failure(identityPath, "invalid or mismatched source identities");

    if (index != LegacyPack && m) {
        std::set<uint32_t> seen;

        for (uint32_t i = 0; i < next->catalog.model_count; ++i) {
            mh3_catalog_model model{};
            mh3_model_catalog_model(&next->catalog, i, &model);
            const uint32_t source = SourceIndex(*m, model.partition_offset, model.file_bytes);

            if (source == UINT32_MAX || Long(SourceRecord(*m, source)) != model.partition_offset || Word(SourceRecord(*m, source) + 8) != model.file_bytes ||
                Word(SourceRecord(*m, source) + 12) != index)
                return Failure(identityPath, "catalog source extent differs from M3IX identity");

            seen.insert(source);
        }

        for (uint32_t i = 0; i < m->sourceCount; ++i)
            if (Word(SourceRecord(*m, i) + 12) == index && !seen.contains(i))
                return Failure(identityPath, "indexed source has no catalog model");
    }

    result = std::move(next);
    return {};
}

// A newly admitted pack's GPU data, made resident on the render thread with
// the packet being recorded (during the area's load) instead of at its first
// draw: a mid-fight model's packs, read from the card there, took ~270 ms of
// one render packet. Only while the budget holds it without an eviction; a
// pack retired meanwhile is skipped. The first draw still notes the demand.
void PrefetchGPU(Manager& m, uintptr_t handle, uint64_t generation) {
    std::lock_guard lock(m.lock);
    const auto found = m.handles.find(handle);

    if (found == m.handles.end() || found->second->generation != generation || found->second->gpuRegistered)
        return;

    auto& asset = *found->second;

    if (NativeModelGPUResidentBytes() > MaxGPUBytes - NativeModelGPUAllocationBytes(asset.pack.gpu_bytes))
        return;

    const auto name = asset.path.string();
    RegisterNativeModelGPU(asset.pack, name.c_str(), handle, generation);
    asset.gpuRegistered = true;
}

// Admission's part on the game thread, in the order the packs are admitted:
// the CPU budget (evicting the least recently used packs) and the generation.
Status ClaimPack(Manager& m, Assets& next) {
    MH3_RETURN_IF_ERROR(ReserveCPU(m, next.metadata.size(), next.identity.size(), next.path));

    if (generation == UINT64_MAX)
        return Failure(next.path, "load generation exhausted");

    next.generation = ++generation;
    return {};
}

Status OpenPack(Manager& m, const std::filesystem::path& path, uint32_t index, std::unique_ptr<Assets>& result) {
    MH3_RETURN_IF_ERROR(ReadPack(m.converted ? nullptr : &m, path, index, result));
    return ClaimPack(m, *result);
}

// OpenPack on the admitting thread, its time counted (open_ms).
Status TimedOpenPack(Manager& m, const std::filesystem::path& path, uint32_t index, std::unique_ptr<Assets>& result) {
    const auto start = std::chrono::steady_clock::now();
    const Status opened = OpenPack(m, path, index, result);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    std::lock_guard lock(m.lock);
    m.stats.open_ms += ms;
    return opened;
}

// Converted model packs read ahead (docs/loading-progress.md): the asset cache
// hands on the pack of each source file the game starts to read, and a worker
// reads and validates it (ReadPack: the pack's meshes, typed materials and
// draws, and its source identities) while the file loads; one, beside the
// disc reader it would otherwise compete with for the card. Certification
// on the game thread then claims the read pack (ClaimPack, Admit) instead of
// reading it. The pack bytes and checks are the same on either thread; the
// budget, generation and admission order stay the game thread's.
class PackPreparer {
  public:
    static PackPreparer& Get() {
        static PackPreparer* preparer = new PackPreparer; // lives until exit
        return *preparer;
    }

    // From the asset cache with its lock held: queues only.
    void Queue(uint32_t key, const std::string& path) {
        std::lock_guard guard(lock);

        if (std::any_of(entries.begin(), entries.end(), [&](const Entry& e) { return e.key == key; }))
            return;

        // Packs nobody claimed make room, oldest first.
        while (entries.size() >= MaxEntries) {
            const auto idle = std::find_if(entries.begin(), entries.end(), [](const Entry& e) { return e.state != Entry::Reading; });

            if (idle == entries.end())
                break;

            entries.erase(idle);
        }

        entries.push_back({key, path});

        while (threads < Threads) {
            ++threads;
            std::thread([this] { Run(); }).detach();
        }

        changed.notify_all();
    }

    // Game thread: the pack read ahead for `key` (waiting while a worker
    // reads it), and how its read went; false when none was read or begun,
    // and a queued one is withdrawn: the caller reads it.
    bool Take(uint32_t key, std::unique_ptr<Assets>& pack, Status& status) {
        std::unique_lock guard(lock);

        for (;;) {
            const auto found = std::find_if(entries.begin(), entries.end(), [&](const Entry& e) { return e.key == key; });

            if (found == entries.end())
                return false;

            if (found->state == Entry::Queued) {
                entries.erase(found);
                return false;
            }

            if (found->state == Entry::Ready) {
                pack = std::move(found->pack);
                status = std::move(found->status);
                entries.erase(found);
                return true;
            }

            // Timed: VitaSDK's condition variable can lose a notification.
            changed.wait_for(guard, std::chrono::milliseconds(5));
        }
    }

  private:
    static constexpr size_t MaxEntries = 16;
    static constexpr unsigned Threads = 1, MaxDiscWaitMs = 100;

    struct Entry {
        uint32_t key;
        std::string path;
        enum State { Queued, Reading, Ready } state = Queued;
        std::unique_ptr<Assets> pack;
        Status status;
    };

    void Run() {
        aurora::pin_current_thread(aurora::ThreadRole::Worker);

        for (;;) {
            std::string path;
            uint32_t key;
            {
                std::unique_lock guard(lock);
                auto next = entries.end();

                // Timed: VitaSDK's condition variable can lose a notification.
                while ((next = std::find_if(entries.begin(), entries.end(), [](const Entry& e) { return e.state == Entry::Queued; })) == entries.end())
                    changed.wait_for(guard, std::chrono::milliseconds(100));

                next->state = Entry::Reading;
                key = next->key;
                path = next->path;
            }

            // The game's disc reads first: a pack read beside one slows it
            // down (one card serves both), and a loading screen lasts as long
            // as its reads. Pi 5 with a cold page cache: the loader's reads
            // took 5.7-9.6 ms instead of 3.3-5.1 ms on average.
            for (unsigned waited = 0; waited < MaxDiscWaitMs && DiscReadWaiting(); ++waited)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));

            std::unique_ptr<Assets> pack;
            Status status = ReadPack(nullptr, path, key, pack);
            std::lock_guard guard(lock);

            // Found again: Queue and Take remove other entries meanwhile,
            // never one being read.
            const auto found = std::find_if(entries.begin(), entries.end(), [&](const Entry& e) { return e.key == key && e.state == Entry::Reading; });

            if (found != entries.end()) {
                found->pack = std::move(pack);
                found->status = std::move(status);
                found->state = Entry::Ready;
            }

            changed.notify_all();
        }
    }

    std::mutex lock;
    std::condition_variable changed;
    std::deque<Entry> entries;
    unsigned threads = 0;
};

// The converted pack of `key`: claimed from the read-ahead, or read here.
Status OpenConvertedPack(Manager& m, const std::filesystem::path& path, uint32_t key, std::unique_ptr<Assets>& result) {
    Status read;

    if (!PackPreparer::Get().Take(key, result, read))
        return TimedOpenPack(m, path, key, result);

    MH3_RETURN_IF_ERROR(read);
    return ClaimPack(m, *result);
}

Status Admit(Manager& m, uint32_t index, std::unique_ptr<Assets> next, bool eagerGPU) {
    std::unique_lock lock(m.lock);
    const auto [entry, inserted] = m.loaded.emplace(index, std::move(next));

    if (!inserted)
        return Status::Error("Native model source pack already admitted");

    auto& asset = *entry->second;
    m.handles.emplace(asset.handle(), &asset);
    lock.unlock();

    // A pack that fails admission is withdrawn again.
    const auto withdraw = [&](Status failure) {
        Retire(asset);
        std::lock_guard relock(m.lock);
        m.handles.erase(asset.handle());
        m.loaded.erase(entry);
        return failure;
    };

    if (eagerGPU) {
        const auto cost = NativeModelGPUAllocationBytes(asset.pack.gpu_bytes);

        if (cost > MaxGPUBytes || NativeModelGPUResidentBytes() > MaxGPUBytes - cost)
            return withdraw(Failure(asset.path, "aggregate GPU admission budget occupied"));

        // Only the startup pack registers eagerly: a failure here is fatal.
        const auto name = asset.path.string();
        RegisterNativeModelGPU(asset.pack, name.c_str(), asset.handle(), asset.generation);
        asset.gpuRegistered = true;
    }

    if (Status registered = RegisterNativeModelPack(&asset.pack, &asset.catalog, asset.handle(), asset.generation); !registered.ok())
        return withdraw(std::move(registered));

    asset.catalogRegistered = true;

    std::lock_guard relock(m.lock);
    asset.lastUse = ++m.clock;
    m.stats.metadata_bytes += asset.metadata.size();
    m.stats.identity_bytes += asset.identity.size();
    ++m.stats.admitted_packs;
    ++m.stats.admissions;
    std::fprintf(stderr, "MH3 native model assets: %u models, %u meshes, %llu metadata + %llu identity bytes, %u GPU bytes (%s)\n", asset.catalog.model_count, asset.pack.counts[MH3_PACK_MESHES],
                 static_cast<unsigned long long>(asset.metadata.size()), static_cast<unsigned long long>(asset.identity.size()), asset.pack.gpu_bytes, eagerGPU ? "resident" : "CPU metadata admitted");

    if (!eagerGPU)
        QueueNativeGraphicsTask(NativeTaskOrder::BeforeDraws, [&m, handle = asset.handle(), generation = asset.generation] { PrefetchGPU(m, handle, generation); });

    return {};
}
} // namespace

void LoadNativeModelAssets(const char* discRoot) {
    if (manager)
        MH3_THROW(std::runtime_error("Native model assets already admitted"));

    const char* overridePack = std::getenv("MH3_NATIVE_MODEL_PACK");
    const char* overrideIndex = std::getenv("MH3_NATIVE_MODEL_INDEX");

    if (overridePack && overrideIndex)
        MH3_THROW(std::runtime_error("Select only one native model pack or index override"));

    const auto root = std::filesystem::path(discRoot ? discRoot : "ux0:/data/mh3-wii") / "native-models";
    const auto indexPath = overrideIndex ? std::filesystem::path(overrideIndex) : root / "models.index.bin";
    auto next = std::make_unique<Manager>();

    if (!overridePack && (overrideIndex || std::filesystem::exists(indexPath))) {
        CheckStatus(OpenIndex(*next, indexPath));
        std::fprintf(stderr, "MH3 native model index: %u packs, %u partition extents, %llu CPU bytes; demand admission enabled\n", next->packCount, next->sourceCount,
                     static_cast<unsigned long long>(next->index.size()));
    } else {
        const auto path = overridePack ? std::filesystem::path(overridePack) : root / "models.bin";

        if (!overridePack && !std::filesystem::exists(path)) {
            if (GameAssetCache()) {
                next->converted = true;
                std::fprintf(stderr, "MH3 native models: converted at load time\n");
                manager = std::move(next);
                GameAssetCache()->SetModelPackReady([](uint32_t key, const std::string& pack) { PackPreparer::Get().Queue(key, pack); });
            }

            return;
        }

        std::unique_ptr<Assets> asset;
        CheckStatus(OpenPack(*next, path, LegacyPack, asset));
        CheckStatus(Admit(*next, LegacyPack, std::move(asset), true));
    }

    manager = std::move(next);
}

Status TryEnsureNativeModelAssets(uint64_t offset, uint32_t bytes) {
    if (!manager)
        return {};

    auto& m = *manager;

    if (m.converted) {
        uint32_t key = 0;
        std::filesystem::path path;
        MH3_RETURN_IF_ERROR(GameAssetCache()->ModelPack(offset, key, path));

        if (path.empty()) {
            std::ostringstream reason;
            reason << "No converted native models at partition offset 0x" << std::hex << offset;
            return Status::Error(reason.str());
        }

        {
            std::lock_guard lock(m.lock);
            const auto found = m.loaded.find(key);

            if (found != m.loaded.end()) {
                found->second->lastUse = ++m.clock;
                return {};
            }
        }

        std::unique_ptr<Assets> asset;
        MH3_RETURN_IF_ERROR(OpenConvertedPack(m, path, key, asset));
        return Admit(m, key, std::move(asset), false);
    }

    if (m.index.empty())
        return {};

    const uint32_t source = SourceIndex(m, offset, bytes);

    if (source == UINT32_MAX) {
        std::ostringstream reason;
        reason << "Native model index has no source extent at partition offset 0x" << std::hex << offset;
        return Status::Error(reason.str());
    }

    const uint32_t index = Word(SourceRecord(m, source) + 12);
    {
        std::lock_guard lock(m.lock);
        const auto found = m.loaded.find(index);

        if (found != m.loaded.end()) {
            found->second->lastUse = ++m.clock;
            return {};
        }
    }

    const auto* record = PackRecord(m, index);
    const std::string name(reinterpret_cast<const char*>(m.index.data() + Word(record)), Word(record + 4));
    const auto path = m.indexPath.parent_path() / name;
    std::unique_ptr<Assets> asset;
    MH3_RETURN_IF_ERROR(TimedOpenPack(m, path, index, asset));
    return Admit(m, index, std::move(asset), false);
}

void EnsureNativeModelAssets(uint64_t offset, uint32_t bytes) {
    CheckStatus(TryEnsureNativeModelAssets(offset, bytes));
}

bool FindNativeModelSource(uint64_t offset, uint64_t* file_offset, uint32_t* file_bytes) {
    if (!manager || !file_offset || !file_bytes)
        return false;

    auto& m = *manager;

    if (m.converted)
        return GameAssetCache()->ModelFile(offset, *file_offset, *file_bytes);

    if (m.index.empty())
        return false;

    const uint32_t source = SourceIndex(m, offset, 1);

    if (source == UINT32_MAX)
        return false;

    *file_offset = Long(SourceRecord(m, source));
    *file_bytes = Word(SourceRecord(m, source) + 8);
    return true;
}

namespace {
// Render thread: the pack whose demand and GPU residency the last mesh draw
// established, in which frame and model GPU registry revision. A model's
// meshes draw in a row, and a pack demanded in the current frame is not
// evicted: while neither changes, its next draws skip the lock and lookups.
struct ResidentDraw {
    uintptr_t handle = 0;
    uint64_t epoch = 0, revision = 0;
    uint32_t frame = 0;
} lastResident;

bool LastResident(uintptr_t handle, uint64_t epoch) {
    return handle == lastResident.handle && epoch == lastResident.epoch && NativeModelGPUFrameSerial() == lastResident.frame && NativeModelGPURevision() == lastResident.revision;
}

void NoteResident(uintptr_t handle, uint64_t epoch, uint32_t frame) {
    lastResident = {handle, epoch, NativeModelGPURevision(), frame};
}

void ObserveDemand(Manager& m, uintptr_t handle, uint64_t epoch) {
    auto& asset = Find(m, handle, epoch);
    const uint32_t frame = NativeModelGPUFrameSerial();

    if (!m.frameKnown || frame != m.demandFrame) {
        m.demandFrame = frame;
        m.frameKnown = true;
        m.stats.demanded_gpu_bytes = 0;
    }

    if (asset.lastDemandFrame == frame)
        return;

    const size_t cost = NativeModelGPUAllocationBytes(asset.pack.gpu_bytes);

    if (cost > MaxGPUBytes || m.stats.demanded_gpu_bytes > MaxGPUBytes - cost)
        Error(asset.path, "logical frame model working set exceeds aggregate GPU budget");

    m.stats.demanded_gpu_bytes += cost;
    asset.lastDemandFrame = frame;
    asset.lastUse = ++m.clock;
}
} // namespace

void ObserveNativeModelAssetDemand(uintptr_t handle, uint64_t epoch) {
    if (!manager || LastResident(handle, epoch))
        return;

    std::lock_guard lock(manager->lock);
    ObserveDemand(*manager, handle, epoch);
}

bool EnsureNativeModelAssetGPU(uintptr_t handle, uint64_t epoch) {
    // Direct native renderer probes retain their own GPU owner/pack lifetime.
    if (!manager)
        return true;

    if (LastResident(handle, epoch))
        return true;

    auto& m = *manager;
    std::lock_guard lock(m.lock);
    auto& asset = Find(m, handle, epoch);
    ObserveDemand(m, handle, epoch);

    if (asset.gpuRegistered) {
        NoteResident(handle, epoch, m.demandFrame);
        return true;
    }

    const size_t cost = NativeModelGPUAllocationBytes(asset.pack.gpu_bytes);

    while (NativeModelGPUResidentBytes() > MaxGPUBytes - cost) {
        if (!EvictOneGPU(m, handle)) {
            ++m.stats.gpu_busy;
            return false;
        }
    }

    const auto name = asset.path.string();
    RegisterNativeModelGPU(asset.pack, name.c_str(), handle, epoch);
    asset.gpuRegistered = true;
    NoteResident(handle, epoch, m.demandFrame);
    return true;
}

bool ReadNativeModelPackFile(uintptr_t handle, uint64_t epoch, NativeModelPackFile* out) {
    if (!manager || !out)
        return false;

    std::lock_guard lock(manager->lock);
    const auto found = manager->handles.find(handle);

    if (found == manager->handles.end() || found->second->generation != epoch)
        return false;

    const auto& asset = *found->second;
    uint32_t hash = 2166136261u;

    for (const uint8_t byte : asset.identity)
        hash = (hash ^ byte) * 16777619u;

    out->path = asset.path.string();
    out->metadata_bytes = uint32_t(asset.metadata.size());
    out->identity_bytes = uint32_t(asset.identity.size());
    out->identity_hash = hash;
    return true;
}

NativeModelAssetStats ReadNativeModelAssetStats() {
    if (!manager)
        return {};

    std::lock_guard lock(manager->lock);
    auto result = manager->stats;
    result.gpu_bytes = NativeModelGPUResidentBytes();
    return result;
}

void ReadNativeModelPackOpenings(uint64_t& admissions, double& open_ms) {
    admissions = 0;
    open_ms = 0;

    if (!manager)
        return;

    std::lock_guard lock(manager->lock);
    admissions = manager->stats.admissions;
    open_ms = manager->stats.open_ms;
}

void UnloadNativeModelAssets() {
    if (!manager)
        return;

    for (auto& item : manager->loaded)
        Retire(*item.second);

    manager.reset();
}
