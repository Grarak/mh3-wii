#include "mh3_prepare_skin.h"
#include <math.h>
#include <string.h>

static uint16_t be16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}

static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static float befloat(const uint8_t *p) {
    const uint32_t bits = be32(p);
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

static int model_fits(const mh3_res_model *m, size_t at, size_t n) {
    return at >= m->offset && at - m->offset <= m->size && n <= m->size - (at - m->offset);
}

/* The matrix-to-node map's count (MDL0 info + 0x24), as the assembler reads it. */
static mh3_skin_status matrix_count(const mh3_res_model *m, uint32_t *count) {
    const size_t info = m->offset + 0x4c;
    size_t mapping;

    if (!model_fits(m, info, 64))
        return MH3_SKIN_INVALID;

    const mh3_res_status s = mh3_res_relative(m->container, info, info + 0x24, 4, &mapping);

    if (s == MH3_RES_ABSENT) {
        *count = 0;
        return MH3_SKIN_OK;
    }

    if (s != MH3_RES_OK || !model_fits(m, mapping, 4))
        return MH3_SKIN_INVALID;

    *count = be32(m->container.data + mapping);
    return MH3_SKIN_OK;
}

/* The four largest influences (the earlier one on equal weights),
 * renormalized to the original sum. */
static void keep_largest(const uint16_t *source, const float *weight, unsigned n, mh3_skin_matrix *out) {
    unsigned order[MH3_SKIN_INFLUENCES];
    uint64_t taken = 0;
    float total = 0.0f, kept = 0.0f;

    for (unsigned i = 0; i < n; i++)
        total += weight[i];

    for (unsigned k = 0; k < MH3_SKIN_INFLUENCES; k++) {
        unsigned best = n;

        for (unsigned i = 0; i < n; i++)
            if (!(taken >> i & 1) && (best == n || weight[i] > weight[best]))
                best = i;

        order[k] = best;
        taken |= UINT64_C(1) << best;
        kept += weight[best];
    }

    out->count = MH3_SKIN_INFLUENCES;

    for (unsigned i = 0; i < MH3_SKIN_INFLUENCES; i++) {
        out->source[i] = source[order[i]];
        out->weight[i] = kept > 0.0f ? weight[order[i]] * (total / kept) : weight[order[i]];
    }
}

mh3_skin_status mh3_prepare_skin_model(const mh3_res_model *m, mh3_skin_matrix *storage, uint32_t capacity, mh3_skin_model *out) {
    if (!m || !storage || !out)
        return MH3_SKIN_INVALID;

    // Revision 11 only, as the model assembler (its info block offset).
    if (m->version != 11)
        return MH3_SKIN_UNSUPPORTED;

    uint32_t count;
    mh3_skin_status status = matrix_count(m, &count);

    if (status)
        return status;

    if (count > capacity || count > MH3_SKIN_MAX_MATRICES)
        return MH3_SKIN_RANGE;

    memset(storage, 0, (size_t)count * sizeof *storage);
    out->matrices = storage;
    out->count = count;
    mh3_res_entry mix;
    const mh3_res_status found = mh3_res_model_find(m, MH3_RES_BYTECODE, "NodeMix", &mix);

    if (found == MH3_RES_ABSENT)
        return MH3_SKIN_OK;

    if (found != MH3_RES_OK)
        return MH3_SKIN_INVALID;

    size_t at = mix.offset;

    for (;;) {
        if (!model_fits(m, at, 1))
            return MH3_SKIN_RANGE;

        const unsigned op = m->container.data[at++];

        if (op == 1)
            return MH3_SKIN_OK;

        if (!op)
            continue;

        if (op == 5) {
            // (matrix, node): the matrix is a skin bone; its node's inverse
            // bind matrix is node data + 0xA0.
            if (!model_fits(m, at, 4))
                return MH3_SKIN_RANGE;

            const uint32_t matrix = be16(m->container.data + at), node = be16(m->container.data + at + 2);
            at += 4;
            mh3_res_entry e;

            if (matrix >= count || mh3_res_model_entry(m, MH3_RES_NODES, node, &e) != MH3_RES_OK || !model_fits(m, e.offset + 0xa0, 48))
                return MH3_SKIN_INVALID;

            mh3_skin_matrix *bone = &storage[matrix];
            bone->bone = 1;

            for (unsigned i = 0; i < 12; i++) {
                bone->inverse_bind[i] = befloat(m->container.data + e.offset + 0xa0 + i * 4);

                if (!isfinite(bone->inverse_bind[i]))
                    return MH3_SKIN_UNSUPPORTED;
            }
        } else if (op == 3) {
            // (matrix, n, (source, weight) * n): an envelope of skin bones.
            if (!model_fits(m, at, 3))
                return MH3_SKIN_RANGE;

            const uint32_t matrix = be16(m->container.data + at), n = m->container.data[at + 2];
            at += 3;

            if (!n || n > 64 || !model_fits(m, at, (size_t)n * 6) || matrix >= count || storage[matrix].count)
                return MH3_SKIN_INVALID;

            uint16_t source[64];
            float weight[64];

            for (unsigned i = 0; i < n; i++) {
                source[i] = be16(m->container.data + at + i * 6);
                weight[i] = befloat(m->container.data + at + i * 6 + 2);

                if (source[i] >= count || !isfinite(weight[i]))
                    return MH3_SKIN_INVALID;
            }

            at += (size_t)n * 6;
            mh3_skin_matrix *envelope = &storage[matrix];

            if (n <= MH3_SKIN_INFLUENCES) {
                envelope->count = (uint8_t)n;

                for (unsigned i = 0; i < n; i++) {
                    envelope->source[i] = source[i];
                    envelope->weight[i] = weight[i];
                }
            } else
                keep_largest(source, weight, n, envelope);
        } else
            return MH3_SKIN_UNSUPPORTED;
    }
}
