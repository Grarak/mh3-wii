#pragma once
#include <cstdint>

/* Opt-in CPU metadata only. Each replica's drawing owner writes and reports
 * its own state. No renderer/game cross-thread read is permitted. */
enum class NativeSourceEpochReason : uint32_t {
    DiscWrite, ResourceCopy, ResourceRetire, PackInstall, PackUnregister, ReplicaSeed, Count
};
enum class NativeSourceEpochProducer : uint32_t {
    Disc, ResourceCopy, ResourceRetire, Memcpy, Memmove, Copy32, Copy32Unknown,
    Fill, HeapCreate, HeapDestroy, HeapAllocate, HeapFree, ViewDma, ViewLc,
    LockedCacheDma, PackManager, ReplicaSeed, Count
};
enum NativeSourceEpochCondition : uint32_t {
    NATIVE_EPOCH_TRACKED_SOURCE = 1u << 0,
    NATIVE_EPOCH_TRACKED_DESTINATION = 1u << 1,
    NATIVE_EPOCH_RETIRED_OWNER = 1u << 2,
    NATIVE_EPOCH_RETIRED_BLOCK = 1u << 3,
    NATIVE_EPOCH_COPIED_OWNER = 1u << 4,
    NATIVE_EPOCH_COPIED_BLOCK = 1u << 5
};
struct NativePrepareCacheCounters {
    uint64_t hits = 0, misses = 0, replacement_misses = 0;
    uint64_t token_clears = 0, capacity_clears = 0, discarded_entries = 0;
};
struct NativeSourceEpochEvent {
    uint64_t epoch = 0, partition_offset = 0;
    uint32_t frame = 0, destination = 0, source = 0, bytes = 0, conditions = 0;
    NativeSourceEpochReason reason = NativeSourceEpochReason::DiscWrite;
    NativeSourceEpochProducer producer = NativeSourceEpochProducer::Disc;
};
struct NativeModelPreparationDiagnostics {
    NativePrepareCacheCounters material, shape;
    uint64_t epoch_reasons[static_cast<unsigned>(NativeSourceEpochReason::Count)]{};
    uint64_t epoch_advances = 0, frames_with_epoch_advance = 0;
    uint32_t last_epoch_frame = UINT32_MAX;
    uint32_t sample_first = 0, sample_last = UINT32_MAX;
    uint64_t matching_events = 0;
    uint32_t stored_events = 0;
    NativeSourceEpochEvent events[64]{};
};
/* Resolves getenv once, after runtime settings are loaded. Disabled builds or
 * an absent setting return null. Cache owners retain this pointer, so a hit
 * performs no getenv, allocation, clock read or extra replica/TLS lookup. */
NativeModelPreparationDiagnostics* NativeModelPreparationDiagnosticsForOwner(unsigned replica);
/* Called only by the current Game owner at the existing statistics interval.
 * Reports counter deltas and at most 64 total sampled writer records. */
void ReportNativeModelPreparationDiagnostics();
