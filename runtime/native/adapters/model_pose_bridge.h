#pragma once
#include "runtime/native/mh3_model_pose.h"
/* Typed producer/publication boundary. Getters declare allocation extents;
 * completed CPU writes establish pose revisions. A bind alone does neither. */
bool PublishNativeModelPose(uint32_t position,uint32_t normal,uint32_t environment,
                            uint32_t count,uint32_t model_reference);
void RetireNativeModelPose(uint32_t address,uint32_t bytes);
void ClearNativeModelPose();
mh3_matrix_sources ReadNativeModelPose(const uint32_t addresses[3],uint32_t model_address=0);
void PublishNativeModelDualTexture(bool enabled);
/* Reapply independent live selector state after an immutable material overlay.
 * Unknown remains unknown; this is never an identity/dual fallback. */
void ApplyNativeModelSelectors(mh3_native_material_state& material);
void PublishNativeModelMatrixRows(const uint32_t rows[8]);
