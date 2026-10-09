#ifndef MH3_MODEL_SUBMISSION_H
#define MH3_MODEL_SUBMISSION_H
#include "mh3_model_catalog.h"
#include "mh3_model_matrices.h"
#include "mh3_model_state.h"
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
/* Resolve metadata and validate the rigid palette and attribute contract. This
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
