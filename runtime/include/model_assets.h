#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include "status.h"
// Graphics-owner lifetime. Metadata and identities remain owned until retirement;
// the backend reads the immutable GPU range directly into its resident storage.
// An explicit MH3_NATIVE_MODEL_PACK must exist. Without an override the optional
// disc-root/native-models/models.index.bin supplies demand-loaded source packs;
// models.bin remains the legacy startup pack when no index exists.
void LoadNativeModelAssets(const char* disc_root);
void UnloadNativeModelAssets();

// Source provenance selects a single indexed pack. Missing source extents and
// invalid files fail explicitly. Metadata/catalog admission does not allocate
// GPU storage and can therefore supply typed CPU producers on skipped frames.
void EnsureNativeModelAssets(uint64_t partition_offset, uint32_t bytes);
Status TryEnsureNativeModelAssets(uint64_t partition_offset, uint32_t bytes);

// The source file extent at partition_offset whose models have a demand-loaded
// pack (converted or not yet): false for other files and for the startup pack.
// Neither admits nor waits.
bool FindNativeModelSource(uint64_t partition_offset, uint64_t* file_offset, uint32_t* file_bytes);

// Observe each model draw before the frame rendering gate, including skipped
// frames. Distinct rounded payload costs establish the full working set and
// pin this frame's demanded GPU storage. A working set over budget is an error.
void ObserveNativeModelAssetDemand(uintptr_t resident_handle, uint64_t resident_generation);

// Called at an admitted draw, after CPU state updates. False means the bounded
// GPU model budget is still occupied; no wait or overwrite is performed.
bool EnsureNativeModelAssetGPU(uintptr_t resident_handle, uint64_t resident_generation);

// Game thread, each frame: queues the render tasks that make the GPU data the
// read-ahead worker read for newly admitted packs resident.
void PumpNativeModelGPUReads();
struct NativeModelAssetStats {
    size_t index_bytes = 0, metadata_bytes = 0, identity_bytes = 0, gpu_bytes = 0, demanded_gpu_bytes = 0;
    uint32_t admitted_packs = 0;
    uint64_t admissions = 0, evictions = 0, gpu_evictions = 0, gpu_busy = 0;
    double open_ms = 0;  // reading and validating admitted packs, on the admitting thread
    double cache_ms = 0; // the asset cache's answer for a source's pack (indexing, its models converted)
    double take_ms = 0;  // waiting for a pack the read-ahead worker is reading
    double ensure_ms = 0; // TryEnsureNativeModelAssets in all, for converted packs
    // Converted packs certification asked the read-ahead for: read already,
    // waited for while being read, read on the admitting thread.
    uint64_t read_ahead_ready = 0, read_ahead_waited = 0, read_ahead_own = 0;
};

// GPU bytes include retired backing pending completion, with Vita rounding.
NativeModelAssetStats ReadNativeModelAssetStats();

// Any thread: the packs admitted so far and the time spent getting them:
// opening them, waiting for the asset cache and for the read-ahead worker.
struct NativeModelPackOpenings {
    uint64_t admissions = 0;
    double open_ms = 0, cache_ms = 0, take_ms = 0, ensure_ms = 0;
    uint64_t ready = 0, waited = 0, own = 0; // read-ahead packs: read already, waited for, read here
};

NativeModelPackOpenings ReadNativeModelPackOpenings();

// An admitted pack's file and its content key (metadata and identity sizes,
// FNV-1a of the identity bytes), for caches derived from the pack.
struct NativeModelPackFile {
    std::string path; // the M3MP file
    uint32_t metadata_bytes = 0, identity_bytes = 0, identity_hash = 0;
};

bool ReadNativeModelPackFile(uintptr_t resident_handle, uint64_t resident_generation, NativeModelPackFile* out);
