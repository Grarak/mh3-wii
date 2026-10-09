#pragma once
#include "../mh3_model_submission.h"
namespace aurora::gl { class NativeMeshBuffer; struct MeshLayout; }
/* Graphics-owner APIs. The caller retains pack metadata and retires its model
 * catalog before unregistering this GPU payload. Registration binds the pack
 * directly to resident data and validates all immutable indices once. */
void RegisterNativeModelGPU(mh3_mesh_pack& pack,const char* path,uintptr_t handle,uint64_t generation);
void UnregisterNativeModelGPU(uintptr_t handle,uint64_t generation);
void ClearNativeModelGPU();
struct NativeModelGPUView {
    const aurora::gl::NativeMeshBuffer* buffer;
    const aurora::gl::MeshLayout* layout;
    const mh3_pack_mesh* mesh;
};
NativeModelGPUView ResolveNativeModelGPU(const mh3_resolved_mesh_submit& mesh);
