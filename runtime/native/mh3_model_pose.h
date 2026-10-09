#ifndef MH3_MODEL_POSE_H
#define MH3_MODEL_POSE_H
#include "mh3_model_state.h"
#ifdef __cplusplus
extern "C" {
#endif

/* RMHP08 ScnMdlSimple owns up to16 aligned view slices. These are allocation
 * extents, not the remaining bytes in a RAM bank. No borrowed array is cached. */
typedef struct mh3_model_pose_owner {
    uint32_t scene_address,model_address,base[3],slice_bytes[3],count;
    uint8_t views,current_view;
    uint64_t revision[16];
} mh3_model_pose_owner;
mh3_res_status mh3_model_pose_owner_read(const mh3_res_memory *memory,uint32_t scene,
                                       mh3_model_pose_owner *out);
/* Pure getter ports80080BC0/80080C0C/80080C68 and8007FD0C. The optional
 * arrays retain null; every address calculation is checked before output. */
mh3_res_status mh3_model_pose_array(const mh3_model_pose_owner *owner,uint32_t kind,uint32_t *out);
mh3_res_status mh3_model_pose_count(mh3_res_span scene,uint32_t *out);
/* A completed CalcView producer supplies its actual pointers/count/model.
 * Missing owner is ABSENT; a matching owner with inconsistent inputs fails.
 * Environment matrices are produced only when the normal array is present.
 * Revision advances on writes, independently of later view-array binds. */
mh3_res_status mh3_model_pose_publish(mh3_model_pose_owner *owner,uint32_t position,
    uint32_t normal,uint32_t environment,uint32_t count,uint32_t model,uint64_t revision);
mh3_res_status mh3_model_pose_sources(const mh3_res_memory *memory,const mh3_model_pose_owner *owner,
    const uint32_t addresses[3],uint32_t model,mh3_matrix_sources *out);
/* Unknown overwrites/free/copies invalidate intersecting produced slices.
 * Writes to the scene descriptor or source MDL0 header retire the owner too. */
void mh3_model_pose_retire(mh3_model_pose_owner *owner,uint32_t address,uint32_t bytes);

/* Typed current-matrix producers8009B13C/8009B1A4/80087D30. Matrix rows
 * and dual-texture enable have independent known bits. No packed GPU writes. */
typedef struct mh3_model_matrix_selectors {
    uint8_t row[8],known,dual_known,dual_enabled;
} mh3_model_matrix_selectors;
mh3_res_status mh3_model_matrix_select(mh3_model_matrix_selectors *state,const uint32_t rows[8]);
void mh3_model_matrix_reset(mh3_model_matrix_selectors *state);
mh3_res_status mh3_model_matrix_dual(mh3_model_matrix_selectors *state,int enabled);
mh3_res_status mh3_model_matrix_overlay(const mh3_model_matrix_selectors *selectors,
                                       mh3_native_material_state *material);
/* Internal typed producer. Mutates only known matrix-row/dual fields. Return
 * bits0..7 for changed rows, bits8..15 for changed dual enables; newly known
 * fields count as changes. Invalid producer inputs are DEBUG contracts. */
uint16_t mh3_model_matrix_overlay_unchecked(const mh3_model_matrix_selectors *selectors,
                                           mh3_native_material_state *material);
mh3_res_status mh3_model_matrix_rows(const uint32_t sources[8],int32_t matrix_id,uint32_t rows[8],int *apply);

#ifdef __cplusplus
}
#endif
#endif
