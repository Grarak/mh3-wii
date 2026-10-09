#ifndef MH3_MESH_PACK_H
#define MH3_MESH_PACK_H
/* External M3MP v1 CPU metadata + GPU-ready payload. Unwired prototype.
 * The owner reads the GPU payload straight into mapped CDRAM, then binds it.
 * All returned pointers borrow the corresponding caller-owned byte spans. */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { MH3_PACK_OK=0, MH3_PACK_INVALID=-1, MH3_PACK_UNSUPPORTED=-2,
               MH3_PACK_RANGE=-3, MH3_PACK_ALIGNMENT=-4 } mh3_pack_status;
typedef struct { const uint8_t *data; size_t size; } mh3_pack_span;
typedef enum { MH3_PACK_MODELS, MH3_PACK_NODES, MH3_PACK_LAYOUTS, MH3_PACK_MESHES,
               MH3_PACK_MATERIALS, MH3_PACK_BINDINGS, MH3_PACK_DRAWS, MH3_PACK_COMPONENTS,
               MH3_PACK_TABLE_COUNT } mh3_pack_table;
typedef struct {
    mh3_pack_span metadata, gpu;
    uint32_t gpu_file_offset, gpu_bytes, counts[8], offsets[8], data_offset;
} mh3_mesh_pack;
typedef struct { uint16_t semantic, offset; uint8_t format, components; } mh3_pack_attribute;
typedef struct { uint32_t count, stride, flags; mh3_pack_attribute attributes[16]; } mh3_pack_layout;
typedef struct {
    uint32_t asset_id, model_id, source_id, flags, layout_index, vertex_count, index_count;
    uint32_t vertex_offset, vertex_bytes, index_offset, index_bytes;
    int32_t matrix_id;
    uint32_t component_first, component_count, template_offset, template_indices;
    float bounds_min[3], bounds_max[3], center[3];
    mh3_pack_span vertices, indices;
} mh3_pack_mesh;
typedef struct {
    uint32_t mesh_index, first_index, index_count, flags;
    float bounds_min[3], bounds_max[3], centroid[3];
} mh3_pack_component;
typedef struct {
    uint32_t mesh_index, material_index, node_index, phase, priority, source_order, asset_id, flags;
} mh3_pack_draw;
typedef struct {
    uint32_t model_id, source_id, matrix_id, flags, billboard_mode;
    int32_t parent_index, first_child_index;
    float bind_model_matrix[12], inverse_bind_model_matrix[12], srt[9], bounds[6];
} mh3_pack_node;
typedef struct {
    uint32_t asset_id, model_id, source_id, flags, cull_mode, texgen_count, channel_count;
    uint32_t tev_count, indirect_count, depth_compare_before_texture;
    uint32_t alpha_compare, depth_state, blend_state, blend_mask;
    uint32_t state_offset, state_count, binding_first, binding_count, shader_pipeline_id;
    const char *name;
} mh3_pack_material;
typedef struct {
    uint32_t material_index, texture_id, map_id, tlut_id, wrap_s, wrap_t, min_filter, mag_filter;
    float lod_bias;
    uint32_t anisotropy, bias_clamp, edge_lod, flags;
    const char *texture_name, *palette_name;
} mh3_pack_binding;
typedef struct { uint32_t bank, address, value, mask; } mh3_pack_state_write;
typedef struct { uint32_t command_count, weight_count, matrix_count; mh3_pack_span bytes; } mh3_pack_recipes;
/* NW4R recipe opcodes: 2=calc(node,parent matrix), 3=weighted matrix,
 * 5=envelope(matrix,node), 6=duplicate(destination matrix,source matrix).
 * Weight ranges are present only for opcode 3. Matrix-to-node IDs are local
 * source node IDs (or -1), whereas node hierarchy links are pack table indices. */
typedef struct {
    uint32_t opcode, destination_or_node, source_or_parent, weight_first, weight_count;
} mh3_pack_recipe;
typedef struct { uint32_t matrix_id; float weight; } mh3_pack_weight;
typedef struct {
    uint32_t id, source_offset, node_first, node_count, mesh_first, mesh_count;
    uint32_t material_first, material_count, draw_first, draw_count, revision, flags;
    const char *name, *file;
    mh3_pack_span node_recipes;
    uint32_t scaling_rule, texture_matrix_mode, view_matrix_count;
    uint32_t needs_normal_matrix_array, needs_texture_matrix_array, envelope_matrix_mode, bounds_valid;
    float declared_bounds[6];
    uint32_t source_vertex_count, source_triangle_count;
} mh3_pack_model;

/* Open and validate metadata once, without reading GPU buffers. All table and
 * attribute references are checked. This deliberately accepts no I/O callback. */
mh3_pack_status mh3_mesh_pack_open_metadata(mh3_pack_span metadata, mh3_mesh_pack *out);
mh3_pack_status mh3_mesh_pack_bind_gpu(mh3_mesh_pack *pack, mh3_pack_span gpu);
mh3_pack_status mh3_mesh_pack_model(const mh3_mesh_pack *pack, uint32_t index, mh3_pack_model *out);
mh3_pack_status mh3_mesh_pack_mesh(const mh3_mesh_pack *pack, uint32_t index, mh3_pack_mesh *out);
mh3_pack_status mh3_mesh_pack_layout(const mh3_mesh_pack *pack, uint32_t index, mh3_pack_layout *out);
mh3_pack_status mh3_mesh_pack_component(const mh3_mesh_pack *pack, uint32_t index, mh3_pack_component *out);
mh3_pack_status mh3_mesh_pack_draw(const mh3_mesh_pack *pack, uint32_t index, mh3_pack_draw *out);
mh3_pack_status mh3_mesh_pack_node(const mh3_mesh_pack *pack, uint32_t index, mh3_pack_node *out);
mh3_pack_status mh3_mesh_pack_material(const mh3_mesh_pack *pack, uint32_t index, mh3_pack_material *out);
mh3_pack_status mh3_mesh_pack_binding(const mh3_mesh_pack *pack, uint32_t index, mh3_pack_binding *out);
mh3_pack_status mh3_mesh_pack_state_write(const mh3_mesh_pack *pack, uint32_t material_index,
                                         uint32_t index, mh3_pack_state_write *out);
mh3_pack_status mh3_mesh_pack_recipes(const mh3_mesh_pack *pack, uint32_t model_index, mh3_pack_recipes *out);
mh3_pack_status mh3_mesh_pack_recipe(const mh3_pack_recipes *recipes, uint32_t index, mh3_pack_recipe *out);
mh3_pack_status mh3_mesh_pack_weight(const mh3_pack_recipes *recipes, uint32_t index, mh3_pack_weight *out);
mh3_pack_status mh3_mesh_pack_matrix_node(const mh3_pack_recipes *recipes, uint32_t index, int32_t *out);
/* Optional load/test gate: O(indices). Never call this per frame or read CDRAM
 * from the CPU during normal submission. */
mh3_pack_status mh3_mesh_pack_validate_indices(const mh3_mesh_pack *pack);
/* Explicit experimental per-mesh ordering, never called automatically.
 * depth_row is the current local-to-camera positive-forward depth row, including
 * current model placement/pose. Bounds are bind-local and cannot replace poses.
 * order and keys are caller-owned CPU arrays of component_count entries.
 * This centroid heuristic cannot resolve intersecting cards or global ordering
 * across different meshes/materials. */
mh3_pack_status mh3_mesh_pack_sort_components(const mh3_mesh_pack *pack, uint32_t mesh_index,
                                              const float depth_row[4], uint32_t *order,
                                              float *keys, size_t capacity);
/* Copy CPU-resident template indices into a caller-owned CDRAM frame-ring slot.
 * Checks permutation/coverage. No GPU-source reads, allocations, finish or waits.
 * The caller must fence ring reuse and enforce the approved blend/depth policy. */
mh3_pack_status mh3_mesh_pack_write_ordered_indices(const mh3_mesh_pack *pack, uint32_t mesh_index,
                                                   const uint32_t *order, size_t count,
                                                   uint8_t *scratch, size_t scratch_bytes,
                                                   void *destination, size_t destination_bytes);

#ifdef __cplusplus
}
#endif
#endif
