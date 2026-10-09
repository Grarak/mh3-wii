#ifndef MH3_PREPARE_WRITER_H
#define MH3_PREPARE_WRITER_H
/* Preparation-only portable serializers. No allocation, guest pointers, GPU
 * API, resource decoder, asset fingerprint, or filesystem publication.
 * Sink callbacks operate on private temporary files, never published assets. */
#include "mh3_model_catalog.h"
#ifdef __cplusplus
extern "C" {
#endif
enum {
    MH3_PREPARE_MAX_METADATA = 8u << 20,
    MH3_PREPARE_MAX_IDENTITY = 4u << 20,
    MH3_PREPARE_MAX_GPU = 32u << 20,
    MH3_PREPARE_MAX_INDEX = 4u << 20
};
typedef enum {
    MH3_PREPARE_OK = 0,
    MH3_PREPARE_INVALID = -1,
    MH3_PREPARE_RANGE = -2,
    MH3_PREPARE_IO = -3,
    MH3_PREPARE_ROLLBACK = -4
} mh3_prepare_status;
typedef struct {
    void *context;
    /* Return zero only after completing the whole requested operation. Writes
     * may partially change the private sink before reporting failure. */
    int (*write)(void *, uint64_t, const uint8_t *, size_t);
    int (*truncate)(void *, uint64_t);
} mh3_prepare_sink;
typedef struct {
    void *context;
    int (*read)(void *, uint64_t, uint8_t *, size_t);
} mh3_prepare_input;
typedef struct {
    uint8_t *data;
    size_t capacity;
    uint32_t size, counts[8], offsets[8], written[8], tables, kind, sealed;
    mh3_prepare_sink gpu;
    uint32_t gpu_bytes;
} mh3_prepare_writer;
/* CPU buffers are caller-owned. Successful begin clears only reserved tables.
 * No later operation may grow beyond its buffer or the subsystem cap. Record
 * words are host numeric values (float fields use float_bits), explicitly
 * written little endian. Records append separately within each table. */
uint32_t mh3_prepare_float_bits(float value);
mh3_prepare_status mh3_prepare_mesh_begin(mh3_prepare_writer *, uint8_t *, size_t, const uint32_t counts[8], mh3_prepare_sink);
mh3_prepare_status mh3_prepare_identity_begin(mh3_prepare_writer *, uint8_t *, size_t, uint32_t models, uint32_t shapes, uint32_t materials);
mh3_prepare_status mh3_prepare_record(mh3_prepare_writer *, uint32_t table, const uint32_t *words, size_t word_count);
/* Blob placement matches PackBuilder. No string interning is implicit: a
 * future source converter owns its bounded string lookup and insertion order. */
mh3_prepare_status mh3_prepare_blob(mh3_prepare_writer *, const void *, size_t, uint32_t alignment, uint32_t *offset);
/* Raw GPU chunks append with requested alignment. On I/O failure truncate to
 * the previous checkpoint; rollback failure poisons the writer. */
mh3_prepare_status mh3_prepare_gpu(mh3_prepare_writer *, const void *, size_t, uint32_t alignment, uint32_t *offset);
/* Finish validates production metadata/catalog readers, complete shapes and
 * all five verified material groups. Mesh finish emits the 64-byte fetch guard.
 * It does not decode vertices or validate their complete index/palette content.
 * That independent admission validation is required before publication. */
mh3_prepare_status mh3_prepare_mesh_finish(mh3_prepare_writer *, mh3_mesh_pack *);
mh3_prepare_status mh3_prepare_identity_finish(mh3_prepare_writer *, const mh3_mesh_pack *, mh3_model_catalog *);
mh3_prepare_status mh3_prepare_abort(mh3_prepare_writer *);
/* Assemble a sealed metadata + GPU temporary into another private sink using
 * caller-owned scratch. Any failure truncates the destination to zero. */
mh3_prepare_status mh3_prepare_emit(const mh3_prepare_writer *, mh3_prepare_input, mh3_prepare_sink, uint8_t *scratch, size_t scratch_bytes);
typedef struct {
    const char *path;
    uint32_t metadata_bytes, identity_bytes, gpu_bytes, file_bytes;
} mh3_prepare_index_pack;
typedef struct {
    uint64_t partition_offset;
    uint32_t source_bytes, pack_index;
} mh3_prepare_index_source;
/* Packs are strictly path-sorted; sources are strictly extent-sorted/disjoint.
 * Input tables/paths remain stable and must not overlap output. All bounds,
 * paths and references are checked before touching output or size. */
mh3_prepare_status mh3_prepare_index(uint8_t *, size_t, const mh3_prepare_index_pack *, uint32_t, const mh3_prepare_index_source *, uint32_t, size_t *);
#ifdef __cplusplus
}
#endif
#endif
