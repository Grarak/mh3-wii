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
/* Actual native matrix setter publications, shared with UI/effects. Selectors
 * are PN rows0..27 and main texture rows30..57, step3; post rows64+ do not
 * change these palette slots. Values are copied and retained across frames,
 * source retirement and model changes until another actual load replaces them. */
void PublishNativeModelPositionMatrix(uint32_t selector,const float matrix[12]);
void PublishNativeModelNormalMatrix(uint32_t selector,const float matrix[9]);
/* 2x4 loads replace only the first two rows. They preserve an actually known
 * third row; a new partial slot remains incomplete for 3x4 model consumption. */
void PublishNativeModelTextureMatrix(uint32_t selector,const float matrix[12],bool matrix3x4=true);
const mh3_model_palette& ReadNativeModelPalette();
uint64_t ReadNativeModelPaletteRevision();
void PublishNativeModelPalette(const mh3_model_palette& palette);
