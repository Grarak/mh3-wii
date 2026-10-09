#ifndef MH3_MODEL_RESOURCES_H
#define MH3_MODEL_RESOURCES_H

/* Portable C resource accessors. No PPC register emulation, guest pointers,
 * allocations, GX calls, or embedded assets. The caller owns every byte span.
 * See docs/optimizations/assets/load-time-asset-conversion.md for guest address mapping and validation. */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MH3_RES_OK = 0,
    MH3_RES_END = 1,
    MH3_RES_ABSENT = 2,
    MH3_RES_INVALID = -1,
    MH3_RES_UNSUPPORTED = -2,
    MH3_RES_RANGE = -3
} mh3_res_status;

typedef struct {
    const uint8_t *data;
    size_t size;
} mh3_res_span;

typedef struct {
    uint8_t *data;
    size_t size;
} mh3_res_mut_span;

typedef struct {
    mh3_res_span container;
    size_t offset, size;
    uint32_t version;
} mh3_res_model;

typedef enum {
    MH3_RES_BYTECODE,
    MH3_RES_NODES,
    MH3_RES_POSITIONS,
    MH3_RES_NORMALS,
    MH3_RES_COLORS,
    MH3_RES_TEXCOORDS,
    MH3_RES_MATERIALS,
    MH3_RES_TEV,
    MH3_RES_SHAPES,
    MH3_RES_TEXTURE_BINDINGS,
    MH3_RES_PALETTE_BINDINGS,
    MH3_RES_FUR_VECTORS,
    MH3_RES_FUR_POSITIONS
} mh3_res_kind;

typedef struct {
    uint32_t id;
    size_t offset;
    const char *name;
} mh3_res_entry;

typedef struct {
    size_t tag_offset, offset;
    uint32_t buffer_size, command_size;
    mh3_res_span commands;
} mh3_res_display_list;

typedef struct {
    mh3_res_entry entry;
    uint32_t size, flags, vertex_count, triangle_count;
    int32_t matrix_id;
    int16_t array_ids[12]; /* position, normal, color0/1, texcoord0..7 */
    mh3_res_display_list setup, primitives;
} mh3_res_shape;

typedef struct {
    uint16_t material_id, shape_id, node_id;
    uint8_t priority;
    size_t offset;
} mh3_res_draw;

/* Resolve a signed BE relative offset stored at field_offset. Zero is ABSENT.
 * Offset ranges use subtraction checks; output pointers never escape span. */
mh3_res_status mh3_res_relative(mh3_res_span span, size_t base, size_t field_offset, size_t required, size_t *result);
mh3_res_status mh3_res_model_open(mh3_res_span container, size_t offset, mh3_res_model *out);
/* Generic ResDic access for runtime resources, including non-model dictionaries.
 * A zero entry data offset is ABSENT, matching the game's optional references.
 * Data can be outside the dictionary; the entire owner span bounds it. */
mh3_res_status mh3_res_dictionary_count(mh3_res_span container, size_t dictionary_offset, uint32_t *count);
mh3_res_status mh3_res_dictionary_data(mh3_res_span container, size_t dictionary_offset, int32_t index, size_t *data_offset);
mh3_res_status mh3_res_model_data(const mh3_res_model *model, mh3_res_kind kind, int32_t index, size_t *data_offset);
mh3_res_status mh3_res_model_dictionary(const mh3_res_model *model, mh3_res_kind kind, size_t *dictionary_offset, uint32_t *count);
mh3_res_status mh3_res_model_entry(const mh3_res_model *model, mh3_res_kind kind, uint32_t id, mh3_res_entry *out);
mh3_res_status mh3_res_model_find(const mh3_res_model *model, mh3_res_kind kind, const char *name, mh3_res_entry *out);
mh3_res_status mh3_res_shape_open(const mh3_res_model *model, uint32_t id, mh3_res_shape *out);
mh3_res_status mh3_res_shape_array(const mh3_res_model *model, const mh3_res_shape *shape, uint32_t attribute, mh3_res_entry *out);
/* parent accessor accepts a shape entry in the same container, then checks MDL0. */
mh3_res_status mh3_res_shape_parent(mh3_res_span container, size_t shape_offset, mh3_res_model *out);
/* cursor is an absolute container offset. END leaves cursor after terminator. */
mh3_res_status mh3_res_draw_next(const mh3_res_model *model, size_t *cursor, mh3_res_draw *out);
/* A native submission layer consumes this span; cache equality skips the first
 * 32 bytes of the setup list exactly as guest 80099f80 does. No GX submission. */
mh3_res_status mh3_res_shape_submit_span(const mh3_res_shape *shape, int primitive, int descriptor_cache_equal, mh3_res_span *out);
/* Explicit mutation adapters for decompiled resource editors, unused at runtime.
 * physical_base is an already resolved Wii GX physical address; no host pointer
 * casts. attr 0..11 matches shape.array_ids. Source files need not be writable. */
mh3_res_status mh3_res_shape_set_array(mh3_res_mut_span container, const mh3_res_shape *shape, uint32_t attribute, uint32_t physical_base, uint8_t stride);
mh3_res_status mh3_res_shape_disable_array(mh3_res_mut_span container, const mh3_res_shape *shape, uint32_t attribute);
/* Standalone serialized NW4R substructure copies. Flags stay big endian.
 * Invalid texture SRT records remain untouched in the destination. */
mh3_res_status mh3_res_tlut_invalidate(mh3_res_mut_span object, uint32_t id);
mh3_res_status mh3_res_tlut_copy(mh3_res_span source, mh3_res_mut_span destination);
mh3_res_status mh3_res_texsrt_copy(mh3_res_span source, mh3_res_mut_span destination);

#ifdef __cplusplus
}
#endif
#endif
