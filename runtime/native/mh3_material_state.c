#include "mh3_material_state.h"
#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

/* These CPU producers preserve scalar arithmetic and nonfinite handling even
 * when their renderer target enables fast math. */
#if defined(__clang__) && !defined(__arm__)
#pragma float_control(precise, on)
#endif

_Static_assert(sizeof(mh3_shader_light) == 80, "shader light layout");
_Static_assert(sizeof(mh3_shader_fog) == 96, "shader fog layout");
_Static_assert(sizeof(mh3_pack_tev_pass) == 9, "typed TEV pass fields");
_Static_assert(offsetof(mh3_pack_tev_stage, indirect_add_previous) - offsetof(mh3_pack_tev_stage, color) == 33, "typed TEV field order");
#define BIT(n) (UINT64_C(1) << (n))
static mh3_res_status fail(mh3_material_diagnostic *out, mh3_res_status status, mh3_material_group group, uint32_t index, uint64_t fields) {
    if (out)
        *out = (mh3_material_diagnostic){group, index, fields};
    return status;
}
static mh3_res_status mask_check(uint64_t known, uint64_t allowed, uint64_t required, mh3_material_group group, uint32_t index, mh3_material_diagnostic *out) {
    if ((known | required) & ~allowed)
        return fail(out, MH3_RES_INVALID, group, index, (known | required) & ~allowed);
    if (required & ~known)
        return fail(out, MH3_RES_ABSENT, group, index, required & ~known);
    return MH3_RES_OK;
}
static int tev_domain(unsigned field, uint8_t value) {
    static const uint8_t maximum[34] = {15, 15, 15, 15, 15, 2, 3, 1, 3, 7, 7, 7, 7, 15, 2, 3, 1, 3, 255, 255, 255, 3, 3, 31, 31, 3, 3, 7, 3, 11, 6, 6, 1, 1};
    if (value > maximum[field])
        return 0;
    if (field == MH3_TEV_COLOR_OPERATION || field == MH3_TEV_ALPHA_OPERATION)
        return value <= 1 || value >= 8;
    if (field == MH3_TEV_TEXTURE_MAP)
        return value < 8 || value == 255;
    if (field == MH3_TEV_TEXTURE_COORDINATE)
        return value < 8 || value == 255;
    if (field == MH3_TEV_RASTER_CHANNEL)
        return value <= 8 || value == 255;
    if (field == MH3_TEV_KONST_COLOR)
        return value < 8 || value >= 12;
    if (field == MH3_TEV_KONST_ALPHA)
        return value < 8 || value >= 16;
    if (field == MH3_TEV_INDIRECT_MATRIX)
        return value <= 3 || (value >= 5 && value <= 7) || (value >= 9 && value <= 11);
    return 1;
}
static mh3_res_status tev_check(const mh3_pack_tev_stage *stage, uint64_t required, uint32_t index, mh3_material_diagnostic *out, int domains) {
    const uint8_t *values = (const uint8_t *)&stage->color;
    mh3_res_status status = mask_check(stage->known, MH3_MATERIAL_TEV_MASK, required, MH3_MATERIAL_TEV, index, out);
    if (status)
        return status;
    if (domains)
        for (unsigned field = 0; field < MH3_TEV_FIELD_COUNT; ++field)
            if ((stage->known & BIT(field)) && !tev_domain(field, values[field]))
                return fail(out, MH3_RES_INVALID, MH3_MATERIAL_TEV, index, BIT(field));
    return MH3_RES_OK;
}
static mh3_res_status pixel_check(const mh3_pack_pixel_inputs *pixel, uint64_t required, mh3_material_diagnostic *out, int domains) {
    static const uint32_t maximum[22] = {7, 255, 7, 255, 3, 1, 7, 1, 1, 1, 1, 7, 7, 15, 1, 1, 1, 1, 255, 2, 2, 0xffffff};
    mh3_res_status status = mask_check(pixel->known, MH3_MATERIAL_PIXEL_MASK, required, MH3_MATERIAL_PIXEL, 0, out);
    if (status)
        return status;
    if (pixel->classification > 3)
        return fail(out, MH3_RES_INVALID, MH3_MATERIAL_PIXEL, 0, 0);
    if (domains)
        for (unsigned field = 0; field < MH3_PIXEL_FIELD_COUNT; ++field)
            if ((pixel->known & BIT(field)) && pixel->values[field] > maximum[field])
                return fail(out, MH3_RES_INVALID, MH3_MATERIAL_PIXEL, 0, BIT(field));
    return MH3_RES_OK;
}
static mh3_res_status channel_check(const mh3_pack_channel *channel, uint32_t required, uint32_t index, mh3_material_diagnostic *out, int domains) {
    mh3_res_status status = mask_check(channel->flags, 63, required, MH3_MATERIAL_CHANNEL, index, out);
    if (status)
        return status;
    const mh3_pack_channel_control *controls[2] = {&channel->color, &channel->alpha};
    if (domains)
        for (unsigned pass = 0; pass < 2; ++pass)
            if (channel->flags & (16u << pass)) {
                const mh3_pack_channel_control *control = controls[pass];
                if (control->material_source > 1 || control->ambient_source > 1 || control->lighting > 1 || control->diffuse > 2 || control->attenuation > 2)
                    return fail(out, MH3_RES_INVALID, MH3_MATERIAL_CHANNEL, index, 16u << pass);
            }
    return MH3_RES_OK;
}
static mh3_res_status constant_check(const mh3_pack_tev_color *color, uint32_t required, int konst, uint32_t index, mh3_material_diagnostic *out, int domains) {
    const mh3_material_group group = konst ? MH3_MATERIAL_KONST : MH3_MATERIAL_COLOR;
    mh3_res_status status = mask_check(color->known, 15, required, group, index, out);
    if (status)
        return status;
    if (domains)
        for (unsigned component = 0; component < 4; ++component)
            if (color->known & (1u << component))
                if (color->rgba[component] < (konst ? 0 : -1024) || color->rgba[component] > (konst ? 255 : 1023))
                    return fail(out, MH3_RES_INVALID, group, index, 1u << component);
    return MH3_RES_OK;
}
static mh3_res_status validate(const mh3_native_material_state *state, const mh3_material_requirements *requirements, mh3_material_diagnostic *out, int domains) {
    if (!state)
        return fail(out, MH3_RES_INVALID, MH3_MATERIAL_TOP, 0, 0);
    if (state->texgen_count > 8 || state->channel_count > 2 || state->tev_count > 16 || state->indirect_count > 4 || state->cull_mode > 3 || state->light_set_reference < -128 ||
        state->light_set_reference > 127 || state->fog_reference < -128 || state->fog_reference > 127)
        return fail(out, MH3_RES_INVALID, MH3_MATERIAL_TOP, 0, 0);
    if (state->clip_mode_known > 1 || (state->clip_mode_known && state->clip_disabled > 1))
        return fail(out, MH3_RES_INVALID, MH3_MATERIAL_CLIP, 0, 1);
    if (requirements && (requirements->require_clip > 1 || requirements->supported_features & ~MH3_MATERIAL_SUPPORT_ALL))
        return fail(out, MH3_RES_INVALID, MH3_MATERIAL_TOP, 0, 0);
    mh3_res_status status;
    for (unsigned index = 0; index < 16; ++index) {
        const uint64_t required = requirements ? requirements->tev[index] : 0;
        if (index >= state->tev_count) {
            if (required)
                return fail(out, MH3_RES_ABSENT, MH3_MATERIAL_TEV, index, required);
            continue;
        }
        status = tev_check(&state->tev[index], required, index, out, domains);
        if (status)
            return status;
    }
    for (unsigned index = 0; index < 8; ++index) {
        const uint32_t required = requirements ? requirements->texgen[index] : 0;
        if (index >= state->texgen_count) {
            if (required)
                return fail(out, MH3_RES_ABSENT, MH3_MATERIAL_TEXGEN, index, required);
            continue;
        }
        const mh3_pack_texgen *texgen = &state->texgen[index];
        static const uint8_t maximum[10] = {1, 1, 3, 12, 7, 7, 63, 63, 1, 1};
        status = mask_check(texgen->known, MH3_MATERIAL_TEXGEN_MASK, required, MH3_MATERIAL_TEXGEN, index, out);
        if (status)
            return status;
        if (domains)
            for (unsigned field = 0; field < 10; ++field)
                if ((texgen->known & (1u << field)) && texgen->values[field] > maximum[field])
                    return fail(out, MH3_RES_INVALID, MH3_MATERIAL_TEXGEN, index, 1u << field);
    }
    for (unsigned index = 0; index < 2; ++index) {
        const uint32_t required = requirements ? requirements->channel[index] : 0;
        if (index >= state->channel_count) {
            if (required)
                return fail(out, MH3_RES_ABSENT, MH3_MATERIAL_CHANNEL, index, required);
            continue;
        }
        status = channel_check(&state->channel[index], required, index, out, domains);
        if (status)
            return status;
    }
    for (unsigned index = 0; index < 4; ++index) {
        status = constant_check(&state->color[index], requirements ? requirements->color[index] : 0, 0, index, out, domains);
        if (status)
            return status;
        status = constant_check(&state->konst[index], requirements ? requirements->konst[index] : 0, 1, index, out, domains);
        if (status)
            return status;
        const mh3_pack_swap_table *swap = &state->swaps[index];
        status = mask_check(swap->known, 15, requirements ? requirements->swaps[index] : 0, MH3_MATERIAL_SWAP, index, out);
        if (status)
            return status;
        if (domains)
            for (unsigned component = 0; component < 4; ++component)
                if ((swap->known & (1u << component)) && swap->rgba[component] > 3)
                    return fail(out, MH3_RES_INVALID, MH3_MATERIAL_SWAP, index, 1u << component);
        const mh3_pack_indirect_stage *indirect = &state->indirect[index];
        const uint32_t required = requirements ? requirements->indirect[index] : 0;
        if (index >= state->indirect_count) {
            if (required)
                return fail(out, MH3_RES_ABSENT, MH3_MATERIAL_INDIRECT, index, required);
            continue;
        }
        status = mask_check(indirect->known, 15, required, MH3_MATERIAL_INDIRECT, index, out);
        if (status)
            return status;
        const uint8_t values[4] = {indirect->coordinate, indirect->map, indirect->scale_s, indirect->scale_t};
        if (domains)
            for (unsigned field = 0; field < 4; ++field)
                if ((indirect->known & (1u << field)) && (field < 2 ? values[field] > 7 && values[field] != 255 : values[field] > 8))
                    return fail(out, MH3_RES_INVALID, MH3_MATERIAL_INDIRECT, index, 1u << field);
    }
    for (unsigned index = 0; index < 3; ++index) {
        const mh3_pack_indirect_matrix *matrix = &state->indirect_matrix[index];
        status = mask_check(matrix->known, 127, requirements ? requirements->indirect_matrix[index] : 0, MH3_MATERIAL_INDIRECT_MATRIX, index, out);
        if (status)
            return status;
        if (domains)
            for (unsigned field = 0; field < 6; ++field)
                if ((matrix->known & (1u << field)) && (matrix->coefficients[field] < -1024 || matrix->coefficients[field] > 1023))
                    return fail(out, MH3_RES_INVALID, MH3_MATERIAL_INDIRECT_MATRIX, index, 1u << field);
        if (domains && (matrix->known & 64) && (matrix->scale_exponent < -17 || matrix->scale_exponent > 46))
            return fail(out, MH3_RES_INVALID, MH3_MATERIAL_INDIRECT_MATRIX, index, 64);
    }
    status = pixel_check(&state->pixel, requirements ? requirements->pixel : 0, out, domains);
    if (status)
        return status;
    if (requirements) {
        if (requirements->require_clip && !state->clip_mode_known)
            return fail(out, MH3_RES_ABSENT, MH3_MATERIAL_CLIP, 0, 1);
        if (state->clip_mode_known && state->clip_disabled && !(requirements->supported_features & MH3_MATERIAL_SUPPORT_CLIP_DISABLED))
            return fail(out, MH3_RES_UNSUPPORTED, MH3_MATERIAL_CLIP, 0, 1);
        const unsigned fields[3] = {MH3_PIXEL_DITHER, MH3_PIXEL_DESTINATION_ALPHA_ENABLE, MH3_PIXEL_DEPTH_TEXTURE_OPERATION};
        const uint32_t capabilities[3] = {MH3_MATERIAL_SUPPORT_DITHER, MH3_MATERIAL_SUPPORT_DESTINATION_ALPHA, MH3_MATERIAL_SUPPORT_DEPTH_TEXTURE};
        for (unsigned index = 0; index < 3; ++index)
            if ((state->pixel.known & BIT(fields[index])) && state->pixel.values[fields[index]] && !(requirements->supported_features & capabilities[index]))
                return fail(out, MH3_RES_UNSUPPORTED, MH3_MATERIAL_PIXEL, 0, BIT(fields[index]));
    }
    return MH3_RES_OK;
}
mh3_res_status mh3_material_state_validate(const mh3_native_material_state *state, const mh3_material_requirements *requirements, mh3_material_diagnostic *out) {
    return validate(state, requirements, out, 1);
}
mh3_res_status mh3_material_state_require(const mh3_native_material_state *state, const mh3_material_requirements *requirements, mh3_material_diagnostic *out) {
    return validate(state, requirements, out, 0);
}
mh3_res_status mh3_material_state_require_begin(mh3_material_requirement_proof *proof, const mh3_native_material_state *state, mh3_material_diagnostic *out) {
    if (!proof)
        return fail(out, MH3_RES_INVALID, MH3_MATERIAL_TOP, 0, 0);
    memset(proof, 0, sizeof *proof);
    const mh3_res_status status = mh3_material_state_validate(state, NULL, out);
    if (!status)
        proof->state = state;
    return status;
}
mh3_res_status mh3_material_state_require_step(mh3_material_requirement_proof *proof, const mh3_native_material_state *state, const mh3_material_requirements *requirements,
                                               mh3_material_diagnostic *out) {
    if (!proof || !state || proof->state != state || proof->capabilities_known > 1)
        return fail(out, MH3_RES_INVALID, MH3_MATERIAL_TOP, 0, 0);
    if (!requirements)
        return MH3_RES_OK;
    if (requirements->require_clip > 1 || requirements->supported_features & ~MH3_MATERIAL_SUPPORT_ALL)
        return fail(out, MH3_RES_INVALID, MH3_MATERIAL_TOP, 0, 0);
    const mh3_material_requirements *checked = &proof->checked;
    mh3_res_status status;
    /* Previously successful fields are known in this unchanged state. Inactive
     * records keep ABSENT precedence over request-domain errors, exactly as in
     * require. Read a known mask only when this phase consumes a new field. */
#define REQUIRE_DELTA(request, prior, active, known, allowed, group, index)                                                                                                                            \
    do {                                                                                                                                                                                               \
        const uint64_t added = (uint64_t)(request) & ~(uint64_t)(prior);                                                                                                                               \
        if (added) {                                                                                                                                                                                   \
            if (!(active))                                                                                                                                                                             \
                return fail(out, MH3_RES_ABSENT, group, index, added);                                                                                                                                 \
            status = mask_check(known, allowed, added, group, index, out);                                                                                                                             \
            if (status)                                                                                                                                                                                \
                return status;                                                                                                                                                                         \
        }                                                                                                                                                                                              \
    } while (0)
    for (unsigned index = 0; index < 16; ++index)
        REQUIRE_DELTA(requirements->tev[index], checked->tev[index], index < state->tev_count, state->tev[index].known, MH3_MATERIAL_TEV_MASK, MH3_MATERIAL_TEV, index);
    for (unsigned index = 0; index < 8; ++index)
        REQUIRE_DELTA(requirements->texgen[index], checked->texgen[index], index < state->texgen_count, state->texgen[index].known, MH3_MATERIAL_TEXGEN_MASK, MH3_MATERIAL_TEXGEN, index);
    for (unsigned index = 0; index < 2; ++index)
        REQUIRE_DELTA(requirements->channel[index], checked->channel[index], index < state->channel_count, state->channel[index].flags, 63, MH3_MATERIAL_CHANNEL, index);
    for (unsigned index = 0; index < 4; ++index) {
        REQUIRE_DELTA(requirements->color[index], checked->color[index], 1, state->color[index].known, 15, MH3_MATERIAL_COLOR, index);
        REQUIRE_DELTA(requirements->konst[index], checked->konst[index], 1, state->konst[index].known, 15, MH3_MATERIAL_KONST, index);
        REQUIRE_DELTA(requirements->swaps[index], checked->swaps[index], 1, state->swaps[index].known, 15, MH3_MATERIAL_SWAP, index);
        REQUIRE_DELTA(requirements->indirect[index], checked->indirect[index], index < state->indirect_count, state->indirect[index].known, 15, MH3_MATERIAL_INDIRECT, index);
    }
    for (unsigned index = 0; index < 3; ++index)
        REQUIRE_DELTA(requirements->indirect_matrix[index], checked->indirect_matrix[index], 1, state->indirect_matrix[index].known, 127, MH3_MATERIAL_INDIRECT_MATRIX, index);
    REQUIRE_DELTA(requirements->pixel, checked->pixel, 1, state->pixel.known, MH3_MATERIAL_PIXEL_MASK, MH3_MATERIAL_PIXEL, 0);
#undef REQUIRE_DELTA
    /* Promises may shrink, even when no required bit changed. Full domain
     * validation does not establish support for enabled optional features. */
    if (!proof->capabilities_known || requirements->supported_features != checked->supported_features || requirements->require_clip != checked->require_clip) {
        if (requirements->require_clip && !state->clip_mode_known)
            return fail(out, MH3_RES_ABSENT, MH3_MATERIAL_CLIP, 0, 1);
        if (state->clip_mode_known && state->clip_disabled && !(requirements->supported_features & MH3_MATERIAL_SUPPORT_CLIP_DISABLED))
            return fail(out, MH3_RES_UNSUPPORTED, MH3_MATERIAL_CLIP, 0, 1);
        const unsigned fields[3] = {MH3_PIXEL_DITHER, MH3_PIXEL_DESTINATION_ALPHA_ENABLE, MH3_PIXEL_DEPTH_TEXTURE_OPERATION};
        const uint32_t capabilities[3] = {MH3_MATERIAL_SUPPORT_DITHER, MH3_MATERIAL_SUPPORT_DESTINATION_ALPHA, MH3_MATERIAL_SUPPORT_DEPTH_TEXTURE};
        for (unsigned index = 0; index < 3; ++index)
            if ((state->pixel.known & BIT(fields[index])) && state->pixel.values[fields[index]] && !(requirements->supported_features & capabilities[index]))
                return fail(out, MH3_RES_UNSUPPORTED, MH3_MATERIAL_PIXEL, 0, BIT(fields[index]));
    }
    /* Commit only after every group/capability passed; a failed phase cannot
     * certify later fields. Union also permits removing/re-adding requirements
     * while the same immutable state remains in this preparation. */
#define UNION_REQUIREMENT(field, count)                                                                                                                                                                \
    for (unsigned index = 0; index < count; ++index)                                                                                                                                                   \
    proof->checked.field[index] |= requirements->field[index]
    UNION_REQUIREMENT(tev, 16);
    UNION_REQUIREMENT(texgen, 8);
    UNION_REQUIREMENT(channel, 2);
    UNION_REQUIREMENT(color, 4);
    UNION_REQUIREMENT(konst, 4);
    UNION_REQUIREMENT(swaps, 4);
    UNION_REQUIREMENT(indirect, 4);
    UNION_REQUIREMENT(indirect_matrix, 3);
#undef UNION_REQUIREMENT
    proof->checked.pixel |= requirements->pixel;
    proof->checked.require_clip = requirements->require_clip;
    proof->checked.supported_features = requirements->supported_features;
    proof->capabilities_known = 1;
    return MH3_RES_OK;
}
static void note_mutation(int *mutation, const void *to, const void *from, size_t bytes) {
    if (mutation && !*mutation && memcmp(to, from, bytes))
        *mutation = 1;
}
#define MERGE_KNOWN(to, from) do { \
    if (mutation && ((from) & ~(to))) *mutation = 1; \
    (to) |= (from); \
} while (0)
static void merge_bytes(void *destination, const void *source, uint64_t known, size_t count, size_t stride, int *mutation) {
    if (!known)
        return;
    if (known == (BIT(count) - 1)) {
        note_mutation(mutation, destination, source, count * stride);
        memcpy(destination, source, count * stride);
        return;
    }
    while (known) {
        const unsigned field = (unsigned)__builtin_ctzll(known);
        assert(field < count);
        void *to = (uint8_t *)destination + field * stride;
        const void *from = (const uint8_t *)source + field * stride;
        note_mutation(mutation, to, from, stride);
        memcpy(to, from, stride);
        known &= known - 1;
    }
}
static void overlay_channels(mh3_pack_channel next[2], const mh3_pack_channel source[2], int *mutation) {
    _Static_assert(sizeof(mh3_pack_channel_control) == 6, "channel control has no padding");
    for (unsigned index = 0; index < 2; ++index) {
        const mh3_pack_channel *from = &source[index];
        mh3_pack_channel *to = &next[index];
        MERGE_KNOWN(to->flags, from->flags);
#define COPY_CHANNEL(flag, member, bytes) do { \
    if (from->flags & (flag)) { \
        note_mutation(mutation, to->member, from->member, (bytes)); \
        memcpy(to->member, from->member, (bytes)); \
    } \
} while (0)
        COPY_CHANNEL(MH3_CHANNEL_MATERIAL_COLOR, material_color, 3);
        COPY_CHANNEL(MH3_CHANNEL_AMBIENT_COLOR, ambient_color, 3);
#undef COPY_CHANNEL
#define COPY_SCALAR(flag, member) do { \
    if (from->flags & (flag)) { \
        if (mutation && !*mutation && to->member != from->member) *mutation = 1; \
        to->member = from->member; \
    } \
} while (0)
        COPY_SCALAR(MH3_CHANNEL_MATERIAL_ALPHA, material_color[3]);
        COPY_SCALAR(MH3_CHANNEL_AMBIENT_ALPHA, ambient_color[3]);
        if (from->flags & MH3_CHANNEL_CONTROL_COLOR) {
            note_mutation(mutation, &to->color, &from->color, sizeof to->color);
            to->color = from->color;
            COPY_SCALAR(MH3_CHANNEL_CONTROL_COLOR, raw_color_control);
        }
        if (from->flags & MH3_CHANNEL_CONTROL_ALPHA) {
            note_mutation(mutation, &to->alpha, &from->alpha, sizeof to->alpha);
            to->alpha = from->alpha;
            COPY_SCALAR(MH3_CHANNEL_CONTROL_ALPHA, raw_alpha_control);
        }
#undef COPY_SCALAR
    }
}
static void overlay_indirect_matrices(mh3_pack_indirect_matrix next[3], const mh3_pack_indirect_matrix source[3], int *mutation) {
    for (unsigned index = 0; index < 3; ++index) {
        MERGE_KNOWN(next[index].known, source[index].known);
        merge_bytes(next[index].coefficients, source[index].coefficients, source[index].known & 63u, 6, 2, mutation);
        if (source[index].known & 64) {
            if (mutation && !*mutation && next[index].scale_exponent != source[index].scale_exponent)
                *mutation = 1;
            next[index].scale_exponent = source[index].scale_exponent;
        }
    }
}
void mh3_material_overlay_channels_unchecked(mh3_native_material_state *destination, const mh3_pack_channel source[2]) {
    assert(destination && source);
    overlay_channels(destination->channel, source, NULL);
}
void mh3_material_overlay_indirect_matrices_unchecked(mh3_native_material_state *destination, const mh3_pack_indirect_matrix source[3]) {
    assert(destination && source);
    overlay_indirect_matrices(destination->indirect_matrix, source, NULL);
}
int mh3_material_overlay_channels_mutation_unchecked(mh3_native_material_state *destination, const mh3_pack_channel source[2]) {
    assert(destination && source);
    if (destination->channel == source)
        return 0;
    int mutation = 0;
    overlay_channels(destination->channel, source, &mutation);
    return mutation;
}
int mh3_material_overlay_indirect_matrices_mutation_unchecked(mh3_native_material_state *destination, const mh3_pack_indirect_matrix source[3]) {
    assert(destination && source);
    if (destination->indirect_matrix == source)
        return 0;
    int mutation = 0;
    overlay_indirect_matrices(destination->indirect_matrix, source, &mutation);
    return mutation;
}
/* Preserve authored sparse writes and inheritance; mutation observes only the
 * bytes and known bits written here. Existing void/checked APIs request none. */
static void overlay_fields_unchecked(mh3_native_material_state *next, const mh3_native_material_state *source, uint32_t top_known, int *mutation) {
#define COPY_TOP(flag, field) do { \
    if (top_known & (flag)) { \
        if (mutation && !*mutation && next->field != source->field) *mutation = 1; \
        next->field = source->field; \
    } \
} while (0)
    COPY_TOP(MH3_MATERIAL_TOP_TEXGEN_COUNT, texgen_count);
    COPY_TOP(MH3_MATERIAL_TOP_CHANNEL_COUNT, channel_count);
    COPY_TOP(MH3_MATERIAL_TOP_TEV_COUNT, tev_count);
    COPY_TOP(MH3_MATERIAL_TOP_INDIRECT_COUNT, indirect_count);
    COPY_TOP(MH3_MATERIAL_TOP_CULL, cull_mode);
    COPY_TOP(MH3_MATERIAL_TOP_LIGHT_REFERENCE, light_set_reference);
    COPY_TOP(MH3_MATERIAL_TOP_FOG_REFERENCE, fog_reference);
    COPY_TOP(MH3_MATERIAL_TOP_GENERATION, generation);
    COPY_TOP(MH3_MATERIAL_TOP_PIXEL_CLASSIFICATION, pixel.classification);
#undef COPY_TOP
    for (unsigned index = 0; index < 16; ++index) {
        MERGE_KNOWN(next->tev[index].known, source->tev[index].known);
        merge_bytes(&next->tev[index].color, &source->tev[index].color, source->tev[index].known, 34, 1, mutation);
    }
    for (unsigned index = 0; index < 8; ++index) {
        MERGE_KNOWN(next->texgen[index].known, source->texgen[index].known);
        merge_bytes(next->texgen[index].values, source->texgen[index].values, source->texgen[index].known, 10, 1, mutation);
    }
    overlay_channels(next->channel, source->channel, mutation);
    for (unsigned index = 0; index < 4; ++index) {
        MERGE_KNOWN(next->color[index].known, source->color[index].known);
        merge_bytes(next->color[index].rgba, source->color[index].rgba, source->color[index].known, 4, 2, mutation);
        MERGE_KNOWN(next->konst[index].known, source->konst[index].known);
        merge_bytes(next->konst[index].rgba, source->konst[index].rgba, source->konst[index].known, 4, 2, mutation);
        MERGE_KNOWN(next->swaps[index].known, source->swaps[index].known);
        merge_bytes(next->swaps[index].rgba, source->swaps[index].rgba, source->swaps[index].known, 4, 1, mutation);
        MERGE_KNOWN(next->indirect[index].known, source->indirect[index].known);
        merge_bytes(&next->indirect[index].coordinate, &source->indirect[index].coordinate, source->indirect[index].known, 4, 1, mutation);
    }
    overlay_indirect_matrices(next->indirect_matrix, source->indirect_matrix, mutation);
    MERGE_KNOWN(next->pixel.known, source->pixel.known);
    merge_bytes(next->pixel.values, source->pixel.values, source->pixel.known, 22, 4, mutation);
    if (source->clip_mode_known) {
        if (mutation && !*mutation && (!next->clip_mode_known || next->clip_disabled != source->clip_disabled))
            *mutation = 1;
        next->clip_mode_known = 1;
        next->clip_disabled = source->clip_disabled;
    }
}
#undef MERGE_KNOWN
int mh3_material_overlay_top_mutation_unchecked(mh3_native_material_state *destination, uint32_t top_known,
    uint32_t texgens, uint32_t channels, uint32_t tevs, uint32_t indirect, uint32_t cull) {
    assert(destination);
    int mutation = 0;
#define TOP(flag, member, value) do { \
    if (top_known & (flag)) { \
        mutation |= destination->member != (value); \
        destination->member = (value); \
    } \
} while (0)
    TOP(MH3_MATERIAL_TOP_TEXGEN_COUNT, texgen_count, texgens);
    TOP(MH3_MATERIAL_TOP_CHANNEL_COUNT, channel_count, channels);
    TOP(MH3_MATERIAL_TOP_TEV_COUNT, tev_count, tevs);
    TOP(MH3_MATERIAL_TOP_INDIRECT_COUNT, indirect_count, indirect);
    TOP(MH3_MATERIAL_TOP_CULL, cull_mode, cull);
#undef TOP
    return mutation;
}
/* Public checked APIs keep admission and atomic rollback for arbitrary input.
 * Internal producers share the field effects, not these boundary checks. */
static mh3_res_status overlay_fields(mh3_native_material_state *next, const mh3_native_material_state *source, uint32_t top_known) {
    for (unsigned index = 0; index < 16; ++index)
        if (source->tev[index].known & ~MH3_MATERIAL_TEV_MASK)
            return MH3_RES_INVALID;
    for (unsigned index = 0; index < 8; ++index)
        if (source->texgen[index].known & ~MH3_MATERIAL_TEXGEN_MASK)
            return MH3_RES_INVALID;
    for (unsigned index = 0; index < 2; ++index)
        if (source->channel[index].flags & ~63u)
            return MH3_RES_INVALID;
    for (unsigned index = 0; index < 4; ++index)
        if ((source->color[index].known | source->konst[index].known | source->swaps[index].known | source->indirect[index].known) & ~15u)
            return MH3_RES_INVALID;
    for (unsigned index = 0; index < 3; ++index)
        if (source->indirect_matrix[index].known & ~127u)
            return MH3_RES_INVALID;
    if (source->pixel.known & ~MH3_MATERIAL_PIXEL_MASK || source->clip_mode_known > 1)
        return MH3_RES_INVALID;
    overlay_fields_unchecked(next, source, top_known, NULL);
    return mh3_material_state_validate(next, NULL, NULL);
}
static int state_overlaps(const mh3_native_material_state *a, const mh3_native_material_state *b) {
    const uintptr_t first = (uintptr_t)a, second = (uintptr_t)b;
    return first <= second ? second - first < sizeof *a : first - second < sizeof *a;
}
mh3_res_status mh3_material_state_overlay_candidate(mh3_native_material_state *candidate, const mh3_native_material_state *current, const mh3_native_material_state *source, uint32_t top_known) {
    if (!candidate || !current || !source || top_known & ~MH3_MATERIAL_TOP_ALL)
        return MH3_RES_INVALID;
    if (state_overlaps(candidate, current) || state_overlaps(candidate, source)) {
        mh3_native_material_state next;
        memcpy(&next, current, sizeof next);
        const mh3_res_status status = overlay_fields(&next, source, top_known);
        if (!status)
            memcpy(candidate, &next, sizeof next);
        return status;
    }
    memcpy(candidate, current, sizeof *candidate);
    return overlay_fields(candidate, source, top_known);
}
mh3_res_status mh3_material_state_overlay(mh3_native_material_state *destination, const mh3_native_material_state *source, uint32_t top_known) {
    return mh3_material_state_overlay_candidate(destination, destination, source, top_known);
}
void mh3_material_state_overlay_unchecked(mh3_native_material_state *destination, const mh3_native_material_state *source, uint32_t top_known) {
    /* Preserve arbitrary overlapping-storage compatibility on the cold path.
     * Normal publication and exact self publication need no state copies. */
    if (destination != source && state_overlaps(destination, source)) {
        const mh3_res_status status = mh3_material_state_overlay(destination, source, top_known);
        assert(status == MH3_RES_OK);
        (void)status;
        return;
    }
#ifndef NDEBUG
    mh3_native_material_state debug_candidate;
    const mh3_res_status status = mh3_material_state_overlay_candidate(&debug_candidate, destination, source, top_known);
    assert(status == MH3_RES_OK);
#endif
    if (destination != source)
        overlay_fields_unchecked(destination, source, top_known, NULL);
}
int mh3_material_state_overlay_mutation_unchecked(mh3_native_material_state *destination, const mh3_native_material_state *source, uint32_t top_known) {
    assert(destination && source);
    assert(destination == source || !state_overlaps(destination, source));
#ifndef NDEBUG
    mh3_native_material_state debug_candidate;
    const mh3_res_status status = mh3_material_state_overlay_candidate(&debug_candidate, destination, source, top_known);
    assert(status == MH3_RES_OK);
#endif
    if (destination == source)
        return 0;
    int mutation = 0;
    overlay_fields_unchecked(destination, source, top_known, &mutation);
    return mutation;
}
mh3_res_status mh3_material_pixel_raster_unchecked(const mh3_pack_pixel_inputs *pixel, mh3_raster_state *raster) {
    if (!pixel || !raster)
        return MH3_RES_INVALID;
    assert(pixel_check(pixel, 0, NULL, 1) == MH3_RES_OK);
    const uint64_t modes = BIT(MH3_PIXEL_BLEND_ENABLE) | BIT(MH3_PIXEL_LOGIC_ENABLE) | BIT(MH3_PIXEL_BLEND_SUBTRACT);
    if ((pixel->known & modes) && (pixel->known & modes) != modes)
        return MH3_RES_ABSENT;
    mh3_raster_state next;
    memcpy(&next, raster, sizeof next);
#define APPLY(field, member)                                                                                                                                                                           \
    if (pixel->known & BIT(field))                                                                                                                                                                     \
    next.member = pixel->values[field]
    APPLY(MH3_PIXEL_DEPTH_TEST, depth_test);
    APPLY(MH3_PIXEL_DEPTH_COMPARE, depth_compare);
    APPLY(MH3_PIXEL_DEPTH_WRITE, depth_write);
    APPLY(MH3_PIXEL_BLEND_SOURCE, src_factor);
    APPLY(MH3_PIXEL_BLEND_DESTINATION, dst_factor);
    APPLY(MH3_PIXEL_LOGIC_OPERATION, logic_op);
    APPLY(MH3_PIXEL_COLOR_WRITE, color_write);
    APPLY(MH3_PIXEL_ALPHA_WRITE, alpha_write);
#undef APPLY
    if ((pixel->known & modes) == modes)
        next.blend_mode = pixel->values[MH3_PIXEL_BLEND_SUBTRACT] ? 3 : pixel->values[MH3_PIXEL_BLEND_ENABLE] ? 1 : pixel->values[MH3_PIXEL_LOGIC_ENABLE] ? 2 : 0;
    memcpy(raster, &next, sizeof next);
    return MH3_RES_OK;
}
mh3_res_status mh3_material_pixel_raster(const mh3_pack_pixel_inputs *pixel, mh3_raster_state *raster) {
    if (!pixel || !raster)
        return MH3_RES_INVALID;
    const mh3_res_status status = pixel_check(pixel, 0, NULL, 1);
    if (status)
        return status;
    return mh3_material_pixel_raster_unchecked(pixel, raster);
}
static float sanitize_direction(float value) {
    uint32_t bits;
    memcpy(&bits, &value, 4);
    if ((bits & 0x7f800000u) != 0x7f800000u)
        return value;
    if (bits & 0x007fffffu)
        return 0;
    return bits & 0x80000000u ? -1.f : 1.f;
}
static int finite_float(const float *value) {
    uint32_t bits;
    memcpy(&bits, value, 4);
    return (bits & 0x7f800000u) != 0x7f800000u;
}
mh3_res_status mh3_material_prepare_light(const mh3_light *source, mh3_shader_light *out) {
    if (!source || !out)
        return MH3_RES_INVALID;
    mh3_shader_light light = {{0}, {0}, {0}, {0}, {0}};
    memcpy(light.position, source->position.v, 12);
    memcpy(light.direction, source->direction.v, 12);
    memcpy(light.angular, source->angular, 12);
    memcpy(light.distance, source->distance, 12);
    const uint8_t *color = (const uint8_t *)&source->color;
    for (unsigned component = 0; component < 4; ++component)
        light.color[component] = color[component] / 255.f;
    if (fabsf(light.distance[0]) < 0.00001f && fabsf(light.distance[1]) < 0.00001f && fabsf(light.distance[2]) < 0.00001f)
        light.distance[0] = 0.00001f;
    const double norm = (double)light.direction[0] * light.direction[0] + (double)light.direction[1] * light.direction[1] + (double)light.direction[2] * light.direction[2];
    if (norm == 0)
        light.direction[0] = light.direction[1] = light.direction[2] = 0;
    else {
        const double inverse = 1.0 / sqrt(norm);
        for (unsigned component = 0; component < 3; ++component)
            light.direction[component] = sanitize_direction((float)(light.direction[component] * inverse));
    }
    memcpy(out, &light, sizeof light);
    return MH3_RES_OK;
}
static float fog_float(float value) {
    uint32_t bits;
    memcpy(&bits, &value, 4);
    bits &= 0xfffff000u;
    memcpy(&value, &bits, 4);
    return value;
}
mh3_res_status mh3_material_prepare_fog(const mh3_native_fog *source, float viewport_width, mh3_shader_fog *out) {
    if (!source || !out)
        return MH3_RES_INVALID;
    if (source->type > 15 || source->type == 8)
        return MH3_RES_UNSUPPORTED;
    if (!finite_float(&source->start) || !finite_float(&source->end) || !finite_float(&source->near_plane) || !finite_float(&source->far_plane) || !finite_float(&viewport_width) ||
        source->range_enabled > 1)
        return MH3_RES_INVALID;
    mh3_shader_fog fog;
    memcpy(&fog, out, sizeof fog);
    const float delta_z = source->far_plane - source->near_plane, delta_fog = source->end - source->start;
    float a = 0, c = 0;
    if (source->type & 8) {
        if (delta_z != 0 && delta_fog != 0) {
            const float inverse = 1.f / delta_fog;
            a = inverse * delta_z;
            c = inverse * (source->start - source->near_plane);
        }
    } else {
        float b = .5f;
        if (delta_z != 0 && delta_fog != 0) {
            a = (source->far_plane * source->near_plane) / (delta_z * delta_fog);
            b = source->far_plane / delta_z;
            c = source->start / delta_fog;
        }
        if (!finite_float(&a) || !finite_float(&b) || !finite_float(&c))
            return MH3_RES_INVALID;
        int exponent = 0;
        while (b > 1.f) {
            b *= .5f;
            ++exponent;
        }
        while (b > 0.f && b < .5f) {
            b *= 2.f;
            --exponent;
        }
        const int shift = exponent + 1;
        if (b < 0 || shift < 0 || shift > 31)
            return MH3_RES_UNSUPPORTED;
        a /= ldexpf(1.f, shift);
        const float magnitude = b * 8388638.f;
        if (magnitude < 0 || magnitude >= 16777216.f)
            return MH3_RES_UNSUPPORTED;
        fog.b = (float)((uint32_t)magnitude & 0xFFFFFFu);
        fog.b_shift = (float)shift;
    }
    if (!finite_float(&a) || !finite_float(&c))
        return MH3_RES_INVALID;
    fog.a = fog_float(a);
    fog.c = fog_float(c);
    fog.color[0] = source->color.r / 255.f;
    fog.color[1] = source->color.g / 255.f;
    fog.color[2] = source->color.b / 255.f;
    fog.color[3] = 1;
    float width = fabsf(viewport_width);
    if (width <= 0)
        width = 1;
    /* The CPU center is translated by342 and masked to10bits by its producer. */
    const int center = (int)(((uint32_t)source->range_center + 342u) & 1023u) - 342;
    fog.range_base[0] = source->range_enabled ? ((float)center / width) * 2.f - 1.f : 0;
    fog.range_base[1] = source->range_enabled ? width : 1;
    fog.range_base[2] = fog.range_base[3] = 0;
    /* Packing reverses each producer pair: its second K occupies the high12bits. */
    if (source->range_enabled) {
        for (unsigned index = 0; index < 10; ++index)
            fog.range_k[index] = (source->range_adjust[index ^ 1] & 0xFFFu) / 64.f;
        fog.range_k[10] = fog.range_k[11] = fog.range_k[9];
    }
    memcpy(out, &fog, sizeof fog);
    return MH3_RES_OK;
}
