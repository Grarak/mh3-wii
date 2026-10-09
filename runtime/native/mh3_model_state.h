#ifndef MH3_MODEL_STATE_H
#define MH3_MODEL_STATE_H
/* Typed scene/material inputs. No GX calls, command lists or GPU ownership. */
#include "mh3_model_matrices.h"
#include "mh3_lighting.h"
#include "mh3_mesh_pack.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct mh3_native_texture_source {
    uint32_t resource_address, payload_address, asset_id, format, width, height, levels;
    float min_lod, max_lod;
    uint64_t generation;
    uint8_t indexed, mutable_source;
} mh3_native_texture_source;

typedef struct mh3_native_sampler {
    uint32_t wrap_s, wrap_t, min_filter, mag_filter, anisotropy;
    float lod_bias;
    uint8_t bias_clamp, edge_lod;
} mh3_native_sampler;

typedef struct mh3_native_texture_binding {
    mh3_native_texture_source source;
    mh3_native_sampler sampler;
    uint32_t map_id, tlut_id;
} mh3_native_texture_binding;

typedef struct mh3_native_palette_source {
    uint32_t resource_address, payload_address, format, entries;
    uint64_t generation;
} mh3_native_palette_source;

typedef struct mh3_native_material_state {
    uint32_t texgen_count, channel_count, tev_count, indirect_count, cull_mode;
    int32_t light_set_reference, fog_reference;
    /* raster_channel is semantic0..8/255 here; immutable M3MT TEV records
     * retain raw BP selectors. The load-time assembler normalizes them once. */
    mh3_pack_tev_stage tev[16];
    mh3_pack_tev_color color[4], konst[4];
    mh3_pack_swap_table swaps[4];
    mh3_pack_indirect_stage indirect[4];
    mh3_pack_indirect_matrix indirect_matrix[3];
    mh3_pack_texgen texgen[8];
    mh3_pack_channel channel[2];
    mh3_pack_pixel_inputs pixel;
    /* Live raster state inherited by effect/model passes; not inferred from
     * immutable M3MT data. Unknown must remain unknown until a producer sets it. */
    uint8_t clip_mode_known, clip_disabled;
    uint64_t generation;
} mh3_native_material_state;

/* Load-time typed assembly; v1 material payloads cannot supply constants. */
mh3_pack_status mh3_material_state_read(const mh3_mesh_pack *pack, uint32_t material_index, mh3_native_material_state *out);
/* Explicit producer generations: changed only by create/update/rebind actions.
 * No content checks. A recycled address must get a new producer generation. */
void mh3_texture_source_created(mh3_native_texture_source *source, uint32_t resource, uint32_t payload, uint64_t generation, int mutable_source);
void mh3_texture_source_updated(mh3_native_texture_source *source, uint64_t generation);
mh3_res_status mh3_texture_source_read(mh3_res_span tex0, uint32_t address, mh3_native_texture_source *out);
mh3_res_status mh3_palette_source_read(mh3_res_span pltt, uint32_t address, mh3_native_palette_source *out);
/* CPU compatibility object construction. Backend consumes typed records above;
 * these packed bytes only preserve translated resource callers' object ABI. */
mh3_res_status mh3_texture_compat_store(mh3_res_mut_span object, const mh3_native_texture_binding *binding);
mh3_res_status mh3_palette_compat_store(mh3_res_mut_span object, const mh3_native_palette_source *palette);
mh3_res_status mh3_texture_binding_read(mh3_res_span texture_info, const mh3_native_texture_source *source, mh3_native_texture_binding *out);
mh3_res_status mh3_texture_info_store_source(mh3_res_mut_span texture_info, uint32_t resource, int palette);
/* ResTexObj/ResTlutObj have a presence word then eight 32/12-byte CPU objects. */
mh3_res_status mh3_resource_objects_present(mh3_res_span objects, uint32_t slot, int palette, int *out);
mh3_res_status mh3_resource_objects_validate(mh3_res_mut_span objects, uint32_t slot, int palette);
mh3_res_status mh3_resource_objects_copy(mh3_res_mut_span destination, mh3_res_span source, int palette);
mh3_res_status mh3_resource_objects_cache(mh3_res_mut_span state, mh3_res_span objects, int palette, uint8_t *changed);
mh3_res_status mh3_texture_scale_size(mh3_res_mut_span state, uint32_t slot, uint16_t width, uint16_t height);
mh3_res_status mh3_texture_cache_palette_changed(mh3_res_mut_span texture_state, uint8_t changed);
mh3_res_status mh3_depth_compare_cache(mh3_res_mut_span state, mh3_res_mut_span sync, int before, int *apply);

typedef struct mh3_native_fog {
    uint32_t type;
    mh3_rgba8 color;
    float start, end, near_plane, far_plane;
    uint8_t range_enabled;
    uint16_t range_center, range_adjust[10];
} mh3_native_fog;

mh3_res_status mh3_fog_read(mh3_res_span record, mh3_native_fog *out);
mh3_res_status mh3_fog_range_table(uint16_t width, const float projection[16], uint16_t out[10]);
mh3_res_status mh3_fog_range_store(mh3_res_mut_span record, uint16_t center, const uint16_t values[10]);
/* Output is written only when selection changes; no change sets *apply=0. */
mh3_res_status mh3_fog_state_select(mh3_res_mut_span state, uint32_t id, mh3_native_fog *out, int *apply);

typedef struct mh3_native_camera {
    mh3_matrix34 view;
    float projection[16];
    uint32_t flags, projection_type;
    mh3_vec3 position, up, target, rotation;
    float twist, fovy, aspect, near_plane, far_plane, top, bottom, left, right;
    float viewport[6];
    uint32_t scissor[4], scissor_offset[2];
} mh3_native_camera;

mh3_res_status mh3_camera_read(mh3_res_span record, mh3_native_camera *out);
mh3_res_status mh3_camera_prepare_projection(mh3_native_camera *camera);
mh3_res_status mh3_camera_prepare_view(mh3_native_camera *camera);
mh3_res_status mh3_camera_store_matrices(mh3_res_mut_span record, const mh3_native_camera *camera);
/* Screen projection preserves Wii depth/viewport convention. Backend clip
 * conversion must be applied once separately, never inside this calculation. */
mh3_res_status mh3_camera_project(mh3_native_camera *camera, const mh3_vec3 *point, mh3_vec3 *out);
/* ScnRoot's 32 inline CameraData records. The current index is at +F4;
 * split-screen selection uses the game's explicit per-player camera IDs. */
mh3_res_status mh3_scene_camera_select(mh3_res_span scene, uint32_t scene_address, int split, int player, mh3_res_span player_camera_ids, uint32_t *out);
/* Resolve resource pixels and offset quantization into a backend rectangle.
 * Wrapped/aliased scissors needing a different1024-period rectangle are
 * explicitly unsupported. Output remains untouched on failure. */
mh3_res_status mh3_camera_effective_scissor(const mh3_native_camera *camera, int32_t out[4]);
/* G3D CameraState differs from CameraData: selected u16 at+2,32 prepared
 * row-major view matrices at+34hex, stride48 (80088380). */
mh3_res_status mh3_scene_view_matrix(mh3_res_span camera_state, mh3_matrix34 *out);

/* 800885F4 Invalidate: CPU cache records borrowed from their actual owners.
 * All requested spans are checked before any write. GPU state is supplied by
 * typed producers separately; these fields preserve translated CPU callers. */
typedef struct {
    mh3_res_mut_span texture_valid, palette_valid, texture_scale, tev, genmode;
    mh3_res_mut_span channel, current_matrix, texture_sources, post_texture;
    mh3_res_mut_span vertex_descriptor, fog, light_cache, view_sources, sync;
} mh3_model_cache_storage;

mh3_res_status mh3_model_cache_invalidate(mh3_model_cache_storage storage, uint32_t flags);

typedef struct mh3_native_light_slots {
    mh3_light lights[8];
    int8_t source_ids[8];
    uint8_t valid_mask;
    uint32_t diffuse_color, diffuse_alpha, specular_color, specular_alpha;
    mh3_rgba8 ambient;
} mh3_native_light_slots;

/* The channel preparation consumer needs only these authored values. */
typedef struct mh3_native_light_selection {
    uint32_t masks[4];
    mh3_rgba8 ambient;
} mh3_native_light_selection;

/* Exact typed slot mutations from one selection. Written lights can retain
 * identical contents; changed bits include all raw light fields. */
typedef struct mh3_native_light_delta {
    uint8_t lights_written, lights_changed, source_ids_changed;
    uint8_t valid_changed, masks_changed, ambient_changed;
} mh3_native_light_delta;

/* Native reconstruction of LightState's slot reuse/eviction and masks. */
mh3_res_status mh3_light_slots_initial(const mh3_light *lights, size_t count, mh3_native_light_slots *out);
mh3_res_status mh3_light_slots_select(const mh3_light *lights, size_t count, const mh3_light_set *set, const mh3_rgba8 *ambient, size_t ambient_count, mh3_native_light_slots *slots);
enum {
    MH3_LIGHT_STATE_BYTES = 0x2a2c
};

/* Serialized LightState compatibility at the verified offsets. Import keeps
 * capacity/address fields and unused source slots, exactly like8007AFD0.
 * ApplyView8007B104 uses MAX(source count,capacity), not MIN. */
mh3_res_status mh3_light_state_import(mh3_res_mut_span state, uint16_t light_count, uint16_t set_count, mh3_res_span lights, mh3_res_span ambient, mh3_res_span sets, const mh3_matrix34 *view,
                                      mh3_native_light_slots *out);
mh3_res_status mh3_light_state_select(mh3_res_mut_span state, uint32_t set_id, mh3_native_light_slots *slots);
/* Owner-internal selection: same fields/errors as the checked full reader,
 * with disjoint serialized state/typed slots. Plans resident IDs before any
 * writes, decodes only newly loaded records, and never copies whole slots.
 * Failure leaves state, slots and delta unchanged. */
mh3_res_status mh3_light_state_select_delta(mh3_res_mut_span state, uint32_t set_id, mh3_native_light_slots *slots, mh3_native_light_delta *delta);
int mh3_light_delta_changed(mh3_native_light_delta delta);
#ifdef __cplusplus
}
#endif
#endif
