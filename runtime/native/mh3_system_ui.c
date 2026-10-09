#include "mh3_system_ui.h"
#include <math.h>
#include <string.h>
static uint64_t bit(unsigned i) {
    return (uint64_t)1 << i;
}
static void pixel(mh3_native_material_state* m, unsigned field, uint32_t value) {
    m->pixel.known |= bit(field);
    m->pixel.values[field] = value;
}
static void pass(mh3_pack_tev_pass* p, unsigned a, unsigned b, unsigned c, unsigned d) {
    *p = (mh3_pack_tev_pass){a, b, c, d, 0, 0, 0, 1, 0};
}
static void stage(mh3_pack_tev_stage* s, unsigned map, unsigned coord, unsigned channel) {
    s->texture_map = map;
    s->texture_coordinate = coord;
    s->raster_channel = channel;
    s->raster_swap = s->texture_swap = 0;
    s->indirect_stage = s->indirect_format = s->indirect_bias = s->indirect_alpha = s->indirect_matrix = 0;
    s->indirect_wrap_s = s->indirect_wrap_t = s->indirect_original_lod = s->indirect_add_previous = 0;
    s->known |= ((bit(MH3_TEV_TEXTURE_SWAP + 1) - 1) & ~(bit(MH3_TEV_TEXTURE_MAP) - 1)) | ((bit(MH3_TEV_FIELD_COUNT) - 1) & ~(bit(MH3_TEV_INDIRECT_STAGE) - 1));
}
void mh3_system_ui_material(mh3_native_material_state* m, mh3_system_ui_pipeline mode, const uint8_t first[4], const uint8_t second[4]) {
    unsigned i;
    m->channel_count = 1;
    m->texgen_count = mode == MH3_SYSTEM_UI_SOLID ? 0 : 1;
    m->indirect_count = 0;
    m->tev_count = mode == MH3_SYSTEM_UI_COLOR_MAPPING ? 2 : 1;
    m->channel[0].color = (mh3_pack_channel_control){1, 0, 0, 0, 2, 0};
    m->channel[0].alpha = (mh3_pack_channel_control){1, 0, 0, 0, 2, 0};
    m->channel[0].flags |= MH3_CHANNEL_CONTROL_COLOR | MH3_CHANNEL_CONTROL_ALPHA;
    m->channel[1].color = m->channel[1].alpha = (mh3_pack_channel_control){0, 0, 0, 0, 2, 0};
    m->channel[1].flags |= MH3_CHANNEL_CONTROL_COLOR | MH3_CHANNEL_CONTROL_ALPHA;
    m->swaps[0].known |= 15;
    for (i = 0; i < 4; ++i)
        m->swaps[0].rgba[i] = i;
    m->texgen[0].known |= 1u << MH3_TEXGEN_PROJECTION;
    m->texgen[0].values[MH3_TEXGEN_PROJECTION] = 0;
    pixel(m, MH3_PIXEL_BLEND_ENABLE, 1);
    pixel(m, MH3_PIXEL_LOGIC_ENABLE, 0);
    pixel(m, MH3_PIXEL_BLEND_SUBTRACT, 0);
    pixel(m, MH3_PIXEL_BLEND_SOURCE, 4);
    pixel(m, MH3_PIXEL_BLEND_DESTINATION, 5);
    pixel(m, MH3_PIXEL_LOGIC_OPERATION, 15);
    /* Authored GX_TF_Z8 (0x11) becomes semantic depth-texture format zero. */
    pixel(m, MH3_PIXEL_DEPTH_TEXTURE_OPERATION, 0);
    pixel(m, MH3_PIXEL_DEPTH_TEXTURE_FORMAT, 0);
    pixel(m, MH3_PIXEL_DEPTH_TEXTURE_BIAS, 0);
    stage(&m->tev[0], mode == MH3_SYSTEM_UI_SOLID ? 255 : 0, mode == MH3_SYSTEM_UI_SOLID ? 255 : 0, mode == MH3_SYSTEM_UI_COLOR_MAPPING ? 255 : 4);
    if (mode == MH3_SYSTEM_UI_SOLID) {
        pass(&m->tev[0].color, 15, 15, 15, 10);
        pass(&m->tev[0].alpha, 7, 7, 7, 5);
    } else if (mode == MH3_SYSTEM_UI_INTENSITY) {
        pass(&m->tev[0].color, 15, 15, 15, 10);
        pass(&m->tev[0].alpha, 7, 4, 5, 7);
    } else if (mode == MH3_SYSTEM_UI_COLOR_MAPPING) {
        pass(&m->tev[0].color, 2, 4, 8, 15);
        pass(&m->tev[0].alpha, 1, 2, 4, 7);
        stage(&m->tev[1], 255, 255, 4);
        pass(&m->tev[1].color, 15, 0, 10, 15);
        pass(&m->tev[1].alpha, 7, 0, 5, 7);
        m->tev[1].known |= bit(MH3_TEV_ALPHA_OUTPUT + 1) - 1;
        for (i = 0; i < 4; ++i) {
            m->color[1].rgba[i] = first[i];
            m->color[2].rgba[i] = second[i];
        }
        m->color[1].known |= 15;
        m->color[2].known |= 15;
    } else {
        pass(&m->tev[0].color, 15, 8, 10, 15);
        pass(&m->tev[0].alpha, 7, 4, 5, 7);
    }
    m->tev[0].known |= bit(MH3_TEV_ALPHA_OUTPUT + 1) - 1;
}
void mh3_system_ui_quad(mh3_system_ui_vertex out[4], const float origin[3], const float size[2], int y_sign, const float coordinates[8][4][2], uint32_t count, const uint8_t colors[4][4]) {
    static const unsigned corner[4] = {0, 1, 3, 2};
    unsigned i, j;
    memset(out, 0, sizeof(*out) * 4);
    for (i = 0; i < 4; ++i) {
        const unsigned c = corner[i];
        out[i].position[0] = origin[0] + ((c & 1) ? size[0] : 0);
        out[i].position[1] = origin[1] + ((c & 2) ? size[1] * (float)y_sign : 0);
        out[i].position[2] = origin[2];
        for (j = 0; j < 8; ++j) {
            out[i].coordinate[j][2] = 1;
            if (j < count && coordinates)
                memcpy(out[i].coordinate[j], coordinates[j][c], 8);
        }
        if (colors)
            memcpy(out[i].color, colors[c], 4);
        else
            memset(out[i].color, 255, 4);
    }
}
int mh3_system_ui_glyph(mh3_system_ui_vertex out[4], const float origin[3], uint32_t width, uint32_t height, float scale_x, float scale_y, uint32_t tw, uint32_t th, uint32_t x, uint32_t y,
                        const uint8_t colors[4][4]) {
    float size[2] = {(float)width * scale_x, (float)height * scale_y};
    float coords[8][4][2] = {0};
    uint16_t left, top, right, bottom;
    if (!tw || !th)
        return 0;
    left = (uint16_t)((x << 15) / tw);
    top = (uint16_t)((y << 15) / th);
    right = (uint16_t)(((x + width) << 15) / tw);
    bottom = (uint16_t)(((y + height) << 15) / th);
    coords[0][0][0] = coords[0][2][0] = (float)left / 32768.0f;
    coords[0][1][0] = coords[0][3][0] = (float)right / 32768.0f;
    coords[0][0][1] = coords[0][1][1] = (float)top / 32768.0f;
    coords[0][2][1] = coords[0][3][1] = (float)bottom / 32768.0f;
    mh3_system_ui_quad(out, origin, size, 1, coords, 1, colors);
    return 1;
}
mh3_res_status mh3_system_ui_build(const mh3_draw_state* state, const mh3_system_ui_vertex* source, uint32_t count, AuroraNativeMaterialGeometry* out) {
    uint32_t i, j;
    AuroraNativeVertex clip;
    if (!state || !source || !out || !out->vertices || !out->indices || count % 4 || out->vertex_count != count || out->index_count != count / 4 * 6)
        return MH3_RES_INVALID;
    for (i = 0; i < count; ++i) {
        AuroraNativeMaterialVertex* v = out->vertices + i;
        const float* p = source[i].position;
        const float uv[2] = {0, 0};
        mh3_draw_vertex(state, p, uv, source[i].color, &clip);
        memcpy(v->position, clip.position, 16);
        for (j = 0; j < 3; ++j)
            v->view_position[j] = state->position[j * 4] * p[0] + state->position[j * 4 + 1] * p[1] + state->position[j * 4 + 2] * p[2] + state->position[j * 4 + 3];
        v->normal[0] = v->normal[1] = 0;
        v->normal[2] = 1;
        memcpy(v->texcoord, source[i].coordinate, sizeof(v->texcoord));
        memcpy(v->color[0], source[i].color, 4);
        memcpy(v->color[1], source[i].color, 4);
    }
    for (i = 0; i < count / 4; ++i) {
        const uint16_t b = (uint16_t)(i * 4);
        const uint16_t indices[6] = {b, b + 1, b + 2, b, b + 2, b + 3};
        memcpy(out->indices + i * 6, indices, 12);
    }
    return MH3_RES_OK;
}
void mh3_system_ui_concat(float out[12], const float left[12], const float right[12]) {
    float result[12];
    unsigned row, col, k;
    for (row = 0; row < 3; ++row)
        for (col = 0; col < 4; ++col) {
            float v = col == 3 ? left[row * 4 + 3] : 0;
            for (k = 0; k < 3; ++k)
                v += left[row * 4 + k] * right[k * 4 + col];
            result[row * 4 + col] = v;
        }
    memcpy(out, result, sizeof result);
}
int mh3_system_ui_indirect_matrix(const float input[6], mh3_pack_indirect_matrix* out) {
    float v[6], maximum = 0;
    int exponent = 0;
    unsigned i;
    if (!input || !out)
        return 0;
    for (i = 0; i < 6; ++i) {
        v[i] = input[i];
        if (!isfinite(v[i]))
            return 0;
        if (fabsf(v[i]) > maximum)
            maximum = fabsf(v[i]);
    }
    if (maximum >= 1)
        while (maximum >= 1 && exponent <= 45) {
            maximum *= 0.5f;
            for (i = 0; i < 6; ++i)
                v[i] *= 0.5f;
            ++exponent;
        }
    else if (maximum < 0.5f)
        while (maximum < 0.5f && exponent >= -16) {
            maximum *= 2;
            for (i = 0; i < 6; ++i)
                v[i] *= 2;
            --exponent;
        }
    if (maximum >= 1.0f)
        return 0;
    out->known = 127;
    out->scale_exponent = exponent;
    for (i = 0; i < 6; ++i)
        out->coefficients[i] = (int16_t)(v[i] * 1024.0f);
    return 1;
}
static uint32_t u32(const uint8_t* p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static uint16_t u16(const uint8_t* p) {
    return (uint16_t)((unsigned)p[0] << 8 | p[1]);
}
static float f32(const uint8_t* p) {
    uint32_t bits = u32(p);
    float v;
    memcpy(&v, &bits, 4);
    return v;
}
static void put32(uint8_t* p, uint32_t v) {
    p[0] = v >> 24;
    p[1] = v >> 16;
    p[2] = v >> 8;
    p[3] = v;
}
static void put16(uint8_t* p, uint16_t v) {
    p[0] = v >> 8;
    p[1] = v;
}
static void putf(uint8_t* p, float v) {
    uint32_t bits;
    memcpy(&bits, &v, 4);
    put32(p, bits);
}
mh3_res_status mh3_system_ui_texture_lod(mh3_native_texture_binding* b) {
    if (!b || !isfinite(b->source.min_lod) || !isfinite(b->source.max_lod) || !isfinite(b->sampler.lod_bias))
        return MH3_RES_INVALID;
    float low = fmaxf(0, fminf(10, b->source.min_lod)), high = fmaxf(0, fminf(10, b->source.max_lod));
    b->source.min_lod = (float)(uint32_t)(low * 16) / 16;
    b->source.max_lod = (float)(uint32_t)(high * 16) / 16;
    float bias = fmaxf(-4, fminf(3.9900000095367432f, b->sampler.lod_bias));
    b->sampler.lod_bias = (float)(int32_t)(bias * 32) / 32;
    return MH3_RES_OK;
}
mh3_res_status mh3_system_ui_texture_info_read(mh3_res_span info, uint64_t generation, mh3_native_texture_binding* out) {
    const uint8_t* p = info.data;
    uint32_t flags;
    mh3_native_texture_binding b;
    if (!p || info.size < 28 || !out)
        return MH3_RES_INVALID;
    memset(&b, 0, sizeof b);
    flags = u32(p + 24);
    b.source.payload_address = u32(p);
    b.source.width = u16(p + 8);
    b.source.height = u16(p + 10);
    b.source.format = flags >> 28;
    b.source.indexed = b.source.format >= 8 && b.source.format <= 10;
    b.source.min_lod = f32(p + 12);
    b.source.max_lod = f32(p + 16);
    b.source.generation = generation;
    if (!isfinite(b.source.min_lod) || !isfinite(b.source.max_lod))
        return MH3_RES_RANGE;
    b.source.levels = 1;
    b.sampler.wrap_s = (flags >> 25) & 3;
    b.sampler.wrap_t = (flags >> 23) & 3;
    b.sampler.min_filter = (flags >> 20) & 7;
    b.sampler.mag_filter = (flags >> 19) & 1;
    b.sampler.bias_clamp = (flags >> 18) & 1;
    b.sampler.edge_lod = (flags >> 17) & 1;
    b.sampler.anisotropy = (flags >> 15) & 3;
    b.sampler.lod_bias = (float)u16(p + 20) / 256.0f;
    if (!b.source.width || !b.source.height)
        return MH3_RES_RANGE;
    mh3_res_status status = mh3_system_ui_texture_lod(&b);
    if (status)
        return status;
    if (flags & (1u << 27))
        b.source.levels = (uint32_t)b.source.max_lod + 1;
    *out = b;
    return MH3_RES_OK;
}
mh3_res_status mh3_system_ui_texture_info_store(mh3_res_mut_span info, const mh3_native_texture_binding* b) {
    uint8_t* p = info.data;
    uint32_t flags, mask = 0xffff8000u;
    if (!p || info.size < 28 || !b)
        return MH3_RES_INVALID;
    if (!isfinite(b->sampler.lod_bias) || fabsf(b->sampler.lod_bias) > 8388607.0f)
        return MH3_RES_RANGE;
    flags = (b->source.format << 28) | (b->source.levels > 1 ? 1u << 27 : 0) | (b->sampler.wrap_s << 25) | (b->sampler.wrap_t << 23) | (b->sampler.min_filter << 20) | (b->sampler.mag_filter << 19) |
            (b->sampler.bias_clamp << 18) | (b->sampler.edge_lod << 17) | (b->sampler.anisotropy << 15);
    put32(p, b->source.payload_address);
    put16(p + 8, (uint16_t)b->source.width);
    put16(p + 10, (uint16_t)b->source.height);
    putf(p + 12, b->source.min_lod);
    putf(p + 16, b->source.max_lod);
    put16(p + 20, (uint16_t)(int32_t)(b->sampler.lod_bias * 256.0f));
    put32(p + 24, (u32(p + 24) & ~mask) | (flags & mask));
    return MH3_RES_OK;
}
static uint32_t method(const mh3_system_ui_callbacks* io, uint32_t object, uint32_t slot) {
    return io->method(io->user, object, slot);
}
static uint32_t child(const mh3_system_ui_callbacks* io, uint32_t self, uint32_t offset) {
    return io->field(io->user, self, offset);
}
static int show_background(const mh3_system_ui_callbacks* io, uint32_t self) {
    return method(io, child(io, self, 4), 0xf0) != 1 && method(io, child(io, self, 4), 0xf0) != 2;
}
void mh3_system_ui_save_draw(const mh3_system_ui_callbacks* io, uint32_t self, unsigned variant) {
    uint32_t object;
    if (variant == 0) {
        method(io, child(io, self, 0x24), 0x98);
        method(io, child(io, self, 0x1c), 0x148);
        if (!method(io, child(io, self, 0x24), 0x10))
            method(io, child(io, self, 0x18), 0x3c);
        else
            method(io, child(io, self, 0x14), 0x110);
        method(io, child(io, self, 0x20), 0x24);
        if (method(io, child(io, self, 0x28), 0xb8))
            method(io, child(io, self, 0x28), 0xa4);
        if (method(io, child(io, self, 0x2c), 0xd8))
            method(io, child(io, self, 0x2c), 0xd0);
        return;
    }
    if (show_background(io, self))
        method(io, method(io, self, 0x54), 0x88);
    method(io, method(io, self, 0x4c), 0x98);
    if (show_background(io, self))
        method(io, method(io, self, 0x34), 0x148);
    if (method(io, method(io, self, 0x48), 0x100))
        method(io, method(io, self, 0x48), 0x24);
    object = method(io, self, 0x4c);
    if (!method(io, object, 0x10))
        method(io, method(io, self, 0x40), 0x3c);
    else
        method(io, method(io, self, 0x3c), 0x110);
    if (variant == 2) {
        if (method(io, method(io, self, 0x50), 0xb8))
            method(io, method(io, self, 0x50), 0xa4);
        if (method(io, method(io, self, 0x44), 0xd8))
            method(io, method(io, self, 0x44), 0xd0);
    }
}
static uint32_t texture_bytes(uint32_t a, int hbm) {
    return (a >> 28) * (hbm ? 32u : 28u);
}
static uint32_t optional(uint32_t a, unsigned flag) {
    return ((a >> flag) & 1) * 4;
}
mh3_res_status mh3_system_ui_layout_size(uint32_t a, int hbm, uint32_t* out) {
    uint32_t size = texture_bytes(a, hbm) + ((a >> 24) & 15) * 20 + ((a >> 18) & 60);
    size += optional(a, 8) + optional(a, 7) + optional(a, 14) + optional(a, 6) + optional(a, 5);
    size += ((a >> 13) & 28) + ((a >> 18) & 3) * 20 + ((a >> 9) & 31) * 16;
    if (!out)
        return MH3_RES_INVALID;
    *out = size;
    return MH3_RES_OK;
}
static void blend(mh3_native_material_state* m, const uint8_t* p) {
    unsigned mode = p ? p[0] : 1;
    pixel(m, MH3_PIXEL_BLEND_ENABLE, mode == 1 || mode == 3);
    pixel(m, MH3_PIXEL_BLEND_SUBTRACT, mode == 3);
    pixel(m, MH3_PIXEL_LOGIC_ENABLE, mode == 2);
    pixel(m, MH3_PIXEL_BLEND_SOURCE, p ? p[1] : 4);
    pixel(m, MH3_PIXEL_BLEND_DESTINATION, p ? p[2] : 5);
    pixel(m, MH3_PIXEL_LOGIC_OPERATION, p ? p[3] : 15);
}
static void fallback(mh3_native_material_state* m, unsigned index, unsigned map, unsigned coord, unsigned channel, unsigned ca, unsigned cb, unsigned cc, unsigned cd, unsigned aa, unsigned ab,
                     unsigned ac, unsigned ad) {
    mh3_pack_tev_stage* s = &m->tev[index];
    stage(s, map, coord, channel);
    pass(&s->color, ca, cb, cc, cd);
    pass(&s->alpha, aa, ab, ac, ad);
    s->known |= bit(MH3_TEV_ALPHA_OUTPUT + 1) - 1;
}
mh3_res_status mh3_system_ui_layout_material(mh3_native_material_state* destination, mh3_res_span header, mh3_res_span storage, int hbm, int vertex_colors, uint8_t alpha, const uint8_t kc[8],
                                             const uint8_t ka[8], mh3_system_ui_layout* out) {
    mh3_native_material_state m;
    mh3_system_ui_layout plan;
    unsigned i, j;
    uint32_t a, b, at, ctrl, mat, swap, alph, bl, ind, indmat, tev, size;
    uint32_t color_offset = hbm ? 0x24 : 0x10, flags = hbm ? 0x4c : 0x38;
    const uint8_t *p = header.data, *data = storage.data;
    int any_vertex, register_color, modulate;
    if (!destination || !out || !p || header.size < flags + 8 || !kc || !ka)
        return MH3_RES_INVALID;
    a = u32(p + flags);
    b = u32(p + flags + 4);
    mh3_system_ui_layout_size(a, hbm, &size);
    if (size > storage.size || (size && !data))
        return MH3_RES_RANGE;
    memset(&plan, 0, sizeof plan);
    plan.texture_count = b >> 28;
    plan.texture_stride = hbm ? 32 : 28;
    plan.storage_size = size;
    plan.texture_srt_count = (b >> 24) & 15;
    plan.indirect_srt_count = (b >> 18) & 3;
    plan.texgen_count = (b >> 20) & 15;
    if (plan.texture_count > 8 || plan.texgen_count > 8 || ((b >> 9) & 31) > 16 || ((b >> 15) & 7) > 4 || plan.texture_count > a >> 28 || plan.texture_srt_count > ((a >> 24) & 15) ||
        plan.texgen_count > ((a >> 20) & 15) || ((b >> 9) & 31) > ((a >> 9) & 31) || ((b >> 15) & 7) > ((a >> 15) & 7) || plan.indirect_srt_count > ((a >> 18) & 3))
        return MH3_RES_RANGE;
    plan.srt_offset = texture_bytes(a, hbm);
    plan.texgen_offset = plan.srt_offset + ((a >> 24) & 15) * 20;
    at = plan.texgen_offset + ((a >> 18) & 60);
    ctrl = at;
    at += optional(a, 8);
    mat = at;
    at += optional(a, 7);
    swap = at;
    at += optional(a, 14);
    alph = at;
    at += optional(a, 6);
    bl = at;
    at += optional(a, 5);
    ind = at;
    at += ((a >> 13) & 28);
    indmat = at;
    at += ((a >> 18) & 3) * 20;
    tev = at;
    plan.indirect_srt_offset = indmat;
    m = *destination;
    m.channel_count = 1;
    m.channel[0].color = (mh3_pack_channel_control){(a & (1u << 8)) ? data[ctrl] : 1, 0, 0, 0, 2, 0};
    m.channel[0].alpha = (mh3_pack_channel_control){(a & (1u << 8)) ? data[ctrl + 1] : 1, 0, 0, 0, 2, 0};
    m.channel[0].flags |= MH3_CHANNEL_CONTROL_COLOR | MH3_CHANNEL_CONTROL_ALPHA;
    if (m.channel[0].color.material_source > 1 || m.channel[0].alpha.material_source > 1)
        return MH3_RES_RANGE;
    any_vertex = m.channel[0].color.material_source == 1 || m.channel[0].alpha.material_source == 1;
    register_color = m.channel[0].color.material_source == 0 || m.channel[0].alpha.material_source == 0;
    modulate = any_vertex && vertex_colors;
    if (register_color) {
        unsigned white = 1;
        for (i = 0; i < 4; ++i) {
            unsigned c = (a & (1u << 7)) ? data[mat + i] : 255;
            if (i == 3)
                c = c * alpha / 255;
            m.channel[0].material_color[i] = c;
            if (c != 255)
                white = 0;
        }
        m.channel[0].flags |= MH3_CHANNEL_MATERIAL_COLOR | MH3_CHANNEL_MATERIAL_ALPHA;
        modulate |= !white;
    }
    m.texgen_count = plan.texgen_count;
    for (i = 0; i < plan.texgen_count; ++i) {
        const uint8_t* r = data + plan.texgen_offset + i * 4;
        unsigned type = r[0], selector = r[2];
        if (type > 1 || selector < 30 || selector > 60 || (selector - 30) % 3 || r[1] > 11)
            return MH3_RES_UNSUPPORTED;
        plan.texgen_type[i] = type;
        plan.texgen_source[i] = r[1];
        plan.texgen_selector[i] = selector;
        m.texgen[i].known |= 1u << MH3_TEXGEN_PROJECTION;
        m.texgen[i].values[MH3_TEXGEN_PROJECTION] = type == 0;
        if (type == 1 && selector != 60) {
            if ((selector - 30) / 3 >= 10)
                return MH3_RES_RANGE;
            plan.texture_matrices[(selector - 30) / 3] = 1;
        }
    }
    for (i = 0; i < 3; ++i) {
        m.color[i + 1].known |= 15;
        for (j = 0; j < 4; ++j)
            m.color[i + 1].rgba[j] = (int16_t)u16(p + color_offset + i * 8 + j * 2);
    }
    for (i = 0; i < 4; ++i) {
        m.konst[i].known |= 15;
        for (j = 0; j < 4; ++j)
            m.konst[i].rgba[j] = p[color_offset + 24 + i * 4 + j];
    }
    for (i = 0; i < 4; ++i) {
        m.swaps[i].known |= 15;
        for (j = 0; j < 4; ++j)
            m.swaps[i].rgba[j] = (a & (1u << 14)) ? (data[swap + i] >> (j * 2)) & 3 : (i == 0 ? j : (j == 3 ? 3 : i - 1));
    }
    m.tev_count = (b >> 9) & 31;
    if (m.tev_count) {
        for (i = 0; i < m.tev_count; ++i) {
            const uint8_t* r = data + tev + i * 16;
            mh3_pack_tev_stage* s = &m.tev[i];
            stage(s, (r[3] & 1) ? 255 : r[2], r[0], r[1]);
            s->raster_swap = (r[3] >> 1) & 3;
            s->texture_swap = (r[3] >> 3) & 3;
            s->color = (mh3_pack_tev_pass){r[4] & 15, r[4] >> 4, r[5] & 15, r[5] >> 4, r[6] & 15, (r[6] >> 4) & 3, r[6] >> 6, r[7] & 1, (r[7] >> 1) & 3};
            s->alpha = (mh3_pack_tev_pass){r[8] & 15, r[8] >> 4, r[9] & 15, r[9] >> 4, r[10] & 15, (r[10] >> 4) & 3, r[10] >> 6, r[11] & 1, (r[11] >> 1) & 3};
            s->konst_color = r[7] >> 3;
            s->konst_alpha = r[11] >> 3;
            s->indirect_stage = r[12];
            s->indirect_format = r[15] & 3;
            s->indirect_bias = r[13] & 7;
            s->indirect_matrix = (r[13] >> 3) & 15;
            s->indirect_wrap_s = r[14] & 7;
            s->indirect_wrap_t = (r[14] >> 3) & 7;
            s->indirect_original_lod = (r[15] >> 2) & 1;
            s->indirect_add_previous = (r[15] >> 3) & 1;
            s->indirect_alpha = (r[15] >> 4) & 3;
            s->known = bit(MH3_TEV_FIELD_COUNT) - 1;
            if (s->indirect_matrix >= 1 && s->indirect_matrix <= 3)
                plan.indirect_matrices[s->indirect_matrix - 1] = 1;
        }
        modulate = 1;
    } else {
        unsigned n = plan.texture_count;
        if (n == 0) {
            fallback(&m, 0, 255, 255, 4, 15, 4, 10, 15, 7, 2, 5, 7);
            m.tev_count = 1;
            modulate = 1;
        } else if (n == 1) {
            fallback(&m, 0, 0, 0, 255, 2, 4, 8, 15, 1, 2, 4, 7);
            m.tev_count = 1;
        } else {
            if (n == 2) {
                fallback(&m, 0, 0, 0, 255, 15, 15, 15, 8, 7, 7, 7, 4);
                fallback(&m, 1, 1, 1, 255, 8, 0, 14, 15, 4, 0, 6, 7);
                m.tev[1].konst_color = kc[0];
                m.tev[1].konst_alpha = ka[0];
                m.tev[1].known |= bit(MH3_TEV_KONST_COLOR) | bit(MH3_TEV_KONST_ALPHA);
            } else
                for (i = 0; i < n; ++i) {
                    fallback(&m, i, i, i, 255, 15, 8, 14, i ? 0 : 15, 7, 4, 6, i ? 0 : 7);
                    m.tev[i].konst_color = kc[i];
                    m.tev[i].konst_alpha = ka[i];
                    m.tev[i].known |= bit(MH3_TEV_KONST_COLOR) | bit(MH3_TEV_KONST_ALPHA);
                }
            m.tev_count = n;
            for (i = 0; i < 4; ++i)
                if (m.color[1].rgba[i] != 0 || m.color[2].rgba[i] != 255)
                    break;
            if (i != 4) {
                fallback(&m, m.tev_count++, 255, 255, 255, 2, 4, 0, 15, 1, 2, 0, 7);
            }
        }
        if (n && modulate)
            fallback(&m, m.tev_count++, 255, 255, 4, 15, 0, 10, 15, 7, 0, 5, 7);
    }
    m.indirect_count = (b >> 15) & 7;
    for (i = 0; i < m.indirect_count; ++i) {
        const uint8_t* r = data + ind + i * 4;
        m.indirect[i] = (mh3_pack_indirect_stage){15, r[0], r[1], r[2], r[3]};
    }
    pixel(&m, MH3_PIXEL_ALPHA_COMPARE0, (a & (1u << 6)) ? data[alph] & 15 : 7);
    pixel(&m, MH3_PIXEL_ALPHA_COMPARE1, (a & (1u << 6)) ? data[alph] >> 4 : 7);
    pixel(&m, MH3_PIXEL_ALPHA_OPERATION, (a & (1u << 6)) ? data[alph + 1] : 0);
    pixel(&m, MH3_PIXEL_ALPHA_REFERENCE0, (a & (1u << 6)) ? data[alph + 2] : 0);
    pixel(&m, MH3_PIXEL_ALPHA_REFERENCE1, (a & (1u << 6)) ? data[alph + 3] : 0);
    blend(&m, (a & (1u << 5)) ? data + bl : 0);
    plan.vertex_colors = any_vertex;
    plan.modulation = modulate;
    *destination = m;
    *out = plan;
    return MH3_RES_OK;
}
mh3_res_status mh3_system_ui_coordinates(mh3_system_ui_vertex* vertices, uint32_t count, const mh3_system_ui_layout* plan, const float matrices[10][12]) {
    unsigned i, j, k;
    float source[8][3], input[3];
    if (!vertices || !plan || !matrices)
        return MH3_RES_INVALID;
    for (i = 0; i < count; ++i) {
        memcpy(source, vertices[i].coordinate, sizeof source);
        for (j = 0; j < plan->texgen_count; ++j) {
            unsigned from = plan->texgen_source[j], selector = plan->texgen_selector[j];
            if (from == 0)
                memcpy(input, vertices[i].position, sizeof input);
            else if (from >= 4 && from <= 11) {
                input[0] = source[from - 4][0];
                input[1] = source[from - 4][1];
                input[2] = 1;
            } else
                return MH3_RES_UNSUPPORTED;
            if (selector == 60) {
                memcpy(vertices[i].coordinate[j], input, sizeof input);
            } else {
                const float* m = matrices[(selector - 30) / 3];
                for (k = 0; k < 3; ++k)
                    vertices[i].coordinate[j][k] = m[k * 4] * input[0] + m[k * 4 + 1] * input[1] + m[k * 4 + 2] * input[2] + m[k * 4 + 3];
            }
            if (plan->texgen_type[j] == 1)
                vertices[i].coordinate[j][2] = 1;
        }
    }
    return MH3_RES_OK;
}
void mh3_system_ui_debug_material(mh3_native_material_state* m, int blending) {
    m->channel_count = 1;
    m->texgen_count = 0;
    m->tev_count = 1;
    m->cull_mode = 0;
    m->channel[0].color = m->channel[0].alpha = (mh3_pack_channel_control){1, 1, 0, 0, 2, 0};
    m->channel[0].flags |= MH3_CHANNEL_CONTROL_COLOR | MH3_CHANNEL_CONTROL_ALPHA;
    mh3_pack_tev_stage* t = &m->tev[0];
    pass(&t->color, 15, 15, 15, 10);
    pass(&t->alpha, 7, 7, 7, 5);
    t->texture_map = t->texture_coordinate = 255;
    t->raster_channel = 4;
    t->known |= (bit(MH3_TEV_ALPHA_OUTPUT + 1) - 1) | bit(MH3_TEV_TEXTURE_MAP) | bit(MH3_TEV_TEXTURE_COORDINATE) | bit(MH3_TEV_RASTER_CHANNEL);
    const uint8_t modes[4] = {blending ? 1 : 0, blending ? 4 : 0, blending ? 5 : 0, blending ? 15 : 5};
    blend(m, modes);
}
void mh3_system_ui_fade_material(mh3_native_material_state* m) {
    unsigned i;
    m->cull_mode = 0;
    m->channel_count = 1;
    m->texgen_count = 0;
    m->tev_count = 1;
    m->indirect_count = 0;
    for (i = 0; i < 2; ++i) {
        m->channel[i].color = m->channel[i].alpha = (mh3_pack_channel_control){0, 0, 0, 0, 2, 0};
        m->channel[i].flags |= MH3_CHANNEL_CONTROL_COLOR | MH3_CHANNEL_CONTROL_ALPHA;
    }
    mh3_pack_tev_stage* s = &m->tev[0];
    s->texture_map = s->texture_coordinate = s->raster_channel = 255;
    s->raster_swap = s->texture_swap = 0;
    pass(&s->color, 15, 15, 15, 2);
    pass(&s->alpha, 7, 7, 7, 1);
    s->known |= (bit(MH3_TEV_ALPHA_OUTPUT + 1) - 1) | bit(MH3_TEV_TEXTURE_MAP) | bit(MH3_TEV_TEXTURE_COORDINATE) | bit(MH3_TEV_RASTER_CHANNEL) | bit(MH3_TEV_RASTER_SWAP) | bit(MH3_TEV_TEXTURE_SWAP);
    m->swaps[0].known |= 15;
    for (i = 0; i < 4; ++i)
        m->swaps[0].rgba[i] = i;
    const uint8_t modes[4] = {1, 4, 5, 5};
    blend(m, modes);
    pixel(m, MH3_PIXEL_ALPHA_COMPARE0, 7);
    pixel(m, MH3_PIXEL_ALPHA_COMPARE1, 7);
    pixel(m, MH3_PIXEL_ALPHA_REFERENCE0, 0);
    pixel(m, MH3_PIXEL_ALPHA_REFERENCE1, 0);
    pixel(m, MH3_PIXEL_ALPHA_OPERATION, 1);
    pixel(m, MH3_PIXEL_ALPHA_WRITE, 0);
    pixel(m, MH3_PIXEL_DEPTH_TEST, 0);
    pixel(m, MH3_PIXEL_DEPTH_COMPARE, 7);
    pixel(m, MH3_PIXEL_DEPTH_WRITE, 0);
}
mh3_res_status mh3_system_ui_lines(const mh3_draw_state* state, const mh3_system_ui_vertex* source, uint32_t count, float width, AuroraNativeMaterialGeometry* out) {
    uint32_t vc, ic, i, j;
    mh3_ui_vertex points[5];
    AuroraNativeVertex clip[16];
    uint16_t indices[24];
    if (!state || !source || !out || count < 2 || count > 5)
        return MH3_RES_INVALID;
    mh3_res_status status = mh3_ui_geometry_counts(MH3_UI_LINE_STRIP, count, &vc, &ic);
    if (status != MH3_RES_OK)
        return status;
    if (out->vertex_count != vc || out->index_count != ic || !out->vertices || !out->indices)
        return MH3_RES_INVALID;
    for (i = 0; i < count; ++i) {
        memcpy(points[i].position, source[i].position, 12);
        memcpy(points[i].color, source[i].color, 4);
        points[i].uv[0] = points[i].uv[1] = 0;
    }
    AuroraNativeGeometry g = {clip, indices, vc, ic, 0, 0};
    status = mh3_ui_build_vertices(state, MH3_UI_LINE_STRIP, points, count, width, &g);
    if (status != MH3_RES_OK)
        return status;
    for (i = 0; i < vc; ++i) {
        const unsigned endpoint = i / 4 + ((i % 4 == 1 || i % 4 == 2) ? 1 : 0);
        AuroraNativeMaterialVertex* v = out->vertices + i;
        const float* p = source[endpoint].position;
        memcpy(v->position, clip[i].position, 16);
        for (j = 0; j < 3; ++j)
            v->view_position[j] = state->position[j * 4] * p[0] + state->position[j * 4 + 1] * p[1] + state->position[j * 4 + 2] * p[2] + state->position[j * 4 + 3];
        v->normal[0] = v->normal[1] = 0;
        v->normal[2] = 1;
        memcpy(v->texcoord, source[endpoint].coordinate, sizeof v->texcoord);
        memcpy(v->color[0], clip[i].color, 4);
        memcpy(v->color[1], clip[i].color, 4);
    }
    memcpy(out->indices, indices, ic * 2);
    return MH3_RES_OK;
}
void mh3_system_ui_projection_record(const mh3_draw_state* state, float out[7]) {
    const float* m = state->projection;
    out[0] = state->perspective ? 0 : 1;
    out[1] = m[0];
    out[2] = m[state->perspective ? 2 : 3];
    out[3] = m[5];
    out[4] = m[state->perspective ? 6 : 7];
    out[5] = m[10];
    out[6] = m[11];
}
int mh3_system_ui_panel_view(const float bounds[4], const float panel[4], float screen_width, const float saved[6], float projection[16], float viewport[6], int32_t scissor[4]) {
    float width = panel[2] - panel[0], scale = screen_width / width;
    if (!isfinite(scale) || width == 0 || !mh3_draw_ortho(projection, 0, bounds[1] - bounds[3], -50, 50 + (bounds[2] - bounds[0]), -1, 1))
        return 0;
    float x = bounds[0] + width * .5f - 50;
    viewport[0] = x + x * (scale - 1);
    viewport[1] = bounds[3] + (panel[3] - panel[1]) * .5f;
    viewport[2] = scale * (100 + bounds[2] - bounds[0]);
    viewport[3] = bounds[1] - bounds[3];
    viewport[4] = saved[4];
    viewport[5] = saved[5];
    for (unsigned i = 0; i < 4; ++i) {
        if (!isfinite(viewport[i]) || viewport[i] < 0 || viewport[i] > 4294967295.0)
            return 0;
        uint32_t v = (uint32_t)viewport[i];
        if (i >= 2 && v > 1500)
            v = 1500;
        if (v > INT32_MAX)
            return 0;
        scissor[i] = (int32_t)v;
    }
    return 1;
}
mh3_res_status mh3_system_ui_hbm_capacity(mh3_res_span resource, mh3_system_ui_capacity* out) {
    if (!resource.data || resource.size < 64 || !out)
        return MH3_RES_INVALID;
    uint32_t a = u32(resource.data + 60);
    mh3_system_ui_capacity c = {0};
    c.resource_flags = a;
    c.texture_count = (a & 15) > 8 ? 8 : a & 15;
    c.srt_count = ((a >> 4) & 15) > 10 ? 10 : (a >> 4) & 15;
    c.texgen_count = ((a >> 8) & 15) > 8 ? 8 : (a >> 8) & 15;
    c.tev_count = ((a >> 18) & 31) > 16 ? 16 : (a >> 18) & 31;
    c.indirect_count = ((a >> 15) & 7) > 4 ? 4 : (a >> 15) & 7;
    c.indirect_srt_count = (a >> 13) & 3;
    c.swap = (a >> 12) & 1;
    c.channel = (a >> 25) & 1;
    c.material_color = (a >> 27) & 1;
    c.alpha = (a >> 23) & 1;
    c.blend = (a >> 24) & 1;
    *out = c;
    return MH3_RES_OK;
}
static int copy_part(mh3_res_mut_span storage, uint32_t offset, mh3_res_span resource, uint32_t start, uint32_t bytes) {
    if (offset > storage.size || bytes > storage.size - offset || start > resource.size || bytes > resource.size - start)
        return 0;
    if (bytes)
        memmove(storage.data + offset, resource.data + start, bytes);
    return 1;
}
mh3_res_status mh3_system_ui_hbm_construct(mh3_res_mut_span header, mh3_res_mut_span storage, mh3_res_span resource, const mh3_system_ui_capacity* c) {
    uint32_t a, b, at, src, texgen, ctrl, mat, swap, alph, bl, ind, indmat, tev;
    unsigned i;
    if (!header.data || header.size < 0x5c || !c || !resource.data || resource.size < 64)
        return MH3_RES_INVALID;
    a = u32(header.data + 0x4c);
    b = u32(header.data + 0x50);
    at = texture_bytes(a, 1);
    texgen = at + ((a >> 24) & 15) * 20;
    ctrl = texgen + ((a >> 18) & 60);
    mat = ctrl + optional(a, 8);
    swap = mat + optional(a, 7);
    alph = swap + optional(a, 14);
    bl = alph + optional(a, 6);
    ind = bl + optional(a, 5);
    indmat = ind + ((a >> 13) & 28);
    tev = indmat + ((a >> 18) & 3) * 20;
    uint32_t required = 64 + (c->resource_flags & 15) * 4 + ((c->resource_flags >> 4) & 15) * 20 + ((c->resource_flags >> 8) & 15) * 4;
    required +=
        (c->channel + c->material_color + c->swap + c->alpha + c->blend) * 4 + ((c->resource_flags >> 13) & 3) * 20 + ((c->resource_flags >> 15) & 7) * 4 + ((c->resource_flags >> 18) & 31) * 16;
    uint32_t size;
    mh3_system_ui_layout_size(a, 1, &size);
    if (resource.size < required || storage.size < size || (size && !storage.data))
        return MH3_RES_RANGE;
    for (i = 0; i < 20; ++i) {
        header.data[4 + i] = resource.data[i];
        if (!resource.data[i]) {
            memset(header.data + 4 + i, 0, 20 - i);
            break;
        }
    }
    memcpy(header.data + 0x24, resource.data + 20, 40);
    src = 64 + (c->resource_flags & 15) * 4;
    if (!copy_part(storage, at, resource, src, c->srt_count * 20))
        return MH3_RES_RANGE;
    src += ((c->resource_flags >> 4) & 15) * 20;
    if (!copy_part(storage, texgen, resource, src, c->texgen_count * 4))
        return MH3_RES_RANGE;
    src += ((c->resource_flags >> 8) & 15) * 4;
    if (c->channel) {
        copy_part(storage, ctrl, resource, src, 4);
        src += 4;
    }
    if (c->material_color) {
        copy_part(storage, mat, resource, src, 4);
        src += 4;
    }
    if (c->swap) {
        copy_part(storage, swap, resource, src, 4);
        src += 4;
    }
    copy_part(storage, indmat, resource, src, c->indirect_srt_count * 20);
    src += ((c->resource_flags >> 13) & 3) * 20;
    copy_part(storage, ind, resource, src, c->indirect_count * 4);
    src += ((c->resource_flags >> 15) & 7) * 4;
    copy_part(storage, tev, resource, src, c->tev_count * 16);
    src += ((c->resource_flags >> 18) & 31) * 16;
    if (c->alpha) {
        copy_part(storage, alph, resource, src, 4);
        src += 4;
    }
    if (c->blend)
        copy_part(storage, bl, resource, src, 4);
    b = (b & 0x0000001f) | (c->texture_count << 28) | (c->srt_count << 24) | (c->texgen_count << 20) | (c->indirect_srt_count << 18) | (c->indirect_count << 15) | (c->tev_count << 9);
    put32(header.data + 0x50, b);
    /* Reserve initialized newly admitted objects. Texture constructors publish
     * the objects and retained palette user data through the adapter. */
    for (i = 0; i < c->texture_count; ++i)
        memset(storage.data + i * 32, 0, 32);
    return MH3_RES_OK;
}
uint32_t mh3_system_ui_panel_selection(int32_t current, int32_t low, int32_t high, int32_t pivot) {
    if (current < low || current > high)
        return UINT32_MAX;
    int64_t difference = (int64_t)current - pivot;
    difference %= 20;
    if (difference < 0)
        difference += 20;
    return (uint32_t)difference;
}
void mh3_system_ui_transform(float out[3], const float matrix[12], const float input[3]) {
    float result[3];
    for (unsigned i = 0; i < 3; ++i)
        result[i] = matrix[i * 4] * input[0] + matrix[i * 4 + 1] * input[1] + matrix[i * 4 + 2] * input[2] + matrix[i * 4 + 3];
    memcpy(out, result, 12);
}
int mh3_system_ui_hud_rectangle(mh3_system_ui_vertex out[4], float left, float top, float width, float height, const uint8_t rgba[4], float u0, float v0, float u1, float v1) {
    float origin[3] = {left, top, 0}, size[2] = {width, height}, uv[8][4][2] = {0};
    uint8_t colors[4][4];
    unsigned i;
    for (i = 0; i < 4; ++i) {
        memcpy(colors[i], rgba, 4);
        uv[0][i][0] = (i & 1) ? u1 : u0;
        uv[0][i][1] = (i & 2) ? v1 : v0;
    }
    mh3_system_ui_quad(out, origin, size, 1, uv, 1, colors);
    for (i = 0; i < 4; ++i)
        for (unsigned j = 0; j < 2; ++j) {
            if (!isfinite(out[i].position[j]) || fabsf(out[i].position[j]) >= 2147483648.0)
                return 0;
            out[i].position[j] = (int16_t)(int32_t)out[i].position[j];
        }
    return 1;
}
mh3_res_status mh3_system_ui_hud_background(uint32_t type, int wide, float aw, float ah, const int16_t atlas[4][2], const uint8_t color[4], mh3_system_ui_hud_quad out[5], uint32_t* count) {
    const uint8_t white[4] = {255, 255, 255, 255};
    if (!out || !count || !atlas || !color || !isfinite(aw) || !isfinite(ah) || aw <= 0 || ah <= 0)
        return MH3_RES_INVALID;
    if (!mh3_system_ui_hud_rectangle(out[0].vertex, wide ? 0 : -107, 0, 854, 480, white, 0, 0, 1, 360 / ah))
        return MH3_RES_RANGE;
    out[0].blend_mode = 1;
    *count = 1;
    if (type < 1 || type > 4)
        return MH3_RES_OK;
    float x = 160 + (wide ? 107 : 0), y = 184;
    for (unsigned i = 0; i < 4; ++i) {
        float left = i == 0 ? x : i == 1 ? x - 44 : i == 2 ? x + 320 - 44 : x + 160 - 44, width = i == 0 ? 320 : 88;
        if (!mh3_system_ui_hud_rectangle(out[i + 1].vertex, left, y, width, 112, i == 0 ? color : white, (float)atlas[i][0] / aw, (float)atlas[i][1] / ah, ((float)atlas[i][0] + width) / aw,
                                         ((float)atlas[i][1] + 112) / ah))
            return MH3_RES_RANGE;
        out[i + 1].blend_mode = i == 0 ? 3 : 1;
    }
    *count = 5;
    return MH3_RES_OK;
}
void mh3_system_ui_hud_material(mh3_native_material_state* m) {
    m->channel_count = m->texgen_count = m->tev_count = 1;
    m->cull_mode = 0;
    m->channel[0].color = m->channel[0].alpha = (mh3_pack_channel_control){1, 0, 0, 0, 2, 0};
    m->channel[0].flags |= MH3_CHANNEL_CONTROL_COLOR | MH3_CHANNEL_CONTROL_ALPHA;
    m->texgen[0].known |= 1u << MH3_TEXGEN_PROJECTION;
    m->texgen[0].values[MH3_TEXGEN_PROJECTION] = 0;
    mh3_pack_tev_stage* t = &m->tev[0];
    unsigned rs = t->raster_swap, ts = t->texture_swap;
    uint64_t known = t->known;
    fallback(m, 0, 0, 0, 4, 15, 8, 10, 15, 7, 4, 5, 7);
    t->raster_swap = rs;
    t->texture_swap = ts;
    t->known = (t->known & ~(bit(MH3_TEV_RASTER_SWAP) | bit(MH3_TEV_TEXTURE_SWAP))) | (known & (bit(MH3_TEV_RASTER_SWAP) | bit(MH3_TEV_TEXTURE_SWAP)));
    const uint8_t modes[4] = {1, 4, 5, 0};
    blend(m, modes);
    pixel(m, MH3_PIXEL_COLOR_WRITE, 1);
    pixel(m, MH3_PIXEL_DEPTH_TEST, 1);
    pixel(m, MH3_PIXEL_DEPTH_COMPARE, 7);
    pixel(m, MH3_PIXEL_DEPTH_WRITE, 0);
}
mh3_res_status mh3_system_ui_text_lines(const mh3_system_ui_text_callbacks* io, uint32_t cursor, uint32_t count, float left, float width, float alignment, float wrap_width) {
    if (!io || !io->measure || !io->draw || !io->newline || !isfinite(left) || !isfinite(width) || !isfinite(alignment) || !isfinite(wrap_width))
        return MH3_RES_INVALID;
    while (count) {
        float measured;
        uint8_t line_break;
        uint32_t consumed = io->measure(io->user, cursor, count, wrap_width, &measured, &line_break);
        if (!consumed || consumed > count || consumed > UINT32_MAX / 2 || !isfinite(measured) || cursor > UINT32_MAX - consumed * 2)
            return MH3_RES_RANGE;
        io->draw(io->user, cursor, consumed, left + alignment * (width - measured));
        if (line_break)
            io->newline(io->user);
        cursor += consumed * 2;
        count -= consumed;
    }
    return MH3_RES_OK;
}
void mh3_system_ui_application_pipeline(mh3_native_material_state* m, int textured) {
    mh3_pack_tev_stage* t = &m->tev[0];
    m->texgen_count = textured ? 1 : 0;
    m->tev_count = 1;
    t->texture_map = t->texture_coordinate = textured ? 0 : 255;
    t->raster_channel = 4;
    if (textured) {
        m->texgen[0].known |= 1u << MH3_TEXGEN_PROJECTION;
        m->texgen[0].values[MH3_TEXGEN_PROJECTION] = 0;
    }
    pass(&t->color, 15, textured ? 8 : 15, textured ? 10 : 15, textured ? 15 : 10);
    pass(&t->alpha, 7, textured ? 4 : 7, textured ? 5 : 7, textured ? 7 : 5);
    t->known |= (bit(MH3_TEV_ALPHA_OUTPUT + 1) - 1) | bit(MH3_TEV_TEXTURE_MAP) | bit(MH3_TEV_TEXTURE_COORDINATE) | bit(MH3_TEV_RASTER_CHANNEL);
}
void mh3_system_ui_direct_stages(mh3_native_material_state* m, const uint32_t* indices, uint32_t count) {
    m->tev_count = count;
    m->indirect_count = 0;
    m->swaps[0].known |= 15;
    for (unsigned i = 0; i < 4; ++i)
        m->swaps[0].rgba[i] = i;
    for (unsigned i = 0; i < count; ++i) {
        mh3_pack_tev_stage* t = &m->tev[indices[i]];
        t->raster_swap = t->texture_swap = 0;
        t->indirect_stage = t->indirect_format = t->indirect_bias = t->indirect_alpha = t->indirect_matrix = t->indirect_wrap_s = t->indirect_wrap_t = t->indirect_original_lod =
            t->indirect_add_previous = 0;
        t->known |= bit(MH3_TEV_RASTER_SWAP) | bit(MH3_TEV_TEXTURE_SWAP) | ((bit(MH3_TEV_FIELD_COUNT) - 1) & ~(bit(MH3_TEV_INDIRECT_STAGE) - 1));
    }
}
