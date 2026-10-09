#include "mh3_model_draw.h"
#include <string.h>

static uint32_t read_be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static int contains_resource(const mh3_res_model *model, size_t offset, size_t bytes) {
    return offset >= model->offset && offset - model->offset <= model->size && bytes <= model->size - (offset - model->offset);
}

mh3_res_status mh3_model_draw_walk(const mh3_res_model *model, size_t cursor, mh3_res_span visibility, uint32_t options, mh3_model_draw_emit emit, void *user, mh3_model_draw_stats *stats) {
    mh3_model_draw_stats totals = {0};
    size_t previous_material = 0;
    int has_previous_material = 0;
    const uint32_t flags = 1u | (options & 8u) | ((options & 4u) ? 16u : 0u);
    mh3_res_status status;
    if (stats)
        memset(stats, 0, sizeof(*stats));
    if (!model || !emit || (!visibility.data && visibility.size))
        return MH3_RES_INVALID;
    for (;;) {
        mh3_model_draw_item item;
        uint32_t node_id;
        int visible;
        memset(&item, 0, sizeof(item));
        status = mh3_res_draw_next(model, &cursor, &item.draw);
        if (status == MH3_RES_END) {
            status = MH3_RES_OK;
            break;
        }
        if (status != MH3_RES_OK)
            break;
        ++totals.records;
        status = mh3_res_model_entry(model, MH3_RES_NODES, item.draw.node_id, &item.node);
        if (status != MH3_RES_OK)
            break;
        status = mh3_res_model_entry(model, MH3_RES_SHAPES, item.draw.shape_id, &item.shape);
        if (status != MH3_RES_OK)
            break;
        if (!contains_resource(model, item.node.offset, 0x18) || !contains_resource(model, item.shape.offset, 0x38)) {
            status = MH3_RES_RANGE;
            break;
        }
        node_id = read_be32(model->container.data + item.node.offset + 0x0c);
        if (visibility.data) {
            if (node_id >= visibility.size) {
                status = MH3_RES_RANGE;
                break;
            }
            visible = visibility.data[node_id] != 0;
        } else {
            visible = (read_be32(model->container.data + item.node.offset + 0x14) & 0x100u) != 0;
        }
        if (!visible) {
            ++totals.hidden_nodes;
            continue;
        }
        if (read_be32(model->container.data + item.shape.offset + 0x34) & 2u) {
            ++totals.hidden_shapes;
            continue;
        }
        status = mh3_res_model_entry(model, MH3_RES_MATERIALS, item.draw.material_id, &item.material);
        if (status != MH3_RES_OK)
            break;
        if (!contains_resource(model, item.material.offset, 0x10)) {
            status = MH3_RES_RANGE;
            break;
        }
        item.material_index = read_be32(model->container.data + item.material.offset + 0x0c);
        item.dispatch_flags = flags;
        if (has_previous_material && previous_material == item.material.offset) {
            item.dispatch_flags |= 16u;
            ++totals.repeated_materials;
        }
        status = emit(user, &item);
        if (status != MH3_RES_OK)
            break;
        previous_material = item.material.offset;
        has_previous_material = 1;
        ++totals.submitted;
    }
    if (stats)
        *stats = totals;
    return status;
}

mh3_res_status mh3_model_material_select(mh3_res_span replacement, uint32_t material, mh3_model_material_bindings *out) {
    /* Serialized replacement field -> dispatch resource slot. These layouts
     * are from Ghidra's 80077ed4 C, not guessed from texture/material names. */
    static const uint16_t strides[11] = {0x104, 0x64, 0x248, 0x28, 8, 12, 0x20, 0x80, 0x40, 0xa0, 0x200};
    static const uint8_t slots[11] = {MH3_MAT_TEXTURE_OBJECTS, MH3_MAT_PALETTE_OBJECTS,       MH3_MAT_TEXTURE_SRT,         MH3_MAT_CHANNELS, MH3_MAT_GENERAL_MODE, MH3_MAT_MISC, MH3_MAT_PIXEL_STATE,
                                      MH3_MAT_TEV_COLORS,      MH3_MAT_INDIRECT_MATRIX_SCALE, MH3_MAT_TEXCOORD_GENERATION, MH3_MAT_TEV};
    unsigned i;
    if (!out)
        return MH3_RES_INVALID;
    memset(out, 0, sizeof(*out));
    if (!replacement.data || replacement.size < 64)
        return MH3_RES_RANGE;
    for (i = 0; i < 11; ++i) {
        const uint32_t base = read_be32(replacement.data + (i + 2) * 4);
        if (base) {
            const uint64_t selected = (uint64_t)base + (uint64_t)material * strides[i];
            if (selected > UINT32_MAX) {
                memset(out, 0, sizeof(*out));
                return MH3_RES_RANGE;
            }
            out->resource_slots[slots[i]] = (uint32_t)selected;
        }
    }
    if (!(read_be32(replacement.data) & 1u))
        for (i = 0; i < 3; ++i)
            out->vertex_tables[i] = read_be32(replacement.data + (i + 13) * 4);
    return MH3_RES_OK;
}
