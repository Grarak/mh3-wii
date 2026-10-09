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
    for (unsigned i = 0; i < 8; i++) {
        const uint32_t source = texture_sources[i];
        rows[i] = source == 1 ? 30 + 3 * i : source == 2 ? 0 : 60;
        if (source == 1 || source == 2)
            identity = 0;
    }
    return identity;
}

void mh3_model_shape_input_unchecked(int32_t matrix_id, int has_normal, const uint8_t texture_sources[32], int current_valid, int view_valid,
                                     const mh3_matrix34 *position, const mh3_matrix34 *normal, mh3_shape_matrix_input *out) {
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
