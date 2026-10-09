#ifndef MH3_FIELD_RESOURCES_H
#define MH3_FIELD_RESOURCES_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MH3_FIELD_RES_OK,
    MH3_FIELD_RES_INVALID,
    MH3_FIELD_RES_RANGE
} mh3_field_res_status;

typedef struct {
    const uint8_t *data;
    size_t bytes;
} mh3_field_res_bytes;
typedef struct {
    mh3_field_res_bytes records;
    uint32_t address;
} mh3_field_res_table;
typedef struct {
    uint32_t bytes, path;
} mh3_field_res_pair;
enum {
    MH3_FIELD_RES_PRIMARY,
    MH3_FIELD_RES_EFFECT,
    MH3_FIELD_RES_EXTRA
};

/* 802AE5C8: MapKind(map)==map selects the primary loaded descriptor. */
uint32_t mh3_field_resources_descriptor(uint8_t map, uint8_t kind, uint32_t primary, uint32_t alternate);
/* A loaded map descriptor starts with its byte area count and has a relocated
 * area-pointer array at +4. The caller supplies that array's checked CPU view.
 * Reject an invalid area before reading an array entry. Output stays untouched
 * on failure. No GPU data or resource file contents are inspected. */
mh3_field_res_status mh3_field_resources_area(mh3_field_res_bytes descriptor, mh3_field_res_bytes areas, uint8_t area, uint32_t *record);

typedef struct {
    void *user;
    mh3_field_res_status (*table)(void *, unsigned kind, uint8_t map, uint8_t area, mh3_field_res_table *out);
    void (*primary)(void *, const mh3_field_res_pair *, uint8_t mode, uint8_t map, uint8_t area, uint8_t part);
    uint8_t (*online_mode)(void *);
    void (*online_prepare)(void *, uint8_t map, uint8_t area);
    void (*environment_prepare)(void *, uint8_t map, uint8_t area);
    uint8_t (*request)(void *, uint8_t kind, uint8_t map, uint8_t area, uint8_t index);
    void (*release)(void *, uint8_t id);
    void (*publish)(void *, uint8_t kind, uint32_t table_address, const uint8_t *ids, size_t bytes);
} mh3_field_res_callbacks;

/* 802AE624: ordered primary/online/environment/effect/extra scheduling.
 * These CPU callbacks run on every logical frame that invokes the function,
 * including frames skipped by graphics. ids is caller-owned compatibility
 * scratch; untouched slots retain their previous values. An effect allocation
 * failure releases only its successful prefix, skips effect publication, and
 * still schedules extras. Extra allocation failures retain the original FF
 * result and publish. Callback side effects are not rolled back on error. */
mh3_field_res_status mh3_field_resources_schedule(uint8_t mode, uint8_t map, uint8_t area, uint8_t *ids, size_t id_bytes, const mh3_field_res_callbacks *callbacks);

#ifdef __cplusplus
}
#endif
#endif
