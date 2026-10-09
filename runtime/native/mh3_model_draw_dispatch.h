#ifndef MH3_MODEL_DRAW_DISPATCH_H
#define MH3_MODEL_DRAW_DISPATCH_H
#include "mh3_model_draw.h"
#include "mh3_model_matrices.h"
#ifdef __cplusplus
extern "C" {
#endif

enum {
    MH3_MODEL_SORT_CAPACITY = 2048
};
typedef struct {
    float depth;
    uint16_t priority, node, material, shape;
} mh3_model_draw_record;

/* The authored sort is deliberately unstable. Its two pivot phases are
 * separate CPU globals; helper recursion always uses phase1. Backends expose
 * ordinary record indices, with swaps/publications visible before comparison. */
typedef struct {
    void *user;
    int (*less)(void *, size_t, size_t);
    void (*swap)(void *, size_t, size_t);
    int32_t (*phase)(void *, unsigned);
    void (*set_phase)(void *, unsigned, int32_t);
} mh3_model_sort_ops;
int mh3_model_draw_less(const mh3_model_draw_record *, const mh3_model_draw_record *, int translucent);
size_t mh3_model_draw_minimum(const mh3_model_sort_ops *, size_t, size_t);
void mh3_model_draw_selection_sort(const mh3_model_sort_ops *, size_t, size_t);
void mh3_model_draw_median(const mh3_model_sort_ops *, size_t, size_t, size_t);
mh3_res_status mh3_model_draw_sort(const mh3_model_sort_ops *, size_t, size_t, unsigned);
void mh3_model_draw_swap(mh3_model_draw_record *, mh3_model_draw_record *);
mh3_res_status mh3_model_draw_record_read(mh3_res_span, mh3_model_draw_record *);
mh3_res_status mh3_model_draw_record_write(mh3_res_mut_span, const mh3_model_draw_record *);

/* Sorted collection selects live node visibility, including replacement
 * visibility indexed by runtime node ID. Hidden shapes remain in the sorting
 * input; the later draw checks their then-current flags. */
typedef struct {
    const mh3_res_model *model;
    size_t bytecode;
    const mh3_res_memory *memory;
    mh3_res_span replacement;
    mh3_matrix_table positions;
    float missing_depth;
} mh3_model_draw_collection;
mh3_res_status mh3_model_draw_collect(const mh3_model_draw_collection *, mh3_model_draw_record *, size_t, uint32_t *);
mh3_res_status mh3_model_draw_sorted(const mh3_res_model *, const mh3_model_draw_record *, uint32_t, uint32_t, mh3_model_draw_emit, void *, mh3_model_draw_stats *);
typedef mh3_res_status (*mh3_model_record_read)(void *, uint32_t, mh3_model_draw_record *);
/* Read one record at a time: callbacks can overwrite shared sort storage.
 * Emitted resource entries carry IDs/offsets; their unused names are null. */
mh3_res_status mh3_model_draw_sorted_read(const mh3_res_model *, uint32_t, uint32_t, mh3_model_record_read, void *, mh3_model_draw_emit, void *, mh3_model_draw_stats *);

typedef struct {
    void *user;
    void (*bind_sources)(void *, const uint32_t[3]);
    void (*complete_sources)(void *);
    mh3_res_status (*pass)(void *, uint32_t, uint32_t, uint32_t, int, int);
} mh3_model_draw_pipeline;
mh3_res_status mh3_model_draw_pipeline_run(const mh3_model_draw_pipeline *, const uint32_t[3], uint32_t, uint32_t, uint32_t, uint32_t);

/* Application identities below are opaque at the C boundary. Callbacks may
 * change live owner bytes or the same options word between opaque and xlu. */
typedef struct {
    void *user;
    void (*draw_task)(void *, uint32_t, unsigned, uint32_t *);
    void (*handler)(void *, uint32_t, uint32_t, uint32_t, uint32_t);
    void (*virtual_handler)(void *, uint32_t, unsigned, uint32_t);
    /* callback object, vtable slot, mask, owner, argument, options */
    void (*callback)(void *, uint32_t, unsigned, unsigned, uint32_t, uint32_t, uint32_t);
    uint32_t (*options)(void *, uint32_t);
    uint32_t (*source)(void *, uint32_t, unsigned);
    /* model, sources, opaque stream, translucent stream, replacement, options */
    mh3_res_status (*draw)(void *, uint32_t, const uint32_t[3], uint32_t, uint32_t, uint32_t, uint32_t);
} mh3_model_scene_draw_ops;
mh3_res_status mh3_model_scene_options(mh3_res_span, uint32_t *);
mh3_res_status mh3_model_scene_draw(const mh3_model_scene_draw_ops *, mh3_res_span, uint32_t, uint32_t);
mh3_res_status mh3_model_scene_task(const mh3_model_scene_draw_ops *, mh3_res_span, uint32_t, uint32_t, uint32_t, uint32_t);
mh3_res_status mh3_model_scene_opa(const mh3_model_scene_draw_ops *, mh3_res_span, uint32_t, uint32_t, uint32_t);
mh3_res_status mh3_model_scene_xlu(const mh3_model_scene_draw_ops *, mh3_res_span, uint32_t, uint32_t, uint32_t);
#ifdef __cplusplus
}
#endif
#endif
