#include "mh3_model_material_updates.h"
#include <limits.h>
#include <math.h>

static mh3_res_status ready(const mh3_native_material_state *state, mh3_res_mut_span bytes, size_t required, uint64_t revision) {
    if (!state || !bytes.data || !revision || revision <= state->generation)
        return MH3_RES_INVALID;

    return bytes.size < required ? MH3_RES_RANGE : MH3_RES_OK;
}

static void packet(uint8_t *out, uint32_t value) {
    out[0] = 0x61;
    out[1] = (uint8_t)(value >> 24);
    out[2] = (uint8_t)(value >> 16);
    out[3] = (uint8_t)(value >> 8);
    out[4] = (uint8_t)value;
}

static void pixel_value(mh3_pack_pixel_inputs *pixel, unsigned field, uint32_t value) {
    pixel->known |= UINT64_C(1) << field;
    pixel->values[field] = value;
}

mh3_res_status mh3_model_material_alpha_compare(mh3_native_material_state *state, mh3_res_mut_span pixel, uint32_t compare0, uint8_t reference0, uint32_t operation, uint32_t compare1,
                                                uint8_t reference1, uint64_t revision) {
    mh3_res_status status = ready(state, pixel, 32, revision);

    if (status != MH3_RES_OK)
        return status;

    if (compare0 > 7 || compare1 > 7 || operation > 3)
        return MH3_RES_INVALID;

    pixel_value(&state->pixel, MH3_PIXEL_ALPHA_COMPARE0, compare0);
    pixel_value(&state->pixel, MH3_PIXEL_ALPHA_REFERENCE0, reference0);
    pixel_value(&state->pixel, MH3_PIXEL_ALPHA_COMPARE1, compare1);
    pixel_value(&state->pixel, MH3_PIXEL_ALPHA_REFERENCE1, reference1);
    pixel_value(&state->pixel, MH3_PIXEL_ALPHA_OPERATION, operation);
    state->pixel.classification = 0;
    state->generation = revision;
    packet(pixel.data, 0xf3000000u | compare0 << 16 | reference0 | (uint32_t)reference1 << 8 | operation << 22 | compare1 << 19);
    return MH3_RES_OK;
}

mh3_res_status mh3_model_material_blend(mh3_native_material_state *state, mh3_res_mut_span pixel, uint32_t mode, uint32_t source, uint32_t destination, uint32_t logic, uint64_t revision) {
    mh3_res_status status = ready(state, pixel, 32, revision);

    if (status != MH3_RES_OK)
        return status;

    if (mode > 3 || source > 7 || destination > 7 || logic > 15)
        return MH3_RES_INVALID;

    const uint32_t blend = mode == 1 || mode == 3, logical = mode == 2, subtract = mode == 3;
    pixel_value(&state->pixel, MH3_PIXEL_BLEND_ENABLE, blend);
    pixel_value(&state->pixel, MH3_PIXEL_LOGIC_ENABLE, logical);
    pixel_value(&state->pixel, MH3_PIXEL_BLEND_SUBTRACT, subtract);
    pixel_value(&state->pixel, MH3_PIXEL_BLEND_SOURCE, source);
    pixel_value(&state->pixel, MH3_PIXEL_BLEND_DESTINATION, destination);
    pixel_value(&state->pixel, MH3_PIXEL_LOGIC_OPERATION, logic);
    state->pixel.classification = 0;
    state->generation = revision;
    packet(pixel.data + 10, 0xfe00ffe3u);
    packet(pixel.data + 15, 0x41000000u | logic << 12 | subtract << 11 | source << 8 | destination << 5 | logical << 1 | blend);
    return MH3_RES_OK;
}

mh3_res_status mh3_model_material_tev_color(mh3_native_material_state *state, mh3_res_mut_span colors, uint32_t id, const uint8_t rgba[4], uint64_t revision) {
    mh3_res_status status = ready(state, colors, 128, revision);

    if (status != MH3_RES_OK)
        return status;

    if (!rgba || id < 1 || id > 3)
        return MH3_RES_INVALID;

    const uint8_t input[4] = {rgba[0], rgba[1], rgba[2], rgba[3]};

    for (unsigned i = 0; i < 4; i++)
        state->color[id].rgba[i] = input[i];

    state->color[id].known = 15;
    state->generation = revision;
    uint8_t *out = colors.data + (id - 1) * 20;
    packet(out, (0xe0u + id * 2) << 24 | (uint32_t)input[3] << 12 | input[0]);
    const uint32_t high = (0xe1u + id * 2) << 24 | (uint32_t)input[1] << 12 | input[2];
    packet(out + 5, high);
    packet(out + 10, high);
    packet(out + 15, high);
    return MH3_RES_OK;
}

mh3_res_status mh3_model_material_konst_color(mh3_native_material_state *state, mh3_res_mut_span colors, uint32_t id, const uint8_t rgba[4], uint64_t revision) {
    mh3_res_status status = ready(state, colors, 128, revision);

    if (status != MH3_RES_OK)
        return status;

    if (!rgba || id > 3)
        return MH3_RES_INVALID;

    const uint8_t input[4] = {rgba[0], rgba[1], rgba[2], rgba[3]};

    for (unsigned i = 0; i < 4; i++)
        state->konst[id].rgba[i] = input[i];

    state->konst[id].known = 15;
    state->generation = revision;
    uint8_t *out = colors.data + 64 + id * 10;
    packet(out, (0xe0u + id * 2) << 24 | 0x800000u | (uint32_t)input[3] << 12 | input[0]);
    packet(out + 5, (0xe1u + id * 2) << 24 | 0x800000u | (uint32_t)input[1] << 12 | input[2]);
    return MH3_RES_OK;
}

mh3_res_status mh3_model_material_indirect_matrix(mh3_native_material_state *state, mh3_res_mut_span indirect, uint32_t id, const float matrix[12], int8_t exponent, uint64_t revision) {
    mh3_res_status status = ready(state, indirect, 64, revision);

    if (status != MH3_RES_OK)
        return status;

    if (!matrix || id < 1 || id > 3)
        return MH3_RES_INVALID;

    mh3_pack_indirect_matrix result = {0};
    result.known = 127;
    /* Preserve truncation and the low signed11 hardware bits. Nonfinite or
     * out-of-int32 inputs need PPC conversion semantics not proven here. */
    for (unsigned row = 0; row < 2; row++)
        for (unsigned col = 0; col < 3; col++) {
            const float scaled = matrix[row * 4 + col] * 1024.0f;

            if (!isfinite(scaled) || (double)scaled < INT32_MIN || (double)scaled > INT32_MAX)
                return MH3_RES_UNSUPPORTED;

            const uint32_t bits = (uint32_t)(int32_t)scaled & 2047u;
            result.coefficients[row * 3 + col] = (int16_t)(bits >= 1024 ? ((int32_t)bits - 2048) : (int32_t)bits);
        }
    const uint32_t encoded = ((uint32_t)((int)exponent + 17)) & 63u;
    result.scale_exponent = (int32_t)encoded - 17;
    static const unsigned offsets[3] = {10, 32, 47};
    uint8_t *out = indirect.data + offsets[id - 1];

    for (unsigned col = 0; col < 3; col++)
        packet(out + col * 5,
               (6 + (id - 1) * 3 + col) << 24 | ((encoded >> (col * 2)) & 3u) << 22 | ((uint32_t)result.coefficients[col] & 2047u) | ((uint32_t)result.coefficients[col + 3] & 2047u) << 11);
    state->indirect_matrix[id - 1] = result;
    state->generation = revision;
    return MH3_RES_OK;
}

mh3_res_status mh3_model_material_channel_color(mh3_native_material_state *state, mh3_res_mut_span channels, uint32_t id, int ambient, const uint8_t rgba[4], uint64_t revision) {
    mh3_res_status status = ready(state, channels, 40, revision);

    if (status != MH3_RES_OK)
        return status;

    if (!rgba || id > 5 || (ambient != 0 && ambient != 1))
        return MH3_RES_INVALID;

    const uint8_t input[4] = {rgba[0], rgba[1], rgba[2], rgba[3]};
    uint8_t *typed = ambient ? state->channel[id & 1].ambient_color : state->channel[id & 1].material_color;
    uint8_t *compat = channels.data + (id & 1) * 20 + (ambient ? 8 : 4);

    if (!(id & 2))
        for (unsigned i = 0; i < 3; i++)
            typed[i] = compat[i] = input[i];
    if ((id & 2) || id >= 4)
        typed[3] = compat[3] = input[3];
    /* Original channel flags control whether these bytes are consumed. A
     * color update must not enable a previously disabled channel field. */
    state->generation = revision;
    return MH3_RES_OK;
}
