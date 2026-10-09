#ifndef MH3_MODEL_VIEW_H
#define MH3_MODEL_VIEW_H
#include "mh3_model_matrices.h"
#ifdef __cplusplus
extern "C" {
#endif

/* 80071CAC is CalcView_LC_DMA_ModelMtx in RMHP08 (the old MAP name was
 * G3dCalcView). These CPU spans contain serialized big-endian float records.
 * Cache pairs are explicit scratch ownership, each with capacity for 40 records.
 * Original 32-byte output padding is preserved, including unproduced cache tail
 * bytes. No allocations, CpuContext, PPC instructions, GX or GPU operations. */
typedef struct mh3_model_view_dma_input {
    mh3_res_model model;
    uint32_t count;
    mh3_matrix34 camera;
    mh3_res_span models, attributes;
    mh3_res_mut_span position, normal, environment, billboard_work;
    mh3_res_mut_span model_cache[2], position_cache[2], normal_cache[2], environment_cache[2];
} mh3_model_view_dma_input;

/* Billboard types 1..6 invoke the existing CPU math producer. The adapter may
 * supply native math directly. It receives the actual current cache record,
 * source matrix index and authored uniform-scale flag; no rendering callback.
 * before_store publishes destination retirement before any CPU output copy. */
typedef struct mh3_model_view_callbacks {
    void *user;
    mh3_res_status (*billboard)(void *user, uint32_t type, uint32_t matrix_index, int uniform_scale, mh3_res_mut_span position);
    mh3_res_status (*before_store)(void *user, uint32_t kind, uint32_t first_index, size_t bytes);
} mh3_model_view_callbacks;

mh3_res_status mh3_model_view_dma(const mh3_model_view_dma_input *input, const mh3_model_view_callbacks *callbacks);

/* Renderer-owned native float arrays for an already admitted static model.
 * Asset admission excludes billboard selectors and billboard-parent nodes.
 * The renderer owns every output until CPU consumption ends; GPU uniforms or
 * palettes must be copied/retained separately before that slot is returned.
 * Capacity and selector contracts are debug assertions in the hot function.
 * count==0 needs no arrays. environment is ignored when normal is absent. */
typedef struct mh3_model_view_native_input {
    uint32_t count;
    mh3_matrix34 camera;
    const mh3_matrix34 *models;
    const uint32_t *attributes;
    mh3_matrix34 *position;
    mh3_matrix33 *normal;
    mh3_matrix34 *environment;
} mh3_model_view_native_input;

void mh3_model_view_native_static(const mh3_model_view_native_input *input);

#ifdef __cplusplus
}
#endif
#endif
