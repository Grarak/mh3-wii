#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
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
struct NativeModelAssetStats {
    size_t index_bytes = 0, metadata_bytes = 0, identity_bytes = 0, gpu_bytes = 0, demanded_gpu_bytes = 0;
    uint32_t admitted_packs = 0;
    uint64_t admissions = 0, evictions = 0, gpu_evictions = 0, gpu_busy = 0;
};
// GPU bytes include retired backing pending completion, with Vita rounding.
NativeModelAssetStats ReadNativeModelAssetStats();
// An admitted pack's file and its content key (metadata and identity sizes,
// FNV-1a of the identity bytes), for caches derived from the pack.
struct NativeModelPackFile {
    std::string path; // the M3MP file
    uint32_t metadata_bytes = 0, identity_bytes = 0, identity_hash = 0;
};
bool ReadNativeModelPackFile(uintptr_t resident_handle, uint64_t resident_generation, NativeModelPackFile* out);
