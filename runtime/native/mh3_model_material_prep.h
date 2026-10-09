#ifndef MH3_MODEL_MATERIAL_PREP_H
#define MH3_MODEL_MATERIAL_PREP_H
#include "mh3_material_state.h"
#ifdef __cplusplus
extern "C" {
#endif

/* Plain ResGenModeData/GenMode2 CPU records, not source GPU commands.
 * All failures leave outputs and compatibility bytes untouched. */
typedef struct mh3_model_genmode {
    uint8_t texgens, channels, tevs, indirect;
    uint32_t cull, flags;
} mh3_model_genmode;
enum {
    MH3_GENMODE_BP = 1,
    MH3_GENMODE_XF = 2,
    MH3_GENMODE_SELECTED = 4
};
mh3_res_status mh3_model_genmode_read(mh3_res_span resource, mh3_model_genmode *out);
mh3_res_status mh3_model_genmode_select(mh3_res_mut_span cache, mh3_res_span resource); /* 80086244 */
/* BP changes TEV/indirect counts and cull. XF additionally publishes the
 * shader's texgen/channel counts. *top_known names the fields actually sent;
 * no change writes only *apply=0. Unknown cache bits survive. */
mh3_res_status mh3_model_genmode_publish(mh3_res_mut_span cache, int xf, mh3_native_material_state *out, uint32_t *top_known, int *apply); /* 80086810/80086FF8 */
/* The same publication as typed counts/cull instead of a whole-state patch. */
mh3_res_status mh3_model_genmode_publish_counts(mh3_res_mut_span cache, int xf, mh3_model_genmode *out, uint32_t *top_known, int *apply);

typedef struct mh3_model_material_misc {
    uint8_t depth_before_texture, indirect_method[3];
    int8_t light_reference, fog_reference, normal_map_light[3];
} mh3_model_material_misc;
mh3_res_status mh3_model_material_misc_read(mh3_res_span resource, mh3_model_material_misc *out);

/* Complete XF channel-register producers. Color writes replace RGBA even
 * when the caller's GXChannelID names color-only or alpha-only: MH3's actual
 * 80077470/800870C8 leaf selects (id&1) and writes the whole32-bit register.
 * The source resource flags decide whether a write happens, not its mask. */
mh3_res_status mh3_model_channel_color(mh3_native_material_state *patch, uint32_t selector, const uint8_t rgba[4], int ambient);                              /* 80077470/800870C8 */
mh3_res_status mh3_model_channel_control(mh3_native_material_state *patch, uint32_t selector, uint32_t resource_control, uint32_t light_mask, int force_off); /* 80087068/80087098 */
/* Channels consist of two20-byte CPU records. Ambient multiplication applies
 * to channel0 only; channel1 uses the authored value. Non-two-channel modes
 * explicitly disable channel1 controls while preserving its colors. */
mh3_res_status mh3_model_channels_prepare(mh3_res_span resource, uint32_t channel_count, const uint32_t light_masks[4], const uint8_t ambient[4], int force_off,
                                          mh3_native_material_state *out); /* 80086B90 */
/* The same channel patch without the surrounding whole-state record. */
mh3_res_status mh3_model_channels_prepare_channels(mh3_res_span resource, uint32_t channel_count, const uint32_t light_masks[4], const uint8_t ambient[4], int force_off,
                                                   mh3_pack_channel out[2]);

#ifdef __cplusplus
}
#endif
#endif
