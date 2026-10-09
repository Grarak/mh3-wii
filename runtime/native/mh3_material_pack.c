#include "mh3_model_state.h"
#include <math.h>
#include <string.h>

static uint32_t word(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t wide(const uint8_t *p) {
    return (uint64_t)word(p) | (uint64_t)word(p + 4) << 32;
}

static int32_t signed_value(uint32_t x) {
    return (int32_t)(x & 0x80000000u ? (int64_t)x - INT64_C(4294967296) : (int64_t)x);
}

static float real(const uint8_t *p) {
    uint32_t x = word(p);
    float f;
    memcpy(&f, &x, 4);
    return f;
}

static int zero(const uint8_t *p, size_t n) {
    size_t i;

    for (i = 0; i < n; i++)
        if (p[i])
            return 0;
    return 1;
}

static const uint8_t *slice(const mh3_pack_native_material *m, size_t at, size_t n) {
    return m && m->bytes.data && at <= m->bytes.size && n <= m->bytes.size - at ? m->bytes.data + at : NULL;
}

mh3_pack_status mh3_mesh_pack_texture_srt(const mh3_pack_native_material *m, uint32_t i, mh3_pack_texture_srt *out) {
    mh3_pack_texture_srt x;
    const uint8_t *p;
    float values[17];
    size_t n;

    if (!out || !m)
        return MH3_PACK_INVALID;

    if (!(m->flags & 1))
        return MH3_PACK_UNSUPPORTED;

    if (i >= 8 || !(p = slice(m, 128 + (size_t)i * 96, 96)))
        return MH3_PACK_RANGE;

    memset(&x, 0, sizeof x);
    x.flags = word(p);
    x.camera_reference = signed_value(word(p + 4));
    x.light_reference = signed_value(word(p + 8));
    x.map_mode = word(p + 12);
    x.effect_flags = word(p + 16);

    if (x.flags > 15 || x.camera_reference < -128 || x.camera_reference > 127 || x.light_reference < -128 || x.light_reference > 127 || x.map_mode > 255 || x.effect_flags > 255 || !zero(p + 88, 8))
        return MH3_PACK_INVALID;

    for (n = 0; n < 17; n++) {
        values[n] = real(p + 20 + n * 4);

        if (!isfinite(values[n]))
            return MH3_PACK_INVALID;
    }

    memcpy(x.scale, values, 8);
    x.rotation_degrees = values[2];
    memcpy(x.translation, values + 3, 8);
    memcpy(x.effect_matrix, values + 5, 48);
    *out = x;
    return MH3_PACK_OK;
}

static int control(const uint8_t *p, mh3_pack_channel_control *x) {
    if (p[0] > 1 || p[1] > 1 || p[2] > 1 || p[3] > 3 || p[4] > 2 || !zero(p + 6, 2))
        return 0;

    x->material_source = p[0];
    x->ambient_source = p[1];
    x->lighting = p[2];
    x->diffuse = p[3];
    x->attenuation = p[4];
    x->light_mask = p[5];
    return 1;
}

mh3_pack_status mh3_mesh_pack_channel(const mh3_pack_native_material *m, uint32_t i, mh3_pack_channel *out) {
    mh3_pack_channel x;
    const uint8_t *p;

    if (!out || !m)
        return MH3_PACK_INVALID;

    if (!(m->flags & 1))
        return MH3_PACK_UNSUPPORTED;

    if (i >= 2 || !(p = slice(m, 896 + (size_t)i * 64, 64)))
        return MH3_PACK_RANGE;

    memset(&x, 0, sizeof x);
    x.flags = word(p);
    memcpy(x.material_color, p + 4, 4);
    memcpy(x.ambient_color, p + 8, 4);
    x.raw_color_control = word(p + 28);
    x.raw_alpha_control = word(p + 32);

    if (x.flags > 63 || !control(p + 12, &x.color) || !control(p + 20, &x.alpha) || !zero(p + 36, 28))
        return MH3_PACK_INVALID;

    *out = x;
    return MH3_PACK_OK;
}

mh3_pack_status mh3_mesh_pack_tev(const mh3_pack_native_material *m, uint32_t i, mh3_pack_tev_stage *out) {
    static const uint8_t maxima[34] = {15, 15, 15, 15, 15, 2, 3, 1, 3, 7, 7, 7, 7, 15, 2, 3, 1, 3, 255, 7, 7, 3, 3, 31, 31, 3, 3, 7, 3, 15, 7, 7, 1, 1};
    mh3_pack_tev_stage x;
    const uint8_t *p;
    uint8_t values[34];
    size_t n;

    if (!out || !m)
        return MH3_PACK_INVALID;

    if (i >= m->tev_count || !(p = slice(m, 1152 + (size_t)i * 48, 48)))
        return MH3_PACK_RANGE;

    memset(&x, 0, sizeof x);
    x.known = wide(p);
    memcpy(values, p + 8, 34);

    if (x.known >> 34 || !zero(p + 42, 6))
        return MH3_PACK_INVALID;

    for (n = 0; n < 34; n++)
        if (values[n] > maxima[n] || (!(x.known & (UINT64_C(1) << n)) && values[n]))
            return MH3_PACK_INVALID;
    if ((values[4] > 1 && values[4] < 8) || (values[13] > 1 && values[13] < 8) || (values[18] > 7 && values[18] != 255))
        return MH3_PACK_INVALID;

    x.color = (mh3_pack_tev_pass){values[0], values[1], values[2], values[3], values[4], values[5], values[6], values[7], values[8]};
    x.alpha = (mh3_pack_tev_pass){values[9], values[10], values[11], values[12], values[13], values[14], values[15], values[16], values[17]};
    x.texture_map = values[18];
    x.texture_coordinate = values[19];
    x.raster_channel = values[20];
    x.raster_swap = values[21];
    x.texture_swap = values[22];
    x.konst_color = values[23];
    x.konst_alpha = values[24];
    x.indirect_stage = values[25];
    x.indirect_format = values[26];
    x.indirect_bias = values[27];
    x.indirect_alpha = values[28];
    x.indirect_matrix = values[29];
    x.indirect_wrap_s = values[30];
    x.indirect_wrap_t = values[31];
    x.indirect_original_lod = values[32];
    x.indirect_add_previous = values[33];
    *out = x;
    return MH3_PACK_OK;
}

mh3_pack_status mh3_mesh_pack_pixel(const mh3_pack_native_material *m, mh3_pack_pixel_inputs *out) {
    static const uint32_t maxima[22] = {7, 255, 7, 255, 3, 1, 7, 1, 1, 1, 1, 7, 7, 15, 1, 1, 1, 1, 255, 3, 3, 0xffffff};
    mh3_pack_pixel_inputs x;
    const uint8_t *p;
    size_t n;
    uint32_t classification;

    if (!out || !m)
        return MH3_PACK_INVALID;

    if (!(p = slice(m, 1024, 128)))
        return MH3_PACK_RANGE;

    memset(&x, 0, sizeof x);
    x.known = wide(p);
    x.classification = word(p + 96);

    if (x.known >> 22 || !zero(p + 100, 28))
        return MH3_PACK_INVALID;

    for (n = 0; n < 22; n++) {
        x.values[n] = word(p + 8 + n * 4);

        if (x.values[n] > maxima[n] || (!(x.known & (UINT64_C(1) << n)) && x.values[n]))
            return MH3_PACK_INVALID;
    }

    if ((x.known & 0x71f) != 0x71f)
        classification = 0;
    else if (x.values[8] || x.values[9] || x.values[10])
        classification = 3;
    else if (x.values[0] == 7 && x.values[2] == 7 && x.values[4] == 0)
        classification = 1;
    else
        classification = 2;

    if (x.classification != classification)
        return MH3_PACK_INVALID;

    *out = x;
    return MH3_PACK_OK;
}

mh3_pack_status mh3_mesh_pack_texgen(const mh3_pack_native_material *m, uint32_t i, mh3_pack_texgen *out) {
    static const uint8_t maxima[10] = {1, 1, 7, 31, 7, 7, 63, 63, 1, 1};
    mh3_pack_texgen x;
    const uint8_t *p;
    size_t n;

    if (!out || !m)
        return MH3_PACK_INVALID;

    if (i >= m->texgen_count || !(p = slice(m, 1152 + (size_t)m->tev_count * 48 + (size_t)i * 32, 32)))
        return MH3_PACK_RANGE;

    memset(&x, 0, sizeof x);
    x.known = word(p);
    memcpy(x.values, p + 4, 10);

    if (x.known >> 10 || !zero(p + 14, 18))
        return MH3_PACK_INVALID;

    for (n = 0; n < 10; n++)
        if (x.values[n] > maxima[n] || (!(x.known & (1u << n)) && x.values[n]))
            return MH3_PACK_INVALID;
    *out = x;
    return MH3_PACK_OK;
}

static int16_t short_value(const uint8_t *p) {
    uint32_t value = (uint32_t)p[0] | (uint32_t)p[1] << 8;
    return (int16_t)(value & 0x8000u ? (int32_t)value - 65536 : (int32_t)value);
}

static mh3_pack_status constants(const mh3_pack_native_material *m, const uint8_t **out) {
    const uint8_t *header = slice(m, 0, 128), *p;

    if (!header)
        return MH3_PACK_INVALID;

    if (word(header + 4) != 2 && word(header + 4) != 3)
        return MH3_PACK_UNSUPPORTED;

    p = slice(m, word(header + 84), 336);

    if (!p || word(header + 88) != 336)
        return MH3_PACK_RANGE;

    if (memcmp(p, "M3MC", 4) || word(p + 8) != 336 || word(p + 12))
        return MH3_PACK_INVALID;

    if (word(p + 4) != 1)
        return MH3_PACK_UNSUPPORTED;

    *out = p;
    return MH3_PACK_OK;
}

mh3_pack_status mh3_mesh_pack_texcoord_map(const mh3_pack_native_material *m, uint8_t out[8], uint8_t *known) {
    const uint8_t *p = slice(m, 0, 128);

    if (!p || !out || !known)
        return MH3_PACK_INVALID;

    if (word(p + 4) != 3)
        return MH3_PACK_UNSUPPORTED;

    const uint32_t mask = word(p + 100);

    if (mask > 255 || !zero(p + 104, 24))
        return MH3_PACK_INVALID;

    for (uint32_t i = 0; i < 8; i++)
        if ((p[92 + i] > 7 && p[92 + i] != 255) || (!(mask & (1u << i)) && p[92 + i]))
            return MH3_PACK_INVALID;
    memcpy(out, p + 92, 8);
    *known = (uint8_t)mask;
    return MH3_PACK_OK;
}

mh3_pack_status mh3_mesh_pack_tev_color(const mh3_pack_native_material *m, uint32_t i, int konst, mh3_pack_tev_color *out) {
    const uint8_t *p;
    mh3_pack_tev_color x;
    mh3_pack_status s;
    size_t j;

    if (!out || (konst != 0 && konst != 1))
        return MH3_PACK_INVALID;

    if (i >= 4)
        return MH3_PACK_RANGE;

    s = constants(m, &p);

    if (s)
        return s;

    p += 16 + (konst ? 64 : 0) + i * 16;
    memset(&x, 0, sizeof x);
    x.known = word(p);

    if (x.known > 15 || !zero(p + 12, 4))
        return MH3_PACK_INVALID;

    for (j = 0; j < 4; j++) {
        x.rgba[j] = short_value(p + 4 + j * 2);

        if (x.rgba[j] < (konst ? 0 : -1024) || x.rgba[j] > (konst ? 255 : 1023) || (!(x.known & (1u << j)) && x.rgba[j]))
            return MH3_PACK_INVALID;
    }

    *out = x;
    return MH3_PACK_OK;
}

mh3_pack_status mh3_mesh_pack_swap_table(const mh3_pack_native_material *m, uint32_t i, mh3_pack_swap_table *out) {
    const uint8_t *p;
    mh3_pack_swap_table x;
    mh3_pack_status s;
    size_t j;

    if (!out)
        return MH3_PACK_INVALID;

    if (i >= 4)
        return MH3_PACK_RANGE;

    s = constants(m, &p);

    if (s)
        return s;

    p += 144 + i * 8;
    memset(&x, 0, sizeof x);
    x.known = word(p);
    memcpy(x.rgba, p + 4, 4);

    if (x.known > 15)
        return MH3_PACK_INVALID;

    for (j = 0; j < 4; j++)
        if (x.rgba[j] > 3 || (!(x.known & (1u << j)) && x.rgba[j]))
            return MH3_PACK_INVALID;
    *out = x;
    return MH3_PACK_OK;
}

mh3_pack_status mh3_mesh_pack_indirect_stage(const mh3_pack_native_material *m, uint32_t i, mh3_pack_indirect_stage *out) {
    const uint8_t *p;
    mh3_pack_indirect_stage x;
    mh3_pack_status s;
    size_t j;

    if (!out)
        return MH3_PACK_INVALID;

    if (i >= 4)
        return MH3_PACK_RANGE;

    s = constants(m, &p);

    if (s)
        return s;

    p += 176 + i * 16;
    memset(&x, 0, sizeof x);
    x.known = word(p);

    if (x.known > 15 || !zero(p + 8, 8))
        return MH3_PACK_INVALID;

    for (j = 0; j < 4; j++)
        if (p[4 + j] > (j < 2 ? 7 : 15) || (!(x.known & (1u << j)) && p[4 + j]))
            return MH3_PACK_INVALID;
    x.coordinate = p[4];
    x.map = p[5];
    x.scale_s = p[6];
    x.scale_t = p[7];
    *out = x;
    return MH3_PACK_OK;
}

mh3_pack_status mh3_mesh_pack_indirect_matrix(const mh3_pack_native_material *m, uint32_t i, mh3_pack_indirect_matrix *out) {
    const uint8_t *p;
    mh3_pack_indirect_matrix x;
    mh3_pack_status s;
    size_t j;

    if (!out)
        return MH3_PACK_INVALID;

    if (i >= 3)
        return MH3_PACK_RANGE;

    s = constants(m, &p);

    if (s)
        return s;

    p += 240 + i * 32;
    memset(&x, 0, sizeof x);
    x.known = word(p);

    if (x.known > 127 || !zero(p + 20, 12))
        return MH3_PACK_INVALID;

    for (j = 0; j < 6; j++) {
        x.coefficients[j] = short_value(p + 4 + j * 2);

        if (x.coefficients[j] < -1024 || x.coefficients[j] > 1023 || (!(x.known & (1u << j)) && x.coefficients[j]))
            return MH3_PACK_INVALID;
    }

    x.scale_exponent = signed_value(word(p + 16));

    if (x.scale_exponent < -17 || x.scale_exponent > 46 || (!(x.known & 64) && x.scale_exponent))
        return MH3_PACK_INVALID;

    *out = x;
    return MH3_PACK_OK;
}

mh3_pack_status mh3_mesh_pack_material_native(const mh3_mesh_pack *pack, uint32_t i, mh3_pack_native_material *out) {
    mh3_pack_native_material x;
    const uint8_t *r, *p;
    size_t at;
    uint32_t offset, size, n;
    mh3_pack_status status;

    if (!pack || !out || !pack->metadata.data || pack->metadata.size < 128)
        return MH3_PACK_INVALID;

    if (word(pack->metadata.data + 4) != 2 && word(pack->metadata.data + 4) != 3)
        return MH3_PACK_UNSUPPORTED;

    if (i >= pack->counts[MH3_PACK_MATERIALS])
        return MH3_PACK_RANGE;

    at = pack->offsets[MH3_PACK_MATERIALS] + (size_t)i * 128;

    if (at > pack->metadata.size || 128 > pack->metadata.size - at)
        return MH3_PACK_RANGE;

    r = pack->metadata.data + at;
    offset = word(r + 84);
    size = word(r + 88);

    if (offset < pack->data_offset || offset > pack->metadata.size || size > pack->metadata.size - offset || size < 1152)
        return MH3_PACK_RANGE;

    p = pack->metadata.data + offset;

    if (memcmp(p, "M3MT", 4))
        return MH3_PACK_INVALID;

    if (word(p + 4) != 1 && word(p + 4) != 2 && word(p + 4) != 3)
        return MH3_PACK_UNSUPPORTED;

    memset(&x, 0, sizeof x);
    x.bytes = (mh3_pack_span){p, size};
    x.flags = word(p + 12);
    x.light_set_reference = signed_value(word(p + 16));
    x.fog_reference = signed_value(word(p + 20));

    for (n = 0; n < 3; n++) {
        x.indirect_method[n] = word(p + 24 + n * 4);
        x.indirect_light_reference[n] = signed_value(word(p + 36 + n * 4));
    }

    x.srt_flags = word(p + 48);
    x.texture_matrix_mode = word(p + 52);
    x.tev_count = word(p + 72);
    x.texgen_count = word(p + 80);

    if (x.flags & ~1u || x.texture_matrix_mode > 2 || x.light_set_reference < -128 || x.light_set_reference > 127 || x.fog_reference < -128 || x.fog_reference > 127 || x.tev_count > 16 ||
        x.texgen_count > 8 || x.tev_count != word(r + 32) || x.texgen_count != word(r + 24) || size != 1152 + x.tev_count * 48 + x.texgen_count * 32 + (word(p + 4) >= 2 ? 336 : 0) ||
        word(p + 8) != size || word(p + 56) != 128 || word(p + 60) != 896 || word(p + 64) != 1024 || word(p + 68) != 1152 || word(p + 76) != 1152 + x.tev_count * 48 ||
        (word(p + 4) == 1 ? !zero(p + 84, 44) : (word(p + 84) != 1152 + x.tev_count * 48 + x.texgen_count * 32 || word(p + 88) != 336 || (word(p + 4) == 2 && !zero(p + 92, 36)))) || !zero(r + 92, 36))
        return MH3_PACK_INVALID;
    for (n = 0; n < 3; n++)
        if (x.indirect_method[n] > 255 || x.indirect_light_reference[n] < -128 || x.indirect_light_reference[n] > 127)
            return MH3_PACK_INVALID;
    if (x.flags & 1) {
        for (n = 0; n < 8; n++) {
            mh3_pack_texture_srt s;
            status = mh3_mesh_pack_texture_srt(&x, n, &s);

            if (status)
                return status;

            if (s.flags != ((x.srt_flags >> (4 * n)) & 15))
                return MH3_PACK_INVALID;
        }

        for (n = 0; n < 2; n++) {
            mh3_pack_channel c;
            status = mh3_mesh_pack_channel(&x, n, &c);

            if (status)
                return status;
        }
    } else if (x.srt_flags || x.texture_matrix_mode || !zero(p + 128, 896))
        return MH3_PACK_INVALID;

    {
        mh3_pack_pixel_inputs pixel;
        status = mh3_mesh_pack_pixel(&x, &pixel);

        if (status)
            return status;
    }

    for (n = 0; n < x.tev_count; n++) {
        mh3_pack_tev_stage t;
        status = mh3_mesh_pack_tev(&x, n, &t);

        if (status)
            return status;
    }

    for (n = 0; n < x.texgen_count; n++) {
        mh3_pack_texgen t;
        status = mh3_mesh_pack_texgen(&x, n, &t);

        if (status)
            return status;
    }

    if (word(p + 4) >= 2) {
        for (n = 0; n < 4; n++) {
            mh3_pack_tev_color c;
            mh3_pack_swap_table t;
            mh3_pack_indirect_stage i;
            status = mh3_mesh_pack_tev_color(&x, n, 0, &c);

            if (status)
                return status;

            status = mh3_mesh_pack_tev_color(&x, n, 1, &c);

            if (status)
                return status;

            status = mh3_mesh_pack_swap_table(&x, n, &t);

            if (status)
                return status;

            status = mh3_mesh_pack_indirect_stage(&x, n, &i);

            if (status)
                return status;
        }

        for (n = 0; n < 3; n++) {
            mh3_pack_indirect_matrix m;
            status = mh3_mesh_pack_indirect_matrix(&x, n, &m);

            if (status)
                return status;
        }
    }

    if (word(p + 4) == 3) {
        uint8_t mapping[8], known;
        status = mh3_mesh_pack_texcoord_map(&x, mapping, &known);

        if (status)
            return status;
    }

    *out = x;
    return MH3_PACK_OK;
}

mh3_pack_status mh3_material_state_read(const mh3_mesh_pack *pack, uint32_t index, mh3_native_material_state *out) {
    mh3_native_material_state x;
    mh3_pack_native_material material;
    mh3_pack_material record;
    mh3_pack_status s;
    uint32_t i;

    if (!out)
        return MH3_PACK_INVALID;

    s = mh3_mesh_pack_material_native(pack, index, &material);

    if (s)
        return s;

    s = mh3_mesh_pack_material(pack, index, &record);

    if (s)
        return s;

    memset(&x, 0, sizeof x);
    x.texgen_count = record.texgen_count;
    x.channel_count = record.channel_count;
    x.tev_count = record.tev_count;
    x.indirect_count = record.indirect_count;
    x.cull_mode = record.cull_mode;
    x.light_set_reference = material.light_set_reference;
    x.fog_reference = material.fog_reference;

    for (i = 0; i < x.tev_count; i++) {
        static const uint8_t raster_channel[8] = {4, 5, 4, 5, 4, 7, 8, 6};
        s = mh3_mesh_pack_tev(&material, i, &x.tev[i]);

        if (s)
            return s;
        /* The immutable payload retains the raw three-bit BP selector. Live
         * material records use the semantic channel IDs used by shader inputs
         * and effect producers, including semantic COLOR_ZERO6. */
        x.tev[i].raster_channel = raster_channel[x.tev[i].raster_channel];
    }

    for (i = 0; i < 4; i++) {
        s = mh3_mesh_pack_tev_color(&material, i, 0, &x.color[i]);

        if (s)
            return s;

        s = mh3_mesh_pack_tev_color(&material, i, 1, &x.konst[i]);

        if (s)
            return s;

        s = mh3_mesh_pack_swap_table(&material, i, &x.swaps[i]);

        if (s)
            return s;

        s = mh3_mesh_pack_indirect_stage(&material, i, &x.indirect[i]);

        if (s)
            return s;
    }

    for (i = 0; i < 3; i++) {
        s = mh3_mesh_pack_indirect_matrix(&material, i, &x.indirect_matrix[i]);

        if (s)
            return s;
    }

    for (i = 0; i < x.texgen_count; i++) {
        s = mh3_mesh_pack_texgen(&material, i, &x.texgen[i]);

        if (s)
            return s;
    }

    for (i = 0; i < x.channel_count; i++) {
        s = mh3_mesh_pack_channel(&material, i, &x.channel[i]);

        if (s)
            return s;
    }

    s = mh3_mesh_pack_pixel(&material, &x.pixel);

    if (s)
        return s;

    *out = x;
    return MH3_PACK_OK;
}
