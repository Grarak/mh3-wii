/* SPDX-License-Identifier: GPL-2.0-or-later
 * Reciprocal interpolation coefficients: Copyright 2018 Dolphin Emulator
 * Project, FloatUtils.cpp; also present in isa/ppc_isa_float.h. The remaining
 * typed matrix/producer implementation is original to this project. */
#include "mh3_model_view.h"
#include "mh3_madd.h"
#include <assert.h>
#include <math.h>
#include <string.h>

static uint32_t word(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void put(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void read34(const uint8_t *p, mh3_matrix34 *matrix) {
    for (size_t r = 0; r < 3; r++)
        for (size_t c = 0; c < 4; c++) {
            uint32_t bits = word(p + 16 * r + 4 * c);
            memcpy(&matrix->v[r][c], &bits, 4);
        }
}

static void store34(uint8_t *p, const mh3_matrix34 *matrix) {
    for (size_t r = 0; r < 3; r++)
        for (size_t c = 0; c < 4; c++) {
            uint32_t bits;
            memcpy(&bits, &matrix->v[r][c], 4);
            put(p + 16 * r + 4 * c, bits);
        }
}

static void store33(uint8_t *p, const mh3_matrix33 *matrix) {
    for (size_t r = 0; r < 3; r++)
        for (size_t c = 0; c < 3; c++) {
            uint32_t bits;
            memcpy(&bits, &matrix->v[r][c], 4);
            put(p + 12 * r + 4 * c, bits);
        }
}

static mh3_matrix33 upper(const mh3_matrix34 *matrix) {
    mh3_matrix33 out;

    for (size_t r = 0; r < 3; r++)
        for (size_t c = 0; c < 3; c++)
            out.v[r][c] = matrix->v[r][c];
    return out;
}

static void concat(const mh3_matrix34 *a, const mh3_matrix34 *b, int array, mh3_matrix34 *out) {
    mh3_matrix34 result;

    for (size_t r = 0; r < 3; r++)
        for (size_t c = 0; c < 4; c++) {
            float value = mh3_madd(a->v[r][2], b->v[2][c], mh3_madd(a->v[r][1], b->v[1][c], a->v[r][0] * b->v[0][c]));

            if (c >= 2)
                value = mh3_madd(c == 3 ? 1.0f : 0.0f, a->v[r][array ? c : 3], value);

            result.v[r][c] = value;
        }
    *out = result;
}

/* Numeric seed used by the original inverse, before one binary32 Newton step.
 * This internal input is a non-singular determinant: finite |d| >= 1e-36, or
 * infinity/NaN. Thus every finite input is a normal binary32 value. Restoring
 * the original quantized seed matters: exact host division gives different
 * final float bits even after refinement. This is float math, not an ISA or
 * register interface. */
static float reciprocal_seed(float determinant) {
    static const uint32_t interpolation[32][2] = {{0x7ff800, 0x3e1}, {0x783800, 0x3a7}, {0x70ea00, 0x371}, {0x6a0800, 0x340}, {0x638800, 0x313}, {0x5d6200, 0x2ea}, {0x579000, 0x2c4},
                                                  {0x520800, 0x2a0}, {0x4cc800, 0x27f}, {0x47ca00, 0x261}, {0x430800, 0x245}, {0x3e8000, 0x22a}, {0x3a2c00, 0x212}, {0x360800, 0x1fb},
                                                  {0x321400, 0x1e5}, {0x2e4a00, 0x1d1}, {0x2aa800, 0x1be}, {0x272c00, 0x1ac}, {0x23d600, 0x19b}, {0x209e00, 0x18b}, {0x1d8800, 0x17c},
                                                  {0x1a9000, 0x16e}, {0x17ae00, 0x15b}, {0x14f800, 0x15b}, {0x124400, 0x143}, {0x0fbe00, 0x143}, {0x0d3800, 0x12d}, {0x0ade00, 0x12d},
                                                  {0x088400, 0x11a}, {0x065000, 0x11a}, {0x041c00, 0x108}, {0x020c00, 0x106}};
    uint32_t bits;
    memcpy(&bits, &determinant, 4);
    const uint32_t sign = bits & UINT32_C(0x80000000), exponent = (bits >> 23) & 255u, fraction = bits & 0x7fffffu;

    if (exponent == 255u && fraction)
        bits |= 0x400000u;
    else if (exponent >= 253u)
        bits = sign;
    else {
        const uint32_t index = fraction >> 8;
        const uint32_t *entry = interpolation[index >> 10];
        const uint32_t estimate = entry[0] - (entry[1] * (index & 1023u) + 1u) / 2u;
        bits = sign | ((253u - exponent) << 23) | estimate;
    }

    float result;
    memcpy(&result, &bits, 4);
    return result;
}

/* SDK inverse uses column zero for its determinant and explicit binary32
 * rounding. Dividing each cofactor separately changes the resulting bits. */
static int inverse_normal(const mh3_matrix34 *matrix, mh3_matrix33 *normal) {
    mh3_matrix33 out;

    for (size_t r = 0; r < 3; r++)
        for (size_t c = 0; c < 3; c++) {
            size_t a = (r + 1) % 3, b = (r + 2) % 3, x = (c + 1) % 3, y = (c + 2) % 3;
            out.v[r][c] = mh3_madd(matrix->v[a][x], matrix->v[b][y], -(matrix->v[a][y] * matrix->v[b][x]));
        }
    float det = mh3_madd(matrix->v[2][0], out.v[2][0], mh3_madd(matrix->v[1][0], out.v[1][0], matrix->v[0][0] * out.v[0][0]));

    if (fabsf(det) < 1.0e-36f) {
        *normal = upper(matrix);
        return 0;
    }

    float reciprocal = reciprocal_seed(det);
    /* At finite seed saturation the original subtract-then-negate yields -0
     * for either determinant sign. Keep that sign explicit across host FMA
     * lowering, which can choose different signed-zero fused forms. */
    if (reciprocal == 0.0f && isfinite(det))
        reciprocal = -0.0f;
    else
        reciprocal = -mh3_madd(reciprocal, det * reciprocal, -(reciprocal + reciprocal));

    for (size_t r = 0; r < 3; r++)
        for (size_t c = 0; c < 3; c++)
            out.v[r][c] *= reciprocal;
    *normal = out;
    return 1;
}

static int inverse34(const mh3_matrix34 *matrix, mh3_matrix34 *out) {
    mh3_matrix33 normal;

    if (!inverse_normal(matrix, &normal))
        return 0;

    mh3_matrix34 result;

    for (size_t r = 0; r < 3; r++) {
        for (size_t c = 0; c < 3; c++)
            result.v[r][c] = normal.v[c][r];

        result.v[r][3] = -mh3_madd(result.v[r][2], matrix->v[2][3], mh3_madd(result.v[r][1], matrix->v[1][3], result.v[r][0] * matrix->v[0][3]));
    }

    *out = result;
    return 1;
}

static int fits(mh3_res_span span, size_t at, size_t bytes) {
    return span.data && at <= span.size && bytes <= span.size - at;
}

static int mutable_fits(mh3_res_mut_span span, size_t bytes) {
    return span.data && bytes <= span.size;
}

static size_t aligned(size_t bytes) {
    return (bytes + 31u) & ~(size_t)31u;
}

static mh3_res_status node(const mh3_res_model *model, int32_t id, const uint8_t **out) {
    if (id < 0) {
        *out = NULL;
        return MH3_RES_OK;
    }

    size_t offset;
    mh3_res_status status = mh3_res_model_data(model, MH3_RES_NODES, id, &offset);

    if (status == MH3_RES_ABSENT) {
        *out = NULL;
        return MH3_RES_OK;
    }

    if (status)
        return status;

    if (!fits(model->container, offset, 0x64) || word(model->container.data + offset) < 0x64)
        return MH3_RES_RANGE;

    *out = model->container.data + offset;
    return MH3_RES_OK;
}

static mh3_res_status store(const mh3_model_view_callbacks *callbacks, uint32_t kind, uint32_t first, mh3_res_mut_span output, const uint8_t *cache, size_t bytes) {
    mh3_res_status status = MH3_RES_OK;

    if (callbacks && callbacks->before_store)
        status = callbacks->before_store(callbacks->user, kind, first, bytes);

    if (status)
        return status;

    memcpy(output.data + (size_t)first * (kind == 1 ? 36u : 48u), cache, bytes);
    return MH3_RES_OK;
}

mh3_res_status mh3_model_view_dma(const mh3_model_view_dma_input *in, const mh3_model_view_callbacks *callbacks) {
    if (!in)
        return MH3_RES_INVALID;

    if (!in->count)
        return MH3_RES_OK;

    if (in->model.version != 11)
        return MH3_RES_UNSUPPORTED;

    if (in->count > 65535u || !fits(in->models, 0, aligned((size_t)in->count * 48u)) || !fits(in->attributes, 0, (size_t)in->count * 4u) ||
        !mutable_fits(in->position, aligned((size_t)in->count * 48u)) || (in->normal.data && !mutable_fits(in->normal, aligned((size_t)in->count * 36u))) ||
        (in->normal.data && in->environment.data && !mutable_fits(in->environment, aligned((size_t)in->count * 48u))))
        return MH3_RES_RANGE;
    const size_t chunk = in->count < 40 ? in->count : 40;

    for (unsigned pair = 0; pair < 2; pair++) {
        if (!mutable_fits(in->model_cache[pair], aligned(chunk * 48u)) || !mutable_fits(in->position_cache[pair], aligned(chunk * 48u)) ||
            (in->normal.data && !mutable_fits(in->normal_cache[pair], aligned(chunk * 36u))) ||
            (in->normal.data && in->environment.data && !mutable_fits(in->environment_cache[pair], aligned(chunk * 48u))))
            return MH3_RES_RANGE;
    }

    size_t info = in->model.offset + 0x4c, table;
    mh3_res_status status = mh3_res_relative(in->model.container, info, info + 0x24, 4, &table);

    if (status)
        return status;

    if (word(in->model.container.data + table) < in->count || !fits(in->model.container, table + 4, (size_t)in->count * 4u))
        return MH3_RES_RANGE;
    /* Unsupported billboard selectors fail before any output or cache writes. */
    for (uint32_t i = 0; i < in->count; i++) {
        uint32_t billboard = word(in->attributes.data + 4 * i) & 255u;

        if (billboard > 6 || (billboard && (!callbacks || !callbacks->billboard)))
            return MH3_RES_UNSUPPORTED;
    }

    memcpy(in->model_cache[0].data, in->models.data, aligned(chunk * 48u));

    for (uint32_t first = 0; first < in->count; first += 40) {
        const unsigned pair = (first / 40) & 1u;
        const size_t count = in->count - first < 40 ? in->count - first : 40;
        const size_t bytes = aligned(count * 48u), normal_bytes = aligned(count * 36u);

        if (first + 40 < in->count) {
            const size_t next = in->count - first - 40 < 40 ? in->count - first - 40 : 40;
            memcpy(in->model_cache[pair ^ 1].data, in->models.data + (size_t)(first + 40) * 48u, aligned(next * 48u));
        }

        for (size_t local = 0; local < count; local++) {
            mh3_matrix34 model, position;
            read34(in->model_cache[pair].data + 48 * local, &model);
            concat(&in->camera, &model, count > 1, &position);
            store34(in->position_cache[pair].data + 48 * local, &position);
        }

        for (size_t local = 0; local < count; local++) {
            const uint32_t id = first + (uint32_t)local, attr = word(in->attributes.data + 4 * id), billboard = attr & 255u;
            const int32_t node_id = (int32_t)word(in->model.container.data + table + 4 + (size_t)id * 4);
            const uint8_t *resource = NULL;
            status = node(&in->model, node_id, &resource);

            if (status)
                return status;

            mh3_matrix34 position, model;
            uint8_t *destination = in->position_cache[pair].data + 48 * local;

            if (billboard) {
                status = callbacks->billboard(callbacks->user, billboard, id, (attr & UINT32_C(0x20000000)) != 0, (mh3_res_mut_span){destination, 48});

                if (status)
                    return status;

                if (!resource)
                    return MH3_RES_INVALID;

                if (word(resource + 0x60)) {
                    if (!mutable_fits(in->billboard_work, ((size_t)id + 1) * 48))
                        return MH3_RES_RANGE;

                    mh3_matrix34 inverse, relative;
                    read34(in->models.data + (size_t)id * 48, &model);
                    read34(destination, &position);

                    if (inverse34(&model, &inverse))
                        concat(&position, &inverse, 0, &relative);
                    else {
                        relative = (mh3_matrix34){{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}}};

                        for (size_t r = 0; r < 3; r++)
                            relative.v[r][3] = in->camera.v[r][3];
                    }

                    store34(in->billboard_work.data + (size_t)id * 48, &relative);
                }
            } else if (resource && (word(resource + 0x14) & 0x400u)) {
                const uint8_t *parent = NULL;
                status = node(&in->model, (int32_t)word(resource + 0x1c), &parent);

                if (status)
                    return status;

                if (!parent)
                    return MH3_RES_INVALID;

                const uint32_t parent_id = word(parent + 0x10);

                if (parent_id >= id || !mutable_fits(in->billboard_work, ((size_t)parent_id + 1) * 48))
                    return MH3_RES_RANGE;

                mh3_matrix34 parent_matrix;
                read34(in->billboard_work.data + (size_t)parent_id * 48, &parent_matrix);
                read34(in->models.data + (size_t)id * 48, &model);
                concat(&parent_matrix, &model, 0, &position);
                store34(destination, &position);
            }
        }

        status = store(callbacks, 0, first, in->position, in->position_cache[pair].data, bytes);

        if (status)
            return status;

        if (in->normal.data) {
            for (size_t local = 0; local < count; local++) {
                mh3_matrix34 position, environment = {{{0}}};
                mh3_matrix33 normal;
                read34(in->position_cache[pair].data + local * 48, &position);

                if (word(in->attributes.data + 4 * (first + local)) & UINT32_C(0x20000000))
                    normal = upper(&position);
                else
                    (void)inverse_normal(&position, &normal);

                store33(in->normal_cache[pair].data + local * 36, &normal);

                if (in->environment.data) {
                    for (size_t r = 0; r < 3; r++)
                        for (size_t c = 0; c < 3; c++)
                            environment.v[r][c] = normal.v[r][c];
                    store34(in->environment_cache[pair].data + local * 48, &environment);
                }
            }

            status = store(callbacks, 1, first, in->normal, in->normal_cache[pair].data, normal_bytes);

            if (status)
                return status;

            if (in->environment.data) {
                status = store(callbacks, 2, first, in->environment, in->environment_cache[pair].data, bytes);

                if (status)
                    return status;
            }
        }
    }

    return MH3_RES_OK;
}

void mh3_model_view_native_static(const mh3_model_view_native_input *in) {
    assert(in);

    if (!in->count)
        return;

    assert(in->count <= 65535u && in->models && in->attributes && in->position);

    for (uint32_t first = 0; first < in->count; first += 40u) {
        const uint32_t count = in->count - first < 40u ? in->count - first : 40u;

        for (uint32_t local = 0; local < count; local++) {
            const uint32_t index = first + local;
            assert(!(in->attributes[index] & 255u));
            concat(&in->camera, &in->models[index], count > 1u, &in->position[index]);
        }

        if (in->normal)
            for (uint32_t local = 0; local < count; local++) {
                const uint32_t index = first + local;

                if (in->attributes[index] & UINT32_C(0x20000000))
                    in->normal[index] = upper(&in->position[index]);
                else
                    (void)inverse_normal(&in->position[index], &in->normal[index]);

                if (in->environment) {
                    mh3_matrix34 environment = {{{0}}};

                    for (size_t row = 0; row < 3; row++)
                        for (size_t column = 0; column < 3; column++)
                            environment.v[row][column] = in->normal[index].v[row][column];
                    in->environment[index] = environment;
                }
            }
    }
}
