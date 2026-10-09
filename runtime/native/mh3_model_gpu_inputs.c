#include "mh3_model_gpu_inputs.h"
#include <float.h>
#include <math.h>
#include <string.h>

static mh3_matrix34 identity(void) {
    const mh3_matrix34 matrix = {{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}}};
    return matrix;
}
/* Explicit classification keeps VitaSDK's isfinite macro from calling
 * __fpclassifyf for every validated float. NaN/Inf checks remain strict. */
static int finite_matrix(const mh3_matrix34 *matrix) {
    for (unsigned r = 0; r < 3; r++)
        for (unsigned c = 0; c < 4; c++)
            if (!__builtin_isfinite(matrix->v[r][c]))
                return 0;
    return 1;
}
static int attribute(const mh3_pack_layout *layout, unsigned semantic) {
    for (unsigned i = 0; i < layout->count; i++)
        if (layout->attributes[i].semantic == semantic)
            return 1;
    return 0;
}
static mh3_res_status main_matrix(const mh3_model_palette *palette, unsigned selector, mh3_matrix34 *out) {
    if (selector == 60) {
        *out = identity();
        return MH3_RES_OK;
    }
    if (selector % 3 || selector >= 60)
        return MH3_RES_UNSUPPORTED;
    unsigned slot = selector / 3;
    if (slot < 10) {
        if (!(palette->position_valid & (1u << slot)))
            return MH3_RES_ABSENT;
        *out = palette->position[slot];
    } else {
        slot -= 10;
        if (!(palette->texture_valid & (1u << slot)))
            return MH3_RES_ABSENT;
        *out = palette->texture[slot];
    }
    return finite_matrix(out) ? MH3_RES_OK : MH3_RES_INVALID;
}
/* Interval bound with a deliberately generous float roundoff guard. Returning
 * false only keeps the exact conditional divide; it never changes coordinates.
 * Unknown UV/normal ranges can certify constant Q, but not varying Q. */
static int nonzero_q(const mh3_resolved_mesh_submit *mesh, const mh3_model_gpu_texgen *gen) {
    if (!gen->projection || gen->normalize)
        return 0;
    float row[4] = {0}, magnitude = 0;
    for (unsigned c = 0; c < 4; c++) {
        if (gen->post_identity)
            row[c] = gen->main.v[2][c];
        else {
            for (unsigned r = 0; r < 3; r++)
                row[c] += gen->post.v[2][r] * gen->main.v[r][c];
            if (c == 3)
                row[c] += gen->post.v[2][3];
        }
        if (!__builtin_isfinite(row[c]))
            return 0;
    }
    if (gen->source_row >= 5 || gen->input_ab11) {
        row[3] += row[2];
        row[2] = 0;
    }
    float lo = row[3], hi = row[3];
    magnitude = fabsf(row[3]);
    for (unsigned c = 0; c < 3; c++) {
        if (row[c] == 0)
            continue;
        float a, b;
        if (gen->source_row == 0) {
            a = mesh->mesh.bounds_min[c];
            b = mesh->mesh.bounds_max[c];
        } else if (gen->source_row == 2) {
            a = 0;
            b = 1;
        } else
            return 0;
        if (!__builtin_isfinite(a) || !__builtin_isfinite(b) || a > b)
            return 0;
        a *= row[c];
        b *= row[c];
        lo += fminf(a, b);
        hi += fmaxf(a, b);
        magnitude += fmaxf(fabsf(a), fabsf(b));
    }
    /* Bound composition roundoff before cancellation, weighted by the
     * original input extents. Unknown ranges cannot certify a cancellation. */
    for (unsigned c = 0; c < 4; c++) {
        float terms = 0;
        if (gen->post_identity)
            terms = fabsf(gen->main.v[2][c]);
        else
            for (unsigned r = 0; r < 3; r++)
                terms += fabsf(gen->post.v[2][r] * gen->main.v[r][c]);
        if (terms == 0)
            continue;
        float extent = 1;
        if (c < 3 && !(c == 2 && (gen->source_row >= 5 || gen->input_ab11))) {
            if (gen->source_row == 0) {
                const float low = mesh->mesh.bounds_min[c], high = mesh->mesh.bounds_max[c];
                if (!__builtin_isfinite(low) || !__builtin_isfinite(high) || low > high)
                    return 0;
                extent = fmaxf(fabsf(low), fabsf(high));
            } else if (gen->source_row != 2)
                return 0;
        }
        magnitude += terms * extent;
    }
    magnitude += fabsf(gen->post.v[2][3]);
    const float error = 64 * FLT_EPSILON * fmaxf(1, magnitude);
    if (!__builtin_isfinite(lo) || !__builtin_isfinite(hi) || !__builtin_isfinite(error))
        return 0;
    return lo > error ? 1 : hi < -error ? -1 : 0;
}
/* Certify the actual vertex equation, not a cancelling composed float row.
 * Packed positions come from finite offline values and validated finite bounds;
 * integer normalized colors cannot contain NaN/Inf. Unknown UV/normal ranges
 * and normalization remain on the conditional fragment path. */
static int zero_q(const mh3_resolved_mesh_submit *mesh, const mh3_model_gpu_texgen *gen) {
    if (!gen->projection || gen->normalize || !gen->post_identity)
        return 0;
    for (unsigned c = 0; c < 4; c++)
        if (gen->main.v[2][c] != 0)
            return 0;
    if (gen->source_row == 0) {
        for (unsigned c = 0; c < 3; c++)
            if (!__builtin_isfinite(mesh->mesh.bounds_min[c]) || !__builtin_isfinite(mesh->mesh.bounds_max[c]) || mesh->mesh.bounds_min[c] > mesh->mesh.bounds_max[c])
                return 0;
        return 1;
    }
    if (gen->source_row == 2) {
        for (unsigned i = 0; i < mesh->layout.count; i++)
            if (mesh->layout.attributes[i].semantic == 2) {
                const unsigned format = mesh->layout.attributes[i].format;
                return format == 4 || format == 7;
            }
    }
    return 0;
}
/* The converter supplies the referenced slot union and per-coordinate identity
 * and position-region presence. Equal signs across all guarded intervals are
 * required; individually nonzero matrices of opposite signs can interpolate
 * through 0. A zero certificate requires every candidate's direct zero row. */
static unsigned indexed_q(const mh3_resolved_mesh_submit *mesh, const mh3_model_gpu_texgen *gen, unsigned coordinate) {
    const mh3_pack_palette_batch *batch = mesh->palette_batch;
    if (!batch)
        return 0;
    mh3_model_gpu_texgen candidate = *gen;
    int have = 0, sign = 0, same_sign = 1, all_zero = 1;
    if (batch->texture_identity_mask & (1u << coordinate)) {
        candidate.main = identity();
        sign = nonzero_q(mesh, &candidate);
        have = 1;
        same_sign = sign != 0;
        all_zero = zero_q(mesh, &candidate);
    }
    for (unsigned slot = 0; slot < 10; slot++) {
        if ((batch->texture_position_mask & (1u << coordinate)) && (batch->position_required & (1u << slot))) {
            candidate.main = mesh->palette->position[slot];
            const int current = nonzero_q(mesh, &candidate);
            if (!have)
                sign = current;
            same_sign &= current != 0 && current == sign;
            all_zero &= zero_q(mesh, &candidate);
            have = 1;
        }
        if (batch->texture_required & (1u << slot)) {
            candidate.main = mesh->palette->texture[slot];
            const int current = nonzero_q(mesh, &candidate);
            if (!have)
                sign = current;
            same_sign &= current != 0 && current == sign;
            all_zero &= zero_q(mesh, &candidate);
            have = 1;
        }
    }
    return !have ? 0 : all_zero ? 2 : same_sign ? 1 : 0;
}
mh3_res_status mh3_model_gpu_prepare(const mh3_resolved_mesh_submit *mesh, uint8_t mask, int normal_required, float near_z, float far_z, mh3_model_gpu_inputs *out) {
    if (!mesh || !out || !mesh->palette || !mesh->projection || !mesh->material || mesh->layout.count > 16 || normal_required < 0 || normal_required > 1 || !__builtin_isfinite(near_z) ||
        !__builtin_isfinite(far_z))
        return MH3_RES_INVALID;
    mh3_model_gpu_inputs result;
    memset(&result, 0, sizeof result);
    result.matrix_index_mask = (uint16_t)((mesh->layout.flags >> MH3_LAYOUT_MATRIX_MASK_SHIFT) & 0x1ffu);
    result.position = result.normal = identity();
    if (result.matrix_index_mask) {
        const mh3_pack_palette_batch *batch = mesh->palette_batch;
        if (!batch)
            return MH3_RES_ABSENT;
        if ((batch->position_required | batch->normal_required | batch->texture_required) & ~0x3ffu)
            return MH3_RES_INVALID;
        if ((result.matrix_index_mask & 1) && (!batch->position_required || (normal_required && !batch->normal_required)))
            return MH3_RES_ABSENT;
        const uint16_t positions = batch->position_required | ((result.matrix_index_mask & 1) ? 0 : 1);
        if ((positions & ~mesh->palette->position_valid) || (batch->texture_required & ~mesh->palette->texture_valid) || (normal_required && (batch->normal_required & ~mesh->palette->normal_valid)))
            return MH3_RES_ABSENT;
        for (unsigned slot = 0; slot < 10; slot++) {
            if (positions & (1u << slot)) {
                if (!finite_matrix(&mesh->palette->position[slot]))
                    return MH3_RES_INVALID;
                result.position_palette[slot] = mesh->palette->position[slot];
            }
            if (batch->texture_required & (1u << slot)) {
                if (!finite_matrix(&mesh->palette->texture[slot]))
                    return MH3_RES_INVALID;
                result.texture_palette[slot] = mesh->palette->texture[slot];
            }
            if (normal_required && (batch->normal_required & (1u << slot))) {
                for (unsigned r = 0; r < 3; r++)
                    for (unsigned c = 0; c < 3; c++)
                        result.normal_palette[slot].v[r][c] = mesh->palette->normal[slot].v[r][c];
                if (!finite_matrix(&result.normal_palette[slot]))
                    return MH3_RES_INVALID;
            }
        }
    }
    if (!(result.matrix_index_mask & 1)) {
        if (!(mesh->palette->position_valid & 1))
            return MH3_RES_ABSENT;
        result.position = mesh->palette->position[0];
        if (!finite_matrix(&result.position))
            return MH3_RES_INVALID;
        result.position_palette[0] = result.position;
    }
    for (unsigned i = 0; i < 16; i++) {
        if (!__builtin_isfinite(mesh->projection[i]))
            return MH3_RES_INVALID;
        result.projection[i] = mesh->projection[i];
    }
    for (unsigned i = 0; i < 4; i++) {
        result.projection[8 + i] = near_z <= far_z ? -mesh->projection[8 + i] : mesh->projection[8 + i] + mesh->projection[12 + i];
        if (!__builtin_isfinite(result.projection[8 + i]))
            return MH3_RES_INVALID;
    }
    if (normal_required) {
        if (!attribute(&mesh->layout, 1))
            return MH3_RES_ABSENT;
        if (!(result.matrix_index_mask & 1)) {
            if (!(mesh->palette->normal_valid & 1))
                return MH3_RES_ABSENT;
            for (unsigned r = 0; r < 3; r++)
                for (unsigned c = 0; c < 3; c++)
                    result.normal.v[r][c] = mesh->palette->normal[0].v[r][c];
            if (!finite_matrix(&result.normal))
                return MH3_RES_INVALID;
            result.normal_palette[0] = result.normal;
        }
    }
    for (unsigned i = 0; i < 8; i++)
        if (mask & (1u << i)) {
            if (i >= mesh->material->texgen_count)
                return MH3_RES_ABSENT;
            const mh3_pack_texgen *source = &mesh->material->texgen[i];
            const int indexed = (result.matrix_index_mask & (2u << i)) != 0;
            const unsigned required = (1u << MH3_TEXGEN_PROJECTION) | (1u << MH3_TEXGEN_INPUT_AB11) | (1u << MH3_TEXGEN_FUNCTION) | (1u << MH3_TEXGEN_SOURCE_ROW) | (1u << MH3_TEXGEN_MATRIX_ROW) |
                                      (1u << MH3_TEXGEN_DUAL_TEXTURE);
            const unsigned consumed = indexed ? required & ~(1u << MH3_TEXGEN_MATRIX_ROW) : required;
            if ((source->known & consumed) != consumed)
                return MH3_RES_ABSENT;
            const uint8_t *v = source->values;
            if (v[MH3_TEXGEN_FUNCTION] != 0)
                return MH3_RES_UNSUPPORTED;
            if (v[MH3_TEXGEN_PROJECTION] > 1 || v[MH3_TEXGEN_INPUT_AB11] > 1 || v[MH3_TEXGEN_DUAL_TEXTURE] > 1)
                return MH3_RES_INVALID;
            mh3_model_gpu_texgen *gen = &result.texgen[i];
            gen->source_row = v[MH3_TEXGEN_SOURCE_ROW];
            gen->projection = v[MH3_TEXGEN_PROJECTION];
            gen->input_ab11 = v[MH3_TEXGEN_INPUT_AB11];
            const unsigned row = gen->source_row;
            if (row == 3 || row == 4 || row > 12)
                return MH3_RES_UNSUPPORTED;
            const unsigned semantic = row <= 2 ? row : row - 1;
            if (!attribute(&mesh->layout, semantic))
                return MH3_RES_ABSENT;
            gen->indexed = (uint8_t)indexed;
            gen->main = identity();
            gen->main_slot = 20;
            gen->main_identity = indexed || v[MH3_TEXGEN_MATRIX_ROW] == 60;
            if (!indexed) {
                mh3_res_status status = main_matrix(mesh->palette, v[MH3_TEXGEN_MATRIX_ROW], &gen->main);
                if (status != MH3_RES_OK)
                    return status;
                gen->main_slot = (uint8_t)(v[MH3_TEXGEN_MATRIX_ROW] / 3);
                if (gen->main_slot < 10)
                    result.position_palette[gen->main_slot] = gen->main;
                else if (gen->main_slot < 20)
                    result.texture_palette[gen->main_slot - 10] = gen->main;
            }
            gen->post = identity();
            gen->post_identity = 1;
            if (v[MH3_TEXGEN_DUAL_TEXTURE]) {
                const unsigned post_required = (1u << MH3_TEXGEN_NORMALIZE) | (1u << MH3_TEXGEN_POST_MATRIX_ROW);
                if ((source->known & post_required) != post_required)
                    return MH3_RES_ABSENT;
                if (v[MH3_TEXGEN_NORMALIZE] > 1)
                    return MH3_RES_INVALID;
                gen->normalize = v[MH3_TEXGEN_NORMALIZE];
                const unsigned post = v[MH3_TEXGEN_POST_MATRIX_ROW];
                if (post != 61) {
                    if (post % 3 || post / 3 >= 8)
                        return MH3_RES_UNSUPPORTED;
                    if (!mesh->texture_matrices || !(mesh->texture_matrix_known & (1u << (post / 3))))
                        return MH3_RES_ABSENT;
                    memcpy(gen->post.v, mesh->texture_matrices[post / 3], sizeof gen->post.v);
                    if (!finite_matrix(&gen->post))
                        return MH3_RES_INVALID;
                    gen->post_identity = 0;
                }
            }
            const unsigned q = indexed ? indexed_q(mesh, gen, i) : nonzero_q(mesh, gen) ? 1 : zero_q(mesh, gen) ? 2 : 0;
            if (q == 1)
                result.nonzero_projective_mask |= 1u << i;
            else if (q == 2)
                result.zero_projective_mask |= 1u << i;
        }
    result.texcoord_mask = mask;
    *out = result;
    return MH3_RES_OK;
}
