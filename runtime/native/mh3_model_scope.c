#include "mh3_model_scope.h"
#include <math.h>
#include <string.h>

static uint32_t word(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static uint16_t half(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}
static void put(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}
static void put_half(uint8_t *p, uint16_t value) {
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)value;
}
static mh3_res_status map(const mh3_res_memory *memory, uint32_t address, size_t size, mh3_res_span *out) {
    mh3_res_memory_view view;
    const mh3_res_status status = mh3_res_runtime_map(memory, address, size, &view);
    if (status)
        return status;
    *out = (mh3_res_span){view.span.data + view.offset, size};
    return MH3_RES_OK;
}
static mh3_res_status inline_record(uint32_t address, uint32_t size, uint32_t offset, uint32_t required, uint32_t *out) {
    if (offset > size || required > size - offset || address > UINT32_MAX - offset)
        return MH3_RES_RANGE;
    *out = address + offset;
    return MH3_RES_OK;
}
static mh3_res_status offset_record(const mh3_res_memory *memory, mh3_res_span material, uint32_t address, uint32_t field, size_t required, uint32_t *out) {
    if (field > material.size || 4 > material.size - field)
        return MH3_RES_RANGE;
    const uint32_t raw = word(material.data + field);
    if (!raw)
        return MH3_RES_ABSENT;
    const int64_t delta = raw & 0x80000000u ? (int64_t)raw - INT64_C(4294967296) : raw;
    const int64_t target = address + delta;
    if (target < 0 || target > UINT32_MAX || (target & 3))
        return MH3_RES_RANGE;
    mh3_res_span span;
    const mh3_res_status status = map(memory, (uint32_t)target, required, &span);
    if (status)
        return status;
    *out = (uint32_t)target;
    return MH3_RES_OK;
}
mh3_res_status mh3_model_material_bindings_read(mh3_res_span swaps, mh3_model_material_bindings *out) {
    if (!swaps.data || !out)
        return MH3_RES_INVALID;
    if (swaps.size < 56)
        return MH3_RES_RANGE;
    mh3_model_material_bindings result;
    for (unsigned i = 0; i < 11; i++)
        result.resource_slots[i] = word(swaps.data + i * 4);
    for (unsigned i = 0; i < 3; i++)
        result.vertex_tables[i] = word(swaps.data + 44 + i * 4);
    *out = result;
    return MH3_RES_OK;
}
mh3_res_status mh3_model_shape_cache(mh3_res_mut_span cache, mh3_res_span source) {
    if (!cache.data || !source.data)
        return MH3_RES_INVALID;
    if (cache.size < 12 || source.size < 12)
        return MH3_RES_RANGE;
    uint8_t record[12];
    memcpy(record, source.data, 12);
    memcpy(cache.data, record, 12);
    return MH3_RES_OK;
}
mh3_res_status mh3_model_material_scope_read(const mh3_res_memory *memory, uint32_t address, uint32_t version, const mh3_model_material_bindings *swaps, mh3_model_material_scope *out) {
    if (!memory || !out)
        return MH3_RES_INVALID;
    mh3_model_material_scope scope;
    memset(&scope, 0, sizeof scope);
    scope.material_address = address;
    if (swaps) {
        scope.selected = *swaps;
        for (unsigned i = 0; i < 11; i++)
            if (swaps->resource_slots[i])
                scope.replacement_mask |= (uint16_t)(1u << i);
    }
    if (!address) {
        *out = scope;
        return MH3_RES_OK;
    }
    if (version != 11)
        return MH3_RES_UNSUPPORTED;
    if (address & 3u)
        return MH3_RES_RANGE;
    mh3_res_span material;
    mh3_res_status status = map(memory, address, 0x44, &material);
    if (status)
        return status;
    const uint32_t size = word(material.data);
    if (size < 0x418)
        return MH3_RES_RANGE;
    status = map(memory, address, size, &material);
    if (status)
        return status;
    status = offset_record(memory, material, address, 0x34, 1, &scope.fur_address);
    if (status && status != MH3_RES_ABSENT)
        return status;
    const uint32_t offsets[11] = {0x40, 0x144, 0x14, 0, 0, 0, 0, 0x3f0, 0, 0x1c, 0x1a8};
    const uint32_t sizes[11] = {260, 100, 8, 32, 32, 128, 64, 40, 160, 12, 0x248};
    uint32_t list = 0;
    for (unsigned i = 0; i < 11; i++) {
        if (scope.selected.resource_slots[i]) {
            if (scope.selected.resource_slots[i] & 3u)
                return MH3_RES_RANGE;
            mh3_res_span replacement;
            status = map(memory, scope.selected.resource_slots[i], sizes[i], &replacement);
        } else if (i == MH3_MAT_TEV) {
            status = offset_record(memory, material, address, 0x28, 32, &scope.selected.resource_slots[i]);
        } else if (i == MH3_MAT_PIXEL_STATE || i == MH3_MAT_TEV_COLORS || i == MH3_MAT_INDIRECT_MATRIX_SCALE || i == MH3_MAT_TEXCOORD_GENERATION) {
            if (!list) {
                status = offset_record(memory, material, address, 0x3c, 384, &list);
                if (status)
                    return status;
            }
            const uint32_t offset = i == MH3_MAT_PIXEL_STATE ? 0 : i == MH3_MAT_TEV_COLORS ? 32 : i == MH3_MAT_INDIRECT_MATRIX_SCALE ? 160 : 224;
            if (list > UINT32_MAX - offset)
                return MH3_RES_RANGE;
            scope.selected.resource_slots[i] = list + offset;
            mh3_res_span list_record;
            status = map(memory, list + offset, sizes[i], &list_record);
        } else
            status = inline_record(address, size, offsets[i], sizes[i], &scope.selected.resource_slots[i]);
        if (status)
            return status;
    }
    *out = scope;
    return MH3_RES_OK;
}
mh3_res_status mh3_model_material_prepare_scope(const mh3_model_material_scope *scope, uint32_t control, uint32_t indirect_operator, int ignore, mh3_model_material_prepare_step prepare, void *owner) {
    if (!scope || !prepare || (ignore != 0 && ignore != 1))
        return MH3_RES_INVALID;
    mh3_model_material_step step;
    memset(&step, 0, sizeof step);
    step.force_light_off = (control & 8) != 0;
    if (ignore || !scope->material_address) {
        if (!indirect_operator)
            return MH3_RES_OK;
        step.action = MH3_MATERIAL_PREP_INDIRECT;
        step.resource_address = scope->replacement_mask & (1u << MH3_MAT_INDIRECT_MATRIX_SCALE) ? scope->selected.resource_slots[MH3_MAT_INDIRECT_MATRIX_SCALE] : 0;
        step.indirect_operator = indirect_operator;
        return prepare(owner, &step);
    }
    static const struct {
        mh3_model_material_action action;
        mh3_material_resource_kind kind;
    } sequence[] = {{MH3_MATERIAL_PREP_FOG, MH3_MAT_MISC},
                    {MH3_MATERIAL_PREP_MISC, MH3_MAT_MISC},
                    {MH3_MATERIAL_PREP_PALETTES, MH3_MAT_PALETTE_OBJECTS},
                    {MH3_MATERIAL_PREP_TEXTURES, MH3_MAT_TEXTURE_OBJECTS},
                    {MH3_MATERIAL_PREP_GENMODE, MH3_MAT_GENERAL_MODE},
                    {MH3_MATERIAL_PREP_PIPELINE, MH3_MAT_TEV},
                    {MH3_MATERIAL_PREP_PIXEL, MH3_MAT_PIXEL_STATE},
                    {MH3_MATERIAL_PREP_COLORS, MH3_MAT_TEV_COLORS},
                    {MH3_MATERIAL_PREP_INDIRECT, MH3_MAT_INDIRECT_MATRIX_SCALE},
                    {MH3_MATERIAL_PREP_LIGHTS, MH3_MAT_MISC},
                    {MH3_MATERIAL_PREP_CHANNELS, MH3_MAT_CHANNELS},
                    {MH3_MATERIAL_PREP_TEXGEN, MH3_MAT_TEXCOORD_GENERATION},
                    {MH3_MATERIAL_PREP_SRT, MH3_MAT_TEXTURE_SRT}};
    for (unsigned i = 0; i < sizeof sequence / sizeof sequence[0]; i++) {
        step.action = sequence[i].action;
        step.resource_address = scope->selected.resource_slots[sequence[i].kind];
        step.indirect_operator = step.action == MH3_MATERIAL_PREP_INDIRECT ? indirect_operator : 0;
        const mh3_res_status status = prepare(owner, &step);
        if (status)
            return status;
    }
    return MH3_RES_OK;
}
mh3_res_status mh3_model_coordinate_table(mh3_res_mut_span cache, const uint8_t mapping[8]) {
    if (!cache.data || !mapping)
        return MH3_RES_INVALID;
    if (cache.size < 108)
        return MH3_RES_RANGE;
    uint8_t table[8];
    memcpy(table, mapping, 8);
    for (unsigned i = 0; i < 8; i++)
        if (table[i] > 7 && table[i] != 255)
            return MH3_RES_UNSUPPORTED;
    const uint32_t flags = word(cache.data);
    if (!(flags & 2) || memcmp(cache.data + 100, table, 8)) {
        memcpy(cache.data + 100, table, 8);
        put(cache.data, (flags & ~1u) | 2u);
    }
    return MH3_RES_OK;
}
mh3_res_status mh3_model_coordinate_prepare(mh3_res_mut_span cache, uint32_t count, uint8_t map_known, const mh3_model_texcoord_sizes *maps, const mh3_model_texcoord_sizes *current,
                                            mh3_model_texcoord_sizes *out, uint8_t *written, int *apply) {
    if (!cache.data || !maps || !current || !out || !written || !apply || count > 8)
        return MH3_RES_INVALID;
    if (cache.size < 108)
        return MH3_RES_RANGE;
    const uint32_t flags = word(cache.data);
    if (!(flags & 2) || flags & 1 || !count) {
        *apply = 0;
        return MH3_RES_OK;
    }
    mh3_prepared_material recipe;
    memset(&recipe, 0, sizeof recipe);
    memcpy(recipe.texcoord_map, cache.data + 100, 8);
    recipe.texcoord_map_known = map_known;
    mh3_model_texcoord_sizes result;
    uint8_t changed = 0;
    mh3_res_status status = mh3_model_texcoord_sizes_select(&recipe, count, maps, current, &result, &changed);
    if (status)
        return status;
    for (unsigned i = 0; i < count; i++)
        if (changed & (1u << i)) {
            const unsigned map = recipe.texcoord_map[i];
            if (result.width[i] != half(cache.data + 4 + map * 4) || result.height[i] != half(cache.data + 6 + map * 4))
                return MH3_RES_INVALID;
        }
    for (unsigned i = 0; i < count; i++)
        if (changed & (1u << i)) {
            put_half(cache.data + 36 + i * 8, result.width[i]);
            put_half(cache.data + 38 + i * 8, result.height[i]);
        }
    put(cache.data, flags | 1u);
    *out = result;
    *written = changed;
    *apply = 1;
    return MH3_RES_OK;
}
mh3_res_status mh3_model_indirect_scope(const mh3_native_material_state *base, const mh3_indirect_output *op, mh3_native_material_state *out) {
    if (!base || !op || !out || op->valid_mask > 7 || (op->active != 0 && op->active != 1))
        return MH3_RES_INVALID;
    mh3_native_material_state result = *base;
    for (unsigned slot = 0; slot < 3; slot++)
        if (op->active && (op->valid_mask & (1u << slot))) {
            float values[6], maximum = 0;
            int exponent = 0;
            for (unsigned row = 0; row < 2; row++)
                for (unsigned col = 0; col < 3; col++) {
                    const float value = op->matrix[slot].v[row][col];
                    if (!isfinite(value))
                        return MH3_RES_UNSUPPORTED;
                    values[row * 3 + col] = value;
                    const float absolute = fabsf(value);
                    if (absolute > maximum)
                        maximum = absolute;
                }
            if (maximum >= 1.0f) {
                while (maximum >= 1.0f && exponent < 46) {
                    maximum *= 0.5f;
                    for (unsigned i = 0; i < 6; i++)
                        values[i] *= 0.5f;
                    exponent++;
                }
            } else if (maximum < 0.5f) {
                do {
                    maximum *= 2.0f;
                    for (unsigned i = 0; i < 6; i++)
                        values[i] *= 2.0f;
                    exponent--;
                } while (maximum < 0.5f && exponent > -17);
            }
            mh3_pack_indirect_matrix matrix = {0};
            matrix.known = 127;
            matrix.scale_exponent = exponent;
            for (unsigned i = 0; i < 6; i++) {
                const float scaled = values[i] * 1024.0f;
                if ((double)scaled < INT32_MIN || (double)scaled > INT32_MAX)
                    return MH3_RES_UNSUPPORTED;
                const uint32_t bits = (uint32_t)(int32_t)scaled & 2047u;
                matrix.coefficients[i] = (int16_t)(bits >= 1024 ? (int32_t)bits - 2048 : (int32_t)bits);
            }
            result.indirect_matrix[slot] = matrix;
        }
    *out = result;
    return MH3_RES_OK;
}
