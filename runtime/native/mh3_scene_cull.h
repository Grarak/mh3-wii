#ifndef MH3_SCENE_CULL_H
#define MH3_SCENE_CULL_H
/* Conservative visibility of whole scene-queue models (game thread).
 *
 * MH3's scene queue (800E3E80/800E41AC) tests view depth only; every listed
 * model within its distance limits is drawn. A model's converted meshes give
 * its vertices: each mesh's position box, in the vertex position space, under
 * every view-position matrix (the model's CALC_VIEW array) that one of its
 * vertices can select. A model whose (matrix, box) pairs one projection plane
 * rejects has no visible vertex and therefore no visible triangle.
 *
 * The record keeps one box per matrix ID: the union of the boxes of the
 * meshes that use the ID. One plane rejecting every merged pair rejects every
 * mesh. */
#include "mh3_mesh_pack.h"
#include "mh3_model_cull.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t id;
    mh3_cull_box box;
} mh3_scene_cull_pair;

typedef struct {
    mh3_scene_cull_pair *pairs;
    uint32_t count, capacity;
} mh3_scene_cull_record;

/* Adds box under view matrix id (merging with an existing pair). Returns 0
 * when the record is full or the box is not finite. */
int mh3_scene_cull_add(mh3_scene_cull_record *record, uint32_t id, const float min[3], const float max[3]);
/* Adds a converted mesh. A rigid mesh (no batches) uses the shape's matrix
 * ID, which must be non-negative, and no per-vertex selector. A batched mesh
 * uses, per batch, the position slots its vertices select (the batch's
 * required slots with a PN selector, else slot 0); each such slot must have
 * been loaded by this or an earlier batch of the same mesh, or be slot 0 of a
 * shape with a matrix ID, because a slot inherited from another draw has no
 * matrix ID in this model. Returns 0 when
 * the mesh cannot be bounded (the model must then never be culled). */
int mh3_scene_cull_add_mesh(mh3_scene_cull_record *record, const mh3_pack_mesh *mesh, int32_t matrix_id, int selectors, const mh3_pack_palette_batch *batches, uint32_t batch_count);
/* Reads a 3x4 big-endian float matrix (guest layout). Returns 0 if a value is
 * not finite. */
int mh3_scene_cull_read_matrix(const uint8_t source[48], float out[12]);

#ifdef __cplusplus
}
#endif
#endif
