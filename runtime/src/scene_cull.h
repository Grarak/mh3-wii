#pragma once
// Game-side visibility of scene-queue models (MH3_MODEL_CULL_GAME).
//
// MH3's scene queue (800E3E80 traversal, 800E41AC visibility) tests view depth
// only, so every listed model within its distance limits is drawn, also beside
// and behind the camera. Right before a bucket draws a model (800E44B0, after
// its pre-draw callback), the model is tested against the projection,
// viewport and scissor the draw records, with its converted meshes' boxes
// under the view-position matrices of its current CALC_VIEW array
// (runtime/native/mh3_scene_cull.h). A model outside skips its draw: no
// dispatch, capture or packet item. The pre-draw callback still runs, and the
// model's GPU assets stay demanded.
//
// Only plain ScnMdl objects (G3dProc 8007DE60) without an enabled draw
// callback and with a certified draw table are tested; any model whose meshes
// cannot all be bounded is drawn.
//
// MH3_MODEL_CULL_GAME=0 turns it off. =2 (development) draws every model and
// checks each model the test rejects against the render path's palettes
// (mh3_model_palette_rigid/apply_batch) and per-batch mesh boxes.
#include <cstdint>

// Game thread. True: the bucket skips this model's draw.
bool SkipNativeSceneModel(uint32_t object);

struct NativeSceneCullStatistics {
    uint64_t models = 0, tested = 0, culled = 0;

    // Not tested: another object class, a draw callback, no draw table, an
    // unbounded mesh.
    uint64_t other_class = 0, callback = 0, no_table = 0, unbounded = 0;

    // MH3_MODEL_CULL_GAME=2: culled models with a visible mesh batch.
    uint64_t checked = 0, mismatches = 0;
};

// Counts since the previous call (game thread).
NativeSceneCullStatistics TakeNativeSceneCullStatistics();
