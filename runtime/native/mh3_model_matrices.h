#ifndef MH3_MODEL_MATRICES_H
#define MH3_MODEL_MATRICES_H
#include "mh3_model_resources.h"
#include "mh3_model_runtime.h"
#include "mh3_mesh_pack.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float v[3];
} mh3_vec3;
typedef struct {
    float v[3][3];
} mh3_matrix33;
typedef struct {
    float v[3][4];
} mh3_matrix34;

/* Native float records or serialized big-endian float records. Counts describe
 * the actual producer's allocation, not merely the enclosing RAM bank. */
typedef struct {
    mh3_res_span bytes;
    size_t count, stride;
    int big_endian;
} mh3_matrix_table;
typedef struct {
    mh3_matrix_table positions, normals, environment;
    uint64_t generation; /* Explicit source revision. Live pose writes and
                         * binding revisions are tracked separately. */
} mh3_matrix_sources;
typedef struct {
    uint32_t address[3];
    uint64_t generation;
} mh3_matrix_binding;
void mh3_matrix_bind(mh3_matrix_binding *state, const uint32_t addresses[3]);

mh3_res_status mh3_matrix_read34(mh3_matrix_table table, size_t index, mh3_matrix34 *out);
mh3_res_status mh3_matrix_read33(mh3_matrix_table table, size_t index, mh3_matrix33 *out);
/* Trusted runtime palette copy, including endian conversion; no admission. */
void mh3_matrix_read34_unchecked(mh3_matrix_table table, size_t index, mh3_matrix34 *out);
void mh3_matrix_concat(const mh3_matrix34 *a, const mh3_matrix34 *b, mh3_matrix34 *out);
/* Returns ABSENT when |det| < 1e-36f, leaving out untouched. The runtime
 * view-normal reader separately applies MH3's upper-3x3 singular fallback. */
mh3_res_status mh3_matrix_inverse_normal(const mh3_matrix34 *position, mh3_matrix33 *out);

/* Native reconstruction of 80087B34 and 80087D30 matrix selection. Direct
 * normal input is 3x4 because GXLoadNrmMtxImm reads its upper 3x3. */
typedef struct {
    int valid, has_normal;
    int32_t matrix_id;
    const mh3_matrix34 *direct_position, *direct_normal;
    uint32_t texture_source[8]; /* 1 normal, 2 position, others identity */
    int current_is_valid, view_array_is_valid;
} mh3_shape_matrix_input;
typedef enum {
    MH3_MATRIX_SCALE_TEXTURES,
    MH3_MATRIX_POSITION_DIRECT,
    MH3_MATRIX_POSITION_INDEXED,
    MH3_MATRIX_NORMAL_DIRECT,
    MH3_MATRIX_NORMAL_INDEXED,
    MH3_MATRIX_TEXTURE_NORMAL,
    MH3_MATRIX_CURRENT_IDENTITY,
    MH3_MATRIX_CURRENT_SELECT,
    MH3_MATRIX_GENMODE,
    MH3_MATRIX_PRIMITIVES
} mh3_matrix_action;
typedef struct {
    mh3_matrix_action action;
    uint32_t index, channel;
    const mh3_matrix34 *direct;
    uint32_t texture_ids[8];
} mh3_matrix_command;
typedef mh3_res_status (*mh3_matrix_emit)(void *user, const mh3_matrix_command *command);
typedef struct {
    int submitted, envelope, current_is_valid;
} mh3_shape_matrix_result;
mh3_res_status mh3_shape_matrix_submit(const mh3_shape_matrix_input *input, mh3_matrix_emit emit, void *user, mh3_shape_matrix_result *out);

/* Native palettes for direct mesh submission. Indexed loads use the decoded
 * descriptor from the offline converter, in original command order. Position
 * slots are PNMTX0..9; texture slots are TEXMTX0..9. Palette APIs trust source
 * storage, descriptors, masks and selectors; they perform no admission checks.
 * The status return is retained for inspection API compatibility (always OK). */
typedef struct {
    mh3_matrix34 position[10], texture[10];
    mh3_matrix33 normal[10];
    uint16_t position_valid, normal_valid;
    uint16_t texture_valid;
    uint32_t texture_ids[8];
} mh3_model_palette;
typedef struct {
    uint8_t array; /* 0 position, 1 normal, 2 environment texture */
    uint16_t source_index, xf_address;
    uint8_t word_count;
} mh3_matrix_load;
mh3_res_status mh3_model_palette_rigid(const mh3_shape_matrix_input *input, const mh3_matrix_sources *sources, mh3_model_palette *out);
mh3_res_status mh3_model_palette_load(const mh3_matrix_sources *sources, const mh3_matrix_load *load, mh3_model_palette *palette);
/* Overlay only produced slots; unknown slots retain owned inherited values.
 * These are the values of actual native matrix publications, not borrowed pose
 * pointers. Both routines are transactional and permit output aliasing. */
mh3_res_status mh3_model_palette_overlay(const mh3_model_palette *inherited, const mh3_model_palette *produced, mh3_model_palette *out);
/* mh3_model_palette_rigid overlaid onto palette in place, without the
 * produced copy: writes only the produced slots and the texture IDs. Returns
 * the produced slots as MH3_PALETTE_PRODUCED_* bits. */
#define MH3_PALETTE_PRODUCED_POSITION 1u
#define MH3_PALETTE_PRODUCED_NORMAL 2u
#define MH3_PALETTE_PRODUCED_TEXTURE(slot) (256u << (slot))
uint32_t mh3_model_palette_rigid_into(const mh3_shape_matrix_input *input, const mh3_matrix_sources *sources, mh3_model_palette *palette);
/* Apply an admitted effective M3PL source map to certified pose extents.
 * Clear known bits preserve the actual inherited slot. Required masks do not
 * validate inherited values. Source pointers/indices are caller obligations.
 * No GX bytes/vertices are
 * read here; the extractor already resolved command order and index spans. */
mh3_res_status mh3_model_palette_apply_batch(const mh3_pack_palette_batch *batch, const mh3_matrix_sources *sources, const mh3_model_palette *inherited, mh3_model_palette *out);

/* 80077814 GetIndMtxOp: typed normal-map matrix inputs. This uses posed view
 * normal/camera matrices; bind matrices only adjust differing node IDs. */
typedef struct {
    uint32_t method;
    int light_present, light_enabled, light_spot, light_specular;
    mh3_vec3 light_position, light_direction;
} mh3_indirect_slot;
typedef struct {
    int material_valid, shape_valid;
    int32_t shape_matrix_id, node_matrix_id;
    mh3_matrix34 camera, node_model, shape_node_inverse;
    mh3_matrix33 view_normal;
    mh3_indirect_slot slot[3];
} mh3_indirect_input;
typedef struct {
    int active;
    mh3_matrix34 normal;
    mh3_matrix34 matrix[3];
    mh3_vec3 light[3];
    uint8_t valid_mask, light_mask;
} mh3_indirect_output;
mh3_res_status mh3_model_indirect_prepare(const mh3_indirect_input *input, mh3_indirect_output *out);

/* Layout locations belong to the compatibility adapter, not the renderer.
 * Every address maps to caller-owned memory through the checked C reader. */
typedef struct {
    uint32_t arrays, texture_sources, current_valid, camera, lights;
} mh3_matrix_runtime_locations;
typedef struct {
    mh3_matrix34 direct_position, direct_normal;
} mh3_matrix_runtime_storage;
mh3_res_status mh3_matrix_runtime_shape(const mh3_res_memory *memory, const mh3_matrix_runtime_locations *locations, uint32_t shape_reference, uint32_t direct_position, uint32_t direct_normal,
                                        mh3_matrix_runtime_storage *storage, mh3_shape_matrix_input *input, mh3_matrix_sources *sources);
mh3_res_status mh3_matrix_runtime_indirect(const mh3_res_memory *memory, const mh3_matrix_runtime_locations *locations, uint32_t material_reference, uint32_t node_reference, uint32_t shape_reference,
                                           mh3_indirect_output *out);
/* Standard IndMtxOp serialized result, preserving vtable and padding words. */
mh3_res_status mh3_matrix_store_indirect(mh3_res_mut_span object, const mh3_indirect_output *out);
mh3_res_status mh3_matrix_store_sources(mh3_res_mut_span state, const uint32_t addresses[3]);
mh3_res_status mh3_matrix_runtime_operator(const mh3_res_memory *memory, uint32_t object, uint32_t reset_entry, uint32_t set_entry);
/* MH3 80085BB8/800882D4: references index relative to the CURRENT light set.
 * Original signed references (including -1) can read preceding serialized
 * bytes; preserve that arithmetic within the checked LightState owner span. */
mh3_res_status mh3_model_light_reference(mh3_res_span light_state, int32_t reference, int32_t *out);

#ifdef __cplusplus
}
#endif
#endif
