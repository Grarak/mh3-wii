#ifndef MH3_PREPARE_CORNERS_H
#define MH3_PREPARE_CORNERS_H
#include "mh3_prepare_writer.h"
#include "mh3_vertex_sources.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Offline source decoder only. Source, scratch, result and diagnostic spans
 * must be pairwise disjoint; overlap is rejected before scratch/sink mutation.
 * No allocation, content fingerprint, guest GX submission or live pose data. */
typedef enum {
    MH3_CORNERS_OK = 0,
    MH3_CORNERS_INVALID = -1,
    MH3_CORNERS_RANGE = -2,
    MH3_CORNERS_UNSUPPORTED = -3,
    MH3_CORNERS_IO = -4,
    MH3_CORNERS_ROLLBACK = -5
} mh3_corners_status;
typedef struct {
    uint32_t left, right, height;
} mh3_corner_tree_node;
typedef struct {
    uint8_t *vertices;
    size_t vertex_bytes;
    mh3_corner_tree_node *tree;
    uint32_t tree_count;
    uint16_t *corners;
    uint32_t corner_count;
    mh3_pack_palette_batch *batches;
    uint32_t batch_count;
    mh3_prepare_sink indices;
} mh3_corner_workspace;
typedef struct {
    mh3_pack_layout layout;
    uint32_t vertex_count, index_count, batch_count, matrix_mask;
    float bounds_min[3], bounds_max[3], center[3];
} mh3_prepared_corners;
/* First validates complete CP/VAT grammar and declared totals, then writes
 * exact F32/U8N + raw normalized U8 matrix groups. Unified vertices preserve
 * first occurrence using a capped AVL byte-equality table; no hashing.
 * Validation leaves the sink unchanged. Once validated, resets the private
 * sink and streams expanded U16 indices in <=4092-byte chunks, with rollback
 * to zero on failure. Output result is untouched on failure; caller
 * discards scratch. ROLLBACK means the private sink must also be discarded.
 * The caller must provide the directory-certified member span, not its outer
 * archive. Source/vertices <=8 MiB, tree/corner capacities <=65536, batches
 * <=(8 MiB-16)/96; combined GPU/source-pack limits belong to the writer. */
mh3_corners_status mh3_prepare_shape(const mh3_res_model *, uint32_t shape, const mh3_corner_workspace *, mh3_prepared_corners *, size_t *error_offset);
/* Serialize prepared batches as M3PL v1 into an explicit bounded caller span. */
mh3_corners_status mh3_prepare_palettes(const mh3_prepared_corners *, const mh3_corner_workspace *, uint8_t *, size_t, size_t *);
#ifdef __cplusplus
}
#endif
#endif
