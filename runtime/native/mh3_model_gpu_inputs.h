#ifndef MH3_MODEL_GPU_INPUTS_H
#define MH3_MODEL_GPU_INPUTS_H
#include "mh3_model_submission.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Native shader recipe. Selectors are resolved here from explicit producers;
 * this is not a register bank or a decoder of resource command bytes. */
typedef struct {
    mh3_matrix34 main,post;
    uint8_t source_row,projection,input_ab11,normalize;
    uint8_t main_identity,post_identity;
} mh3_model_gpu_texgen;
typedef struct {
    float projection[16];
    mh3_matrix34 position,normal;
    mh3_model_gpu_texgen texgen[8];
    uint8_t texcoord_mask,nonzero_projective_mask;
} mh3_model_gpu_inputs;
/* Prepare only consumed inputs, transactionally. near_z/far_z are the logical
 * viewport values; projection follows mh3_draw_vertex's reversed-depth rule.
 * Bound-based Q certification is conservative; uncertified does not mean zero.
 * No immutable vertex/index bytes are inspected. */
mh3_res_status mh3_model_gpu_prepare(const mh3_resolved_mesh_submit *mesh,
    uint8_t texcoord_mask,int normal_required,float near_z,float far_z,
    mh3_model_gpu_inputs *out);
#ifdef __cplusplus
}
#endif
#endif
