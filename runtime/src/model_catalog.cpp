// Disc/guest address ownership boundary. Lookup algorithms and byte copies are
// portable C. No GX, content/name matching or renderer fallback is present.
#include "abi_bridge.h"
#include "fatal.h"
#include <aurora/fast_map.h>
#include "guest_span.h"
#include "guest_worker.h"
#include "memory.h"
#include "model_assets.h"
#include "runtime/native/adapters/model_catalog_bridge.h"
#include "runtime/native/adapters/model_pose_bridge.h"
#include "runtime/native/adapters/model_scope_bridge.h"
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

    // The model's draw table: it lives exactly as long as this certificate.
    std::shared_ptr<NativeModelDrawTable> table;
};

bool SameOwner(const NativeModelOwner& a, const NativeModelOwner& b) {
    return a.pack == b.pack && a.catalog == b.catalog && a.catalog_model_index == b.catalog_model_index && a.container_address == b.container_address && a.model_address == b.model_address &&
           a.resident_handle == b.resident_handle && a.resident_generation == b.resident_generation && a.source_generation == b.source_generation;
}

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

    // live_blocks keeps the address order RetireLiveBlocks walks; live_index
    // finds a block's node by key without the tree walk (nodes stay put until
    // erased). Change both through PutLiveBlock and EraseLiveBlock.
    std::map<uint64_t, NativeLiveMaterialBlock> live_blocks;
    FastMap<uint64_t, NativeLiveMaterialBlock*> live_index;

    // Certificate ranges sorted by start, with the running maximum end: a
    // write that ends before every range or starts after all of their ends
    // retires no certificate. Rebuilt after the owner set changed.
    std::vector<std::pair<uint32_t, uint64_t>> certificate_ranges;
    std::vector<uint64_t> certificate_ends;
    bool certificates_dirty = true;
    uint64_t material_revision = 0;

    // Tracker generation of the last CertifyLoadedNativeModels pass.
    uint64_t discovered_generation = UINT64_MAX;
    NativeModelPreparationDiagnostics* diagnostics = nullptr;
    bool diagnostics_initialized = false;
};

Catalog catalog;

// Advanced by every live block erase or full (re)publication; not by an
// in-place producer update (WritableNativeLiveMaterialBlock).
uint64_t live_version = 1;
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
                MH3_THROW(std::runtime_error("MH3_PREPARE_DIAGNOSTICS expects 1 or first,last"));

            const auto b = std::from_chars(a.ptr + 1, end, last);

            if (b.ec != std::errc{} || b.ptr != end || last < first)
                MH3_THROW(std::runtime_error("MH3_PREPARE_DIAGNOSTICS expects 1 or first,last"));
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

// Typed writes of a writable block were validated with no other block over
// its bytes and against the current owners: a new block or owner over them
// takes that back (ForbidInPlaceWrites).
void ForbidInPlaceWrites(uint32_t address, uint64_t bytes) {
    // Every admitted group is at most 480 bytes.
    const uint32_t start = address > 480 ? address - 480 : 0;
    const uint64_t end = uint64_t(address) + bytes;

    for (auto it = C().live_blocks.lower_bound(uint64_t(start) << 3); it != C().live_blocks.end() && it->second.address < end; ++it)
        if (uint64_t(it->second.address) + it->second.bytes > address)
            it->second.writable = false;
}

NativeLiveMaterialBlock& PutLiveBlock(uint64_t key, NativeLiveMaterialBlock block) {
    const uint32_t address = block.address, bytes = block.bytes;
    const auto [it, inserted] = C().live_blocks.insert_or_assign(key, std::move(block));

    if (inserted) {
        C().live_index.insert_or_assign(key, &it->second);
        ForbidInPlaceWrites(address, bytes);
    }

    it->second.writable = false;
    return it->second;
}

std::map<uint64_t, NativeLiveMaterialBlock>::iterator EraseLiveBlock(std::map<uint64_t, NativeLiveMaterialBlock>::iterator it) {
    C().live_index.erase(it->first);
    return C().live_blocks.erase(it);
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
    MH3_THROW(std::runtime_error("Native model provenance failed: " + std::to_string(status)));
}

inline void Checked(mh3_res_status status) {
    if (status != MH3_RES_OK) [[unlikely]]
        CheckedFailed(status);
}

// Checked for a caller that recovers from the failure.
Status CheckedStatus(mh3_res_status status) {
    if (status != MH3_RES_OK) [[unlikely]]
        return Status::Error("Native model provenance failed: " + std::to_string(status));

    return {};
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
        MH3_THROW(std::runtime_error("Native model provenance generation overflow"));

    return C().tracker.generation + 1;
}

uint32_t Canonical(uint32_t address, uint32_t bytes) {
    uint32_t canonical = 0;
    Checked(mh3_source_canonical_address(address, bytes, &canonical));
    return canonical;
}

Status CanonicalAddress(uint32_t address, uint32_t bytes, uint32_t& canonical) {
    return CheckedStatus(mh3_source_canonical_address(address, bytes, &canonical));
}

// A guest word for a caller that recovers from an unmapped address.
Status ReadGuest32(uint32_t address, uint32_t& value) {
    if (!Memory::TryRead32(address, value))
        return Status::Error(Memory::UnmappedMessage(address));

    return {};
}

// What std::vector::at reports for an index past the end.
Status RangeCheck(size_t index, size_t size) {
    if (index >= size)
        return Status::Error("vector::_M_range_check: __n (which is " + std::to_string(index) + ") >= this->size() (which is " + std::to_string(size) + ")");

    return {};
}

void InvalidateSourceCache(Catalog& c, NativeSourceEpochReason reason, NativeSourceEpochProducer producer,
    uint32_t destination, uint32_t source, uint32_t bytes, uint64_t offset, uint32_t conditions, uint32_t frame) {
    if (c.source_epoch == UINT64_MAX)
        MH3_THROW(std::runtime_error("Native model source cache epoch overflow"));

    ++c.source_epoch;
    EpochDiagnostic(c, reason, producer, destination, source, bytes, offset, conditions, frame);
}

void OwnersChanged() {
    C().certificates_dirty = true;
}

// A new owner's certificate may reject typed writes inside its container.
void OwnerAdded(const CachedOwner& owner) {
    OwnersChanged();
    ForbidInPlaceWrites(owner.certificate.container_address, owner.certificate.file_bytes);
}

// False when no certificate can intersect [address, address + bytes).
bool MayHitCertificate(uint32_t address, uint32_t bytes) {
    auto& c = C();

    if (c.certificates_dirty) {
        c.certificate_ranges.clear();

        for (const auto& item : c.owners) {
            const auto& certificate = item.second.certificate;
            c.certificate_ranges.emplace_back(certificate.container_address, uint64_t(certificate.container_address) + certificate.file_bytes);
        }

        std::sort(c.certificate_ranges.begin(), c.certificate_ranges.end());
        c.certificate_ends.resize(c.certificate_ranges.size());
        uint64_t end = 0;

        for (size_t i = 0; i < c.certificate_ranges.size(); i++)
            c.certificate_ends[i] = end = std::max(end, c.certificate_ranges[i].second);

        c.certificates_dirty = false;
    }

    const uint64_t end = uint64_t(address) + bytes;

    // Ranges starting before the write's end; one of them must end after its start.
    const size_t n = size_t(std::lower_bound(c.certificate_ranges.begin(), c.certificate_ranges.end(), std::make_pair(uint32_t(0), uint64_t(0)),
                                             [end](const std::pair<uint32_t, uint64_t>& range, const std::pair<uint32_t, uint64_t>&) { return range.first < end; }) -
                            c.certificate_ranges.begin());
    return n && c.certificate_ends[n - 1] > address;
}

void InvalidateBlockCache() {
    if (C().block_epoch == UINT64_MAX)
        MH3_THROW(std::runtime_error("Native model block cache epoch overflow"));

    ++C().block_epoch;
}

bool RetireCertificates(uint32_t address, uint32_t bytes) {
    bool retired = false;

    if (!MayHitCertificate(address, bytes))
        return false;

    for (auto it = C().owners.begin(); it != C().owners.end();) {
        if (mh3_source_owner_intersects(&it->second.certificate, address, bytes)) {
            it = C().owners.erase(it);
            OwnersChanged();
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
        MH3_THROW(std::runtime_error("Invalid native material group"));

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
            it = EraseLiveBlock(it);
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

    if (!MayHitCertificate(source, bytes))
        return result;

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

        // Tables hold absolute addresses: the copy builds its own.
        copy.table.reset();
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
    for (const auto& item : certified) {
        auto [entry, inserted] = C().owners.try_emplace(item.owner.model_address, item);

        if (inserted) {
            OwnerAdded(item);
            continue;
        }

        // Recertifying an unchanged owner keeps its draw table (and its
        // container, so typed writes inside it stay validated).
        const bool same = SameOwner(entry->second.owner, item.owner);
        auto table = same ? std::move(entry->second.table) : nullptr;
        entry->second = item;
        entry->second.table = std::move(table);

        if (same)
            OwnersChanged();
        else
            OwnerAdded(item);
    }
}

uint32_t Copy(uint32_t destination, uint32_t source, uint32_t bytes, NativeSourceEpochProducer producer) {
    if (bytes) {
        void* dst = GuestRange(destination, bytes);
        const void* src = GuestRange(source, bytes);

        // Resolve provenance transaction first; capacity failure leaves guest
        // bytes untouched. Tracking does not inspect either byte payload.
        RecordNativeResourceCopy(destination, source, bytes, producer);
        mh3_model_copy_bytes(dst, src, bytes);
    }

    return destination;
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
    ReportNativeModelDrawTables();
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

uint64_t ReadNativeMaterialBlockRevision() {
    return C().block_epoch;
}

uint64_t ReadNativeLiveMaterialRevision() {
    return live_version;
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
#ifdef MH3_DEVELOPMENT
    GuestWorker::NoteShared("resource copy");
#endif

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
    for (const auto& copy : owner_copies) {
        C().owners.insert_or_assign(copy.owner.model_address, copy);
        OwnerAdded(copy);
    }

    for (const auto& copy : copies)
        PutLiveBlock(LiveKey(copy.address, copy.kind), copy);

    if (!copies.empty())
        ++live_version;

    if (retired_owner || retired_block || tracked || !copies.empty() || !owner_copies.empty())
        InvalidateSourceCache(c, NativeSourceEpochReason::ResourceCopy, producer, destination, source, bytes, 0,
            (tracked_source ? NATIVE_EPOCH_TRACKED_SOURCE : 0u) | (tracked_destination ? NATIVE_EPOCH_TRACKED_DESTINATION : 0u) |
            (retired_owner ? NATIVE_EPOCH_RETIRED_OWNER : 0u) | (retired_block ? NATIVE_EPOCH_RETIRED_BLOCK : 0u) |
            (!owner_copies.empty() ? NATIVE_EPOCH_COPIED_OWNER : 0u) | (!copies.empty() ? NATIVE_EPOCH_COPIED_BLOCK : 0u), frame);
}

void RetireNativeResourceOwner(uint32_t address, uint32_t bytes, NativeSourceEpochProducer producer, uint32_t diagnostic_frame) {
    if (!bytes)
        return;
#ifdef MH3_DEVELOPMENT
    GuestWorker::NoteShared("resource owner retirement");
#endif

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
        MH3_THROW(std::runtime_error("Native vertex retirement owner already installed"));

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

Status RegisterNativeModelPack(const mh3_mesh_pack* pack, const mh3_model_catalog* catalog, uintptr_t handle, uint64_t generation) {
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
        return Status::Error("Invalid external native model pack registration");

    for (const auto& p : Packs()) {
        if (p.handle == handle)
            return Status::Error("Native model resident handle already registered");

        for (uint32_t i = 0; i < catalog->model_count; i++) {
            mh3_catalog_model model{};
            mh3_model_catalog_model(catalog, i, &model);
            uint32_t found = 0;

            if (mh3_model_catalog_find(p.catalog, model.partition_offset, model.model_offset, &found) == MH3_PACK_OK)
                return Status::Error("Ambiguous external native model pack identity");
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
                return Status::Error("Native model immutable admission failed");

            auto& prepared = admitted.meshes[resource.pack_index];

            if (prepared.palette_batch_count) {
                mh3_pack_palette_batches serialized{};

                if (mh3_mesh_pack_palette_batches(pack, resource.pack_index, &serialized) != MH3_PACK_OK)
                    return Status::Error("Native indexed matrix admission failed");

                auto& batches = admitted.palette_batches[resource.pack_index];
                batches.resize(serialized.count);

                for (uint32_t n = 0; n < serialized.count; n++)
                    if (mh3_mesh_pack_palette_batch(&serialized, n, &batches[n]) != MH3_PACK_OK)
                        return Status::Error("Native indexed matrix record admission failed");
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
        MH3_RETURN_IF_ERROR(RangeCheck(resource.pack_index, admitted.material_catalog_indices.size()));
        admitted.material_catalog_indices[resource.pack_index] = i;

        if (catalog->version == 2)
            for (uint32_t kind = 0; kind < 5; kind++) {
                if (mh3_model_catalog_block(catalog, i * 5 + kind, &admitted.material_blocks.at(resource.pack_index)[kind]) != MH3_PACK_OK)
                    return Status::Error("Native material block admission failed");
            }
    }

    for (uint32_t i = 0; i < admitted.materials.size(); i++) {
        trace("material", i);

        if (mh3_model_material_prepare(pack, i, &admitted.materials[i]) != MH3_PACK_OK)
            return Status::Error("Native model requires complete supported typed material payload");

        const auto& material = admitted.materials[i];
        const uint32_t required = (1u << material.state.texgen_count) - 1;

        if ((material.texcoord_map_known & required) != required)
            return Status::Error("Native model requires M3MT v3 exact texture-coordinate map provenance");

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
                        return Status::Error("Shared material source extent has inconsistent typed ownership");
                } else
                    admitted.origins.emplace(key, origin);
            }
        }
    }

    InstallPack(std::make_shared<const Pack>(std::move(admitted)));
    trace("publish", 0);
    return {};
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
                if (owner->second.owner.resident_handle == handle) {
                    owner = C().owners.erase(owner);
                    OwnersChanged();
                } else
                    ++owner;
            }

            for (auto block = C().live_blocks.begin(); block != C().live_blocks.end();) {
                if (block->second.resident_handle == handle) {
                    block = EraseLiveBlock(block);
                    ++live_version;
                } else
                    ++block;
            }

            InvalidateSourceCache(c, NativeSourceEpochReason::PackUnregister, NativeSourceEpochProducer::PackManager, 0, 0, 0, 0, 0, frame);
            return;
        }
    MH3_THROW(std::runtime_error("Unknown native model resident handle"));
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

Status TryResolveNativeModelOwner(uint32_t model_address, NativeModelOwner& owner) {
    Initialize();
    MH3_RETURN_IF_ERROR(CanonicalAddress(model_address, 16, model_address));
    const auto cached = C().owners.find(model_address);

    if (cached != C().owners.end()) {
        owner = cached->second.owner;
        return {};
    }

    mh3_source_identity identity{};
    const auto source_status = mh3_source_resolve(&C().tracker, model_address, 16, &identity);

    if (source_status == MH3_RES_OK)
        MH3_RETURN_IF_ERROR(TryEnsureNativeModelAssets(identity.partition_offset, 16));
    else if (source_status != MH3_RES_ABSENT)
        return CheckedStatus(source_status);

    for (const auto& p : Packs()) {
        mh3_source_owner certificate{};
        const auto status = mh3_source_owner_certify(&C().tracker, p.catalog, model_address, &certificate);

        if (status == MH3_RES_ABSENT)
            continue;

        MH3_RETURN_IF_ERROR(CheckedStatus(status));
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

            MH3_RETURN_IF_ERROR(CheckedStatus(sibling_status));
            mh3_catalog_model model{};
            mh3_model_catalog_model(p.catalog, j, &model);
            const uint32_t address = sibling.model_address;

            // Provenance selected the owner; header fields validate layout,
            // rather than identifying bytes by a magic/name/content match.
            uint32_t word = 0;
            MH3_RETURN_IF_ERROR(ReadGuest32(address, word));
            bool differs = word != 0x4d444c30;

            if (!differs) {
                MH3_RETURN_IF_ERROR(ReadGuest32(address + 4, word));
                differs = word != model.model_bytes;
            }

            if (!differs) {
                MH3_RETURN_IF_ERROR(ReadGuest32(address + 8, word));
                differs = word != model.revision;
            }

            if (!differs) {
                uint32_t parent = 0;
                MH3_RETURN_IF_ERROR(ReadGuest32(address + 12, word));
                MH3_RETURN_IF_ERROR(CanonicalAddress(address + word, 1, parent));
                differs = parent != container;
            }

            if (differs)
                return Status::Error("Loaded native MDL0 header differs from registered source identity");

            NativeModelOwner related{p.pack, p.catalog, j, container, address, p.handle, p.generation, generation};
            siblings.push_back({related, sibling, nullptr});
        }

        owner = {p.pack, p.catalog, index, container, model_address, p.handle, p.generation, generation};
        Certify(siblings);
        return {};
    }

    return Status::Error("Model has no complete native loader provenance or external converted pack");
}

NativeModelOwner ResolveNativeModelOwner(uint32_t model_address) {
    // A certified owner (most draws) resolves without a Status.
    Initialize();
    const auto cached = C().owners.find(Canonical(model_address, 16));

    if (cached != C().owners.end())
        return cached->second.owner;

    NativeModelOwner owner{};
    CheckStatus(TryResolveNativeModelOwner(model_address, owner));
    return owner;
}

Status TryResolveNativeModelResource(const NativeModelOwner& owner, uint32_t resource, bool material, mh3_catalog_resource& result) {
    NativeModelOwner current{};
    MH3_RETURN_IF_ERROR(TryResolveNativeModelOwner(owner.model_address, current));

    if (current.pack != owner.pack || current.catalog != owner.catalog || current.resident_handle != owner.resident_handle || current.resident_generation != owner.resident_generation ||
        current.source_generation != owner.source_generation || current.catalog_model_index != owner.catalog_model_index)
        return Status::Error("Native model owner was invalidated");
    MH3_RETURN_IF_ERROR(CanonicalAddress(resource, material ? 16 : 64, resource));
    const uint64_t key = (uint64_t(resource) << 1) | uint32_t(material);
    const auto cached = C().resources.find(key);

    if (cached != C().resources.end() && cached->second.epoch == C().source_epoch && cached->second.generation == owner.source_generation) {
        if (cached->second.resource.model_index != owner.catalog_model_index)
            return Status::Error("Native model resource cache has a different owner");

        result = cached->second.resource;
        return {};
    }

    if (resource < owner.container_address)
        return Status::Error("Native model resource precedes its container");

    uint32_t id = 0, word = 0;
    MH3_RETURN_IF_ERROR(ReadGuest32(resource + (material ? 12 : 60), id));
    mh3_catalog_resource r{};
    bool differs = mh3_model_catalog_find_resource(owner.catalog, owner.catalog_model_index, material, id, &r) != MH3_PACK_OK || resource - owner.container_address != r.source_offset;

    if (!differs) {
        MH3_RETURN_IF_ERROR(ReadGuest32(resource, word));
        differs = word != r.source_bytes;
    }

    if (!differs) {
        uint32_t parent = 0;
        MH3_RETURN_IF_ERROR(ReadGuest32(resource + 4, word));
        MH3_RETURN_IF_ERROR(CanonicalAddress(resource + word, 1, parent));
        differs = parent != owner.model_address;
    }

    if (differs)
        return Status::Error("Native model subresource identity differs from registered source");

    C().resources.insert_or_assign(key, CachedResource{C().source_epoch, owner.source_generation, r});
    result = r;
    return {};
}

namespace {
// The cached resource of a still-current owner, as TryResolveNativeModelResource
// finds it; false when that has to resolve (or report) it.
bool CachedNativeModelResource(const NativeModelOwner& owner, uint32_t resource, bool material, mh3_catalog_resource& result) {
    Initialize();
    uint32_t model = 0;

    if (mh3_source_canonical_address(owner.model_address, 16, &model) != MH3_RES_OK || mh3_source_canonical_address(resource, material ? 16 : 64, &resource) != MH3_RES_OK)
        return false;

    const auto found = C().owners.find(model);

    if (found == C().owners.end())
        return false;

    const auto& current = found->second.owner;

    if (current.pack != owner.pack || current.catalog != owner.catalog || current.resident_handle != owner.resident_handle || current.resident_generation != owner.resident_generation ||
        current.source_generation != owner.source_generation || current.catalog_model_index != owner.catalog_model_index)
        return false;

    const auto cached = C().resources.find((uint64_t(resource) << 1) | uint32_t(material));

    if (cached == C().resources.end() || cached->second.epoch != C().source_epoch || cached->second.generation != owner.source_generation ||
        cached->second.resource.model_index != owner.catalog_model_index)
        return false;

    result = cached->second.resource;
    return true;
}
} // namespace

mh3_catalog_resource ResolveNativeModelResource(const NativeModelOwner& owner, uint32_t resource, bool material) {
    mh3_catalog_resource result{};

    if (!CachedNativeModelResource(owner, resource, material, result))
        CheckStatus(TryResolveNativeModelResource(owner, resource, material, result));

    return result;
}

std::shared_ptr<NativeModelDrawTable> FindNativeModelDrawTable(uint32_t model_address) {
    const auto cached = C().owners.find(Canonical(model_address, 16));
    return cached != C().owners.end() ? cached->second.table : nullptr;
}

Status AttachNativeModelDrawTable(const NativeModelOwner& owner, std::shared_ptr<NativeModelDrawTable> table) {
    const auto cached = C().owners.find(owner.model_address);

    if (cached == C().owners.end() || !SameOwner(cached->second.owner, owner))
        return Status::Error("Native model draw table has no certified owner");

    cached->second.table = std::move(table);
    return {};
}

size_t CertifyLoadedNativeModels() {
    auto& c = Initialize();

    // Nothing was loaded, copied or retired since the last pass.
    if (c.tracker.generation == c.discovered_generation)
        return 0;

    c.discovered_generation = c.tracker.generation;

    // The containers of complete model files: each range holding a file start.
    struct Candidate {
        uint32_t container;
        uint64_t file;
    };

    std::vector<Candidate> candidates;

    for (size_t i = 0; i < c.tracker.count; i++) {
        const auto& range = c.tracker.ranges[i];
        uint64_t file = 0;
        uint32_t bytes = 0;

        if (!FindNativeModelSource(range.partition_offset, &file, &bytes) || file < range.partition_offset || file - range.partition_offset >= range.bytes)
            continue;

        candidates.push_back({range.address + uint32_t(file - range.partition_offset), file});
    }

    size_t certified = 0;
    std::vector<uint32_t> models;

    // A file that fails is left uncertified; its models resolve (and report
    // their failure) at their first draw.
    const auto certify = [&](const Candidate& candidate) {
        MH3_RETURN_IF_ERROR(TryEnsureNativeModelAssets(candidate.file, 16));
        models.clear();

        for (const auto& p : Packs())
            for (uint32_t j = 0; j < p.catalog->model_count; j++) {
                mh3_catalog_model model{};
                mh3_model_catalog_model(p.catalog, j, &model);

                if (model.partition_offset != candidate.file || uint64_t(candidate.container) + model.model_offset > UINT32_MAX)
                    continue;

                const uint32_t address = candidate.container + model.model_offset;
                mh3_source_owner certificate{};

                if (!C().owners.contains(address) && mh3_source_owner_certify(&c.tracker, p.catalog, address, &certificate) == MH3_RES_OK)
                    models.push_back(address);
            }
        // Certification may admit packs: resolve outside the pack walk.
        for (const uint32_t address : models)
            if (!C().owners.contains(address)) {
                NativeModelOwner owner{};
                MH3_RETURN_IF_ERROR(TryResolveNativeModelOwner(address, owner));
                ++certified;
            }
        return Status{};
    };

    for (const auto& candidate : candidates) {
        const Status status = certify(candidate);
#ifdef MH3_DEVELOPMENT
        if (!status.ok())
            std::fprintf(stderr, "MH3 native catalog: loaded model file %llx at %08x not certified: %s\n", static_cast<unsigned long long>(candidate.file), candidate.container, status.message().c_str());
#else
        (void)status;
#endif
    }

    return certified;
}

size_t CountNativeModelOwners() {
    return C().owners.size();
}

void ListNativeModelOwnersWithoutDrawTable(std::vector<NativeModelOwner>& out) {
    out.clear();

    for (const auto& item : C().owners)
        if (!item.second.table)
            out.push_back(item.second.owner);
}

Status ReadNativePreparedMesh(const NativeModelOwner& owner, uint32_t shape, const mh3_prepared_mesh*& mesh) {
    mh3_catalog_resource resource{};
    MH3_RETURN_IF_ERROR(TryResolveNativeModelResource(owner, shape, false, resource));

    if (resource.pack_index == UINT32_MAX)
        return Status::Error("Native model shape has no converted mesh");

    for (const auto& pack : Packs())
        if (pack.handle == owner.resident_handle && pack.generation == owner.resident_generation) {
            MH3_RETURN_IF_ERROR(RangeCheck(resource.pack_index, pack.meshes.size()));
            mesh = &pack.meshes[resource.pack_index];
            return {};
        }
    return Status::Error("Native model resident allocation was retired");
}

const mh3_prepared_mesh* ReadNativePreparedMeshAt(const NativeModelOwner& owner, uint32_t pack_index) {
    for (const auto& pack : Packs())
        if (pack.handle == owner.resident_handle && pack.generation == owner.resident_generation)
            return pack_index < pack.meshes.size() ? &pack.meshes[pack_index] : nullptr;
    return nullptr;
}

const NativePreparedMaterial& ReadNativePreparedMaterial(const NativeModelOwner& owner, uint32_t material) {
    const mh3_catalog_resource resource = ResolveNativeModelResource(owner, material, true);

    for (const auto& pack : Packs())
        if (pack.handle == owner.resident_handle && pack.generation == owner.resident_generation)
            return pack.materials.at(resource.pack_index);
    MH3_THROW(std::runtime_error("Native model material resident allocation was retired"));
}

NativeMaterialBlockDefaults ReadNativeMaterialBlockDefaults(const NativeModelOwner& owner, uint32_t material, mh3_model_material_block kind, uint32_t actual_block) {
    const mh3_catalog_resource resource = ResolveNativeModelResource(owner, material, true);

    if (kind < 0 || kind >= MH3_MODEL_MATERIAL_BLOCK_COUNT)
        MH3_THROW(std::runtime_error("Invalid native material block kind"));

    for (const auto& pack : Packs())
        if (pack.handle == owner.resident_handle && pack.generation == owner.resident_generation) {
            const auto& block = pack.material_blocks.at(resource.pack_index)[kind];

            if (!(block.flags & 1))
                MH3_THROW(std::runtime_error("Native material block requires verified M3ID v2 ownership"));

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
    MH3_THROW(std::runtime_error("Native material block resident owner was retired"));
}

uint64_t NextNativeMaterialRevision() {
    if (C().material_revision == UINT64_MAX)
        MH3_THROW(std::runtime_error("Native material producer revision overflow"));

    return ++C().material_revision;
}

const NativeLiveMaterialBlock& ReadNativeLiveMaterialBlockView(uint32_t address, mh3_model_material_block kind) {
    Initialize();
    address = Canonical(address, 1);
    const auto key = LiveKey(address, kind);
    const auto cached = C().live_index.find(key);

    if (cached != C().live_index.end())
        return *cached->second;

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

        NativeLiveMaterialBlock block{pack.materials.at(source.material_index).state, address, source.bytes, kind, pack.handle, pack.generation, identity.partition_offset, generation, {}};
        block.fields.generation = NextNativeMaterialRevision();
        return PutLiveBlock(key, std::move(block));
    }

    std::ostringstream detail;
    detail << "Material block has no admitted typed source extent: address=0x" << std::hex << address << " kind=" << std::dec << uint32_t(kind) << " partition_offset=0x" << std::hex
           << identity.partition_offset;
    MH3_THROW(std::runtime_error(detail.str()));
}

NativeLiveMaterialBlock ReadNativeLiveMaterialBlock(uint32_t address, mh3_model_material_block kind) {
    return ReadNativeLiveMaterialBlockView(address, kind);
}

NativeLiveMaterialBlock* WritableNativeLiveMaterialBlock(uint32_t address, mh3_model_material_block kind) {
    auto& c = Initialize();
    const auto found = c.live_index.find(LiveKey(Canonical(address, 1), kind));

    if (found == c.live_index.end())
        return nullptr;

    auto* block = found->second;

    // A disc write or copy over the bytes would have erased the node; the
    // tracker check keeps the in-place path to untracked bytes regardless.
    if (!block->writable || mh3_source_intersects(&c.tracker, block->address, block->bytes))
        return nullptr;

    return block;
}

void MarkNativeLiveMaterialBlockWritable(uint32_t address, mh3_model_material_block kind) {
    auto& c = Initialize();
    const auto found = c.live_index.find(LiveKey(Canonical(address, 1), kind));

    if (found == c.live_index.end())
        MH3_THROW(std::runtime_error("Writable native material block is not live"));

    found->second->writable = true;
}

void PublishNativeLiveMaterialBlock(const NativeLiveMaterialBlock& block) {
    if (!block.fields.generation || block.fields.generation > C().material_revision || Canonical(block.address, block.bytes) != block.address)
        MH3_THROW(std::runtime_error("Invalid typed native material publication"));

    for (const auto& pack : Packs())
        if (pack.handle == block.resident_handle && pack.generation == block.resident_generation) {
            const auto origin = pack.origins.find(std::make_pair(block.origin_offset, uint32_t(block.kind)));

            if (origin == pack.origins.end() || origin->second.bytes != block.bytes)
                MH3_THROW(std::runtime_error("Native material publication changed source layout identity"));

            PutLiveBlock(LiveKey(block.address, block.kind), block);
            ++live_version;
            return;
        }
    MH3_THROW(std::runtime_error("Native material publication refers to a retired resident allocation"));
}

void ReadNativeModelCoordinateMap(uint32_t pipeline, uint8_t mapping[8], uint8_t* known) {
    if (!mapping || !known)
        MH3_THROW(std::runtime_error("Native coordinate mapping requires outputs"));

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
    MH3_THROW(std::runtime_error("Native coordinate mapping owner was retired"));
}

const NativeFrozenMaterialBlock* FreezeNativeLiveMaterialBlock(const NativeLiveMaterialBlock& block) {
    if (const auto* current = block.frozen.get(); current && current->fields.generation == block.fields.generation)
        return current;
    // Items of unrendered packets may still point at the replaced copy: the
    // render thread deletes it after the current packet (model_render_prep.cpp).
    auto frozen = std::make_unique<NativeFrozenMaterialBlock>();
    frozen->fields = block.fields;
    std::memset(frozen->texcoord_map, 0, sizeof frozen->texcoord_map);
    frozen->texcoord_known = 0;

    if (block.kind == MH3_MODEL_MATERIAL_PIPELINE) {
        bool found = false;

        for (const auto& pack : Packs())
            if (pack.handle == block.resident_handle && pack.generation == block.resident_generation) {
                const auto origin = pack.origins.find(std::make_pair(block.origin_offset, uint32_t(MH3_MODEL_MATERIAL_PIPELINE)));

                if (origin == pack.origins.end())
                    break;

                const auto& prepared = pack.materials.at(origin->second.material_index);
                std::memcpy(frozen->texcoord_map, prepared.texcoord_map, 8);
                frozen->texcoord_known = prepared.texcoord_map_known;
                found = true;
                break;
            }
        if (!found)
            MH3_THROW(std::runtime_error("Native coordinate mapping owner was retired"));
    }

    block.frozen.Reset(frozen.release());
    return block.frozen.get();
}

namespace {
// The fields a typed block's group publication reads are equal.
bool SameGroup(const mh3_native_material_state& a, const mh3_native_material_state& b, mh3_model_material_block kind) {
    switch (kind) {
    case MH3_MODEL_MATERIAL_PIXEL:
        return !std::memcmp(&a.pixel, &b.pixel, sizeof a.pixel);
    case MH3_MODEL_MATERIAL_COLORS:
        return !std::memcmp(a.color, b.color, sizeof a.color) && !std::memcmp(a.konst, b.konst, sizeof a.konst);
    case MH3_MODEL_MATERIAL_INDIRECT:
        return !std::memcmp(a.indirect_matrix, b.indirect_matrix, sizeof a.indirect_matrix) && !std::memcmp(a.indirect, b.indirect, sizeof a.indirect);
    case MH3_MODEL_MATERIAL_TEXGEN:
        return a.texgen_count == b.texgen_count && !std::memcmp(a.texgen, b.texgen, sizeof a.texgen);
    default:
        return false;
    }
}

// Word compare (memcmp is a byte loop on the Vita).
bool SameWords(const void* a, const void* b, size_t bytes) {
    const auto* x = static_cast<const uint32_t*>(a);
    const auto* y = static_cast<const uint32_t*>(b);

    for (size_t i = 0; i < bytes / 4; i++)
        if (x[i] != y[i])
            return false;
    return true;
}

static_assert(sizeof(mh3_pack_pixel_inputs) % 4 == 0 && sizeof(mh3_pack_tev_color[4]) % 4 == 0);
} // namespace

const NativeMaterialGroupVersion* VersionNativeLiveMaterialGroup(const NativeLiveMaterialBlock& block) {
    auto& copy = block.group;
    const auto& fields = block.fields;
    const auto* current = copy.get();

    if (current && copy.generation == fields.generation)
        return current;
    // Setters stamp a new generation even when they store the same values.
    const bool pixel = block.kind == MH3_MODEL_MATERIAL_PIXEL;

    if (current && (pixel ? SameWords(&current->pixel, &fields.pixel, sizeof fields.pixel)
                          : SameWords(current->color, fields.color, sizeof fields.color) && SameWords(current->konst, fields.konst, sizeof fields.konst))) {
        copy.generation = fields.generation;
        return current;
    }

    auto* version = new NativeMaterialGroupVersion;

    if (pixel)
        std::memcpy(&version->pixel, &fields.pixel, sizeof fields.pixel);
    else {
        std::memcpy(version->color, fields.color, sizeof fields.color);
        std::memcpy(version->konst, fields.konst, sizeof fields.konst);
    }

    copy.Reset(version);
    copy.generation = fields.generation;
    return version;
}

void CopyNativeLiveMaterialBlock(uint32_t destination, uint32_t source, mh3_model_material_block kind) {
    const auto& from = ReadNativeLiveMaterialBlockView(source, kind);
    const uint32_t bytes = from.bytes;
    destination = Canonical(destination, bytes);
    source = Canonical(source, bytes);
    void* output = GuestRange(destination, bytes);
    const void* input = GuestRange(source, bytes);

    if (destination == source)
        return;
    // Instance materials re-copy their groups every frame (8007E4FC). Onto a
    // writable block of the same size the copy replaces its fields in place;
    // the destination keeps no byte provenance, as after a typed write. An
    // unchanged group keeps its generation (frozen copies stay current).
    if (auto* to = WritableNativeLiveMaterialBlock(destination, kind); to && to->bytes == bytes && to->address == destination &&
        (uint64_t(destination) + bytes <= source || uint64_t(source) + bytes <= destination)) {
        mh3_model_copy_bytes(output, input, bytes);
        const uint64_t generation = SameGroup(to->fields, from.fields, kind) ? to->fields.generation : NextNativeMaterialRevision();
        to->fields = from.fields;
        to->fields.generation = generation;
        to->resident_handle = from.resident_handle;
        to->resident_generation = from.resident_generation;
        to->origin_offset = from.origin_offset;
        to->source_generation = from.source_generation;
        return;
    }

    auto block = from;
    const uint64_t revision = NextNativeMaterialRevision();
    RecordNativeTypedMaterialCopy(destination, source, bytes, kind);
    mh3_model_copy_bytes(output, input, bytes);
    block.address = destination;
    block.fields.generation = revision;
    PublishNativeLiveMaterialBlock(block);
    MarkNativeLiveMaterialBlockWritable(destination, kind);
}

extern "C" void MH3_ModelMemcpy(CpuContext* cpu) {
    cpu->gpr[3] = Copy(cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], NativeSourceEpochProducer::Memcpy);
}
REGISTER_NATIVE_FUNCTION_AS(0x80004000, MH3_ModelMemcpy, "NativeResourceMemcpy");
extern "C" void MH3_ModelMemmove(CpuContext* cpu) {
    cpu->gpr[3] = Copy(cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], NativeSourceEpochProducer::Memmove);
}
REGISTER_NATIVE_FUNCTION_AS(0x8045BC18, MH3_ModelMemmove, "NativeResourceMemmove");
