#ifndef MH3_MODEL_SCOPE_H
#define MH3_MODEL_SCOPE_H
#include "mh3_model_draw.h"
#include "mh3_model_runtime.h"
#include "mh3_model_submission.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct mh3_model_material_scope {
    uint32_t material_address, fur_address;
    uint16_t replacement_mask;
    mh3_model_material_bindings selected;
} mh3_model_material_scope;
mh3_res_status mh3_model_material_bindings_read(mh3_res_span swaps, mh3_model_material_bindings *out);
/* ShpState's12-byte CPU descriptor comparison record. This compatibility
 * copy is not a decoder or a renderer vertex declaration. */
mh3_res_status mh3_model_shape_cache(mh3_res_mut_span cache, mh3_res_span source);
/* Verified MDL0v11 material accessor layout and Draw1Mat1Shp's swap selection.
 * version belongs to the caller's certified model owner. Address arithmetic
 * maps whole CPU records, never source list commands. Nonzero replacements
 * select their actual pointer; zero slots select the source material record.
 * This does not establish loader/producer identity or execute a draw. */
mh3_res_status mh3_model_material_scope_read(const mh3_res_memory *memory, uint32_t material_address, uint32_t model_version, const mh3_model_material_bindings *swaps, mh3_model_material_scope *out);
/* The scope mh3_model_material_scope_read returns with `swaps` for a material
 * whose scope without replacements is `base` (as that read returned it): the
 * nonzero replacement slots and vertex tables replace the base's, with the
 * read's alignment and mapping checks. Base must hold no replacements. */
mh3_res_status mh3_model_material_scope_overlay(const mh3_res_memory *memory, const mh3_model_material_scope *base, const mh3_model_material_bindings *swaps, mh3_model_material_scope *out);

typedef enum mh3_model_material_action {
    MH3_MATERIAL_PREP_FOG,
    MH3_MATERIAL_PREP_MISC,
    MH3_MATERIAL_PREP_PALETTES,
    MH3_MATERIAL_PREP_TEXTURES,
    MH3_MATERIAL_PREP_GENMODE,
    MH3_MATERIAL_PREP_PIPELINE,
    MH3_MATERIAL_PREP_PIXEL,
    MH3_MATERIAL_PREP_COLORS,
    MH3_MATERIAL_PREP_INDIRECT,
    MH3_MATERIAL_PREP_LIGHTS,
    MH3_MATERIAL_PREP_CHANNELS,
    MH3_MATERIAL_PREP_TEXGEN,
    MH3_MATERIAL_PREP_SRT
} mh3_model_material_action;
typedef struct mh3_model_material_step {
    mh3_model_material_action action;
    uint32_t resource_address, indirect_operator;
    uint8_t force_light_off;
} mh3_model_material_step;
typedef mh3_res_status (*mh3_model_material_prepare_step)(void *owner, const mh3_model_material_step *step);
/* 80076220: exact CPU ordering. The typed consumer owns fog/light outputs,
 * imported texture objects and typed material fields. A nonzero operator must
 * be resolved to its native producer by that consumer, never a guest callback.
 * Invalid/ignored material with an operator still emits the indirect override
 * (resource0 requests the original empty base). Callback failure stops the
 * sequence. No runtime adapter is installed by this standalone helper. */
mh3_res_status mh3_model_material_prepare_scope(const mh3_model_material_scope *scope, uint32_t control, uint32_t indirect_operator, int ignore_material, mh3_model_material_prepare_step prepare,
                                                void *owner);

/* Verified108-byte TexCoordScaleState CPU cache. The map bytes come from the
 * admitted M3MTv3 recipe/live typed producer, not a source GPU list decoder.
 * Supplied map dimensions must match this cache's explicit texture producer.
 * NULL mappings leave the previous manual coordinate unchanged. */
mh3_res_status mh3_model_coordinate_table(mh3_res_mut_span cache, const uint8_t map[8]); /* 800866AC */
mh3_res_status mh3_model_coordinate_prepare(mh3_res_mut_span cache, uint32_t count, uint8_t map_known, const mh3_model_texcoord_sizes *maps, const mh3_model_texcoord_sizes *current,
                                            mh3_model_texcoord_sizes *out, uint8_t *written, int *apply); /* 80086674/800853B4 */

/* Standard IndMtxOp replaces only its valid slots. The source overlay is the
 * typed IndTexMtxInfo base, or an empty base for an ignored material. Converts
 * its two coefficient rows with the original auto exponent/truncation rules;
 * no source command bytes are serialized or replayed. Transactional. */
mh3_res_status mh3_model_indirect_scope(const mh3_native_material_state *base, const mh3_indirect_output *operator_output, mh3_native_material_state *out);
/* The same on the three indirect matrices alone. */
mh3_res_status mh3_model_indirect_matrices(const mh3_pack_indirect_matrix base[3], const mh3_indirect_output *operator_output, mh3_pack_indirect_matrix out[3]);

#ifdef __cplusplus
}
#endif
#endif
