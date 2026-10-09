// Disc/guest address ownership boundary. Lookup algorithms and byte copies are
// portable C. No GX, content/name matching or renderer fallback is present.
#include "abi_bridge.h"
#include "fast_map.h"
#include "guest_span.h"
#include "memory.h"
#include "model_assets.h"
#include "runtime/native/adapters/model_catalog_bridge.h"
#include "runtime/native/adapters/model_pose_bridge.h"
#include <algorithm>
#include <array>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <map>
#include <sstream>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <charconv>

namespace {
constexpr size_t capacity = 8192;
struct Pack {
    const mh3_mesh_pack* pack;
    const mh3_model_catalog* catalog;
    uintptr_t handle;
    uint64_t generation;
    std::vector<mh3_prepared_mesh> meshes;
    std::vector<std::vector<mh3_pack_palette_batch>> palette_batches;
    std::vector<NativePreparedMaterial> materials;
    std::vector<uint32_t> material_catalog_indices;
    std::vector<std::array<mh3_catalog_material_block, 5>> material_blocks;
    struct Origin {
        uint32_t material_index, bytes, source_offset, model_index;
    };
    std::map<std::pair<uint64_t, uint32_t>, Origin> origins;
};
struct CachedOwner {
    NativeModelOwner owner;
    mh3_source_owner certificate;
};
struct CachedResource {
    uint64_t epoch, generation;
    mh3_catalog_resource resource;
};
struct CachedBlock {
    uint64_t epoch, block_epoch, source_generation;
    uintptr_t resident_handle;
    uint64_t resident_generation;
    uint32_t material_index;
};
// The provenance state (game thread). Admitted packs are immutable.
struct Catalog {
    std::array<mh3_source_range, capacity> ranges{}, work{}, transfer{};
    mh3_source_tracker tracker{};
    bool initialized = false;
    std::vector<std::shared_ptr<const Pack>> packs;
    // source_epoch: any provenance/structure change (copies, loads, retirement).
    // block_epoch: typed material group writes, which change block provenance
    // but never a resource header, so resolved resources stay valid across them.
    uint64_t source_epoch = 0, block_epoch = 0;
    FastMap<uint32_t, CachedOwner> owners;
    FastMap<uint64_t, CachedResource> resources;
    FastMap<uint64_t, CachedBlock> blocks;
    std::map<uint64_t, NativeLiveMaterialBlock> live_blocks;
    uint64_t material_revision = 0;
    NativeModelPreparationDiagnostics* diagnostics = nullptr;
    bool diagnostics_initialized = false;
};
Catalog catalog;
// Recent ReadNativeLiveMaterialBlockView results (game thread), direct-mapped
// by key. Map nodes stay put until erased, so a remembered node is valid
// while live_version is unchanged; every live block erase advances it.
struct LiveLookup {
    uint64_t key = UINT64_MAX, version = 0;
    const NativeLiveMaterialBlock* block = nullptr;
};
std::array<LiveLookup, 64> live_lookups{};
uint64_t live_version = 1;
LiveLookup& LiveSlot(uint64_t key) {
    return live_lookups[(key ^ (key >> 9)) & (live_lookups.size() - 1)];
}
NativeModelPreparationDiagnostics diagnostics;
struct DiagnosticConfig {
    bool enabled = false;
    uint32_t first = 0, last = UINT32_MAX;
    DiagnosticConfig() {
#ifdef MH3_DEVELOPMENT
        const char* setting = std::getenv("MH3_PREPARE_DIAGNOSTICS");
        if (!setting || !*setting || !std::strcmp(setting, "0"))
            return;
        if (std::strcmp(setting, "1")) {
            const char* end = setting + std::strlen(setting);
            const auto a = std::from_chars(setting, end, first);
            if (a.ec != std::errc{} || a.ptr == end || *a.ptr != ',')
                throw std::runtime_error("MH3_PREPARE_DIAGNOSTICS expects 1 or first,last");
            const auto b = std::from_chars(a.ptr + 1, end, last);
            if (b.ec != std::errc{} || b.ptr != end || last < first)
                throw std::runtime_error("MH3_PREPARE_DIAGNOSTICS expects 1 or first,last");
        }
        enabled = true;
#endif
    }
};
void InitializeDiagnostics(Catalog& c) {
    if (!c.diagnostics_initialized) {
        c.diagnostics = ActiveNativeModelPreparationDiagnostics();
        c.diagnostics_initialized = true;
    }
}
uint32_t DiagnosticFrame(const Catalog& c, uint32_t captured = UINT32_MAX) {
    return c.diagnostics ? (captured == UINT32_MAX ? Memory::Read32(0x80796d28) : captured) : UINT32_MAX;
}
void EpochDiagnostic(Catalog& c, NativeSourceEpochReason reason, NativeSourceEpochProducer producer,
    uint32_t destination, uint32_t source, uint32_t bytes, uint64_t offset, uint32_t conditions, uint32_t frame) {
    if (!c.diagnostics)
        return;
    auto& d = *c.diagnostics;
    ++d.epoch_advances;
    ++d.epoch_reasons[static_cast<unsigned>(reason)];
    if (d.last_epoch_frame != frame) {
        ++d.frames_with_epoch_advance;
        d.last_epoch_frame = frame;
    }
    if (frame < d.sample_first || frame > d.sample_last)
        return;
    ++d.matching_events;
    if (d.stored_events < 64)
        d.events[d.stored_events++] = {c.source_epoch, offset, frame, destination, source, bytes, conditions, reason, producer};
}
Catalog& C() {
    return catalog;
}
// The admitted packs, by reference.
struct PackIterator {
    std::vector<std::shared_ptr<const Pack>>::const_iterator it;
    const Pack& operator*() const { return **it; }
    PackIterator& operator++() {
        ++it;
        return *this;
    }
    bool operator!=(const PackIterator& other) const { return it != other.it; }
};
struct PackList {
    const std::vector<std::shared_ptr<const Pack>>& packs;
    PackIterator begin() const { return {packs.begin()}; }
    PackIterator end() const { return {packs.end()}; }
};
PackList Packs() {
    return {C().packs};
}
NativeModelVertexRetireHook vertex_retire = nullptr;
[[noreturn, gnu::cold, gnu::noinline]] void CheckedFailed(mh3_res_status status) {
    throw std::runtime_error("Native model provenance failed: " + std::to_string(status));
}
inline void Checked(mh3_res_status status) {
    if (status != MH3_RES_OK) [[unlikely]]
        CheckedFailed(status);
}
Catalog& Initialize() {
    auto& c = C();
    if (!c.initialized) {
        Checked(mh3_source_tracker_init(&c.tracker, c.ranges.data(), c.work.data(), c.transfer.data(), capacity));
        c.initialized = true;
        InitializeDiagnostics(c);
    }
    return c;
}
uint64_t NextGeneration() {
    if (C().tracker.generation == UINT64_MAX)
        throw std::runtime_error("Native model provenance generation overflow");
    return C().tracker.generation + 1;
}
uint32_t Canonical(uint32_t address, uint32_t bytes) {
    uint32_t canonical = 0;
    Checked(mh3_source_canonical_address(address, bytes, &canonical));
    return canonical;
}
void InvalidateSourceCache(Catalog& c, NativeSourceEpochReason reason, NativeSourceEpochProducer producer,
    uint32_t destination, uint32_t source, uint32_t bytes, uint64_t offset, uint32_t conditions, uint32_t frame) {
    if (c.source_epoch == UINT64_MAX)
        throw std::runtime_error("Native model source cache epoch overflow");
    ++c.source_epoch;
    EpochDiagnostic(c, reason, producer, destination, source, bytes, offset, conditions, frame);
}
void InvalidateBlockCache() {
    if (C().block_epoch == UINT64_MAX)
        throw std::runtime_error("Native model block cache epoch overflow");
    ++C().block_epoch;
}
bool RetireCertificates(uint32_t address, uint32_t bytes) {
    bool retired = false;
    for (auto it = C().owners.begin(); it != C().owners.end();) {
        if (mh3_source_owner_intersects(&it->second.certificate, address, bytes)) {
            it = C().owners.erase(it);
            retired = true;
        } else
            ++it;
    }
    return retired;
}
void CheckTypedMaterialWrite(uint32_t address, uint32_t bytes, mh3_model_material_block kind) {
    for (const auto& item : C().owners) {
        const auto status = mh3_source_owner_material_write(&item.second.certificate, item.second.owner.catalog, address, bytes, uint32_t(kind));
        if (status != MH3_RES_OK && status != MH3_RES_ABSENT)
            Checked(status);
    }
}
uint64_t LiveKey(uint32_t address, mh3_model_material_block kind) {
    if (kind < 0 || kind >= MH3_MODEL_MATERIAL_BLOCK_COUNT)
        throw std::runtime_error("Invalid native material group");
    return (uint64_t(address) << 3) | uint32_t(kind);
}
bool RetireLiveBlocks(uint32_t address, uint32_t bytes) {
    bool retired = false;
    // Every admitted group is at most480 bytes. Avoid walking all records for
    // unrelated generic copies, while still finding a partially hit record.
    const uint32_t start = address > 480 ? address - 480 : 0;
    auto it = C().live_blocks.lower_bound(uint64_t(start) << 3);
    const uint64_t end = (uint64_t)address + bytes;
    while (it != C().live_blocks.end() && it->second.address < end) {
        const auto& block = it->second;
        if ((uint64_t)block.address + block.bytes > address) {
            it = C().live_blocks.erase(it);
            ++live_version;
            retired = true;
        } else
            ++it;
    }
    return retired;
}
std::vector<NativeLiveMaterialBlock> SnapshotLiveCopy(uint32_t source, uint32_t destination, uint32_t bytes) {
    std::vector<NativeLiveMaterialBlock> result;
    auto it = C().live_blocks.lower_bound(uint64_t(source) << 3);
    while (it != C().live_blocks.end() && it->second.address < (uint64_t)source + bytes) {
        const auto& block = it->second;
        if ((uint64_t)block.address + block.bytes <= (uint64_t)source + bytes) {
            auto copy = block;
            copy.address = destination + (block.address - source);
            copy.fields.generation = NextNativeMaterialRevision();
            result.push_back(copy);
        }
        ++it;
    }
    return result;
}
std::vector<CachedOwner> SnapshotOwnerCopy(uint32_t source, uint32_t destination, uint32_t bytes, uint64_t generation) {
    std::vector<CachedOwner> result;
    for (const auto& item : C().owners) {
        const auto& certificate = item.second.certificate;
        if (certificate.container_address < source || (uint64_t)certificate.container_address + certificate.file_bytes > (uint64_t)source + bytes)
            continue;
        auto copy = item.second;
        copy.certificate.container_address = destination + (certificate.container_address - source);
        copy.certificate.model_address = copy.certificate.container_address + (certificate.model_address - certificate.container_address);
        copy.certificate.load_generation = generation;
        copy.owner.container_address = copy.certificate.container_address;
        copy.owner.model_address = copy.certificate.model_address;
        copy.owner.source_generation = generation;
        result.push_back(copy);
    }
    return result;
}
void InstallPack(const std::shared_ptr<const Pack>& pack, uint32_t diagnostic_frame = UINT32_MAX) {
    auto& c = C();
    InitializeDiagnostics(c);
    const uint32_t frame = DiagnosticFrame(c, diagnostic_frame);
    c.packs.push_back(pack);
    c.resources.clear();
    c.blocks.clear();
    InvalidateSourceCache(c, NativeSourceEpochReason::PackInstall, NativeSourceEpochProducer::PackManager, 0, 0, 0, 0, 0, frame);
}
// Certificates carry state the tracker alone cannot rebuild once typed writes
// retire content provenance.
void Certify(const std::vector<CachedOwner>& certified) {
    for (const auto& item : certified)
        C().owners.insert_or_assign(item.owner.model_address, item);
}
void Copy(CpuContext* cpu, NativeSourceEpochProducer producer) {
    const uint32_t destination = cpu->gpr[3], source = cpu->gpr[4], bytes = cpu->gpr[5];
    if (bytes) {
        void* dst = GuestRange(destination, bytes);
        const void* src = GuestRange(source, bytes);
        // Resolve provenance transaction first; capacity failure leaves guest
        // bytes untouched. Tracking does not inspect either byte payload.
        RecordNativeResourceCopy(destination, source, bytes, producer);
        mh3_model_copy_bytes(dst, src, bytes);
    }
    cpu->gpr[3] = destination;
}
} // namespace

NativeModelPreparationDiagnostics* ActiveNativeModelPreparationDiagnostics() {
    static const DiagnosticConfig config;
    if (!config.enabled)
        return nullptr;
    auto& d = diagnostics;
    d.sample_first = config.first;
    d.sample_last = config.last;
    return &d;
}

void ReportNativeModelPreparationDiagnostics() {
    auto* d = ActiveNativeModelPreparationDiagnostics();
    if (!d)
        return;
    struct Previous {
        NativePrepareCacheCounters material, shape;
        uint64_t reasons[static_cast<unsigned>(NativeSourceEpochReason::Count)]{};
        uint64_t advances = 0, frames = 0;
        uint32_t printed_events = 0;
    };
    static Previous previous;
    const auto n = [](uint64_t value) { return static_cast<unsigned long long>(value); };
    const auto& m = d->material;
    const auto& s = d->shape;
    std::fprintf(stderr, "MH3 prepare diagnostics: Game material hit %llu miss %llu replacement %llu; shape hit %llu miss %llu; token clears %llu/%llu capacity clears %llu/%llu discarded %llu/%llu; epoch advances %llu across %llu writer frames; native_frame=%u; writer sample %u/64 of %llu matching (frames %u..%u)\n",
        n(m.hits - previous.material.hits), n(m.misses - previous.material.misses), n(m.replacement_misses - previous.material.replacement_misses),
        n(s.hits - previous.shape.hits), n(s.misses - previous.shape.misses),
        n(m.token_clears - previous.material.token_clears), n(s.token_clears - previous.shape.token_clears),
        n(m.capacity_clears - previous.material.capacity_clears), n(s.capacity_clears - previous.shape.capacity_clears),
        n(m.discarded_entries - previous.material.discarded_entries), n(s.discarded_entries - previous.shape.discarded_entries),
        n(d->epoch_advances - previous.advances), n(d->frames_with_epoch_advance - previous.frames), Memory::Read32(0x80796d28),
        d->stored_events, n(d->matching_events), d->sample_first, d->sample_last);
    constexpr const char* reasons[]{"DI", "copy", "retire", "install", "unregister"};
    constexpr const char* producers[]{"DI", "resource-copy", "resource-retire", "memcpy", "memmove", "copy32", "copy32-unknown", "fill", "heap-create", "heap-destroy", "heap-allocate", "heap-free", "view-DMA", "view-LC", "locked-cache-DMA", "pack-manager"};
    std::fprintf(stderr, "MH3 source epoch reasons:");
    for (unsigned i = 0; i < static_cast<unsigned>(NativeSourceEpochReason::Count); ++i) {
        std::fprintf(stderr, " %s %llu", reasons[i], n(d->epoch_reasons[i] - previous.reasons[i]));
        previous.reasons[i] = d->epoch_reasons[i];
    }
    std::fprintf(stderr, "\n");
    for (uint32_t i = previous.printed_events; i < d->stored_events; ++i) {
        const auto& e = d->events[i];
        const unsigned reason = static_cast<unsigned>(e.reason), producer = static_cast<unsigned>(e.producer);
        std::fprintf(stderr, "MH3 source epoch writer: frame %u epoch %llu reason %s producer %s dst %08x src %08x bytes %u conditions %02x DI-offset %llu\n",
            e.frame, n(e.epoch), reason < static_cast<unsigned>(NativeSourceEpochReason::Count) ? reasons[reason] : "unknown",
            producer < static_cast<unsigned>(NativeSourceEpochProducer::Count) ? producers[producer] : "unknown",
            e.destination, e.source, e.bytes, e.conditions, n(e.partition_offset));
    }
    previous.material = m;
    previous.shape = s;
    previous.advances = d->epoch_advances;
    previous.frames = d->frames_with_epoch_advance;
    previous.printed_events = d->stored_events;
}

uint64_t ReadNativeModelStructureRevision() {
    return C().source_epoch;
}

void RecordNativeDiscWrite(uint32_t destination, uint64_t offset, uint32_t bytes, uint32_t diagnostic_frame) {
    if (!bytes)
        return;
    auto& c = Initialize();
    const uint32_t frame = DiagnosticFrame(c, diagnostic_frame);
    destination = Canonical(destination, bytes);
    Checked(mh3_source_record_disc(&C().tracker, destination, bytes, offset, NextGeneration()));
    RetireNativeModelPose(destination, bytes);
    if (vertex_retire)
        vertex_retire(destination, bytes);
    const bool retired_owner = RetireCertificates(destination, bytes), retired_block = RetireLiveBlocks(destination, bytes);
    InvalidateSourceCache(c, NativeSourceEpochReason::DiscWrite, NativeSourceEpochProducer::Disc, destination, 0, bytes, offset,
        (retired_owner ? NATIVE_EPOCH_RETIRED_OWNER : 0u) | (retired_block ? NATIVE_EPOCH_RETIRED_BLOCK : 0u), frame);
}
void RecordNativeResourceCopy(uint32_t destination, uint32_t source, uint32_t bytes, NativeSourceEpochProducer producer, uint32_t diagnostic_frame) {
    if (!bytes || destination == source)
        return;
    auto& c = Initialize();
    const uint32_t frame = DiagnosticFrame(c, diagnostic_frame);
    destination = Canonical(destination, bytes);
    source = Canonical(source, bytes);
    if (destination == source)
        return;
    // Most runtime copies have no disc ownership. Avoid walking all intervals
    // unless a source or overwritten destination intersects tracked bytes.
    const bool tracked_source = mh3_source_intersects(&c.tracker, source, bytes);
    // Keep the old short circuit when diagnostics are disabled. An enabled
    // sample distinguishes a source-only copy from overwritten provenance.
    const bool tracked_destination = (!tracked_source || c.diagnostics) && mh3_source_intersects(&c.tracker, destination, bytes);
    const bool tracked = tracked_source || tracked_destination;
    const auto copies = SnapshotLiveCopy(source, destination, bytes);
    const uint64_t generation = NextGeneration();
    const auto owner_copies = SnapshotOwnerCopy(source, destination, bytes, generation);
    if (tracked || !owner_copies.empty())
        Checked(mh3_source_record_copy(&C().tracker, destination, source, bytes, generation));
    RetireNativeModelPose(destination, bytes);
    if (vertex_retire)
        vertex_retire(destination, bytes);
    // A typed mutation may already have retired the byte provenance. Unknown
    // writes must still retire the independent allocation certificate.
    const bool retired_owner = RetireCertificates(destination, bytes), retired_block = RetireLiveBlocks(destination, bytes);
    // A complete memmove/memcpy of a certified container preserves its proven
    // structure, including typed material holes. Partial copies never certify
    // a model. Snapshots also preserve overlapping memmove source semantics.
    for (const auto& copy : owner_copies)
        C().owners.insert_or_assign(copy.owner.model_address, copy);
    for (const auto& copy : copies)
        C().live_blocks.insert_or_assign(LiveKey(copy.address, copy.kind), copy);
    if (retired_owner || retired_block || tracked || !copies.empty() || !owner_copies.empty())
        InvalidateSourceCache(c, NativeSourceEpochReason::ResourceCopy, producer, destination, source, bytes, 0,
            (tracked_source ? NATIVE_EPOCH_TRACKED_SOURCE : 0u) | (tracked_destination ? NATIVE_EPOCH_TRACKED_DESTINATION : 0u) |
            (retired_owner ? NATIVE_EPOCH_RETIRED_OWNER : 0u) | (retired_block ? NATIVE_EPOCH_RETIRED_BLOCK : 0u) |
            (!owner_copies.empty() ? NATIVE_EPOCH_COPIED_OWNER : 0u) | (!copies.empty() ? NATIVE_EPOCH_COPIED_BLOCK : 0u), frame);
}
void RetireNativeResourceOwner(uint32_t address, uint32_t bytes, NativeSourceEpochProducer producer, uint32_t diagnostic_frame) {
    if (!bytes)
        return;
    auto& c = Initialize();
    const uint32_t frame = DiagnosticFrame(c, diagnostic_frame);
    address = Canonical(address, bytes);
    const bool tracked = mh3_source_intersects(&C().tracker, address, bytes);
    if (tracked)
        Checked(mh3_source_retire(&C().tracker, address, bytes));
    RetireNativeModelPose(address, bytes);
    if (vertex_retire)
        vertex_retire(address, bytes);
    const bool retired_owner = RetireCertificates(address, bytes), retired_block = RetireLiveBlocks(address, bytes);
    if (retired_owner || retired_block || tracked)
        InvalidateSourceCache(c, NativeSourceEpochReason::ResourceRetire, producer, address, 0, bytes, 0,
            (tracked ? NATIVE_EPOCH_TRACKED_DESTINATION : 0u) | (retired_owner ? NATIVE_EPOCH_RETIRED_OWNER : 0u) | (retired_block ? NATIVE_EPOCH_RETIRED_BLOCK : 0u), frame);
}
void SetNativeModelVertexRetireHook(NativeModelVertexRetireHook hook) {
    if (vertex_retire && hook != vertex_retire)
        throw std::runtime_error("Native vertex retirement owner already installed");
    vertex_retire = hook;
}
void RecordNativeTypedMaterialWrite(uint32_t address, uint32_t bytes, mh3_model_material_block kind) {
    if (!bytes)
        return;
    Initialize();
    address = Canonical(address, bytes);
    CheckTypedMaterialWrite(address, bytes, kind);
    // Retire content provenance, not the independently certified load owner.
    // A reader without the newly published typed record now fails explicitly.
    if (mh3_source_intersects(&C().tracker, address, bytes))
        Checked(mh3_source_retire(&C().tracker, address, bytes));
    RetireLiveBlocks(address, bytes);
    InvalidateBlockCache();
}
void RecordNativeTypedMaterialCopy(uint32_t destination, uint32_t source, uint32_t bytes, mh3_model_material_block kind) {
    if (!bytes || destination == source)
        return;
    Initialize();
    destination = Canonical(destination, bytes);
    source = Canonical(source, bytes);
    if (destination == source)
        return;
    CheckTypedMaterialWrite(destination, bytes, kind);
    if (mh3_source_intersects(&C().tracker, source, bytes) || mh3_source_intersects(&C().tracker, destination, bytes))
        Checked(mh3_source_record_copy(&C().tracker, destination, source, bytes, NextGeneration()));
    RetireLiveBlocks(destination, bytes);
    InvalidateBlockCache();
}
void RegisterNativeModelPack(const mh3_mesh_pack* pack, const mh3_model_catalog* catalog, uintptr_t handle, uint64_t generation) {
    const auto trace = [](const char* stage, uint32_t index) {
#ifdef MH3_DEVELOPMENT
        if (std::getenv("MH3_NATIVE_ASSET_TRACE"))
            std::fprintf(stderr, "MH3 native catalog trace: %s %u\n", stage, index);
#else
        (void)stage;
        (void)index;
#endif
    };
    trace("validate", 0);
    if (!handle || !generation || mh3_model_catalog_validate_pack(catalog, pack) != MH3_PACK_OK)
        throw std::runtime_error("Invalid external native model pack registration");
    for (const auto& p : Packs()) {
        if (p.handle == handle)
            throw std::runtime_error("Native model resident handle already registered");
        for (uint32_t i = 0; i < catalog->model_count; i++) {
            mh3_catalog_model model{};
            mh3_model_catalog_model(catalog, i, &model);
            uint32_t found = 0;
            if (mh3_model_catalog_find(p.catalog, model.partition_offset, model.model_offset, &found) == MH3_PACK_OK)
                throw std::runtime_error("Ambiguous external native model pack identity");
        }
    }
    Pack admitted{pack, catalog, handle, generation, {}, {}, {}, {}, {}, {}};
    trace("prepare meshes", pack->counts[MH3_PACK_MESHES]);
    admitted.meshes.resize(pack->counts[MH3_PACK_MESHES]);
    admitted.palette_batches.resize(admitted.meshes.size());
    for (uint32_t i = 0; i < catalog->model_count; i++) {
        mh3_catalog_model model{};
        mh3_model_catalog_model(catalog, i, &model);
        for (uint32_t j = 0; j < model.shape_count; j++) {
            mh3_catalog_resource resource{};
            mh3_model_catalog_resource(catalog, 0, model.shape_first + j, &resource);
            if (resource.pack_index == UINT32_MAX)
                continue;
            if (mh3_model_mesh_prepare(catalog, pack, i, resource.source_id, handle, generation, &admitted.meshes[resource.pack_index]) != MH3_PACK_OK)
                throw std::runtime_error("Native model immutable admission failed");
            auto& prepared = admitted.meshes[resource.pack_index];
            if (prepared.palette_batch_count) {
                mh3_pack_palette_batches serialized{};
                if (mh3_mesh_pack_palette_batches(pack, resource.pack_index, &serialized) != MH3_PACK_OK)
                    throw std::runtime_error("Native indexed matrix admission failed");
                auto& batches = admitted.palette_batches[resource.pack_index];
                batches.resize(serialized.count);
                for (uint32_t n = 0; n < serialized.count; n++)
                    if (mh3_mesh_pack_palette_batch(&serialized, n, &batches[n]) != MH3_PACK_OK)
                        throw std::runtime_error("Native indexed matrix record admission failed");
                prepared.palette_batches = batches.data();
            }
        }
    }
    admitted.materials.resize(pack->counts[MH3_PACK_MATERIALS]);
    trace("prepare materials", pack->counts[MH3_PACK_MATERIALS]);
    admitted.material_catalog_indices.resize(admitted.materials.size(), UINT32_MAX);
    admitted.material_blocks.resize(admitted.materials.size());
    for (uint32_t i = 0; i < catalog->material_count; i++) {
        mh3_catalog_resource resource{};
        mh3_model_catalog_resource(catalog, 1, i, &resource);
        admitted.material_catalog_indices.at(resource.pack_index) = i;
        if (catalog->version == 2)
            for (uint32_t kind = 0; kind < 5; kind++) {
                if (mh3_model_catalog_block(catalog, i * 5 + kind, &admitted.material_blocks.at(resource.pack_index)[kind]) != MH3_PACK_OK)
                    throw std::runtime_error("Native material block admission failed");
            }
    }
    for (uint32_t i = 0; i < admitted.materials.size(); i++) {
        trace("material", i);
        if (mh3_model_material_prepare(pack, i, &admitted.materials[i]) != MH3_PACK_OK)
            throw std::runtime_error("Native model requires complete supported typed material payload");
        const auto& material = admitted.materials[i];
        const uint32_t required = (1u << material.state.texgen_count) - 1;
        if ((material.texcoord_map_known & required) != required)
            throw std::runtime_error("Native model requires M3MT v3 exact texture-coordinate map provenance");
        if (catalog->version == 2) {
            mh3_catalog_resource resource{};
            mh3_catalog_model model{};
            mh3_model_catalog_resource(catalog, 1, admitted.material_catalog_indices.at(i), &resource);
            mh3_model_catalog_model(catalog, resource.model_index, &model);
            for (uint32_t kind = 0; kind < 5; kind++) {
                const auto& block = admitted.material_blocks[i][kind];
                if (!(block.flags & 1))
                    continue;
                const auto key = std::make_pair(model.partition_offset + block.source_offset, kind);
                Pack::Origin origin{i, block.source_bytes, block.source_offset, resource.model_index};
                const auto alias = admitted.origins.find(key);
                if (alias != admitted.origins.end()) {
                    mh3_native_material_state a{}, b{};
                    const auto& other = admitted.materials.at(alias->second.material_index);
                    const uint32_t count = kind == MH3_MODEL_MATERIAL_TEXGEN ? material.state.texgen_count : 1;
                    if (alias->second.bytes != origin.bytes || mh3_model_material_group_select(&material, mh3_model_material_block(kind), count, &a) != MH3_PACK_OK ||
                        mh3_model_material_group_select(&other, mh3_model_material_block(kind), count, &b) != MH3_PACK_OK || std::memcmp(&a, &b, sizeof a) ||
                        (kind == MH3_MODEL_MATERIAL_PIPELINE && (material.texcoord_map_known != other.texcoord_map_known || std::memcmp(material.texcoord_map, other.texcoord_map, 8))))
                        throw std::runtime_error("Shared material source extent has inconsistent typed ownership");
                } else
                    admitted.origins.emplace(key, origin);
            }
        }
    }
    InstallPack(std::make_shared<const Pack>(std::move(admitted)));
    trace("publish", 0);
}
void UnregisterNativeModelPack(uintptr_t handle, uint32_t diagnostic_frame) {
    auto& c = C();
    InitializeDiagnostics(c);
    const uint32_t frame = DiagnosticFrame(c, diagnostic_frame);
    for (auto it = C().packs.begin(); it != C().packs.end(); ++it)
        if ((*it)->handle == handle) {
            C().packs.erase(it);
            C().resources.clear();
            C().blocks.clear();
            for (auto owner = C().owners.begin(); owner != C().owners.end();) {
                if (owner->second.owner.resident_handle == handle)
                    owner = C().owners.erase(owner);
                else
                    ++owner;
            }
            for (auto block = C().live_blocks.begin(); block != C().live_blocks.end();) {
                if (block->second.resident_handle == handle) {
                    block = C().live_blocks.erase(block);
                    ++live_version;
                } else
                    ++block;
            }
            InvalidateSourceCache(c, NativeSourceEpochReason::PackUnregister, NativeSourceEpochProducer::PackManager, 0, 0, 0, 0, 0, frame);
            return;
        }
    throw std::runtime_error("Unknown native model resident handle");
}
bool NativeModelPackHasLiveSources(uintptr_t handle) {
    for (const auto& owner : C().owners)
        if (owner.second.owner.resident_handle == handle)
            return true;
    for (const auto& block : C().live_blocks)
        if (block.second.resident_handle == handle)
            return true;
    for (const auto& pack : Packs())
        if (pack.handle == handle) {
            int live = 0;
            Checked(mh3_source_catalog_live(&C().tracker, pack.catalog, &live));
            return live != 0;
        }
    return false;
}
NativeModelOwner ResolveNativeModelOwner(uint32_t model_address) {
    Initialize();
    model_address = Canonical(model_address, 16);
    const auto cached = C().owners.find(model_address);
    if (cached != C().owners.end())
        return cached->second.owner;
    mh3_source_identity identity{};
    const auto source_status = mh3_source_resolve(&C().tracker, model_address, 16, &identity);
    if (source_status == MH3_RES_OK)
        EnsureNativeModelAssets(identity.partition_offset, 16);
    else if (source_status != MH3_RES_ABSENT)
        Checked(source_status);
    for (const auto& p : Packs()) {
        mh3_source_owner certificate{};
        const auto status = mh3_source_owner_certify(&C().tracker, p.catalog, model_address, &certificate);
        if (status == MH3_RES_ABSENT)
            continue;
        Checked(status);
        const uint32_t index = certificate.catalog_index, container = certificate.container_address;
        const uint64_t generation = certificate.load_generation;
        // Certify every cataloged sibling now. A subsequent verified mutation
        // may remove material content provenance before another model is used.
        std::vector<CachedOwner> siblings;
        for (uint32_t j = 0; j < p.catalog->model_count; j++) {
            mh3_source_owner sibling{};
            const auto sibling_status = mh3_source_owner_sibling(&certificate, p.catalog, j, &sibling);
            if (sibling_status == MH3_RES_ABSENT)
                continue;
            Checked(sibling_status);
            mh3_catalog_model model{};
            mh3_model_catalog_model(p.catalog, j, &model);
            const uint32_t address = sibling.model_address;
            // Provenance selected the owner; header fields validate layout,
            // rather than identifying bytes by a magic/name/content match.
            if (Memory::Read32(address) != 0x4d444c30 || Memory::Read32(address + 4) != model.model_bytes || Memory::Read32(address + 8) != model.revision ||
                Canonical(address + Memory::Read32(address + 12), 1) != container)
                throw std::runtime_error("Loaded native MDL0 header differs from registered source identity");
            NativeModelOwner related{p.pack, p.catalog, j, container, address, p.handle, p.generation, generation};
            siblings.push_back({related, sibling});
        }
        NativeModelOwner owner{p.pack, p.catalog, index, container, model_address, p.handle, p.generation, generation};
        Certify(siblings);
        return owner;
    }
    throw std::runtime_error("Model has no complete native loader provenance or external converted pack");
}
mh3_catalog_resource ResolveNativeModelResource(const NativeModelOwner& owner, uint32_t resource, bool material) {
    const NativeModelOwner current = ResolveNativeModelOwner(owner.model_address);
    if (current.pack != owner.pack || current.catalog != owner.catalog || current.resident_handle != owner.resident_handle || current.resident_generation != owner.resident_generation ||
        current.source_generation != owner.source_generation || current.catalog_model_index != owner.catalog_model_index)
        throw std::runtime_error("Native model owner was invalidated");
    resource = Canonical(resource, material ? 16 : 64);
    const uint64_t key = (uint64_t(resource) << 1) | uint32_t(material);
    const auto cached = C().resources.find(key);
    if (cached != C().resources.end() && cached->second.epoch == C().source_epoch && cached->second.generation == owner.source_generation) {
        if (cached->second.resource.model_index != owner.catalog_model_index)
            throw std::runtime_error("Native model resource cache has a different owner");
        return cached->second.resource;
    }
    if (resource < owner.container_address)
        throw std::runtime_error("Native model resource precedes its container");
    const uint32_t id = Memory::Read32(resource + (material ? 12 : 60));
    mh3_catalog_resource r{};
    if (mh3_model_catalog_find_resource(owner.catalog, owner.catalog_model_index, material, id, &r) != MH3_PACK_OK || resource - owner.container_address != r.source_offset ||
        Memory::Read32(resource) != r.source_bytes || Canonical(resource + Memory::Read32(resource + 4), 1) != owner.model_address)
        throw std::runtime_error("Native model subresource identity differs from registered source");
    C().resources.insert_or_assign(key, CachedResource{C().source_epoch, owner.source_generation, r});
    return r;
}
const mh3_prepared_mesh& ReadNativePreparedMesh(const NativeModelOwner& owner, uint32_t shape) {
    const mh3_catalog_resource resource = ResolveNativeModelResource(owner, shape, false);
    if (resource.pack_index == UINT32_MAX)
        throw std::runtime_error("Native model shape has no converted mesh");
    for (const auto& pack : Packs())
        if (pack.handle == owner.resident_handle && pack.generation == owner.resident_generation)
            return pack.meshes.at(resource.pack_index);
    throw std::runtime_error("Native model resident allocation was retired");
}
const NativePreparedMaterial& ReadNativePreparedMaterial(const NativeModelOwner& owner, uint32_t material) {
    const mh3_catalog_resource resource = ResolveNativeModelResource(owner, material, true);
    for (const auto& pack : Packs())
        if (pack.handle == owner.resident_handle && pack.generation == owner.resident_generation)
            return pack.materials.at(resource.pack_index);
    throw std::runtime_error("Native model material resident allocation was retired");
}
NativeMaterialBlockDefaults ReadNativeMaterialBlockDefaults(const NativeModelOwner& owner, uint32_t material, mh3_model_material_block kind, uint32_t actual_block) {
    const mh3_catalog_resource resource = ResolveNativeModelResource(owner, material, true);
    if (kind < 0 || kind >= MH3_MODEL_MATERIAL_BLOCK_COUNT)
        throw std::runtime_error("Invalid native material block kind");
    for (const auto& pack : Packs())
        if (pack.handle == owner.resident_handle && pack.generation == owner.resident_generation) {
            const auto& block = pack.material_blocks.at(resource.pack_index)[kind];
            if (!(block.flags & 1))
                throw std::runtime_error("Native material block requires verified M3ID v2 ownership");
            actual_block = Canonical(actual_block, block.source_bytes);
            // Material tables have 128-byte records in a U32-sized metadata span,
            // so their admitted index fits the lower29 bits. Keep shared TEV
            // scopes separate instead of evicting each other on alternate draws.
            const uint64_t key = (uint64_t(actual_block) << 32) | (uint64_t(resource.pack_index) << 3) | uint32_t(kind);
            const auto cached = C().blocks.find(key);
            uint64_t generation = 0;
            if (cached != C().blocks.end() && cached->second.epoch == C().source_epoch && cached->second.block_epoch == C().block_epoch && cached->second.resident_handle == owner.resident_handle &&
                cached->second.resident_generation == owner.resident_generation && cached->second.material_index == resource.pack_index)
                generation = cached->second.source_generation;
            else {
                Checked(mh3_source_resolve_material_block(&C().tracker, owner.catalog, actual_block, pack.material_catalog_indices.at(resource.pack_index), uint32_t(kind), &generation));
                C().blocks.insert_or_assign(key, CachedBlock{C().source_epoch, C().block_epoch, generation, owner.resident_handle, owner.resident_generation, resource.pack_index});
            }
            return {&pack.materials.at(resource.pack_index), actual_block, block.source_bytes, kind, generation};
        }
    throw std::runtime_error("Native material block resident owner was retired");
}
uint64_t NextNativeMaterialRevision() {
    if (C().material_revision == UINT64_MAX)
        throw std::runtime_error("Native material producer revision overflow");
    return ++C().material_revision;
}
const NativeLiveMaterialBlock& ReadNativeLiveMaterialBlockView(uint32_t address, mh3_model_material_block kind) {
    Initialize();
    address = Canonical(address, 1);
    const auto key = LiveKey(address, kind);
    auto& recent = LiveSlot(key);
    if (recent.key == key && recent.version == live_version)
        return *recent.block;
    const auto cached = C().live_blocks.find(key);
    if (cached != C().live_blocks.end()) {
        recent = {key, live_version, &cached->second};
        return cached->second;
    }
    mh3_source_identity identity{};
    Checked(mh3_source_resolve(&C().tracker, address, 1, &identity));
    EnsureNativeModelAssets(identity.partition_offset, 1);
    for (const auto& pack : Packs()) {
        const auto origin = pack.origins.find(std::make_pair(identity.partition_offset, uint32_t(kind)));
        if (origin == pack.origins.end())
            continue;
        const auto& source = origin->second;
        Canonical(address, source.bytes);
        uint64_t generation = 0;
        Checked(mh3_source_resolve_material_block(&C().tracker, pack.catalog, address, pack.material_catalog_indices.at(source.material_index), uint32_t(kind), &generation));
        // If this is the complete original container, certify its model before
        // this producer removes nonstructural byte provenance. A standalone
        // copied block needs only its own complete source proof.
        if (address >= source.source_offset) {
            mh3_catalog_model model{};
            mh3_model_catalog_model(pack.catalog, source.model_index, &model);
            const uint64_t candidate = (uint64_t)address - source.source_offset + model.model_offset;
            if (candidate <= UINT32_MAX) {
                mh3_source_owner owner{};
                const auto status = mh3_source_owner_certify(&C().tracker, pack.catalog, uint32_t(candidate), &owner);
                if (status == MH3_RES_OK)
                    (void)ResolveNativeModelOwner(uint32_t(candidate));
                else if (status != MH3_RES_ABSENT)
                    Checked(status);
            }
        }
        NativeLiveMaterialBlock block{pack.materials.at(source.material_index).state, address, source.bytes, kind, pack.handle, pack.generation, identity.partition_offset, generation};
        block.fields.generation = NextNativeMaterialRevision();
        const auto& admitted = C().live_blocks.emplace(key, std::move(block)).first->second;
        recent = {key, live_version, &admitted};
        return admitted;
    }
    std::ostringstream detail;
    detail << "Material block has no admitted typed source extent: address=0x" << std::hex << address << " kind=" << std::dec << uint32_t(kind) << " partition_offset=0x" << std::hex
           << identity.partition_offset;
    throw std::runtime_error(detail.str());
}
NativeLiveMaterialBlock ReadNativeLiveMaterialBlock(uint32_t address, mh3_model_material_block kind) {
    return ReadNativeLiveMaterialBlockView(address, kind);
}
void PublishNativeLiveMaterialBlock(const NativeLiveMaterialBlock& block) {
    if (!block.fields.generation || block.fields.generation > C().material_revision || Canonical(block.address, block.bytes) != block.address)
        throw std::runtime_error("Invalid typed native material publication");
    for (const auto& pack : Packs())
        if (pack.handle == block.resident_handle && pack.generation == block.resident_generation) {
            const auto origin = pack.origins.find(std::make_pair(block.origin_offset, uint32_t(block.kind)));
            if (origin == pack.origins.end() || origin->second.bytes != block.bytes)
                throw std::runtime_error("Native material publication changed source layout identity");
            C().live_blocks.insert_or_assign(LiveKey(block.address, block.kind), block);
            return;
        }
    throw std::runtime_error("Native material publication refers to a retired resident allocation");
}
void ReadNativeModelCoordinateMap(uint32_t pipeline, uint8_t mapping[8], uint8_t* known) {
    if (!mapping || !known)
        throw std::runtime_error("Native coordinate mapping requires outputs");
    const auto& block = ReadNativeLiveMaterialBlockView(pipeline, MH3_MODEL_MATERIAL_PIPELINE);
    for (const auto& pack : Packs())
        if (pack.handle == block.resident_handle && pack.generation == block.resident_generation) {
            const auto origin = pack.origins.find(std::make_pair(block.origin_offset, uint32_t(MH3_MODEL_MATERIAL_PIPELINE)));
            if (origin == pack.origins.end())
                break;
            const auto& prepared = pack.materials.at(origin->second.material_index);
            std::memcpy(mapping, prepared.texcoord_map, 8);
            *known = prepared.texcoord_map_known;
            return;
        }
    throw std::runtime_error("Native coordinate mapping owner was retired");
}
void CopyNativeLiveMaterialBlock(uint32_t destination, uint32_t source, mh3_model_material_block kind) {
    auto block = ReadNativeLiveMaterialBlock(source, kind);
    destination = Canonical(destination, block.bytes);
    source = Canonical(source, block.bytes);
    void* output = GuestRange(destination, block.bytes);
    const void* input = GuestRange(source, block.bytes);
    if (destination == source)
        return;
    const uint64_t revision = NextNativeMaterialRevision();
    RecordNativeTypedMaterialCopy(destination, source, block.bytes, kind);
    mh3_model_copy_bytes(output, input, block.bytes);
    block.address = destination;
    block.fields.generation = revision;
    PublishNativeLiveMaterialBlock(block);
}
extern "C" void MH3_ModelMemcpy(CpuContext* cpu) {
    Copy(cpu, NativeSourceEpochProducer::Memcpy);
}
REGISTER_NATIVE_FUNCTION_AS(0x80004000, MH3_ModelMemcpy, "NativeResourceMemcpy");
extern "C" void MH3_ModelMemmove(CpuContext* cpu) {
    Copy(cpu, NativeSourceEpochProducer::Memmove);
}
REGISTER_NATIVE_FUNCTION_AS(0x8045BC18, MH3_ModelMemmove, "NativeResourceMemmove");
