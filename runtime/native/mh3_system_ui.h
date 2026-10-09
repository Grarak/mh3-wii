#ifndef MH3_SYSTEM_UI_H
#define MH3_SYSTEM_UI_H
#include "mh3_ui.h"
#include "mh3_model_state.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    float position[3], coordinate[8][3];
    uint8_t color[4];
} mh3_system_ui_vertex;

typedef enum {
    MH3_SYSTEM_UI_MODULATE,
    MH3_SYSTEM_UI_INTENSITY,
    MH3_SYSTEM_UI_COLOR_MAPPING,
    MH3_SYSTEM_UI_SOLID
} mh3_system_ui_pipeline;

/* Authored pipeline updates overlay inherited material fields. */
void mh3_system_ui_material(mh3_native_material_state *, mh3_system_ui_pipeline, const uint8_t first[4], const uint8_t second[4]);
void mh3_system_ui_quad(mh3_system_ui_vertex out[4], const float origin[3], const float size[2], int y_sign, const float coordinates[8][4][2], uint32_t count, const uint8_t colors[4][4]);
int mh3_system_ui_glyph(mh3_system_ui_vertex out[4], const float origin[3], uint32_t width, uint32_t height, float scale_x, float scale_y, uint32_t texture_width, uint32_t texture_height,
                        uint32_t cell_x, uint32_t cell_y, const uint8_t colors[4][4]);
mh3_res_status mh3_system_ui_build(const mh3_draw_state *, const mh3_system_ui_vertex *, uint32_t count, AuroraNativeMaterialGeometry *);
void mh3_system_ui_concat(float out[12], const float left[12], const float right[12]);
int mh3_system_ui_indirect_matrix(const float input[6], mh3_pack_indirect_matrix *);
mh3_res_status mh3_system_ui_texture_info_read(mh3_res_span info, uint64_t generation, mh3_native_texture_binding *);
mh3_res_status mh3_system_ui_texture_info_store(mh3_res_mut_span info, const mh3_native_texture_binding *);
mh3_res_status mh3_system_ui_texture_lod(mh3_native_texture_binding *);
typedef struct {
    uint32_t texture_count, texture_stride, srt_offset, texgen_offset, indirect_srt_offset;
    uint32_t storage_size, texture_srt_count, indirect_srt_count;
    uint8_t texgen_count, vertex_colors, modulation, texture_matrices[10], indirect_matrices[3];
    uint8_t texgen_type[8], texgen_source[8], texgen_selector[8];
} mh3_system_ui_layout;

mh3_res_status mh3_system_ui_layout_size(uint32_t capacity, int hbm, uint32_t *size);
/* Decode the entire authored material, retaining fields it does not publish. */
mh3_res_status mh3_system_ui_layout_material(mh3_native_material_state *, mh3_res_span header, mh3_res_span storage, int hbm, int vertex_colors, uint8_t alpha, const uint8_t konst_color[8],
                                             const uint8_t konst_alpha[8], mh3_system_ui_layout *);
mh3_res_status mh3_system_ui_coordinates(mh3_system_ui_vertex *, uint32_t, const mh3_system_ui_layout *, const float matrices[10][12]);
void mh3_system_ui_debug_material(mh3_native_material_state *, int blending);
void mh3_system_ui_fade_material(mh3_native_material_state *);
mh3_res_status mh3_system_ui_lines(const mh3_draw_state *, const mh3_system_ui_vertex *, uint32_t count, float width, AuroraNativeMaterialGeometry *);
/* Pure viewport/projection CPU records used by save UI controls. */
void mh3_system_ui_projection_record(const mh3_draw_state *, float out[7]);
int mh3_system_ui_panel_view(const float bounds[4], const float panel_bounds[4], float screen_width, const float saved_viewport[6], float projection[16], float viewport[6], int32_t scissor[4]);
typedef struct {
    uint32_t texture_count, srt_count, texgen_count, tev_count, indirect_count, indirect_srt_count;
    uint8_t swap, channel, material_color, alpha, blend;
    uint32_t resource_flags;
} mh3_system_ui_capacity;

mh3_res_status mh3_system_ui_hbm_capacity(mh3_res_span resource, mh3_system_ui_capacity *);
mh3_res_status mh3_system_ui_hbm_construct(mh3_res_mut_span header, mh3_res_mut_span storage, mh3_res_span resource, const mh3_system_ui_capacity *);
uint32_t mh3_system_ui_panel_selection(int32_t current, int32_t low, int32_t high, int32_t pivot);
void mh3_system_ui_transform(float out[3], const float matrix[12], const float input[3]);
typedef struct {
    mh3_system_ui_vertex vertex[4];
    uint8_t blend_mode;
} mh3_system_ui_hud_quad;

int mh3_system_ui_hud_rectangle(mh3_system_ui_vertex out[4], float left, float top, float width, float height, const uint8_t rgba[4], float u0, float v0, float u1, float v1);
mh3_res_status mh3_system_ui_hud_background(uint32_t type, int wide, float atlas_width, float atlas_height, const int16_t atlas_origins[4][2], const uint8_t title_rgba[4],
                                            mh3_system_ui_hud_quad out[5], uint32_t *count);
void mh3_system_ui_hud_material(mh3_native_material_state *);
void mh3_system_ui_application_pipeline(mh3_native_material_state *, int textured);
void mh3_system_ui_direct_stages(mh3_native_material_state *, const uint32_t *stages, uint32_t count);
typedef struct {
    void *user;
    uint32_t (*measure)(void *, uint32_t cursor, uint32_t count, float width, float *line_width, uint8_t *line_break);
    void (*draw)(void *, uint32_t cursor, uint32_t count, float x);
    void (*newline)(void *);
} mh3_system_ui_text_callbacks;

mh3_res_status mh3_system_ui_text_lines(const mh3_system_ui_text_callbacks *, uint32_t cursor, uint32_t count, float left, float width, float alignment, float wrap_width);
typedef struct {
    void *user;
    uint32_t (*field)(void *, uint32_t, uint32_t);
    uint32_t (*method)(void *, uint32_t, uint32_t);
} mh3_system_ui_callbacks;

void mh3_system_ui_save_draw(const mh3_system_ui_callbacks *, uint32_t self, unsigned variant);
#ifdef __cplusplus
}
#endif
#endif
