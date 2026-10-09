#ifndef MH3_RESOURCE_LIFETIME_H
#define MH3_RESOURCE_LIFETIME_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MH3_LIFETIME_OK, MH3_LIFETIME_INVALID, MH3_LIFETIME_BOUNDS
} mh3_lifetime_status;
typedef struct { uint32_t address, bytes; } mh3_lifetime_range;
typedef struct {
    uint32_t address, begin, end, options, free_head, free_tail, used_head, used_tail;
} mh3_lifetime_expheap;
typedef struct {
    uint16_t state, settings;
    uint32_t size, previous, next;
} mh3_lifetime_expblock;
typedef struct { uint32_t address, value; } mh3_lifetime_link_write;
typedef struct {
    mh3_lifetime_range retired;
    uint32_t region_begin, region_end;
    mh3_lifetime_link_write links[2];
} mh3_lifetime_free_plan;
typedef enum {
    MH3_LIFETIME_COPY_UNCHANGED,
    MH3_LIFETIME_COPY_SOURCE,
    MH3_LIFETIME_COPY_UNKNOWN
} mh3_lifetime_copy_kind;

/* CPU heap records retain the verified big endian SDK layout. These records
 * are bookkeeping, never GPU command or payload input. Outputs are unchanged
 * on error. Addresses must use one already validated canonical RAM view. */
mh3_lifetime_status mh3_lifetime_expheap_read(const void *data, size_t bytes,
    uint32_t address, mh3_lifetime_expheap *out);
mh3_lifetime_status mh3_lifetime_expblock_read(const void *data, size_t bytes,
    mh3_lifetime_expblock *out);
mh3_lifetime_status mh3_lifetime_expblock_extent(const mh3_lifetime_expheap *heap,
    uint32_t payload, const mh3_lifetime_expblock *block, mh3_lifetime_range *out);
mh3_lifetime_status mh3_lifetime_expheap_free_plan(const mh3_lifetime_expheap *heap,
    uint32_t payload, const mh3_lifetime_expblock *block, mh3_lifetime_free_plan *out);
mh3_lifetime_status mh3_lifetime_expheap_create_plan(uint32_t address, uint32_t bytes,
    mh3_lifetime_range *out);
mh3_lifetime_status mh3_lifetime_expheap_initialize(void *heap, size_t heap_bytes,
    void *free_block, size_t block_bytes, uint32_t begin, uint32_t end);
/* The original request arithmetic wraps at 32 bits, including size+3. */
uint32_t mh3_lifetime_expheap_request_size(uint32_t size);
uint32_t mh3_lifetime_alignment_magnitude(int32_t alignment);
typedef mh3_lifetime_status (*mh3_lifetime_read_block)(void *context,
    uint32_t address, mh3_lifetime_expblock *out);
mh3_lifetime_status mh3_lifetime_expheap_available(const mh3_lifetime_expheap *heap,
    int32_t alignment, mh3_lifetime_read_block read, void *context, uint32_t *out);

mh3_lifetime_status mh3_lifetime_validate_copy32(uint32_t destination,
    uint32_t source, uint32_t bytes);
mh3_lifetime_status mh3_lifetime_validate_zero32(uint32_t destination,uint32_t bytes);
mh3_lifetime_status mh3_lifetime_forward_copy_kind(uint32_t destination,
    uint32_t source, uint32_t bytes, mh3_lifetime_copy_kind *out);
/* Independent 8-byte load/store units preserve the authored forward overlap
 * behavior. No restrict qualifiers and no whole-copy memmove substitution. */
mh3_lifetime_status mh3_lifetime_copy32(void *destination,size_t destination_bytes,
    const void *source,size_t source_bytes,uint32_t bytes);
mh3_lifetime_status mh3_lifetime_fill(void *destination,size_t destination_bytes,
    uint8_t value,uint32_t bytes);

#ifdef __cplusplus
}
#endif
#endif
