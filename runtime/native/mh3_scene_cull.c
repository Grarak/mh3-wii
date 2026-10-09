#include "mh3_scene_cull.h"
#include <string.h>

static int finite3(const float v[3]) {
    return __builtin_isfinite(v[0]) && __builtin_isfinite(v[1]) && __builtin_isfinite(v[2]);
}

int mh3_scene_cull_add(mh3_scene_cull_record *record, uint32_t id, const float min[3], const float max[3]) {
    if (!record || !min || !max || !finite3(min) || !finite3(max))
        return 0;
    for (uint32_t i = 0; i < record->count; i++) {
        mh3_cull_box *box = &record->pairs[i].box;
        if (record->pairs[i].id != id)
            continue;
        for (unsigned c = 0; c < 3; c++) {
            if (min[c] < box->min[c])
                box->min[c] = min[c];
            if (max[c] > box->max[c])
                box->max[c] = max[c];
        }
        return 1;
    }
    if (record->count == record->capacity)
        return 0;
    mh3_scene_cull_pair *pair = &record->pairs[record->count++];
    pair->id = id;
    memcpy(pair->box.min, min, sizeof pair->box.min);
    memcpy(pair->box.max, max, sizeof pair->box.max);
    return 1;
}

int mh3_scene_cull_add_mesh(mh3_scene_cull_record *record, const mh3_pack_mesh *mesh, int32_t matrix_id, int selectors, const mh3_pack_palette_batch *batches, uint32_t batch_count) {
    if (!record || !mesh)
        return 0;
    if (!batch_count) {
        if (matrix_id < 0 || selectors)
            return 0;
        return mh3_scene_cull_add(record, (uint32_t)matrix_id & 0xffffu, mesh->bounds_min, mesh->bounds_max);
    }
    if (!batches)
        return 0;
    /* A shape with a matrix ID first loads it into slot 0 (the rigid palette). */
    uint32_t loaded[10] = {(uint32_t)matrix_id & 0xffffu};
    uint16_t known = matrix_id >= 0 ? 1 : 0;
    for (uint32_t b = 0; b < batch_count; b++) {
        const mh3_pack_palette_batch *batch = &batches[b];
        for (unsigned slot = 0; slot < 10; slot++)
            if (batch->position_known & (1u << slot)) {
                if (batch->position_source[slot] == UINT16_MAX)
                    return 0;
                loaded[slot] = batch->position_source[slot];
                known |= (uint16_t)(1u << slot);
            }
        const uint16_t used = selectors ? batch->position_required : 1;
        if (!used || (used & ~known) || (used >> 10))
            return 0;
        for (unsigned slot = 0; slot < 10; slot++)
            if ((used & (1u << slot)) && !mh3_scene_cull_add(record, loaded[slot], mesh->bounds_min, mesh->bounds_max))
                return 0;
    }
    return 1;
}

int mh3_scene_cull_read_matrix(const uint8_t source[48], float out[12]) {
    for (unsigned i = 0; i < 12; i++) {
        const uint8_t *p = source + i * 4;
        const uint32_t bits = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
        memcpy(&out[i], &bits, 4);
        if (!__builtin_isfinite(out[i]))
            return 0;
    }
    return 1;
}
