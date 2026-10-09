#ifndef MH3_UI_H
#define MH3_UI_H
#include "mh3_draw_state.h"
#include "mh3_model_resources.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct mh3_ui_vertex {
    float position[3], uv[2];
    uint8_t color[4];
} mh3_ui_vertex;

typedef enum mh3_ui_primitive {
    MH3_UI_POINTS,
    MH3_UI_LINES,
    MH3_UI_TRIANGLES,
    MH3_UI_QUADS,
    MH3_UI_LINE_STRIP,
    MH3_UI_TRIANGLE_STRIP,
    MH3_UI_TRIANGLE_FAN
} mh3_ui_primitive;

typedef enum mh3_ui_position_format {
    MH3_UI_POSITION_I16_2,
    MH3_UI_POSITION_FLOAT_2,
    MH3_UI_POSITION_FLOAT_3
} mh3_ui_position_format;

typedef enum mh3_ui_uv_format {
    MH3_UI_UV_NONE,
    MH3_UI_UV_FLOAT,
    MH3_UI_UV_I16
} mh3_ui_uv_format;

/* These are CPU arrays, not GPU payloads. The adapter supplies checked spans;
 * native attributes are written directly into the admitted frame allocation. */
typedef struct mh3_ui_stream {
    mh3_res_span positions, colors, uvs;
    uint32_t count;
    mh3_ui_position_format position_format;
    mh3_ui_uv_format uv_format;
    uint32_t color_stride;
    float uv_scale[2], uv_matrix[12];
    uint8_t transform_uv;
} mh3_ui_stream;

typedef struct mh3_ui_texture_header {
    uint32_t width, height, format, payload_address, wrap_s, wrap_t;
} mh3_ui_texture_header;

mh3_res_status mh3_ui_texture_header_read(mh3_res_span header, mh3_ui_texture_header *out);

mh3_res_status mh3_ui_geometry_counts(mh3_ui_primitive primitive, uint32_t count, uint32_t *vertices, uint32_t *indices);
/* Width is in target pixels. Points and lines become triangles with the same
 * raster state, endpoint colors and homogeneous depth as the original draw. */
mh3_res_status mh3_ui_build_vertices(const mh3_draw_state *state, mh3_ui_primitive primitive, const mh3_ui_vertex *vertices, uint32_t count, float width, AuroraNativeGeometry *out);
mh3_res_status mh3_ui_build_stream(const mh3_draw_state *state, mh3_ui_primitive primitive, const mh3_ui_stream *stream, float width, AuroraNativeGeometry *out);
/* One CPU glyph record contains four positions, four UVs and one RGBA word. */
mh3_res_status mh3_ui_build_glyphs(const mh3_draw_state *state, mh3_res_span records, mh3_res_span glyph_indices, uint32_t count, AuroraNativeGeometry *out);
void mh3_ui_rect(mh3_ui_vertex out[4], float left, float top, float right, float bottom, float depth, const uint8_t color[4], float u0, float v0, float u1, float v1);
#ifdef __cplusplus
}
#endif
#endif
