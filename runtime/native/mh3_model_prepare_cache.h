#ifndef MH3_MODEL_PREPARE_CACHE_H
#define MH3_MODEL_PREPARE_CACHE_H
#include "mh3_model_scope.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct mh3_model_scope_recipe {
    uint64_t structure_revision;
    mh3_model_material_bindings replacements;
    mh3_model_material_scope scope;
} mh3_model_scope_recipe;
/* Only structural pointers are memoized. Group contents and render state are
 * consumed through their live producers on every scope execution. */
int mh3_model_scope_recipe_matches(const mh3_model_scope_recipe *, uint64_t, uint32_t, const mh3_model_material_bindings *);
/* CURRENT row selection is independent of matrix values. Exact input/output
 * aliasing is supported. Actual palette loads remain at submission. */
int mh3_model_shape_rows_unchecked(const uint32_t texture_sources[8], uint32_t rows[8]);
/* Preserve skipped-frame index rejection without constructing a palette or
 * reading matrix values. Pose tables come from their completed producer. */
mh3_res_status mh3_model_shape_pose_extents(const mh3_shape_matrix_input *, const mh3_matrix_sources *);
/* Shape fields belong to a certified structural recipe; selectors/validity
 * remain live. Normal without direct position is intentionally ignored. */
void mh3_model_shape_input_unchecked(int32_t matrix_id, int has_normal, const uint8_t texture_sources[32], int current_valid, int view_valid,
                                     const mh3_matrix34 *position, const mh3_matrix34 *normal, mh3_shape_matrix_input *out);
#ifdef __cplusplus
}
#endif
#endif
