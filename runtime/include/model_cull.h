#pragma once
// Native visibility culling of model draws (render thread). MH3 draws every
// listed model within its distance limits, also beside and behind the camera:
// its calc-view pass (func_800E3E80, func_800E41AC) tests view depth only and
// nothing on the draw path tests a bounding volume. A draw whose converted
// mesh box, under every position matrix its vertices use, lies outside the
// projected scissor rectangle produces no pixel; SubmitNativeModelMesh skips
// it after the asset residency bookkeeping. MH3_MODEL_CULL=0 turns it off.
#include "runtime/native/mh3_model_submission.h"
#include <cstdint>

bool CullNativeModelMesh(const mh3_resolved_mesh_submit& mesh);

struct NativeModelCullStatistics {
    uint64_t draws = 0, culled = 0;
};
// Counts since the previous call (render thread).
NativeModelCullStatistics TakeNativeModelCullStatistics();
