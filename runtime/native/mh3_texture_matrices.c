/* Typed ports of MH3 80075F00/800D754C and built-in scene mapping functions.
 * XSI operation order follows the local source and decompiled branch tables.
 * Native sin/cos is not claimed bit-identical to the original lookup tables. */
#include "mh3_texture_matrices.h"
#include <math.h>
#include <string.h>
static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static float real(const uint8_t *p) {
    uint32_t bits = be32(p);
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

static void store_real(uint8_t *p, float f) {
    uint32_t b;
    memcpy(&b, &f, 4);
    p[0] = (uint8_t)(b >> 24);
    p[1] = (uint8_t)(b >> 16);
    p[2] = (uint8_t)(b >> 8);
    p[3] = (uint8_t)b;
}

static mh3_matrix34 identity(void) {
    const mh3_matrix34 m = {{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}}};
    return m;
}

/* Preserve IEEE finite admission without VitaSDK's per-float __fpclassifyf
 * call. No consumed/unused field rule changes. */
static int finite_matrix(const mh3_matrix34 *m) {
    size_t r, c;

    for (r = 0; r < 3; r++)
        for (c = 0; c < 4; c++)
            if (!__builtin_isfinite(m->v[r][c]))
                return 0;
    return 1;
}

mh3_res_status mh3_texture_srt_read(mh3_res_span bytes, mh3_native_texture_srt *out) {
    mh3_native_texture_srt x;
    size_t i, j;
    const uint8_t *p = bytes.data;

    if (!p || !out)
        return MH3_RES_INVALID;

    if (bytes.size < 0x248)
        return MH3_RES_RANGE;

    memset(&x, 0, sizeof x);
    x.flags = be32(p);
    x.mode = be32(p + 4);

    for (i = 0; i < 8; i++) {
        mh3_pack_texture_srt *s = &x.slot[i];
        const uint8_t *values = p + 8 + i * 20, *effect = p + 0xa8 + i * 52;
        s->flags = (x.flags >> (4 * i)) & 15;
        s->scale[0] = real(values);
        s->scale[1] = real(values + 4);
        s->rotation_degrees = real(values + 8);
        s->translation[0] = real(values + 12);
        s->translation[1] = real(values + 16);
        s->camera_reference = (int8_t)effect[0];
        s->light_reference = (int8_t)effect[1];
        s->map_mode = effect[2];
        s->effect_flags = effect[3];

        for (j = 0; j < 12; j++)
            s->effect_matrix[j] = real(effect + 4 + j * 4);
    }

    *out = x;
    return MH3_RES_OK;
}

mh3_res_status mh3_texture_srt_read_for_prepare(mh3_res_span bytes, mh3_native_texture_srt *out) {
    size_t i, j;
    const uint8_t *p = bytes.data;

    if (!p || !out)
        return MH3_RES_INVALID;

    if (bytes.size < 0x248)
        return MH3_RES_RANGE;

    out->flags = be32(p);
    out->mode = be32(p + 4);

    for (i = 0; i < 8; i++) {
        mh3_pack_texture_srt *s = &out->slot[i];
        const uint8_t *values, *effect;
        s->flags = (out->flags >> (4 * i)) & 15;

        if (!(s->flags & 1))
            continue;

        values = p + 8 + i * 20;
        effect = p + 0xa8 + i * 52;
        s->map_mode = effect[2];
        s->effect_flags = effect[3];

        if (s->map_mode) {
            s->camera_reference = (int8_t)effect[0];
            s->light_reference = (int8_t)effect[1];
        }

        s->scale[0] = (s->flags & 2) ? 1.0f : real(values);
        s->scale[1] = (s->flags & 2) ? 1.0f : real(values + 4);
        s->rotation_degrees = (s->flags & 4) ? 0.0f : real(values + 8);
        s->translation[0] = (s->flags & 8) ? 0.0f : real(values + 12);
        s->translation[1] = (s->flags & 8) ? 0.0f : real(values + 16);
        /* A mapped nonidentity SRT combines its stored effect even when the
         * effect-identity bit is set. The complete identity nibble skips it. */
        if (!(s->effect_flags & 1) || (s->map_mode && s->flags != 15))
            for (j = 0; j < 12; j++)
                s->effect_matrix[j] = real(effect + 4 + j * 4);
    }

    return MH3_RES_OK;
}

mh3_res_status mh3_texture_srt_calc(const mh3_pack_texture_srt *s, uint32_t mode, int set, mh3_matrix34 *matrix) {
    mh3_matrix34 m;
    uint32_t index;
    float su, sv, tu, tv, sine = 0, cosine = 1, a, b, c, d;
    size_t col;

    if (!s || !matrix || !(s->flags & 1))
        return MH3_RES_INVALID;

    if (mode != 1)
        return MH3_RES_UNSUPPORTED;

    index = (s->flags >> 1) & 7;

    if (index == 7) {
        if (set)
            *matrix = identity();

        return MH3_RES_OK;
    }

    su = s->scale[0];
    sv = s->scale[1];
    tu = s->translation[0];
    tv = s->translation[1];

    if (!(index & 1) && (!isfinite(su) || !isfinite(sv)))
        return MH3_RES_INVALID;

    if (!(index & 2)) {
        float angle = s->rotation_degrees * 0.01745329251994329577f;

        if (!isfinite(angle))
            return MH3_RES_INVALID;

        sine = sinf(angle);
        cosine = cosf(angle);
    }

    if (!(index & 4) && (!isfinite(tu) || !isfinite(tv)))
        return MH3_RES_INVALID;

    if (set) {
        m = identity();

        switch (index) {
        case 0: /* SRT */
            a = su * cosine;
            b = su * sine;
            c = sv * sine;
            d = sv * cosine;
            m.v[0][0] = a;
            m.v[0][1] = -b;
            m.v[0][3] = (b - a * tu) - b * tv;
            m.v[1][0] = c;
            m.v[1][1] = d;
            m.v[1][3] = (-d - c * tu) + d * tv + 1.0f;
            break;
        case 1: /* RT */
            m.v[0][0] = cosine;
            m.v[0][1] = -sine;
            m.v[0][3] = (sine - cosine * tu) - sine * tv;
            m.v[1][0] = sine;
            m.v[1][1] = cosine;
            m.v[1][3] = (-cosine - sine * tu) + cosine * tv + 1.0f;
            break;
        case 2: /* ST */
            m.v[0][0] = su;
            m.v[0][3] = -su * tu;
            m.v[1][1] = sv;
            m.v[1][3] = sv * (tv - 1.0f) + 1.0f;
            break;
        case 3: /* T */
            m.v[0][3] = -tu;
            m.v[1][3] = tv;
            break;
        case 4: /* SR */
            a = su * cosine;
            b = su * sine;
            c = sv * sine;
            d = sv * cosine;
            m.v[0][0] = a;
            m.v[0][1] = -b;
            m.v[0][3] = b;
            m.v[1][0] = c;
            m.v[1][1] = d;
            m.v[1][3] = -d + 1.0f;
            break;
        case 5: /* R */
            m.v[0][0] = cosine;
            m.v[0][1] = -sine;
            m.v[0][3] = sine;
            m.v[1][0] = sine;
            m.v[1][1] = cosine;
            m.v[1][3] = 1.0f - cosine;
            break;
        case 6: /* S */
            m.v[0][0] = su;
            m.v[1][1] = sv;
            m.v[1][3] = 1.0f - sv;
            break;
        default:
            return MH3_RES_INVALID;
        }
    } else {
        if (!finite_matrix(matrix))
            return MH3_RES_INVALID;

        m = *matrix;

        if (index == 3) {
            m.v[0][3] -= tu;
            m.v[1][3] += tv;
        } else if (index == 2 || index == 6) {
            for (col = 0; col < 3; col++) {
                m.v[0][col] *= su;
                m.v[1][col] *= sv;
            }

            if (index == 6) {
                m.v[0][3] *= su;
                m.v[1][3] = sv * (m.v[1][3] - 1.0f) + 1.0f;
            } else {
                m.v[0][3] = (m.v[0][3] - tu) * su;
                m.v[1][3] = (m.v[1][3] + tv - 1.0f) * sv + 1.0f;
            }
        } else {
            a = cosine;
            b = sine;
            c = sine;
            d = cosine;

            if (index == 0 || index == 4) {
                a = su * cosine;
                b = su * sine;
                c = sv * sine;
                d = sv * cosine;
            }

            for (col = 0; col < 3; col++) {
                float x = m.v[0][col], y = m.v[1][col];
                m.v[0][col] = a * x - b * y;
                m.v[1][col] = c * x + d * y;
            }

            if (index == 0 || index == 1) {
                float x = m.v[0][3] - tu, y = m.v[1][3] + tv;
                m.v[0][3] = a * x - b * y + b;
                m.v[1][3] = c * x + d * y - d + 1.0f;
            } else {
                float x = m.v[0][3], y = m.v[1][3];
                m.v[0][3] = a * x - b * y + b;
                m.v[1][3] = c * x + d * y - d + 1.0f;
            }
        }

        m.v[2][0] = 0;
        m.v[2][1] = 0;
        m.v[2][2] = 1;
        m.v[2][3] = 0;
    }

    if (!finite_matrix(&m))
        return MH3_RES_INVALID;

    *matrix = m;
    return MH3_RES_OK;
}

static mh3_vec3 negate(mh3_vec3 v) {
    size_t i;

    for (i = 0; i < 3; i++)
        v.v[i] = -v.v[i];

    return v;
}

static mh3_vec3 cross(mh3_vec3 a, mh3_vec3 b) {
    mh3_vec3 x = {{a.v[1] * b.v[2] - a.v[2] * b.v[1], a.v[2] * b.v[0] - a.v[0] * b.v[2], a.v[0] * b.v[1] - a.v[1] * b.v[0]}};
    return x;
}

static int zero(mh3_vec3 v) {
    return v.v[0] == 0 && v.v[1] == 0 && v.v[2] == 0;
}

static mh3_res_status normalize(mh3_vec3 *v) {
    float length = sqrtf(v->v[0] * v->v[0] + v->v[1] * v->v[1] + v->v[2] * v->v[2]);
    size_t i;

    if (!isfinite(length) || length == 0)
        return MH3_RES_UNSUPPORTED;

    for (i = 0; i < 3; i++)
        v->v[i] /= length;

    return MH3_RES_OK;
}

static mh3_vec3 transform(const mh3_matrix34 *m, mh3_vec3 v) {
    mh3_vec3 x;
    size_t r;

    for (r = 0; r < 3; r++)
        x.v[r] = m->v[r][0] * v.v[0] + m->v[r][1] * v.v[1] + m->v[r][2] * v.v[2];

    return x;
}

static mh3_vec3 vertical_up(mh3_vec3 look) {
    mh3_vec3 up = {{0, 1, 0}};

    if (fabsf(look.v[0]) < 0.000001f && fabsf(look.v[2]) < 0.000001f) {
        up.v[1] = 0;
        up.v[2] = look.v[1] <= 0 ? -1.0f : 1.0f;
    }

    return up;
}

static mh3_res_status half_angle(mh3_vec3 a, mh3_vec3 b, mh3_vec3 *out) {
    size_t i;
    mh3_res_status status;
    mh3_vec3 x;
    a = negate(a);
    b = negate(b);
    status = normalize(&a);

    if (status)
        return status;

    status = normalize(&b);

    if (status)
        return status;

    for (i = 0; i < 3; i++)
        x.v[i] = a.v[i] + b.v[i];

    if (!zero(x)) {
        status = normalize(&x);

        if (status)
            return status;
    }

    *out = x;
    return MH3_RES_OK;
}

static mh3_vec3 light_look(const mh3_texture_mapping_input *in) {
    mh3_vec3 look = mh3_light_ray_direction(&in->light);

    if (!(in->light.flags & (MH3_LIGHT_SPOT | MH3_LIGHT_SPECULAR)) || zero(look)) {
        look = negate(transform(&in->inverse_current, in->light.position));

        if (zero(look))
            look.v[1] = -1;
    } else
        look = transform(&in->inverse_current, look);

    return look;
}

static mh3_res_status mapping(uint32_t mode, const mh3_texture_mapping_input *in, mh3_matrix34 *out);

mh3_res_status mh3_texture_mapping(uint32_t mode, const mh3_texture_mapping_input *in, mh3_matrix34 *out) {
    const mh3_res_status status = mapping(mode, in, out);

    /* A direction of zero length (normalize: the original's PSVECNormalize
     * gives NaNs there, which no texture coordinate survives): the identity
     * for this frame, so no NaN reaches later checks. Seen in the two-player
     * arena. */
    if (status == MH3_RES_UNSUPPORTED && mode <= 4) {
        *out = identity();
        return MH3_RES_OK;
    }

    return status;
}

static mh3_res_status mapping(uint32_t mode, const mh3_texture_mapping_input *in, mh3_matrix34 *out) {
    mh3_matrix34 m;
    mh3_vec3 look, up, right, camlook;
    mh3_res_status status;
    size_t r, c;

    if (!out)
        return MH3_RES_INVALID;

    if (mode == 0) {
        *out = identity();
        return MH3_RES_OK;
    }

    if (mode > 4)
        return MH3_RES_UNSUPPORTED;

    if (!in)
        return MH3_RES_INVALID;

    if (mode == 2) {
        if (!finite_matrix(&in->projection))
            return MH3_RES_INVALID;

        if (in->reference_present) {
            if (!finite_matrix(&in->referenced) || !finite_matrix(&in->inverse_current))
                return MH3_RES_INVALID;

            mh3_matrix_concat(&in->referenced, &in->inverse_current, &m);
            mh3_matrix_concat(&in->projection, &m, &m);
        } else
            m = in->projection;
    } else if (mode == 1 || mode == 3) {
        if (in->reference_present) {
            if (!finite_matrix(&in->referenced) || !finite_matrix(&in->inverse_current) || !finite_matrix(&in->environment))
                return MH3_RES_INVALID;

            mh3_matrix_concat(&in->referenced, &in->inverse_current, &m);

            for (r = 0; r < 3; r++)
                m.v[r][3] = 0;

            mh3_matrix_concat(&in->environment, &m, &m);
        } else if (in->light_present && (in->light.flags & MH3_LIGHT_ENABLED)) {
            const mh3_matrix34 env = {{{0.5f, 0, 0, 0.5f}, {0, -0.5f, 0, 0.5f}, {0, 0, 0, 1}}};

            if (!finite_matrix(&in->inverse_current))
                return MH3_RES_INVALID;

            look = light_look(in);
            status = normalize(&look);

            if (status)
                return status;

            up = vertical_up(look);
            right = cross(look, up);
            status = normalize(&right);

            if (status)
                return status;

            up = cross(right, look);
            m = identity();

            for (c = 0; c < 3; c++) {
                m.v[0][c] = right.v[c];
                m.v[1][c] = up.v[c];
                m.v[2][c] = -look.v[c];
            }

            mh3_matrix_concat(&m, &in->inverse_current, &m);

            for (r = 0; r < 3; r++)
                m.v[r][3] = 0;

            mh3_matrix_concat(&env, &m, &m);
        } else
            m = in->environment;
    } else {
        if (!finite_matrix(&in->current) || !finite_matrix(&in->inverse_current) || !finite_matrix(&in->environment))
            return MH3_RES_INVALID;

        for (c = 0; c < 3; c++)
            camlook.v[c] = -in->current.v[2][c];

        if (in->light_present && (in->light.flags & MH3_LIGHT_ENABLED)) {
            look = light_look(in);

            if (in->light.flags & MH3_LIGHT_SPECULAR)
                look = negate(look);
            else {
                status = half_angle(camlook, look, &look);

                if (status)
                    return status;
            }

            up = vertical_up(look);
        } else if (in->reference_present) {
            mh3_vec3 ray;

            if (!finite_matrix(&in->referenced))
                return MH3_RES_INVALID;

            for (c = 0; c < 3; c++) {
                up.v[c] = in->referenced.v[1][c];
                ray.v[c] = -in->referenced.v[2][c];
            }

            status = half_angle(camlook, ray, &look);

            if (status)
                return status;
        } else {
            *out = in->environment;
            return MH3_RES_OK;
        }

        status = normalize(&look);

        if (status)
            return status;

        right = cross(up, look);
        status = normalize(&right);

        if (status)
            return status;

        up = cross(look, right);
        m = identity();

        for (c = 0; c < 3; c++) {
            m.v[0][c] = right.v[c];
            m.v[1][c] = up.v[c];
            m.v[2][c] = look.v[c];
        }

        mh3_matrix_concat(&m, &in->inverse_current, &m);

        for (r = 0; r < 3; r++)
            m.v[r][3] = 0;

        mh3_matrix_concat(&in->environment, &m, &m);
    }

    if (!finite_matrix(&m))
        return MH3_RES_INVALID;

    *out = m;
    return MH3_RES_OK;
}

static void read34(const uint8_t *p, mh3_matrix34 *m) {
    size_t r, c;

    for (r = 0; r < 3; r++)
        for (c = 0; c < 4; c++)
            m->v[r][c] = real(p + (r * 4 + c) * 4);
}

mh3_res_status mh3_texture_mapping_read(mh3_res_mut_span cameras, mh3_res_span lights, uint32_t mode, int32_t camref, int32_t lightref, mh3_texture_mapping_input *out) {
    mh3_texture_mapping_input x;
    uint32_t selected, flags;
    size_t r, c;
    mh3_res_status status;
    int needs_inverse;

    if (!cameras.data || !out)
        return MH3_RES_INVALID;

    if (mode > 4)
        return MH3_RES_UNSUPPORTED;

    if (cameras.size < 0x1a34)
        return MH3_RES_RANGE;

    flags = (uint32_t)cameras.data[0] << 8 | cameras.data[1];
    selected = (uint32_t)cameras.data[2] << 8 | cameras.data[3];

    if (selected >= 32)
        return MH3_RES_INVALID;

    memset(&x, 0, sizeof x);
    read34(cameras.data + 0x34 + selected * 48, &x.current);
    read34(cameras.data + 0x1434 + selected * 48, &x.environment);
    read34(cameras.data + 0xe34 + ((camref >= 0 && camref < 32) ? (uint32_t)camref : selected) * 48, &x.projection);

    if (camref >= 0 && camref < 32) {
        x.reference_present = 1;
        read34(cameras.data + 0x34 + (uint32_t)camref * 48, &x.referenced);
    }

    {
        int32_t id;
        status = mh3_model_light_reference(lights, lightref, &id);

        if (status)
            return status;

        if (id >= 0) {
            status = mh3_light_read((mh3_res_span){lights.data + 0x2c + (size_t)id * 68, 68}, &x.light);

            if (status)
                return status;

            x.light_present = 1;
        }
    }
    /* Projection without a camera reference and environment fallback do not
     * touch the original inverse cache. Specular mapping always prepares it. */
    needs_inverse = mode == 4 || (mode == 2 && x.reference_present) || ((mode == 1 || mode == 3) && (x.reference_present || (x.light_present && (x.light.flags & MH3_LIGHT_ENABLED))));

    if (needs_inverse && (flags & 1))
        read34(cameras.data + 4, &x.inverse_current);
    else if (needs_inverse) {
        mh3_matrix33 inverse_normal;
        status = mh3_matrix_inverse_normal(&x.current, &inverse_normal);

        if (status)
            return status;

        for (r = 0; r < 3; r++) {
            for (c = 0; c < 3; c++)
                x.inverse_current.v[r][c] = inverse_normal.v[c][r];

            x.inverse_current.v[r][3] = -(x.inverse_current.v[r][0] * x.current.v[0][3] + x.inverse_current.v[r][1] * x.current.v[1][3] + x.inverse_current.v[r][2] * x.current.v[2][3]);
        }
    } else
        x.inverse_current = identity();

    if (needs_inverse && !finite_matrix(&x.inverse_current))
        return MH3_RES_INVALID;

    if (needs_inverse && !(flags & 1)) {
        for (r = 0; r < 3; r++)
            for (c = 0; c < 4; c++)
                store_real(cameras.data + 4 + (r * 4 + c) * 4, x.inverse_current.v[r][c]);
        cameras.data[1] |= 1;
    }

    *out = x;
    return MH3_RES_OK;
}

mh3_res_status mh3_texture_matrices_prepare(const mh3_native_texture_srt *srt, const mh3_matrix34 maps[8], const uint32_t sources[8], uint32_t previous, mh3_texture_matrices *out, uint32_t *next) {
    mh3_texture_matrices x;
    uint32_t mask = previous;
    size_t i, r, c;
    mh3_res_status status;

    if (!srt || !out || !next)
        return MH3_RES_INVALID;

    memset(&x, 0, sizeof x);

    for (i = 0; i < 8; i++) {
        const mh3_pack_texture_srt *slot = &srt->slot[i];
        mh3_matrix34 m = identity();
        int ident = 1;

        if (!(slot->flags & 1))
            continue;

        x.present |= (uint8_t)(1u << i);

        if (slot->map_mode) {
            if (!maps || !sources)
                return MH3_RES_INVALID;

            if (sources[i] > 2)
                return MH3_RES_UNSUPPORTED;

            if (!finite_matrix(&maps[i]))
                return MH3_RES_INVALID;

            m = maps[i];
            x.source[i] = sources[i];
            ident = 0;
        }

        if (slot->flags != 15 || !(slot->effect_flags & 1)) {
            mh3_matrix34 effect;

            if (!ident || !(slot->effect_flags & 1)) {
                for (r = 0; r < 3; r++)
                    for (c = 0; c < 4; c++)
                        effect.v[r][c] = slot->effect_matrix[r * 4 + c];
                if (!finite_matrix(&effect))
                    return MH3_RES_INVALID;

                if (ident) {
                    m = effect;
                    ident = 0;
                } else
                    mh3_matrix_concat(&m, &effect, &m);
            }

            if (!slot->map_mode) {
                status = mh3_texture_srt_calc(slot, srt->mode, ident, &m);

                if (status)
                    return status;
            } else {
                mh3_matrix34 sr = identity();
                float tmp;
                status = mh3_texture_srt_calc(slot, srt->mode, 1, &sr);

                if (status)
                    return status;

                tmp = sr.v[0][2];
                sr.v[0][2] = sr.v[0][3];
                sr.v[0][3] = tmp;
                tmp = sr.v[1][2];
                sr.v[1][2] = sr.v[1][3];
                sr.v[1][3] = tmp;
                mh3_matrix_concat(&sr, &m, &m);
            }

            ident = 0;
        }

        x.matrix[i] = m;

        if (ident) {
            x.identity |= (uint8_t)(1u << i);

            if (!(mask & (1u << i)))
                x.apply |= (uint8_t)(1u << i);

            mask |= 1u << i;
        } else {
            x.apply |= (uint8_t)(1u << i);
            mask &= ~(1u << i);
        }
    }

    *out = x;
    *next = mask;
    return MH3_RES_OK;
}

mh3_res_status mh3_texture_matrix_from_normal(const mh3_matrix33 *normal, mh3_matrix34 *out) {
    mh3_matrix34 x = {{{0}}};
    size_t r, c;

    if (!normal || !out)
        return MH3_RES_INVALID;

    for (r = 0; r < 3; r++)
        for (c = 0; c < 3; c++) {
            if (!isfinite(normal->v[r][c]))
                return MH3_RES_INVALID;

            x.v[r][c] = normal->v[r][c];
        }
    *out = x;
    return MH3_RES_OK;
}
