#include "mh3_material_group_publication.h"
#include <assert.h>
#include <stddef.h>
#include <string.h>

#define BIT(n) (UINT64_C(1) << (n))
#define PIXEL_FIELDS (BIT(MH3_PIXEL_DEPTH_TEXTURE_OPERATION) - 1)
#define TEXGEN_FIELDS (MH3_MATERIAL_TEXGEN_MASK & ~((1u << MH3_TEXGEN_MATRIX_ROW) | (1u << MH3_TEXGEN_DUAL_TEXTURE)))
_Static_assert(MH3_PIXEL_DEPTH_TEXTURE_OPERATION == 19, "original pixel block fields");
_Static_assert(sizeof(mh3_pack_tev_pass) == 9, "typed TEV pass fields");
_Static_assert(offsetof(mh3_pack_tev_stage, indirect_add_previous) - offsetof(mh3_pack_tev_stage, color) == 33, "typed TEV field order");
_Static_assert(offsetof(mh3_pack_indirect_stage, scale_t) - offsetof(mh3_pack_indirect_stage, coordinate) == 3, "typed indirect field order");

#ifndef NDEBUG
static int overlaps(const void *a, const void *b, size_t bytes) {
    const uintptr_t x = (uintptr_t)a, y = (uintptr_t)b;
    return x <= y ? y - x < bytes : x - y < bytes;
}
static int tev_domain(unsigned field, uint8_t value) {
    static const uint8_t maximum[34] = {15, 15, 15, 15, 15, 2, 3, 1, 3, 7, 7, 7, 7, 15, 2, 3, 1, 3, 255, 255, 255, 3, 3, 31, 31, 3, 3, 7, 3, 11, 6, 6, 1, 1};
    if (value > maximum[field])
        return 0;
    if (field == MH3_TEV_COLOR_OPERATION || field == MH3_TEV_ALPHA_OPERATION)
        return value <= 1 || value >= 8;
    if (field == MH3_TEV_TEXTURE_MAP || field == MH3_TEV_TEXTURE_COORDINATE)
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
static void pixel_contract(const mh3_pack_pixel_inputs *pixel) {
    static const uint32_t maximum[19] = {7, 255, 7, 255, 3, 1, 7, 1, 1, 1, 1, 7, 7, 15, 1, 1, 1, 1, 255};
    uint64_t known = pixel->known & PIXEL_FIELDS;
    while (known) {
        const unsigned field = (unsigned)__builtin_ctzll(known);
        assert(pixel->values[field] <= maximum[field]);
        known &= known - 1;
    }
}
/* Check only selected source masks and domains active in the inherited owner.
 * Unselected and unknown source storage has no meaning at this boundary. No
 * full material candidate or repeated whole-state validation is required. */
static void group_contract(const mh3_native_material_state *destination, const mh3_native_material_state *source, mh3_model_material_block block, uint32_t count) {
    assert(source->tev_count <= 16 && source->texgen_count <= 8 && source->indirect_count <= 4);
    assert(destination->tev_count <= 16 && destination->texgen_count <= 8 && destination->indirect_count <= 4);
    switch (block) {
    case MH3_MODEL_MATERIAL_PIXEL:
        pixel_contract(&source->pixel);
        break;
    case MH3_MODEL_MATERIAL_COLORS:
        for (unsigned i = 0; i < 4; ++i) {
            assert(!((source->color[i].known | source->konst[i].known) & ~15u));
            for (unsigned f = 0; f < 4; ++f) {
                if (source->color[i].known & (1u << f))
                    assert(source->color[i].rgba[f] >= -1024 && source->color[i].rgba[f] <= 1023);
                if (source->konst[i].known & (1u << f))
                    assert(source->konst[i].rgba[f] >= 0 && source->konst[i].rgba[f] <= 255);
            }
        }
        break;
    case MH3_MODEL_MATERIAL_INDIRECT:
        for (unsigned i = 0; i < 3; ++i) {
            const mh3_pack_indirect_matrix *matrix = &source->indirect_matrix[i];
            assert(!(matrix->known & ~127u));
            for (unsigned f = 0; f < 6; ++f)
                if (matrix->known & (1u << f))
                    assert(matrix->coefficients[f] >= -1024 && matrix->coefficients[f] <= 1023);
            if (matrix->known & 64u)
                assert(matrix->scale_exponent >= -17 && matrix->scale_exponent <= 46);
        }
        for (unsigned i = 0; i < destination->indirect_count; ++i) {
            if (source->indirect[i].known & 4u)
                assert(source->indirect[i].scale_s <= 8);
            if (source->indirect[i].known & 8u)
                assert(source->indirect[i].scale_t <= 8);
        }
        break;
    case MH3_MODEL_MATERIAL_TEXGEN: {
        static const uint8_t maximum[10] = {1, 1, 3, 12, 7, 7, 63, 63, 1, 1};
        assert(count == source->texgen_count);
        for (unsigned i = 0; i < 8; ++i) {
            uint32_t known = source->texgen[i].known & ~((1u << MH3_TEXGEN_MATRIX_ROW) | (1u << MH3_TEXGEN_DUAL_TEXTURE));
            assert(!(known & ~MH3_MATERIAL_TEXGEN_MASK));
            if (i < destination->texgen_count)
                while (known) {
                    const unsigned f = (unsigned)__builtin_ctz(known);
                    assert(source->texgen[i].values[f] <= maximum[f]);
                    known &= known - 1;
                }
        }
        break;
    }
    case MH3_MODEL_MATERIAL_PIPELINE:
        assert(source->tev_count);
        for (unsigned i = 0; i < 16; ++i) {
            uint64_t known = source->tev[i].known;
            assert(!(known & ~MH3_MATERIAL_TEV_MASK));
            if (i < destination->tev_count)
                while (known) {
                    const unsigned f = (unsigned)__builtin_ctzll(known);
                    assert(tev_domain(f, ((const uint8_t *)&source->tev[i].color)[f]));
                    known &= known - 1;
                }
        }
        for (unsigned i = 0; i < 4; ++i) {
            assert(!(source->swaps[i].known & ~15u));
            for (unsigned f = 0; f < 4; ++f)
                if (source->swaps[i].known & (1u << f))
                    assert(source->swaps[i].rgba[f] <= 3);
            if (i < destination->indirect_count) {
                if (source->indirect[i].known & 1u)
                    assert(source->indirect[i].coordinate <= 7 || source->indirect[i].coordinate == 255);
                if (source->indirect[i].known & 2u)
                    assert(source->indirect[i].map <= 7 || source->indirect[i].map == 255);
            }
        }
        break;
    default:
        assert(0);
        break;
    }
}
#endif

/* Only consumed, authored values are compared. Copies retain the oracle's
 * dense-field shortcut and sparse field widths, without record padding. */
static int merge_u8(uint8_t *to, const uint8_t *from, uint64_t known, unsigned count, uint64_t consumed) {
    int changed = 0;
    uint64_t compared = known & consumed;
    while (compared) {
        const unsigned f = (unsigned)__builtin_ctzll(compared);
        if (to[f] != from[f]) {
            changed = 1;
            break;
        }
        compared &= compared - 1;
    }
    if (known == BIT(count) - 1)
        memcpy(to, from, count);
    else
        while (known) {
            const unsigned f = (unsigned)__builtin_ctzll(known);
            to[f] = from[f];
            known &= known - 1;
        }
    return changed;
}
static void merge_i16(int16_t *to, const int16_t *from, uint32_t known, unsigned count) {
    if (known == (1u << count) - 1)
        memcpy(to, from, count * sizeof *to);
    else
        while (known) {
            const unsigned f = (unsigned)__builtin_ctz(known);
            to[f] = from[f];
            known &= known - 1;
        }
}
static int merge_pixel(mh3_pack_pixel_inputs *to, const mh3_pack_pixel_inputs *from, uint64_t consumed) {
    const uint64_t known = from->known & PIXEL_FIELDS;
    uint64_t compared = known & consumed;
    int changed = 0;
    while (compared) {
        const unsigned f = (unsigned)__builtin_ctzll(compared);
        if (to->values[f] != from->values[f]) {
            changed = 1;
            break;
        }
        compared &= compared - 1;
    }
    if (known == PIXEL_FIELDS)
        memcpy(to->values, from->values, 19 * sizeof *to->values);
    else {
        uint64_t pending = known;
        while (pending) {
            const unsigned f = (unsigned)__builtin_ctzll(pending);
            to->values[f] = from->values[f];
            pending &= pending - 1;
        }
    }
    to->known |= known;
    return changed;
}
/* Keep the uncommon output==source temporary out of ordinary preview frames. */
__attribute__((noinline)) static void preview_source_alias(const mh3_pack_pixel_inputs *current, const mh3_pack_pixel_inputs *source, mh3_pack_pixel_inputs *out) {
    mh3_pack_pixel_inputs preview;
    memcpy(&preview, current, sizeof preview);
    merge_pixel(&preview, source, 0);
    memcpy(out, &preview, sizeof preview);
}
void mh3_material_group_pixel_preview_unchecked(const mh3_pack_pixel_inputs *current, const mh3_pack_pixel_inputs *source, mh3_pack_pixel_inputs *out) {
    assert(current && source && out);
    assert(out == current || !overlaps(out, current, sizeof *out));
    assert(out == source || !overlaps(out, source, sizeof *out));
    assert(current == source || !overlaps(current, source, sizeof *out));
    assert((pixel_contract(source), 1));
    if (current == source) {
        if (out != current)
            memcpy(out, current, sizeof *out);
        return;
    }
    if (out == source) {
        preview_source_alias(current, source, out);
        return;
    }
    if (out != current)
        memcpy(out, current, sizeof *out);
    merge_pixel(out, source, 0);
}
int mh3_material_group_publish_unchecked(mh3_native_material_state *destination, const mh3_native_material_state *source, mh3_model_material_block block, uint32_t count,
                                         const mh3_material_requirements *consumed) {
    assert(destination && block >= 0 && block < MH3_MODEL_MATERIAL_BLOCK_COUNT);
    if (!count && (block == MH3_MODEL_MATERIAL_INDIRECT || block == MH3_MODEL_MATERIAL_TEXGEN))
        return 0;
    assert(source);
    assert(destination == source || !overlaps(destination, source, sizeof *destination));
    assert((group_contract(destination, source, block, count), 1));
    if (destination == source)
        return 0;
    int changed = 0;
    switch (block) {
    case MH3_MODEL_MATERIAL_PIXEL:
        return merge_pixel(&destination->pixel, &source->pixel, consumed ? consumed->pixel : 0);
    case MH3_MODEL_MATERIAL_COLORS:
        for (unsigned i = 0; i < 4; ++i) {
            merge_i16(destination->color[i].rgba, source->color[i].rgba, source->color[i].known, 4);
            destination->color[i].known |= source->color[i].known;
            merge_i16(destination->konst[i].rgba, source->konst[i].rgba, source->konst[i].known, 4);
            destination->konst[i].known |= source->konst[i].known;
        }
        break;
    case MH3_MODEL_MATERIAL_INDIRECT:
        for (unsigned i = 0; i < 3; ++i) {
            const mh3_pack_indirect_matrix *from = &source->indirect_matrix[i];
            mh3_pack_indirect_matrix *to = &destination->indirect_matrix[i];
            merge_i16(to->coefficients, from->coefficients, from->known & 63u, 6);
            if (from->known & 64u)
                to->scale_exponent = from->scale_exponent;
            to->known |= from->known;
        }
        for (unsigned i = 0; i < 4; ++i) {
            const uint32_t known = source->indirect[i].known & 12u;
            changed |= merge_u8((uint8_t *)&destination->indirect[i] + offsetof(mh3_pack_indirect_stage, coordinate),
                                (const uint8_t *)&source->indirect[i] + offsetof(mh3_pack_indirect_stage, coordinate), known, 4, consumed && !changed ? consumed->indirect[i] : 0);
            destination->indirect[i].known |= known;
        }
        break;
    case MH3_MODEL_MATERIAL_TEXGEN:
        for (unsigned i = 0; i < 8; ++i) {
            const uint32_t known = source->texgen[i].known & TEXGEN_FIELDS;
            changed |= merge_u8(destination->texgen[i].values, source->texgen[i].values, known, 10, consumed && !changed ? consumed->texgen[i] : 0);
            destination->texgen[i].known |= known;
        }
        break;
    case MH3_MODEL_MATERIAL_PIPELINE:
        for (unsigned i = 0; i < 16; ++i) {
            const uint64_t known = source->tev[i].known;
            changed |= merge_u8((uint8_t *)&destination->tev[i] + offsetof(mh3_pack_tev_stage, color), (const uint8_t *)&source->tev[i] + offsetof(mh3_pack_tev_stage, color), known, 34,
                                consumed && !changed ? consumed->tev[i] : 0);
            destination->tev[i].known |= known;
        }
        for (unsigned i = 0; i < 4; ++i) {
            const uint32_t swap = source->swaps[i].known, indirect = source->indirect[i].known & 3u;
            changed |= merge_u8(destination->swaps[i].rgba, source->swaps[i].rgba, swap, 4, consumed && !changed ? consumed->swaps[i] : 0);
            destination->swaps[i].known |= swap;
            changed |= merge_u8((uint8_t *)&destination->indirect[i] + offsetof(mh3_pack_indirect_stage, coordinate),
                                (const uint8_t *)&source->indirect[i] + offsetof(mh3_pack_indirect_stage, coordinate), indirect, 4, consumed && !changed ? consumed->indirect[i] : 0);
            destination->indirect[i].known |= indirect;
        }
        break;
    default:
        assert(0);
        break;
    }
    return changed;
}
