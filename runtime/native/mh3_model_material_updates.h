#ifndef MH3_MODEL_MATERIAL_UPDATES_H
#define MH3_MODEL_MATERIAL_UPDATES_H
#include "mh3_model_state.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Verified RMHP08 CPU resource setters. These publish typed values and preserve
 * the original resource bytes for CPU compatibility only. The bytes are never
 * submitted/replayed to a GPU. A producer revision must strictly advance.
 * Errors leave both outputs unchanged, including short compatibility spans. */
mh3_res_status mh3_model_material_alpha_compare(mh3_native_material_state *state,
    mh3_res_mut_span pixel,uint32_t compare0,uint8_t reference0,uint32_t operation,
    uint32_t compare1,uint8_t reference1,uint64_t revision); /* 80095F94 */
mh3_res_status mh3_model_material_blend(mh3_native_material_state *state,
    mh3_res_mut_span pixel,uint32_t mode,uint32_t source,uint32_t destination,
    uint32_t logic,uint64_t revision); /* 80096100 */
mh3_res_status mh3_model_material_tev_color(mh3_native_material_state *state,
    mh3_res_mut_span colors,uint32_t id,const uint8_t rgba[4],uint64_t revision); /* 80096300 */
mh3_res_status mh3_model_material_konst_color(mh3_native_material_state *state,
    mh3_res_mut_span colors,uint32_t id,const uint8_t rgba[4],uint64_t revision); /* 80096548 */
mh3_res_status mh3_model_material_indirect_matrix(mh3_native_material_state *state,
    mh3_res_mut_span indirect,uint32_t id,const float matrix[12],int8_t exponent,
    uint64_t revision); /* 80096C10 */
mh3_res_status mh3_model_material_channel_color(mh3_native_material_state *state,
    mh3_res_mut_span channels,uint32_t id,int ambient,const uint8_t rgba[4],
    uint64_t revision); /* 80096DE0 / 80096E68 */
#ifdef __cplusplus
}
#endif
#endif
