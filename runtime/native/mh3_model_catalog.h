#ifndef MH3_MODEL_CATALOG_H
#define MH3_MODEL_CATALOG_H
/* Immutable source identities accompanying an external M3MP. No resource byte
 * patches, strings, fingerprints, allocator or rendering API are required. */
#include "mh3_mesh_pack.h"
#include "mh3_model_resources.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct mh3_model_catalog {
    mh3_pack_span bytes;
    uint32_t model_count, model_offset, shape_count, shape_offset;
    uint32_t material_count, material_offset;
    uint32_t version, block_count, block_offset;
} mh3_model_catalog;
typedef struct {
    uint64_t partition_offset;
    uint32_t file_bytes, model_offset, model_bytes, model_id, pack_index, revision;
    uint32_t parent_offset, shape_first, shape_count, material_first, material_count;
} mh3_catalog_model;
typedef struct {
    uint32_t model_index, source_id, source_offset, source_bytes, asset_id, pack_index;
} mh3_catalog_resource;
enum { MH3_CATALOG_MATERIAL_BLOCKS=5 };
typedef struct {
    uint32_t material_index,kind,source_offset,source_bytes,flags;
} mh3_catalog_material_block;
/* Transactional M3ID v1/v2 reader. References, ordering, reserved fields and all
 * source extents are checked. UINT32_MAX denotes an unconverted shape. */
mh3_pack_status mh3_model_catalog_open(mh3_pack_span bytes, mh3_model_catalog *out);
mh3_pack_status mh3_model_catalog_validate_pack(const mh3_model_catalog *catalog,
                                                const mh3_mesh_pack *pack);
mh3_pack_status mh3_model_catalog_model(const mh3_model_catalog *catalog,uint32_t index,
                                        mh3_catalog_model *out);
mh3_pack_status mh3_model_catalog_resource(const mh3_model_catalog *catalog,int material,
                                           uint32_t index,mh3_catalog_resource *out);
mh3_pack_status mh3_model_catalog_find(const mh3_model_catalog *catalog,
                                       uint64_t partition_file_offset,uint32_t model_offset,
                                       uint32_t *index);
mh3_pack_status mh3_model_catalog_find_resource(const mh3_model_catalog *catalog,
                                                uint32_t model_index,int material,
                                                uint32_t source_id,mh3_catalog_resource *out);
/* v2 block order: pixel, colors, indirect, texgen, pipeline. flags bit0 means
 * the extractor verified static field ownership. A clear bit is unsupported,
 * not permission to inspect/replay packed commands. v1 returns UNSUPPORTED. */
mh3_pack_status mh3_model_catalog_block(const mh3_model_catalog *catalog,uint32_t index,
                                       mh3_catalog_material_block *out);
mh3_pack_status mh3_model_catalog_material_block_find(const mh3_model_catalog *catalog,
    uint32_t material_index,uint32_t kind,mh3_catalog_material_block *out);

/* Provenance consists of known byte intervals, not address guesses. All storage
 * is caller owned; work/transfer need capacity entries each and must be distinct.
 * No operation changes ranges/count when it fails, including capacity failure.
 * Producers must report every overwrite/copy/retirement that can affect owners.
 * A DI-only tracker is insufficient to claim a final loaded BRRES owner. */
typedef struct {
    uint32_t address, bytes;
    uint64_t partition_offset, generation;
} mh3_source_range;
typedef struct {
    mh3_source_range *ranges, *work, *transfer;
    size_t count, capacity;
    uint64_t generation;
} mh3_source_tracker;
typedef struct {uint64_t partition_offset, generation;} mh3_source_identity;
/* Certified once from a complete loaded file. Typed nonstructural mutations
 * preserve this allocation/model identity while retiring content provenance;
 * unknown writes, DI replacement and allocation retirement invalidate it. */
typedef struct {
    uint32_t container_address,model_address,catalog_index,file_bytes;
    uint64_t load_generation;
} mh3_source_owner;
mh3_res_status mh3_source_owner_certify(const mh3_source_tracker *tracker,
    const mh3_model_catalog *catalog,uint32_t model_address,mh3_source_owner *out);
/* A complete certified container also certifies the allocation identity of
 * its other cataloged models. The bridge validates their live headers before
 * publication; no fresh unchanged-content proof is required after typed edits. */
mh3_res_status mh3_source_owner_sibling(const mh3_source_owner *owner,
    const mh3_model_catalog *catalog,uint32_t catalog_index,mh3_source_owner *out);
int mh3_source_owner_intersects(const mh3_source_owner *owner,uint32_t address,uint32_t bytes);
/* Explicit native material producer only: prove a write fits a verified group
 * and never touches layout/identity headers. ABSENT means no owner overlap;
 * unsupported/range means the caller cannot retain that owner for this write. */
mh3_res_status mh3_source_owner_material_write(const mh3_source_owner *owner,
    const mh3_model_catalog *catalog,uint32_t address,uint32_t bytes,uint32_t kind);
mh3_res_status mh3_source_tracker_init(mh3_source_tracker *tracker,
                                      mh3_source_range *ranges,mh3_source_range *work,
                                      mh3_source_range *transfer,size_t capacity);
mh3_res_status mh3_source_record_disc(mh3_source_tracker *tracker,uint32_t address,
                                     uint32_t bytes,uint64_t partition_offset,uint64_t generation);
/* Copy snapshots source intervals before modifying destination, so overlapping
 * memmove is exact. Unknown source bytes invalidate corresponding destinations.
 * generation is the new copy producer's monotonic allocation/content event. */
mh3_res_status mh3_source_record_copy(mh3_source_tracker *tracker,uint32_t destination,
                                     uint32_t source,uint32_t bytes,uint64_t generation);
mh3_res_status mh3_source_retire(mh3_source_tracker *tracker,uint32_t address,uint32_t bytes);
int mh3_source_intersects(const mh3_source_tracker *tracker,uint32_t address,uint32_t bytes);
mh3_res_status mh3_source_resolve(const mh3_source_tracker *tracker,uint32_t address,
                                 uint32_t bytes,mh3_source_identity *out);
/* Require full file coverage at the derived BRRES base, not just a magic word or
 * MDL0 header. The parent offset must be the offline-verified BRRES offset zero.
 * ABSENT means provenance is incomplete, never permission to match a name. */
mh3_res_status mh3_source_resolve_model(const mh3_source_tracker *tracker,
                                       const mh3_model_catalog *catalog,uint32_t model_address,
                                       uint32_t *catalog_index,uint32_t *container_address,
                                       uint64_t *generation);
/* Resolve a scoped block (including a copied block) by its complete byte
 * provenance and the exact offline span. This certifies origin, not that an
 * unreported live CPU mutation has not happened. Mutation producers must
 * publish typed overrides/retire that origin before a draw consumes defaults. */
mh3_res_status mh3_source_resolve_material_block(const mh3_source_tracker *tracker,
    const mh3_model_catalog *catalog,uint32_t address,uint32_t material_index,
    uint32_t kind,uint64_t *generation);
/* RMHP08 80004000 memcpy preserves overlap by copying backward when needed,
 * as does 8045BC18 memmove. Native library memmove expresses that byte behavior
 * without keeping PPC alignment/paired-load implementations. Zero bytes allow
 * null pointers and return destination exactly as both guest entries do. */
void *mh3_model_copy_bytes(void *destination,const void *source,size_t bytes);
/* Canonical cached alias for the verified 24 MiB MEM1 / 64 MiB MEM2 mapping.
 * I/O and copied resource references may use physical or uncached aliases of
 * the same allocation. Bounds and outputs are transactional. */
mh3_res_status mh3_source_canonical_address(uint32_t address,uint32_t bytes,uint32_t *out);
#ifdef __cplusplus
}
#endif
#endif
