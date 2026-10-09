#ifndef MH3_MODEL_SUBMISSION_H
#define MH3_MODEL_SUBMISSION_H
#include "mh3_model_catalog.h"
#include "mh3_model_matrices.h"
#include "mh3_model_state.h"
#include "mh3_texture_matrices.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct mh3_model_texcoord_sizes {
    uint16_t width[8], height[8];
    uint8_t known;
} mh3_model_texcoord_sizes;

/* Direct native model boundary, not a GPU uniform ABI. Borrowed CPU metadata,
 * resident GPU ranges and typed live inputs must survive the submit call. The
 * backend retains GPU allocations through its existing asynchronous fences and
 * copies small pose/material uniforms before returning. It never expands or
 * reads the immutable vertex/index payload on the CPU during a normal draw. */
typedef struct mh3_resolved_mesh_submit {
    const mh3_mesh_pack *pack;
    uintptr_t resident_handle;
    uint64_t resident_generation, source_generation, pose_generation;
    uint32_t model_id, shape_id, asset_id, mesh_index;
    mh3_pack_mesh mesh;
    mh3_pack_layout layout;
    const mh3_model_palette *palette;
    const mh3_native_material_state *material;
    /* Wii convention row-major projection. position[0] is already posed view
     * space; projection/clip conversion is applied once by the native backend. */
    const float *projection;             /* 16 floats */
    const float (*texture_matrices)[12]; /* 8 prepared post-texture matrices */
    uint8_t texture_matrix_known, texture_matrix_3x4;
    /* Exact manual coordinate scale, selected by ResTev's map table. NULL map
     * entries inherit the previous coordinate value; bound map dimensions and
     * coordinate dimensions are distinct state, never interchangeable guesses. */
    const mh3_model_texcoord_sizes *texcoord_sizes;
    uint32_t first_index, index_count;
    const mh3_pack_palette_batch *palette_batch; /* admitted typed batch; null for rigid */

    /* Skinned (docs/optimizations/rendering/gpu-skinning.md): the whole mesh in one submit, its
     * palette rows view-space 3x4 matrices (rigid rows the draw matrix, bone
     * rows the skinning matrix); palette and palette_batch are null. */
    const float (*skin_palette)[12];
    uint32_t skin_row_count;
    uint8_t skin_texgen[8];
} mh3_resolved_mesh_submit;

/* Admit once per resident generation. Metadata-only packs may have both GPU
 * spans absent; actual submission must resolve the validated resident mesh.
 * No borrowed live material/pose/camera
 * pointers are cached. Callers key this immutable record by resident handle,
 * generation and catalog model/shape ID, and retire it with the allocation. */
typedef struct mh3_prepared_mesh {
    mh3_resolved_mesh_submit immutable;
    uint8_t normal_required;
    uint16_t matrix_index_mask;
    uint32_t palette_batch_count;
    /* Owner allocates/decodes these once after prepare; no binary reads during
     * bind_batch. NULL until admitted; non-indexed meshes have count0. */
    const mh3_pack_palette_batch *palette_batches;

    /* Skinned meshes: the M3SK rows, decoded by the owner like the batches. */
    uint32_t skin_row_count;
    const mh3_pack_skin_row *skin_rows;
    uint8_t skin_texgen[8];
} mh3_prepared_mesh;

typedef struct mh3_prepared_material {
    mh3_native_material_state state;
    mh3_native_texture_srt srt;
    uint32_t binding_count;
    mh3_pack_binding bindings[8];
    uint8_t texcoord_map[8], texcoord_map_known;
} mh3_prepared_material;

/* Field ownership of the resource submitters, not an instruction stream. The
 * selected record is an overlay: its top-level counts/references/generation
 * are deliberately unknown and must not replace inherited pass state. */
typedef enum mh3_model_material_block {
    MH3_MODEL_MATERIAL_PIXEL,
    MH3_MODEL_MATERIAL_COLORS,
    MH3_MODEL_MATERIAL_INDIRECT,
    MH3_MODEL_MATERIAL_TEXGEN,
    MH3_MODEL_MATERIAL_PIPELINE,
    MH3_MODEL_MATERIAL_BLOCK_COUNT
} mh3_model_material_block;

/* Pure C replacements for the field effects of 80094480/8009454C/80096634/
 * 80097210/8009A244 once the caller resolves a certified typed block producer.
 * count is the caller's indirect/texgen count; ignored by the other blocks.
 * Texgen count changes require a new typed producer because inactive source
 * fields are not retained by the current M3MT payload. Zero indirect/texgen
 * count has the original conditional no-publication behavior (empty overlay).
 * This never reads source command bytes or invents fields from inherited state.
 * Output is transactional. Apply with mh3_material_state_overlay(...,0). */
mh3_pack_status mh3_model_material_group_select(const mh3_prepared_material *prepared, mh3_model_material_block block, uint32_t count, mh3_native_material_state *out);
/* Same field ownership for an explicitly published live material record.
 * Its declared counts come from its producer, never inherited GPU state. */
mh3_pack_status mh3_model_material_fields_select(const mh3_native_material_state *fields, mh3_model_material_block block, uint32_t count, mh3_native_material_state *out);
/* Typed TexCoordScaleState::LoadTexCoordScale_: caller supplies
 * actual map dimensions from texture producers and inherited coordinate sizes.
 * Exact NULL entries preserve inherited values/known bits. No map is inferred
 * from TEV stages; zero dimensions are rejected pending an explicit producer
 * for their original hardware underflow semantics. Both outputs transactional. */
mh3_res_status mh3_model_texcoord_sizes_select(const mh3_prepared_material *prepared, uint32_t count, const mh3_model_texcoord_sizes *map_sizes, const mh3_model_texcoord_sizes *current,
                                               mh3_model_texcoord_sizes *out, uint8_t *written);
/* The same selection from a bare coordinate-to-map table (no recipe). */
mh3_res_status mh3_model_texcoord_sizes_select_map(const uint8_t texcoord_map[8], uint8_t texcoord_map_known, uint32_t count, const mh3_model_texcoord_sizes *map_sizes,
                                                   const mh3_model_texcoord_sizes *current, mh3_model_texcoord_sizes *out, uint8_t *written);
/* Immutable defaults/known masks, SRT recipes and texture binding IDs are
 * assembled once. Strings borrow pack metadata, never a live source object. */
mh3_pack_status mh3_model_material_prepare(const mh3_mesh_pack *pack, uint32_t material_index, mh3_prepared_material *out);
mh3_pack_status mh3_model_mesh_prepare(const mh3_model_catalog *catalog, const mh3_mesh_pack *pack, uint32_t catalog_model_index, uint32_t shape_id, uintptr_t resident_handle,
                                       uint64_t resident_generation, mh3_prepared_mesh *out);
/* Constant-time draw binding: no binary lookup or immutable metadata decode.
 * source_generation must come from the current verified owner; live resource
 * compatibility/material checks remain explicit producers/backend gates.
 * Palette values and required/known masks are not validated again. */
mh3_pack_status mh3_model_mesh_bind_pose(const mh3_prepared_mesh *prepared, uint64_t resident_generation, uint64_t source_generation, uint64_t pose_generation, const mh3_model_palette *palette,
                                         mh3_resolved_mesh_submit *out);
mh3_pack_status mh3_model_mesh_bind_batch(const mh3_prepared_mesh *prepared, uint32_t batch_index, uint64_t resident_generation, uint64_t source_generation, uint64_t palette_generation,
                                          const mh3_model_palette *palette, mh3_resolved_mesh_submit *out);
/* A skinned mesh's single submit with its palette rows (skin_row_count
 * view-space 3x4 matrices, borrowed until the submit returns). */
mh3_pack_status mh3_model_mesh_bind_skin(const mh3_prepared_mesh *prepared, uint64_t resident_generation, uint64_t source_generation, uint64_t pose_generation,
                                         const float (*rows)[12], mh3_resolved_mesh_submit *out);
/* Convenience tooling/test API. Runtime admission should use prepare followed
 * by bind_pose, rather than decoding immutable metadata on every draw.
 * Resolve metadata and the attribute contract, and bind the rigid palette. This
 * does not read GPU bytes, submit a draw, infer absent vertex defaults, validate
 * material dependencies or silently change alpha/blend/vegetation ordering.
 * resident_handle/generations and live shader inputs are explicit producers.
 * The caller fills projection/material/post-matrix pointers after this function
 * and must satisfy backend material validation before admission/submission. */
mh3_pack_status mh3_model_mesh_resolve(const mh3_model_catalog *catalog, const mh3_mesh_pack *pack, uint32_t catalog_model_index, uint32_t shape_id, uintptr_t resident_handle,
                                       uint64_t resident_generation, uint64_t source_generation, uint64_t pose_generation, const mh3_model_palette *palette, mh3_resolved_mesh_submit *out);
#ifdef __cplusplus
}
#endif
#endif
