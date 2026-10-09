#ifndef MH3_MODEL_RUNTIME_H
#define MH3_MODEL_RUNTIME_H
/* Compatibility boundary for serialized resources still owned by guest RAM.
 * Ordinary typed C, no CpuContext, register emulation, GX or GPU calls.
 * Native resource/mesh consumers can use mh3_model_resources directly. */
#include "mh3_model_resources.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    mh3_res_span mem1, mem2, locked_cache;
} mh3_res_memory;
typedef struct {
    mh3_res_span span;
    size_t offset;
    uint32_t guest_base;
} mh3_res_memory_view;
/* Checked compatibility boundary for native producers borrowing an owned bank. */
mh3_res_status mh3_res_runtime_map(const mh3_res_memory *memory, uint32_t address, size_t required, mh3_res_memory_view *out);

mh3_res_status mh3_res_runtime_reference(const mh3_res_memory *memory, uint32_t reference, int require_valid, uint32_t *result);
mh3_res_status mh3_res_runtime_valid(const mh3_res_memory *memory, uint32_t reference, uint32_t *result);
mh3_res_status mh3_res_runtime_dictionary(const mh3_res_memory *memory, uint32_t reference, int32_t index, uint32_t *result);
mh3_res_status mh3_res_runtime_dictionary_count(const mh3_res_memory *memory, uint32_t reference, uint32_t *result);
mh3_res_status mh3_res_runtime_model(const mh3_res_memory *memory, uint32_t reference, mh3_res_kind kind, int32_t index, uint32_t *result);
mh3_res_status mh3_res_runtime_model_count(const mh3_res_memory *memory, uint32_t reference, mh3_res_kind kind, uint32_t *result);
mh3_res_status mh3_res_runtime_shape_parent(const mh3_res_memory *memory, uint32_t reference, uint32_t *result);
/* Attribute 0=position (required), 1=normal, 2/3=color, 4..11=UV,
 * 12=fur vector, 13=fur position. Optional -1 resources return null; other negatives fail. */
mh3_res_status mh3_res_runtime_shape_array(const mh3_res_memory *memory, uint32_t reference, uint32_t attribute, uint32_t *result);
mh3_res_status mh3_res_runtime_shape_color(const mh3_res_memory *memory, uint32_t reference, uint32_t channel, uint32_t *result);
mh3_res_status mh3_res_runtime_shape_texcoord(const mh3_res_memory *memory, uint32_t reference, uint32_t channel, uint32_t *result);
mh3_res_status mh3_res_runtime_shape_tag(const mh3_res_memory *memory, uint32_t reference, int primitive, int require_valid, uint32_t *result);
mh3_res_status mh3_res_runtime_tag_list(const mh3_res_memory *memory, uint32_t reference, uint32_t *result);
mh3_res_status mh3_res_runtime_shape_setup(const mh3_res_memory *memory, uint32_t reference, uint32_t *result);
/* Checked reference then serialized word at the proven resource field offset. */
mh3_res_status mh3_res_runtime_word(const mh3_res_memory *memory, uint32_t reference, uint32_t field, uint32_t *result);
mh3_res_status mh3_res_runtime_shape_visible(const mh3_res_memory *memory, uint32_t reference, uint32_t *result);
mh3_res_status mh3_res_runtime_node_visible(const mh3_res_memory *memory, uint32_t reference, uint32_t *result);
mh3_res_status mh3_res_runtime_shape_has_attribute(const mh3_res_memory *memory, uint32_t reference, uint32_t attribute, uint32_t *result);
/* Generic pointer wrappers, preserving cached/uncached/physical guest aliases.
 * Alignment is either 0 (raw byte pointer) or 4 (typed aligned resource). */
mh3_res_status mh3_res_runtime_offset(const mh3_res_memory *memory, uint32_t reference, int32_t delta, uint32_t alignment, uint32_t *result);
#ifdef __cplusplus
}
#endif
#endif
