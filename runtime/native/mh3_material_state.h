#ifndef MH3_MATERIAL_STATE_H
#define MH3_MATERIAL_STATE_H
#include "mh3_model_state.h"
#include "mh3_draw_state.h"
#ifdef __cplusplus
extern "C" {
#endif

enum {
    MH3_MATERIAL_MAX_TEV=16,MH3_MATERIAL_MAX_TEXGEN=8,MH3_MATERIAL_MAX_CHANNEL=2,
    MH3_MATERIAL_MAX_INDIRECT=4,MH3_MATERIAL_MAX_MATRIX=3,
    MH3_MATERIAL_TOP_TEXGEN_COUNT=1u<<0,MH3_MATERIAL_TOP_CHANNEL_COUNT=1u<<1,
    MH3_MATERIAL_TOP_TEV_COUNT=1u<<2,MH3_MATERIAL_TOP_INDIRECT_COUNT=1u<<3,
    MH3_MATERIAL_TOP_CULL=1u<<4,MH3_MATERIAL_TOP_LIGHT_REFERENCE=1u<<5,
    MH3_MATERIAL_TOP_FOG_REFERENCE=1u<<6,MH3_MATERIAL_TOP_GENERATION=1u<<7,
    MH3_MATERIAL_TOP_PIXEL_CLASSIFICATION=1u<<8,MH3_MATERIAL_TOP_ALL=(1u<<9)-1
};
#define MH3_MATERIAL_TEV_MASK ((UINT64_C(1)<<MH3_TEV_FIELD_COUNT)-1)
#define MH3_MATERIAL_PIXEL_MASK ((UINT64_C(1)<<MH3_PIXEL_FIELD_COUNT)-1)
#define MH3_MATERIAL_TEXGEN_MASK ((1u<<MH3_TEXGEN_FIELD_COUNT)-1)
#define MH3_MATERIAL_PIXEL_RASTER_MASK ( \
    (UINT64_C(1)<<MH3_PIXEL_DEPTH_TEST)|(UINT64_C(1)<<MH3_PIXEL_DEPTH_COMPARE)| \
    (UINT64_C(1)<<MH3_PIXEL_DEPTH_WRITE)|(UINT64_C(1)<<MH3_PIXEL_BLEND_ENABLE)| \
    (UINT64_C(1)<<MH3_PIXEL_LOGIC_ENABLE)|(UINT64_C(1)<<MH3_PIXEL_BLEND_SUBTRACT)| \
    (UINT64_C(1)<<MH3_PIXEL_BLEND_SOURCE)|(UINT64_C(1)<<MH3_PIXEL_BLEND_DESTINATION)| \
    (UINT64_C(1)<<MH3_PIXEL_LOGIC_OPERATION)|(UINT64_C(1)<<MH3_PIXEL_COLOR_WRITE)| \
    (UINT64_C(1)<<MH3_PIXEL_ALPHA_WRITE))
typedef enum mh3_material_group {
    MH3_MATERIAL_TOP,MH3_MATERIAL_TEV,MH3_MATERIAL_TEXGEN,MH3_MATERIAL_CHANNEL,
    MH3_MATERIAL_COLOR,MH3_MATERIAL_KONST,MH3_MATERIAL_SWAP,MH3_MATERIAL_INDIRECT,
    MH3_MATERIAL_INDIRECT_MATRIX,MH3_MATERIAL_PIXEL,MH3_MATERIAL_CLIP
} mh3_material_group;
typedef struct mh3_material_diagnostic {
    mh3_material_group group;
    uint32_t index;
    uint64_t fields;
} mh3_material_diagnostic;
enum {
    MH3_MATERIAL_SUPPORT_CLIP_DISABLED=1u<<0,
    MH3_MATERIAL_SUPPORT_DITHER=1u<<1,
    MH3_MATERIAL_SUPPORT_DESTINATION_ALPHA=1u<<2,
    MH3_MATERIAL_SUPPORT_DEPTH_TEXTURE=1u<<3,
    MH3_MATERIAL_SUPPORT_ALL=(1u<<4)-1
};
/* Explicit shader dependencies, not defaults. A zero mask means unconsumed.
 * Capability bits are promises from the selected backend, never inferred. */
typedef struct mh3_material_requirements {
    uint64_t tev[16],pixel;
    uint32_t texgen[8],channel[2],color[4],konst[4],swaps[4],indirect[4],indirect_matrix[3];
    uint32_t supported_features;
    uint8_t require_clip;
} mh3_material_requirements;
/* Unknown fields (including their bytes and padding) survive. All checks
 * finish before destination mutation. Clip is copied only when src knows it. */
mh3_res_status mh3_material_state_overlay(mh3_native_material_state *destination,
    const mh3_native_material_state *source,uint32_t top_known);
/* Null requirements checks known value domains only. ABSENT identifies missing
 * consumed fields; UNSUPPORTED identifies an unpromised backend capability. */
mh3_res_status mh3_material_state_validate(const mh3_native_material_state *state,
    const mh3_material_requirements *requirements,mh3_material_diagnostic *diagnostic);
/* Alpha comparisons and other non-raster fields stay shader/backend owned.
 * A partial blend-mode triple returns ABSENT without changing any raster byte. */
mh3_res_status mh3_material_pixel_raster(const mh3_pack_pixel_inputs *pixel,mh3_raster_state *raster);

/* Byte-identical float layouts consumed by the material shader serializer. */
typedef struct mh3_shader_light {
    float position[4],direction[4],color[4],angular[4],distance[4];
} mh3_shader_light;
typedef struct mh3_shader_fog {
    float color[4],a,b,c,b_shift,range_base[4],range_k[12];
} mh3_shader_fog;
mh3_res_status mh3_material_prepare_light(const mh3_light *source,mh3_shader_light *out);
/* Perspective writes A/B/C/shift; orthographic preserves out->b/b_shift,
 * exactly as the original producer does. Viewport width is logical pixels. */
mh3_res_status mh3_material_prepare_fog(const mh3_native_fog *source,float viewport_width,mh3_shader_fog *out);
#ifdef __cplusplus
}
#endif
#endif
