#pragma once
#include "../mh3_model_submission.h"
#include <memory>
namespace aurora::gl {
class NativeMeshBuffer;
struct MeshLayout;
} // namespace aurora::gl

/* Graphics-owner APIs. The caller retains pack metadata and retires its model
 * catalog before unregistering this GPU payload. Registration binds the pack
 * directly to resident data and validates all immutable indices once. */
void RegisterNativeModelGPU(mh3_mesh_pack& pack, const char* path, uintptr_t handle, uint64_t generation);
/* Registration in two parts: the payload read from the pack file into a new
 * buffer (the slow part, which needs nothing of the pack's owner), then bound
 * to the pack. A payload dropped unregistered is released like a resident's. */
struct NativeModelGPUPayloadRelease {
    void operator()(aurora::gl::NativeMeshBuffer* buffer) const;
};

using NativeModelGPUPayload = std::unique_ptr<aurora::gl::NativeMeshBuffer, NativeModelGPUPayloadRelease>;
NativeModelGPUPayload ReadNativeModelGPU(const char* path, uint64_t offset, uint32_t bytes);
void RegisterNativeModelGPU(mh3_mesh_pack& pack, const char* path, uintptr_t handle, uint64_t generation, NativeModelGPUPayload buffer);
void UnregisterNativeModelGPU(uintptr_t handle, uint64_t generation);
void ClearNativeModelGPU();
/* Physical model backing only, including retired storage until notifications
 * complete. Queries collect completed storage without waiting. The allocation
 * cost includes Vita's 256 KiB memblock rounding. */
size_t NativeModelGPUAllocationBytes(uint32_t payload_bytes);
size_t NativeModelGPUResidentBytes();

// Logical frame serial advances on skipped and canceled game frames too.
uint32_t NativeModelGPUFrameSerial();

// Advances whenever a payload is registered, unregistered or cleared.
uint64_t NativeModelGPURevision();
struct NativeModelGPUView {
    const aurora::gl::NativeMeshBuffer* buffer;
    const aurora::gl::MeshLayout* layout;
    const mh3_pack_mesh* mesh;
};

NativeModelGPUView ResolveNativeModelGPU(const mh3_resolved_mesh_submit& mesh);
