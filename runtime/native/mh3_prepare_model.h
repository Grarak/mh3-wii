#ifndef MH3_PREPARE_MODEL_H
#define MH3_PREPARE_MODEL_H
#include "mh3_prepare_corners.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Preparation-only caller inventory. Model spans are exact certified BRRES
 * members; partition_offset already includes container_offset. IDs are from
 * the canonical append-only inventory, never inferred from this subset. */
typedef struct {
    mh3_res_model model;
    const char *file;
    uint64_t partition_offset;
    uint32_t container_offset, asset_id;
    const uint16_t *component_shapes;
    uint32_t component_shape_count;
} mh3_prepare_model_source;

/* Strictly sorted by (partition_offset,name), globally unique ID. Selected
 * joins must also match dictionary-owned TEX0 bytes in the exact source span. */
typedef struct {
    uint64_t partition_offset;
    const char *name;
    uint32_t container_bytes, resource_offset, resource_bytes, id, width, height;
} mh3_prepare_texture;

typedef struct {
    const char *name;
    uint32_t offset;
} mh3_prepare_string;

typedef struct {
    uint32_t nodes, shapes, materials, bindings, draws, commands, weights, matrices, components;
    uint32_t starts[8];
    size_t info, mapping;
} mh3_prepare_model_plan;

typedef struct {
    uint8_t *metadata;
    size_t metadata_bytes;
    uint8_t *identities;
    size_t identity_bytes;
    uint8_t *scratch;
    size_t scratch_bytes;
    mh3_pack_layout *layouts;
    uint32_t layout_count;
    mh3_prepare_string *strings;
    uint32_t string_count;
    mh3_prepare_model_plan *plans;
    uint32_t plan_count;
    mh3_corner_workspace shape;
    mh3_prepare_input indices;
    mh3_prepare_sink gpu;

    /* GPU skinning (docs/optimizations/rendering/gpu-skinning.md): storage for one model's skin
     * tables (MH3_SKIN_MAX_MATRICES entries), with the shape workspace's
     * skin_rows and skin_lookup. Absent: indexed shapes keep palette batches. */
    mh3_skin_matrix *skin_matrices;
} mh3_prepare_model_workspace;

typedef struct {
    mh3_prepare_writer mesh, identity;
    mh3_mesh_pack pack;
    mh3_model_catalog catalog;
} mh3_prepared_model_pack;

typedef struct {
    uint32_t model_id, kind, resource_id;
    size_t offset;
} mh3_prepare_model_diagnostic;

/* Ordinary C, no allocation or publication. Exact layouts only, all shapes.
 * Optional component descriptors require explicit sorted source shape IDs;
 * they never enable renderer sorting. Success borrows caller buffers/sinks. Failure leaves
 * result untouched, invalidates private metadata and rolls GPU/indices to0;
 * ROLLBACK requires discarding the affected private files too.
 * Production metadata/catalog joins run before result publication. A bounded
 * GPU semantic verifier is still required before filesystem publication. */
mh3_prepare_status mh3_prepare_model_pack(const mh3_prepare_model_source *, uint32_t, const mh3_prepare_texture *, uint32_t, const mh3_prepare_model_workspace *, mh3_prepared_model_pack *,
                                          mh3_prepare_model_diagnostic *);
#ifdef __cplusplus
}
#endif
#endif
