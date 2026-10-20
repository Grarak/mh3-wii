#ifndef MH3_EFFECT_DRAW_H
#define MH3_EFFECT_DRAW_H
/* NW4R particle resource/geometry producers. All addresses and guest ABI
 * marshalling live in the C++ adapter; these records contain host values. */
#include "mh3_draw_state.h"
#include "mh3_model_state.h"
#include "mh3_mesh_pack.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct mh3_effect_material {
    uint16_t flags;
    uint8_t stage_count, texture_count, indirect_mask;
    int8_t texture_maps[3];
    mh3_pack_tev_stage stages[4];
    mh3_pack_pixel_inputs pixel;
    float indirect_matrix[6];
    int8_t indirect_exponent;
    uint8_t rgb_sources[8], alpha_sources[8];
} mh3_effect_material;

/* Reads the 0xAC-byte emitter draw settings; rejects impossible counts/selectors.
 * Output is untouched on failure. Alpha reference/colour channels are live
 * particle inputs, set separately after resource setup. */
mh3_res_status mh3_effect_material_read(mh3_res_span resource, mh3_effect_material *out);
void mh3_effect_material_apply(const mh3_effect_material *resource, const mh3_native_material_state *inherited, mh3_native_material_state *out);
void mh3_effect_color(uint8_t source, const uint8_t colors[4][4], uint8_t out[3]);
uint8_t mh3_effect_alpha(uint8_t source, const uint8_t colors[4][4]);
void mh3_effect_register_colors(const mh3_effect_material *material, const uint8_t colors[4][4], uint8_t tev[3][4], uint8_t konst[4][4]);
/* NW4R effect texture resource differs from g3d TEX0. An authored mip count
 * of zero means a base-only image: effective levels=1 and max_lod=0. */
mh3_res_status mh3_effect_texture_read(mh3_res_span resource, uint32_t address, uint32_t map, uint32_t wrap_s, uint32_t wrap_t, mh3_native_texture_binding *binding,
                                       mh3_native_palette_source *palette);

typedef struct mh3_effect_texture_transform {
    float scale[2], translation[2], tile[2], offset[2], rotation;
    uint8_t flip_s, flip_t, position_source;
} mh3_effect_texture_transform;

/* sin/cos retain the game's table lookup; the caller supplies their values. */
void mh3_effect_texture_matrix(const mh3_effect_texture_transform *input, float sine, float cosine, const float position_post_matrix[12], float out[12]);

typedef struct mh3_effect_quad {
    float position[4][3];
} mh3_effect_quad;

typedef struct mh3_effect_billboard {
    float matrix[12], pivot[2], axis_x[2], axis_y[2];
} mh3_effect_billboard;

void mh3_effect_billboard_axes(mh3_effect_billboard *out, const float matrix[12], const float view[12], const float pivot[2], uint8_t facing_kind);
void mh3_effect_billboard_quad(const mh3_effect_billboard *draw, const float position[3], float width, float height, float angle, float sine, float cosine, mh3_effect_quad *out);
typedef struct mh3_effect_directional_inputs {
    float view_local[12], size_matrix[12], position[3], direction[3], history[3], fallback[3];
    uint8_t screen_facing;
} mh3_effect_directional_inputs;

void mh3_effect_directional_matrix(const mh3_effect_directional_inputs *input, float history_out[3], float matrix_out[12]);
void mh3_effect_quad_axes(const float center[3], const float diagonal_a[3], const float diagonal_b[3], mh3_effect_quad *out);
void mh3_effect_quad_transform(const float matrix[12], const mh3_effect_quad *input, mh3_effect_quad *out);
void mh3_effect_movie_material(const mh3_native_material_state *inherited, mh3_native_material_state *out);
void mh3_effect_movie_restore(const mh3_native_material_state *inherited, mh3_native_material_state *out);
void mh3_effect_movie_quad(int16_t x, int16_t y, int16_t width, int16_t height, int16_t inset, mh3_effect_quad *out);
/* Triangle-list conversion preserves GX quad winding. */
typedef struct mh3_effect_particle {
    float position[3], size[2], scale[2], manager_scale[2], rotation, sine, cosine;
    uint16_t emitter_flags;
} mh3_effect_particle;

typedef struct mh3_effect_billboard_source {
    void *context;
    uint32_t (*first)(void *context);
    uint32_t (*next)(void *context, uint32_t particle);
    void (*read)(void *context, uint32_t particle, mh3_effect_particle *out);
    void (*prepare)(void *context, uint32_t particle, int first);
    void (*orient)(void *context, uint32_t particle, mh3_effect_particle *out);
    void (*submit)(void *context, const mh3_effect_quad *quad);
    uint8_t width_only;
} mh3_effect_billboard_source;

typedef void (*mh3_effect_particle_draw)(void *context, uint32_t particle, const mh3_effect_particle *values, float width, float height);
void mh3_effect_draw_particles(float minimum_size, const mh3_effect_billboard_source *source, mh3_effect_particle_draw draw);
void mh3_effect_draw_billboards(const mh3_effect_billboard *draw, float minimum_size, const mh3_effect_billboard_source *source);
typedef struct mh3_effect_vertex_inputs {
    float texture_matrix[8][12], normal[3];
    uint8_t position_source[8], color[2][4];
    /* Bit n set: no stage reads texture coordinate n; it is written as zero. */
    uint8_t unused_coordinates;
} mh3_effect_vertex_inputs;

void mh3_effect_material_vertices(const mh3_draw_state *draw, const mh3_effect_quad *quad, const float uv[4][2], const mh3_effect_vertex_inputs *inputs, AuroraNativeMaterialVertex out[4]);
/* mh3_effect_material_vertices for the compact quad format: the corners as
 * given and two of the eight texture coordinates, slots[0] and slots[1], by
 * the same arithmetic (an unused one is zero). */
void mh3_effect_quad_vertices(const mh3_effect_quad *quad, const float uv[4][2], const mh3_effect_vertex_inputs *inputs, const uint8_t slots[2], AuroraNativeQuadVertex out[4]);
/* A point's or line's corners for the compact quads, in view space: the
 * clip-space spread of mh3_effect_expand_point and _line taken back through
 * the projection's x and y scales. 0 when the projection moves x or y by
 * anything else (GX projections do not). */
int mh3_effect_expand_view(const mh3_draw_state *draw, const mh3_effect_quad *quad, float width, int point, float out[4][3]);
void mh3_effect_expand_point(const mh3_draw_state *draw, float width, AuroraNativeMaterialVertex vertices[4]);
void mh3_effect_expand_line(const mh3_draw_state *draw, float width, AuroraNativeMaterialVertex vertices[4]);
void mh3_effect_quad_indices(uint16_t base, uint16_t out[6]);
typedef struct mh3_effect_ribbon_source {
    void *context;
    uint32_t (*first)(void *context);
    uint32_t (*next)(void *context, uint32_t particle);
    void (*pair)(void *context, uint32_t particle, int origin, float position[2][3]);
    void (*submit)(void *context, const mh3_effect_quad *quad, const float uv[4][2]);
    uint32_t count;
    uint8_t closure, reverse, individual_uv;
} mh3_effect_ribbon_source;

void mh3_effect_draw_ribbon(const mh3_effect_ribbon_source *source);
typedef struct mh3_effect_ribbon_pair {
    float position[2][3], texture_t;
} mh3_effect_ribbon_pair;

typedef struct mh3_effect_smooth_ribbon_source {
    mh3_effect_ribbon_source ribbon;
    uint32_t subdivisions;
    float origin_delta[3];
} mh3_effect_smooth_ribbon_source;

void mh3_effect_spline_weights(float t, float out[3]);
void mh3_effect_spline_pair(const mh3_effect_ribbon_pair frames[3], const float weights[3], mh3_effect_ribbon_pair *out);
void mh3_effect_draw_smooth_ribbon(const mh3_effect_smooth_ribbon_source *source);
typedef struct mh3_effect_tube_frame {
    float center[3], axis_y[3], axis_z[3], texture_t;
} mh3_effect_tube_frame;

void mh3_effect_spline_tube(const mh3_effect_tube_frame frames[3], const float weights[3], mh3_effect_tube_frame *out);
void mh3_effect_tube_quad(const mh3_effect_tube_frame frames[2], const float angles[2][2], uint32_t sample, uint32_t segments, int reverse, mh3_effect_quad *out, float uv[4][2]);
void mh3_effect_strip_indices(uint16_t vertex_count, uint16_t *out);
#ifdef __cplusplus
}
#endif
#endif
