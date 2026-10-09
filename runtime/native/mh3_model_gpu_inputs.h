#ifndef MH3_MODEL_GPU_INPUTS_H
#define MH3_MODEL_GPU_INPUTS_H
#include "mh3_model_submission.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Native shader recipe. Selectors are resolved here from explicit producers;
 * this is not a register bank or a decoder of resource command bytes. */
typedef struct {
    mh3_matrix34 main, post;
    uint8_t source_row, projection, input_ab11, normalize;
    uint8_t main_identity, post_identity, main_slot, indexed;
} mh3_model_gpu_texgen;
typedef struct {
    float projection[16];
    mh3_matrix34 position, normal;
    // Canonical palette slots. Normal matrices use the shader's padded 3x4
    // layout; only required slots and consumed inherited selectors are copied.
    mh3_matrix34 position_palette[10], normal_palette[10], texture_palette[10];
    mh3_model_gpu_texgen texgen[8];
    uint8_t texcoord_mask, nonzero_projective_mask, zero_projective_mask;
    uint16_t matrix_index_mask;
} mh3_model_gpu_inputs;
/* Draw-time metadata without a second copy of the 1440-byte palette. */
typedef struct {
    float projection[16];
    mh3_matrix34 position, normal;
    mh3_model_gpu_texgen texgen[8];
    uint8_t texcoord_mask, nonzero_projective_mask, zero_projective_mask;
    uint16_t matrix_index_mask;
} mh3_model_gpu_draw_inputs;
/* Prepare only consumed inputs, transactionally. near_z/far_z are the logical
 * viewport values; projection follows mh3_draw_vertex's reversed-depth rule.
 * Bound-based Q certification is conservative; uncertified does not mean zero.
 * Exact zero additionally requires a direct zero matrix row and a known-finite
 * position/color source; composed cancellation and unknown sources stay exact.
 * No immutable vertex/index bytes are inspected. */
mh3_res_status mh3_model_gpu_prepare(const mh3_resolved_mesh_submit *mesh, uint8_t texcoord_mask, int normal_required, float near_z, float far_z, mh3_model_gpu_inputs *out);
/* Prepare directly into final draw metadata and three 10-slot palette arrays.
 * The caller owns writable arrays in its admitted GPU frame allocation.
 * Failure can leave outputs partially written; consume them only on success.
 * Palette storage, coefficients, masks and main/post selectors are trusted
 * producer inputs, without admission checks. Conservative projective-Q rules
 * match the transactional API above. No live guest pose pointer is retained. */
mh3_res_status mh3_model_gpu_prepare_draw(const mh3_resolved_mesh_submit *mesh, uint8_t texcoord_mask, int normal_required, float near_z, float far_z, mh3_model_gpu_draw_inputs *out,
                                        mh3_matrix34 *positions, mh3_matrix34 *normals, mh3_matrix34 *textures);
/* Whether the admitted coordinate recipe can use the fixed Identity/Affine
 * vertex form. An explicit identity post matrix also qualifies when the main
 * transform is identity; dynamic/indexed/projective/normalized forms do not. */
int mh3_model_gpu_texgen_is_plain(const mh3_model_gpu_texgen *gen);
#ifdef __cplusplus
}
#endif
#endif
