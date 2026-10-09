// Development-only game-thread frame split (game_frame.cpp). MH3_SPLIT_SCOPE
// adds the scope's wall time to one draw-pass family; nested scopes of the
// same family count once. Scopes on the guest worker count nothing: its jobs
// have their own line. Release builds compile both away.
#pragma once
#ifdef MH3_DEVELOPMENT
#include "isa/ppc_isa_thread_state.h"
#include <chrono>
#include <cstdint>
#ifdef __vita__
#include <psp2/kernel/processmgr.h>
#endif

enum MH3SplitFamily : unsigned {
    MH3_SPLIT_TRAVERSE,
    MH3_SPLIT_MODEL,
    MH3_SPLIT_EFFECT,
    MH3_SPLIT_NODE_CALLBACK,
    MH3_SPLIT_FONT,
    MH3_SPLIT_FADE,
    MH3_SPLIT_HUD,
    MH3_SPLIT_MOVIE,
    // Parts of the model family: material scope preparation, shape/item
    // preparation (pose, matrices, selectors) and the packet record.
    MH3_SPLIT_MATERIAL_PREP,
    MH3_SPLIT_SHAPE_PREP,
    MH3_SPLIT_MODEL_RECORD,
    // DrawScene around the buckets: camera/scene calculation before and after
    // the traversal (including the light setting import), the scene finish
    // (cache invalidation, indirect reset) and the post effects.
    MH3_SPLIT_SCENE_SETUP,
    MH3_SPLIT_SCENE_FINISH,
    MH3_SPLIT_POST,
    // The whole DrawScene and its bucket walk (DrawAll), to locate the rest.
    MH3_SPLIT_SCENE,
    MH3_SPLIT_BUCKETS,
    // Effect::Calc of every effect (the particle simulation), a logic part.
    MH3_SPLIT_EFFECT_CALC,
    // MH3_PROFILE=split: the scene processes of model nodes (0x8007DE60 task
    // 1 world matrices with animation, 2 materials, 3 vertices, 4 view
    // matrices, 8 frame update; the rest: gather and draw).
    MH3_SPLIT_G3D_WORLD,
    MH3_SPLIT_G3D_MATERIAL,
    MH3_SPLIT_G3D_VERTEX,
    MH3_SPLIT_G3D_VIEW,
    MH3_SPLIT_G3D_UPDATE,
    MH3_SPLIT_G3D_OTHER,
    // The frame's draw around the scene (frame_draw.cpp): the 2D layers and
    // the UI sprites.
    MH3_SPLIT_LAYERS,
    MH3_SPLIT_UI,
    MH3_SPLIT_FAMILIES
};

// The split's clock. On the Vita steady_clock reads gettimeofday through a
// system call (6 us); the process time is a 0.7 us microsecond counter.
struct MH3SplitClock {
    using duration = std::chrono::microseconds;
    using rep = duration::rep;
    using period = duration::period;
    using time_point = std::chrono::time_point<MH3SplitClock>;
    static constexpr bool is_steady = true;
    static time_point now() noexcept {
#ifdef __vita__
        return time_point(duration(rep(sceKernelGetProcessTimeWide())));
#else
        return time_point(std::chrono::duration_cast<duration>(std::chrono::steady_clock::now().time_since_epoch()));
#endif
    }
};

void MH3FrameSplitCall(unsigned kind);

// The MH3 field split line (field_frame.cpp), which it then resets.
void MH3PrintFieldSplit(unsigned frames);

// The MH3 scene changes line (scene_changes.cpp), which it then resets.
void MH3PrintSceneChanges(unsigned frames);

// The MH3 world split line (scene_world.cpp), which it then resets.
void MH3PrintWorldSplit(unsigned frames);

// The split draw (frame_draw.cpp): with a handoff set, the frame draw calls it
// at the scene's place and leaves the scene to RunDeferredScene (false when
// none was left).
namespace FrameDraw {
void SetSceneHandoff(void (*handoff)());
bool RunDeferredScene(CpuContext* cpu);
} // namespace FrameDraw

// The single view's scene draw split at the bucket draw (native_scene_queue.cpp):
// Prepare walks the scene graph (the effect launch, the buckets' reset, the
// root's camera, the traverse, the root's view matrices), DrawPrepared draws
// the buckets and finishes.
namespace SceneQueue {
void Prepare(CpuContext* cpu);
void DrawPrepared(CpuContext* cpu);
} // namespace SceneQueue

// The MH3 layer split line (frame_draw.cpp), which it then resets.
void MH3PrintLayerSplit(unsigned frames);

// The MH3 hud split line (hud_draw.cpp), which it then resets.
void MH3PrintHudSplit(unsigned frames);
void MH3FrameSplitTime(unsigned family, MH3SplitClock::duration elapsed);
extern uint8_t mh3SplitDepth[MH3_SPLIT_FAMILIES];
extern uint32_t mh3SplitFrame; // frame bodies started
struct MH3FrameSplitScope {
    unsigned family;
    bool active;
    MH3SplitClock::time_point start;
    explicit MH3FrameSplitScope(unsigned value) : family(value), active(PpcThread::CurrentThread() != PpcThread::Worker) {
        if (active && !mh3SplitDepth[family]++)
            start = MH3SplitClock::now();
    }

    ~MH3FrameSplitScope() {
        if (active && !--mh3SplitDepth[family])
            MH3FrameSplitTime(family, MH3SplitClock::now() - start);
    }
};

// Per-draw parts (material/shape preparation, packet record) run about a
// hundred times per frame; MH3_PROFILE=split times them (two clock reads
// each, ~1.5 ms/frame on the Vita).
extern bool mh3SplitDetail;
struct MH3FrameSplitDetailScope {
    unsigned family;
    bool active;
    MH3SplitClock::time_point start;
    explicit MH3FrameSplitDetailScope(unsigned value) : family(value), active(mh3SplitDetail && PpcThread::CurrentThread() != PpcThread::Worker) {
        if (active && !mh3SplitDepth[family]++)
            start = MH3SplitClock::now();
    }

    ~MH3FrameSplitDetailScope() {
        if (active && !--mh3SplitDepth[family])
            MH3FrameSplitTime(family, MH3SplitClock::now() - start);
    }
};
#define MH3_SPLIT_JOIN2(a, b) a##b
#define MH3_SPLIT_JOIN(a, b) MH3_SPLIT_JOIN2(a, b)
#define MH3_SPLIT_SCOPE(family) MH3FrameSplitScope MH3_SPLIT_JOIN(mh3SplitScope, __LINE__)(family)
#define MH3_SPLIT_DETAIL_SCOPE(family) MH3FrameSplitDetailScope MH3_SPLIT_JOIN(mh3SplitScope, __LINE__)(family)
#else
#define MH3_SPLIT_SCOPE(family)
#define MH3_SPLIT_DETAIL_SCOPE(family)
#endif
