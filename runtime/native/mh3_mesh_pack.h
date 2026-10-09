#ifndef MH3_MESH_PACK_H
#define MH3_MESH_PACK_H
/* External M3MP v1/v2 CPU metadata + GPU-ready payload. Unwired prototype.
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
 * attribute references are checked. Output is untouched on failure.
 * This deliberately accepts no I/O callback. */
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
/* M3MP v2 typed immutable resource inputs. These do not reconstruct inherited
 * pass state, animation/replacement values or the selected shader layout.
 * A clear known bit means the corresponding value must come from a typed live
 * producer. Source register writes are diagnostic provenance, never a renderer
 * command stream. V1 returns UNSUPPORTED. Outputs remain untouched on failure. */
typedef struct {
    uint32_t flags, srt_flags, texture_matrix_mode, tev_count, texgen_count;
    int32_t light_set_reference, fog_reference, indirect_light_reference[3];
    uint32_t indirect_method[3];
    mh3_pack_span bytes;
} mh3_pack_native_material;
typedef struct {
    uint32_t flags, map_mode, effect_flags;
    int32_t camera_reference, light_reference;
    float scale[2], rotation_degrees, translation[2], effect_matrix[12];
} mh3_pack_texture_srt;
typedef struct {
    uint8_t material_source, ambient_source, lighting, diffuse, attenuation, light_mask;
} mh3_pack_channel_control;
enum {
    MH3_CHANNEL_MATERIAL_COLOR=1u<<0, MH3_CHANNEL_MATERIAL_ALPHA=1u<<1,
    MH3_CHANNEL_AMBIENT_COLOR=1u<<2, MH3_CHANNEL_AMBIENT_ALPHA=1u<<3,
    MH3_CHANNEL_CONTROL_COLOR=1u<<4, MH3_CHANNEL_CONTROL_ALPHA=1u<<5
};
typedef struct {
    uint32_t flags, raw_color_control, raw_alpha_control;
    uint8_t material_color[4], ambient_color[4];
    mh3_pack_channel_control color, alpha;
} mh3_pack_channel;
typedef struct { uint8_t a,b,c,d,operation,bias,scale,clamp,output; } mh3_pack_tev_pass;
typedef struct {
    uint64_t known;
    mh3_pack_tev_pass color, alpha;
    uint8_t texture_map, texture_coordinate, raster_channel, raster_swap, texture_swap, konst_color, konst_alpha;
    uint8_t indirect_stage, indirect_format, indirect_bias, indirect_alpha, indirect_matrix;
    uint8_t indirect_wrap_s, indirect_wrap_t, indirect_original_lod, indirect_add_previous;
} mh3_pack_tev_stage;
enum {
    MH3_TEV_COLOR_A, MH3_TEV_COLOR_B, MH3_TEV_COLOR_C, MH3_TEV_COLOR_D,
    MH3_TEV_COLOR_OPERATION, MH3_TEV_COLOR_BIAS, MH3_TEV_COLOR_SCALE, MH3_TEV_COLOR_CLAMP, MH3_TEV_COLOR_OUTPUT,
    MH3_TEV_ALPHA_A, MH3_TEV_ALPHA_B, MH3_TEV_ALPHA_C, MH3_TEV_ALPHA_D,
    MH3_TEV_ALPHA_OPERATION, MH3_TEV_ALPHA_BIAS, MH3_TEV_ALPHA_SCALE, MH3_TEV_ALPHA_CLAMP, MH3_TEV_ALPHA_OUTPUT,
    MH3_TEV_TEXTURE_MAP, MH3_TEV_TEXTURE_COORDINATE, MH3_TEV_RASTER_CHANNEL,
    MH3_TEV_RASTER_SWAP, MH3_TEV_TEXTURE_SWAP, MH3_TEV_KONST_COLOR, MH3_TEV_KONST_ALPHA,
    MH3_TEV_INDIRECT_STAGE, MH3_TEV_INDIRECT_FORMAT, MH3_TEV_INDIRECT_BIAS, MH3_TEV_INDIRECT_ALPHA,
    MH3_TEV_INDIRECT_MATRIX, MH3_TEV_INDIRECT_WRAP_S, MH3_TEV_INDIRECT_WRAP_T,
    MH3_TEV_INDIRECT_ORIGINAL_LOD, MH3_TEV_INDIRECT_ADD_PREVIOUS, MH3_TEV_FIELD_COUNT
};
enum {
    MH3_PIXEL_ALPHA_COMPARE0, MH3_PIXEL_ALPHA_REFERENCE0, MH3_PIXEL_ALPHA_COMPARE1, MH3_PIXEL_ALPHA_REFERENCE1, MH3_PIXEL_ALPHA_OPERATION,
    MH3_PIXEL_DEPTH_TEST, MH3_PIXEL_DEPTH_COMPARE, MH3_PIXEL_DEPTH_WRITE, MH3_PIXEL_BLEND_ENABLE, MH3_PIXEL_LOGIC_ENABLE,
    MH3_PIXEL_BLEND_SUBTRACT, MH3_PIXEL_BLEND_SOURCE, MH3_PIXEL_BLEND_DESTINATION, MH3_PIXEL_LOGIC_OPERATION,
    MH3_PIXEL_COLOR_WRITE, MH3_PIXEL_ALPHA_WRITE, MH3_PIXEL_DITHER, MH3_PIXEL_DESTINATION_ALPHA_ENABLE, MH3_PIXEL_DESTINATION_ALPHA,
    MH3_PIXEL_DEPTH_TEXTURE_OPERATION, MH3_PIXEL_DEPTH_TEXTURE_FORMAT, MH3_PIXEL_DEPTH_TEXTURE_BIAS, MH3_PIXEL_FIELD_COUNT
};
typedef struct {
    uint64_t known;
    uint32_t values[MH3_PIXEL_FIELD_COUNT];
    /* 0 unresolved, 1 unconditional resource alpha/blend disabled,
     * 2 conditional resource alpha/blend disabled, 3 resource blend/logic/subtract.
     * This is not a GXM pass type or permission to change alpha semantics. */
    uint32_t classification;
} mh3_pack_pixel_inputs;
enum {
    MH3_TEXGEN_PROJECTION, MH3_TEXGEN_INPUT_AB11, MH3_TEXGEN_FUNCTION, MH3_TEXGEN_SOURCE_ROW,
    MH3_TEXGEN_EMBOSS_SOURCE, MH3_TEXGEN_EMBOSS_LIGHT, MH3_TEXGEN_MATRIX_ROW,
    MH3_TEXGEN_POST_MATRIX_ROW, MH3_TEXGEN_NORMALIZE, MH3_TEXGEN_DUAL_TEXTURE, MH3_TEXGEN_FIELD_COUNT
};
typedef struct { uint32_t known; uint8_t values[MH3_TEXGEN_FIELD_COUNT]; } mh3_pack_texgen;
/* M3MT v2 constants. Known masks identify individual RGBA components, swap
 * mappings, indirect fields or six row-major coefficients + scale exponent.
 * Coefficients represent signed fixed point /1024; exponent is applied once. */
typedef struct {uint32_t known;int16_t rgba[4];} mh3_pack_tev_color;
typedef struct {uint32_t known;uint8_t rgba[4];} mh3_pack_swap_table;
typedef struct {uint32_t known;uint8_t coordinate,map,scale_s,scale_t;} mh3_pack_indirect_stage;
typedef struct {uint32_t known;int16_t coefficients[6];int32_t scale_exponent;} mh3_pack_indirect_matrix;
mh3_pack_status mh3_mesh_pack_tev_color(const mh3_pack_native_material *material,uint32_t index,int konst,mh3_pack_tev_color *out);
mh3_pack_status mh3_mesh_pack_swap_table(const mh3_pack_native_material *material,uint32_t index,mh3_pack_swap_table *out);
mh3_pack_status mh3_mesh_pack_indirect_stage(const mh3_pack_native_material *material,uint32_t index,mh3_pack_indirect_stage *out);
mh3_pack_status mh3_mesh_pack_indirect_matrix(const mh3_pack_native_material *material,uint32_t index,mh3_pack_indirect_matrix *out);
/* M3MT v3 only. Eight exact ResTev map IDs (0..7/255) and an explicit known
 * mask. Old material payloads return UNSUPPORTED without touching outputs. */
mh3_pack_status mh3_mesh_pack_texcoord_map(const mh3_pack_native_material *material,
                                         uint8_t out[8],uint8_t *known);
mh3_pack_status mh3_mesh_pack_material_native(const mh3_mesh_pack *pack, uint32_t material_index, mh3_pack_native_material *out);
mh3_pack_status mh3_mesh_pack_texture_srt(const mh3_pack_native_material *material, uint32_t slot, mh3_pack_texture_srt *out);
mh3_pack_status mh3_mesh_pack_channel(const mh3_pack_native_material *material, uint32_t channel, mh3_pack_channel *out);
mh3_pack_status mh3_mesh_pack_tev(const mh3_pack_native_material *material, uint32_t stage, mh3_pack_tev_stage *out);
mh3_pack_status mh3_mesh_pack_pixel(const mh3_pack_native_material *material, mh3_pack_pixel_inputs *out);
mh3_pack_status mh3_mesh_pack_texgen(const mh3_pack_native_material *material, uint32_t slot, mh3_pack_texgen *out);
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
