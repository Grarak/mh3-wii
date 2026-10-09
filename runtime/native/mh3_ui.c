#include "mh3_ui.h"
#include <math.h>
#include <string.h>

static uint16_t u16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}

static uint32_t u32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static float f32(const uint8_t *p) {
    const uint32_t bits = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
    float value;
    memcpy(&value, &bits, 4);
    return value;
}

static int span_has(mh3_res_span span, size_t size) {
    return span.data && size <= span.size;
}

mh3_res_status mh3_ui_texture_header_read(mh3_res_span header, mh3_ui_texture_header *out) {
    if (!out)
        return MH3_RES_INVALID;

    if (!span_has(header, 36))
        return MH3_RES_RANGE;

    const mh3_ui_texture_header value = {u16(header.data + 2), u16(header.data), u32(header.data + 4), u32(header.data + 8), u32(header.data + 12), u32(header.data + 16)};

    if (!value.width || !value.height || !value.payload_address || value.wrap_s > 2 || value.wrap_t > 2)
        return MH3_RES_INVALID;

    if (value.format > 6 && value.format != 14)
        return MH3_RES_UNSUPPORTED;

    *out = value;
    return MH3_RES_OK;
}

mh3_res_status mh3_ui_geometry_counts(mh3_ui_primitive primitive, uint32_t count, uint32_t *vertices, uint32_t *indices) {
    if (!vertices || !indices)
        return MH3_RES_INVALID;

    uint64_t v = count, i = 0;

    switch (primitive) {
    case MH3_UI_POINTS:
        v = (uint64_t)count * 4;
        i = (uint64_t)count * 6;
        break;
    case MH3_UI_LINES:
        if (count % 2)
            return MH3_RES_INVALID;

        v = (uint64_t)count * 2;
        i = (uint64_t)count * 3;
        break;
    case MH3_UI_LINE_STRIP:
        v = count > 1 ? (uint64_t)(count - 1) * 4 : 0;
        i = count > 1 ? (uint64_t)(count - 1) * 6 : 0;
        break;
    case MH3_UI_TRIANGLES:
        if (count % 3)
            return MH3_RES_INVALID;

        i = count;
        break;
    case MH3_UI_QUADS:
        if (count % 4)
            return MH3_RES_INVALID;

        i = (uint64_t)count / 4 * 6;
        break;
    case MH3_UI_TRIANGLE_STRIP:
    case MH3_UI_TRIANGLE_FAN:
        i = count >= 3 ? (uint64_t)(count - 2) * 3 : 0;
        break;
    default:
        return MH3_RES_UNSUPPORTED;
    }

    if (v > 65535 || i > UINT32_MAX)
        return MH3_RES_RANGE;

    *vertices = (uint32_t)v;
    *indices = (uint32_t)i;
    return MH3_RES_OK;
}

static void triangle(uint16_t *indices, uint32_t *cursor, uint32_t a, uint32_t b, uint32_t c) {
    indices[(*cursor)++] = (uint16_t)a;
    indices[(*cursor)++] = (uint16_t)b;
    indices[(*cursor)++] = (uint16_t)c;
}

static void quad(uint16_t *indices, uint32_t *cursor, uint32_t base) {
    triangle(indices, cursor, base, base + 1, base + 2);
    triangle(indices, cursor, base, base + 2, base + 3);
}

static mh3_res_status output_valid(mh3_ui_primitive primitive, uint32_t count, AuroraNativeGeometry *out) {
    uint32_t v, i;
    const mh3_res_status status = mh3_ui_geometry_counts(primitive, count, &v, &i);

    if (status != MH3_RES_OK)
        return status;

    if (!out || (v && !out->vertices) || (i && !out->indices) || out->vertex_count != v || out->index_count != i)
        return MH3_RES_RANGE;

    return MH3_RES_OK;
}

typedef void (*fetch_vertex)(const void *, uint32_t, mh3_ui_vertex *);
static void fetch_array(const void *data, uint32_t index, mh3_ui_vertex *out) {
    *out = ((const mh3_ui_vertex *)data)[index];
}

static void fetch_stream(const void *data, uint32_t index, mh3_ui_vertex *out) {
    const mh3_ui_stream *s = data;
    const uint32_t components = s->position_format == MH3_UI_POSITION_FLOAT_3 ? 3 : 2;
    const uint32_t step = s->position_format == MH3_UI_POSITION_I16_2 ? 2 : 4;
    const uint8_t *p = s->positions.data + (size_t)index * components * step;
    out->position[2] = 0;

    for (uint32_t j = 0; j < components; ++j)
        out->position[j] = step == 2 ? (float)(int16_t)u16(p + j * 2) : f32(p + j * 4);

    memcpy(out->color, s->colors.data + (size_t)index * s->color_stride, 4);
    out->uv[0] = out->uv[1] = 0;

    if (s->uv_format != MH3_UI_UV_NONE) {
        const uint32_t uv_step = s->uv_format == MH3_UI_UV_I16 ? 2 : 4;
        p = s->uvs.data + (size_t)index * 2 * uv_step;

        for (uint32_t j = 0; j < 2; ++j)
            out->uv[j] = (uv_step == 2 ? (float)(int16_t)u16(p + j * 2) : f32(p + j * 4)) * s->uv_scale[j];

        if (s->transform_uv) {
            const float u = out->uv[0], v = out->uv[1];
            const float q = s->uv_matrix[10] + s->uv_matrix[11];
            /* TEX0 expands to (s,t,1,1). RMHP08's texture SRT producers
             * leave q constant; its division belongs before interpolation. */
            out->uv[0] = (s->uv_matrix[0] * u + s->uv_matrix[1] * v + s->uv_matrix[2] + s->uv_matrix[3]) / q;
            out->uv[1] = (s->uv_matrix[4] * u + s->uv_matrix[5] * v + s->uv_matrix[6] + s->uv_matrix[7]) / q;
        }
    }
}

static void transformed(const mh3_draw_state *state, fetch_vertex fetch, const void *data, uint32_t index, AuroraNativeVertex *out) {
    mh3_ui_vertex vertex;
    fetch(data, index, &vertex);
    mh3_draw_vertex(state, vertex.position, vertex.uv, vertex.color, out);
}

static mh3_res_status build(const mh3_draw_state *state, mh3_ui_primitive primitive, fetch_vertex fetch, const void *data, uint32_t count, float width, AuroraNativeGeometry *out) {
    mh3_res_status status = output_valid(primitive, count, out);

    if (status != MH3_RES_OK)
        return status;

    if (!state || !data)
        return MH3_RES_INVALID;

    uint32_t cursor = 0;

    if (primitive == MH3_UI_POINTS || primitive == MH3_UI_LINES || primitive == MH3_UI_LINE_STRIP) {
        if (!isfinite(width) || width < 0 || !isfinite(state->viewport[2]) || !isfinite(state->viewport[3]) || state->viewport[2] <= 0 || state->viewport[3] <= 0)
            return MH3_RES_INVALID;

        const uint32_t segments = primitive == MH3_UI_POINTS ? count : primitive == MH3_UI_LINES ? count / 2 : count > 1 ? count - 1 : 0;

        for (uint32_t segment = 0; segment < segments; ++segment) {
            AuroraNativeVertex a, b;
            const uint32_t first = primitive == MH3_UI_LINES ? segment * 2 : segment;
            transformed(state, fetch, data, first, &a);

            if (primitive == MH3_UI_POINTS)
                b = a;
            else
                transformed(state, fetch, data, first + 1, &b);

            float dx = width / state->viewport[2], dy = width / state->viewport[3];

            if (primitive != MH3_UI_POINTS) {
                const float px = (b.position[0] * a.position[3] - a.position[0] * b.position[3]) * state->viewport[2];
                const float py = (b.position[1] * a.position[3] - a.position[1] * b.position[3]) * state->viewport[3];
                const float length = hypotf(px, py);

                if (length > 0) {
                    dx = (-py / length) * width / state->viewport[2];
                    dy = (px / length) * width / state->viewport[3];
                } else {
                    dx = 0;
                    dy = width / state->viewport[3];
                }
            }

            AuroraNativeVertex *v = out->vertices + segment * 4;
            v[0] = a;
            v[1] = primitive == MH3_UI_POINTS ? a : b;
            v[2] = b;
            v[3] = primitive == MH3_UI_POINTS ? b : a;

            for (unsigned j = 0; j < 4; ++j) {
                const float sx = (j == 0 || j == 3) ? -1.f : 1.f;
                const float sy = (j < 2) ? -1.f : 1.f;

                if (primitive == MH3_UI_POINTS) {
                    v[j].position[0] += sx * dx * v[j].position[3];
                    v[j].position[1] += sy * dy * v[j].position[3];
                } else {
                    const float sign = j < 2 ? 1.f : -1.f;
                    v[j].position[0] += sign * dx * v[j].position[3];
                    v[j].position[1] += sign * dy * v[j].position[3];
                }
            }

            quad(out->indices, &cursor, segment * 4);
        }

        return MH3_RES_OK;
    }

    for (uint32_t i = 0; i < count; ++i)
        transformed(state, fetch, data, i, out->vertices + i);

    switch (primitive) {
    case MH3_UI_TRIANGLES:
        for (uint32_t i = 0; i < count; i += 3)
            triangle(out->indices, &cursor, i, i + 1, i + 2);

        break;
    case MH3_UI_QUADS:
        for (uint32_t i = 0; i < count; i += 4)
            quad(out->indices, &cursor, i);

        break;
    case MH3_UI_TRIANGLE_STRIP:
        for (uint32_t i = 2; i < count; ++i)
            triangle(out->indices, &cursor, i - 2 + (i & 1), i - 1 - (i & 1), i);

        break;
    case MH3_UI_TRIANGLE_FAN:
        for (uint32_t i = 2; i < count; ++i)
            triangle(out->indices, &cursor, 0, i - 1, i);

        break;
    default:
        return MH3_RES_UNSUPPORTED;
    }

    return MH3_RES_OK;
}

mh3_res_status mh3_ui_build_vertices(const mh3_draw_state *state, mh3_ui_primitive primitive, const mh3_ui_vertex *vertices, uint32_t count, float width, AuroraNativeGeometry *out) {
    return build(state, primitive, fetch_array, vertices, count, width, out);
}

mh3_res_status mh3_ui_build_stream(const mh3_draw_state *state, mh3_ui_primitive primitive, const mh3_ui_stream *stream, float width, AuroraNativeGeometry *out) {
    if (!stream || stream->position_format > MH3_UI_POSITION_FLOAT_3 || stream->uv_format > MH3_UI_UV_I16)
        return MH3_RES_INVALID;

    if (stream->transform_uv) {
        if (stream->uv_matrix[8] != 0 || stream->uv_matrix[9] != 0)
            return MH3_RES_UNSUPPORTED;

        const float q = stream->uv_matrix[10] + stream->uv_matrix[11];

        if (!isfinite(q) || q == 0)
            return MH3_RES_INVALID;
    }

    const uint32_t components = stream->position_format == MH3_UI_POSITION_FLOAT_3 ? 3 : 2;
    const uint32_t step = stream->position_format == MH3_UI_POSITION_I16_2 ? 2 : 4;

    if (!span_has(stream->positions, (size_t)stream->count * components * step) || !span_has(stream->colors, stream->count ? (size_t)(stream->count - 1) * stream->color_stride + 4 : 0) ||
        (stream->uv_format != MH3_UI_UV_NONE && !span_has(stream->uvs, (size_t)stream->count * 2 * (stream->uv_format == MH3_UI_UV_I16 ? 2 : 4))))
        return MH3_RES_RANGE;
    return build(state, primitive, fetch_stream, stream, stream->count, width, out);
}

mh3_res_status mh3_ui_build_glyphs(const mh3_draw_state *state, mh3_res_span records, mh3_res_span glyph_indices, uint32_t count, AuroraNativeGeometry *out) {
    if (!state || count > 16383 || !span_has(glyph_indices, (size_t)count * 2))
        return MH3_RES_RANGE;

    const mh3_res_status status = output_valid(MH3_UI_QUADS, count * 4, out);

    if (status != MH3_RES_OK)
        return status;

    for (uint32_t glyph = 0; glyph < count; ++glyph) {
        const size_t offset = (size_t)u16(glyph_indices.data + glyph * 2) * 84;

        if (offset > records.size || !span_has(records, offset + 84))
            return MH3_RES_RANGE;
    }

    uint32_t cursor = 0;

    for (uint32_t glyph = 0; glyph < count; ++glyph) {
        const uint8_t *record = records.data + (size_t)u16(glyph_indices.data + glyph * 2) * 84;

        for (unsigned vertex = 0; vertex < 4; ++vertex) {
            const float position[3] = {f32(record + vertex * 12), f32(record + vertex * 12 + 4), f32(record + vertex * 12 + 8)};
            const float uv[2] = {f32(record + 48 + vertex * 8), f32(record + 52 + vertex * 8)};
            mh3_draw_vertex(state, position, uv, record + 80, out->vertices + glyph * 4 + vertex);
        }

        quad(out->indices, &cursor, glyph * 4);
    }

    return MH3_RES_OK;
}

void mh3_ui_rect(mh3_ui_vertex out[4], float left, float top, float right, float bottom, float depth, const uint8_t color[4], float u0, float v0, float u1, float v1) {
    const float x[4] = {left, right, right, left}, y[4] = {top, top, bottom, bottom};
    const float u[4] = {u0, u1, u1, u0}, v[4] = {v0, v0, v1, v1};

    for (unsigned i = 0; i < 4; ++i) {
        out[i].position[0] = x[i];
        out[i].position[1] = y[i];
        out[i].position[2] = depth;
        out[i].uv[0] = u[i];
        out[i].uv[1] = v[i];
        memcpy(out[i].color, color, 4);
    }
}
