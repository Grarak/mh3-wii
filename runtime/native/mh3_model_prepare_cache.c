#include "mh3_model_prepare_cache.h"
#include <assert.h>
#include <string.h>

int mh3_model_scope_recipe_matches(const mh3_model_scope_recipe *recipe, uint64_t revision, uint32_t material,
                                   const mh3_model_material_bindings *replacements) {
    assert(recipe && replacements);
    if (recipe->structure_revision != revision || recipe->scope.material_address != material)
        return 0;
    for (unsigned i = 0; i < MH3_MAT_RESOURCE_COUNT; i++)
        if (recipe->replacements.resource_slots[i] != replacements->resource_slots[i])
            return 0;
    for (unsigned i = 0; i < 3; i++)
        if (recipe->replacements.vertex_tables[i] != replacements->vertex_tables[i])
            return 0;
    return 1;
}

int mh3_model_shape_rows_unchecked(const uint32_t texture_sources[8], uint32_t rows[8]) {
    int identity = 1;
    assert(texture_sources && rows);
    for (unsigned i = 0; i < 8; i++) {
        const uint32_t source = texture_sources[i];
        rows[i] = source == 1 ? 30 + 3 * i : source == 2 ? 0 : 60;
        if (source == 1 || source == 2)
            identity = 0;
    }
    return identity;
}

static mh3_res_status extent(mh3_matrix_table table, size_t index, size_t bytes) {
    if (!table.bytes.data)
        return MH3_RES_INVALID;
    if (index >= table.count || table.stride < bytes || index > SIZE_MAX / table.stride)
        return MH3_RES_RANGE;
    const size_t at = index * table.stride;
    return at > table.bytes.size || bytes > table.bytes.size - at ? MH3_RES_RANGE : MH3_RES_OK;
}

mh3_res_status mh3_model_shape_pose_extents(const mh3_shape_matrix_input *in, const mh3_matrix_sources *sources) {
    if (!in || !sources)
        return MH3_RES_INVALID;
    if (!in->valid)
        return MH3_RES_OK;
    if (in->matrix_id < 0)
        return sources->positions.bytes.data ? MH3_RES_OK : MH3_RES_INVALID;
    const size_t id = (uint32_t)in->matrix_id & 0xffff;
    mh3_res_status status;
    if (!in->direct_position) {
        status = extent(sources->positions, id, 48);
        if (status)
            return status;
        if (in->has_normal) {
            status = extent(sources->normals, id, 36);
            if (status)
                return status;
        }
    }
    for (unsigned i = 0; i < 8; i++)
        if (in->texture_source[i] == 1)
            return sources->normals.bytes.data ? extent(sources->normals, id, 36) : extent(sources->positions, id, 48);
    return MH3_RES_OK;
}

void mh3_model_shape_input_unchecked(int32_t matrix_id, int has_normal, const uint8_t texture_sources[32], int current_valid, int view_valid,
                                     const mh3_matrix34 *position, const mh3_matrix34 *normal, mh3_shape_matrix_input *out) {
    assert(texture_sources && out && (has_normal == 0 || has_normal == 1));
    assert((current_valid == 0 || current_valid == 1) && (view_valid == 0 || view_valid == 1));
    memset(out, 0, sizeof *out);
    out->valid = 1;
    out->matrix_id = matrix_id;
    out->has_normal = has_normal;
    out->direct_position = position;
    out->direct_normal = position ? normal : NULL;
    out->current_is_valid = current_valid;
    out->view_array_is_valid = view_valid;
    for (unsigned i = 0; i < 8; i++) {
        const uint8_t *p = texture_sources + 4 * i;
        out->texture_source[i] = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
    }
}
