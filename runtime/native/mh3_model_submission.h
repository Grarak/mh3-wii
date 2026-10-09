#ifndef MH3_MODEL_SUBMISSION_H
#define MH3_MODEL_SUBMISSION_H
#include "mh3_model_catalog.h"
#include "mh3_model_matrices.h"
#include "mh3_model_state.h"
#include "mh3_texture_matrices.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Direct native model boundary, not a GPU uniform ABI. Borrowed CPU metadata,
 * resident GPU ranges and typed live inputs must survive the submit call. The
 * backend retains GPU allocations through its existing asynchronous fences and
 * copies small pose/material uniforms before returning. It never expands or
 * reads the immutable vertex/index payload on the CPU during a normal draw. */
typedef struct mh3_resolved_mesh_submit {
    const mh3_mesh_pack *pack;
    uintptr_t resident_handle;
    uint64_t resident_generation, source_generation, pose_generation;
    uint32_t model_id, shape_id, asset_id, mesh_index;
    mh3_pack_mesh mesh;
    mh3_pack_layout layout;
    const mh3_model_palette *palette;
    const mh3_native_material_state *material;
    /* Wii convention row-major projection. position[0] is already posed view
     * space; projection/clip conversion is applied once by the native backend. */
    const float *projection; /* 16 floats */
    const float (*texture_matrices)[12]; /* 8 prepared post-texture matrices */
    uint8_t texture_matrix_known, texture_matrix_3x4;
    uint32_t first_index, index_count;
} mh3_resolved_mesh_submit;
/* Admit once per resident generation. No borrowed live material/pose/camera
 * pointers are cached. Callers key this immutable record by resident handle,
 * generation and catalog model/shape ID, and retire it with the allocation. */
typedef struct mh3_prepared_mesh {
    mh3_resolved_mesh_submit immutable;
    uint8_t normal_required;
} mh3_prepared_mesh;
typedef struct mh3_prepared_material {
    mh3_native_material_state state;
    mh3_native_texture_srt srt;
    uint32_t binding_count;
    mh3_pack_binding bindings[8];
} mh3_prepared_material;
/* Immutable defaults/known masks, SRT recipes and texture binding IDs are
 * assembled once. Strings borrow pack metadata, never a live source object. */
mh3_pack_status mh3_model_material_prepare(const mh3_mesh_pack *pack,uint32_t material_index,
                                          mh3_prepared_material *out);
mh3_pack_status mh3_model_mesh_prepare(const mh3_model_catalog *catalog,
                                      const mh3_mesh_pack *pack,uint32_t catalog_model_index,
                                      uint32_t shape_id,uintptr_t resident_handle,
                                      uint64_t resident_generation,mh3_prepared_mesh *out);
/* Constant-time draw binding: no binary lookup or immutable metadata decode.
 * source_generation must come from the current verified owner; live resource
 * compatibility/material checks remain explicit producers/backend gates. */
mh3_pack_status mh3_model_mesh_bind_pose(const mh3_prepared_mesh *prepared,
                                        uint64_t resident_generation,uint64_t source_generation,
                                        uint64_t pose_generation,const mh3_model_palette *palette,
                                        mh3_resolved_mesh_submit *out);
/* Convenience tooling/test API. Runtime admission should use prepare followed
 * by bind_pose, rather than decoding immutable metadata on every draw.
 * Resolve metadata and validate the rigid palette and attribute contract. This
 * does not read GPU bytes, submit a draw, infer absent vertex defaults, validate
 * material dependencies or silently change alpha/blend/vegetation ordering.
 * resident_handle/generations and live shader inputs are explicit producers.
 * The caller fills projection/material/post-matrix pointers after this function
 * and must satisfy backend material validation before admission/submission. */
mh3_pack_status mh3_model_mesh_resolve(const mh3_model_catalog *catalog,
                                      const mh3_mesh_pack *pack,uint32_t catalog_model_index,
                                      uint32_t shape_id,uintptr_t resident_handle,
                                      uint64_t resident_generation,uint64_t source_generation,
                                      uint64_t pose_generation,const mh3_model_palette *palette,
                                      mh3_resolved_mesh_submit *out);
#ifdef __cplusplus
}
#endif
#endif
