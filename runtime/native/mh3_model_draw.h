#ifndef MH3_MODEL_DRAW_H
#define MH3_MODEL_DRAW_H

#include "mh3_model_resources.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Native reconstruction of NW4R G3dDrawOpaXlu (80078b00). Resource offsets
 * belong to model.container; no emulated registers, stack or GX commands. */
typedef struct {
    mh3_res_draw draw;
    mh3_res_entry material, shape, node;
    uint32_t material_index, dispatch_flags;
} mh3_model_draw_item;

typedef struct {
    uint32_t records, submitted, hidden_nodes, hidden_shapes, repeated_materials;
} mh3_model_draw_stats;

typedef mh3_res_status (*mh3_model_draw_emit)(void *user, const mh3_model_draw_item *item);

/* An empty visibility span selects the node's current resource visibility.
 * A nonempty span contains one byte per runtime node ID. Read live resource
 * flags on each iteration; animations may change them between submissions.
 * Callback order is bytecode order. Only a submitted material updates cache. */
mh3_res_status mh3_model_draw_walk(const mh3_res_model *model, size_t bytecode, mh3_res_span visibility, uint32_t options, mh3_model_draw_emit emit, void *user, mh3_model_draw_stats *stats);
/* A model's material, shape and node entries by draw-stream ID: their offsets
 * from the MDL0 (mh3_res_model.offset) as mh3_res_model_entry resolves them,
 * UINT32_MAX where unknown. Entries carry no name. */
typedef struct {
    const uint32_t *materials, *shapes, *nodes;
    uint32_t material_count, shape_count, node_count;
} mh3_model_draw_entries;

/* mh3_model_draw_walk that takes the IDs `entries` holds from them instead of
 * the model's dictionaries (NULL: the dictionaries for all). */
mh3_res_status mh3_model_draw_walk_entries(const mh3_res_model *model, size_t bytecode, mh3_res_span visibility, uint32_t options, const mh3_model_draw_entries *entries,
                                           mh3_model_draw_emit emit, void *user, mh3_model_draw_stats *stats);
/* An entry the table holds, as mh3_res_model_entry would resolve it; 0 when
 * the table does not hold it. */
int mh3_model_draw_entry(const mh3_res_model *model, const mh3_model_draw_entries *entries, mh3_res_kind kind, uint32_t id, mh3_res_entry *out);

typedef enum {
    MH3_MAT_TEXTURE_OBJECTS,
    MH3_MAT_PALETTE_OBJECTS,
    MH3_MAT_GENERAL_MODE,
    MH3_MAT_TEV,
    MH3_MAT_PIXEL_STATE,
    MH3_MAT_TEV_COLORS,
    MH3_MAT_INDIRECT_MATRIX_SCALE,
    MH3_MAT_CHANNELS,
    MH3_MAT_TEXCOORD_GENERATION,
    MH3_MAT_MISC,
    MH3_MAT_TEXTURE_SRT,
    MH3_MAT_RESOURCE_COUNT
} mh3_material_resource_kind;

/* Typed result of the per-material replacement selection in 80077ed4.
 * Values are opaque resource addresses at the compatibility boundary, never
 * CPU casts. Fields 13..15 are optional position/normal/color array replacement
 * tables (not transform palettes). The ABI adapter serializes
 * this result only while the downstream renderer still expects guest RAM. */
typedef struct {
    uint32_t resource_slots[MH3_MAT_RESOURCE_COUNT];
    uint32_t vertex_tables[3];
} mh3_model_material_bindings;

mh3_res_status mh3_model_material_select(mh3_res_span replacement, uint32_t material, mh3_model_material_bindings *out);

#ifdef __cplusplus
}
#endif
#endif
