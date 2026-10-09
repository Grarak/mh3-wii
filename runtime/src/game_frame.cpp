// Game frame, RMHP08. C reconstruction of the main loop's per-frame body
// (0x80046D98, called by the main loop 0x8003F278 between the frame setup
// 0x8003FF20 and the display copy 0x8003FFFC), decompiled with Ghidra
// (scripts/dol2elf.py -> ELF import).
//
// One call is one fixed logic step followed by its draw:
//   logic: the task pass (0x800416F8) clears the draw buckets (0x80048FFC,
//          0x80048890) and runs the 16 task slots, which advance the game by
//          one frame and queue what they want drawn;
//   draw:  the scene pass (0x80049308) renders the queued buckets, then fonts,
//          the fade overlay, the HUD and the pause/fade hooks.
// The phase byte (0x8065AA83) records how far the frame got (1 started,
// 2 logic, 3 draw, 4 done).
#include "abi_bridge.h"
#include "cheats.h"
#include "draw_pages.h"
#include "frame_split.h"
#include "game_step.h"
#include "game_tasks.h"
#include "guest_call.h"
#include "guest_worker.h"
#include "memory.h"
#include "native_frame_packet.h"
#include "native_graphics.h"
#include "native_owner.h"
#include "village_boot.h"
#include <initializer_list>
#include <utility>
#ifdef MH3_DEVELOPMENT
#include "dev_options.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#endif

// model_scene_state.cpp: the overlapped draw's model cache invalidation.
void SetModelInvalidateDeferral(bool enabled);
void ApplyDeferredModelInvalidate();

// model_catalog.cpp: the pose and vertex cache retirements of a logic step
// that ran beside the draw.
void ApplyDeferredDrawCacheRetirements();

namespace {

constexpr uint32_t kSystem = 0x8065AA60; // game system flags
enum : uint32_t {
    kBootState = 0x22,   // u8 0x8065AA82: 0 boot, 1-2 running
    kFramePhase = 0x23,  // u8 0x8065AA83
    kScreenGrab = 0x872, // u8 0x8065B2D2: copy the grabbed screen back to the EFB
};

enum : uint32_t {
    WPADDisconnect = 0x804EEF40,
    PollControllers = 0x80041F6C,
    StartBootTask = 0x80041854, // (task function, arg)
    BootTask = 0x80059648,
    flfntStackReset = 0x8005B52C,
    UpdateScene = 0x80046EFC,       // scene root update when a scene is loaded
    Setup2DProjection = 0x80046E98, // ortho projection + identity position matrix
    MovieUpdate = 0x80435B54,
    NetworkResetCheck = 0x8044E258,
    RunTasks = 0x800416F8,
    GrabScreen = 0x800586A0,
    ResetFade = 0x800D2230,
    TickSystemTimers = 0x80044AF4,
    DrawScene = 0x80049308,
    flfntFlush = 0x8005C174,
    DrawFadeOverlay = 0x800582EC,
    TickFades = 0x800581A4,
    RestoreGrabbedScreen = 0x80058F84,
    DrawHud = 0x800D10B0,
    MovieDraw = 0x80435D84,
    PauseHook = 0x800D2678,
    OverlayHook = 0x800D298C,
};

#ifdef MH3_DEVELOPMENT
// MH3_STEP_PROBE=T[,T...][,step=N][,settle][,all] (docs/flags.md): find state
// that counts steps rather than time, before the first logic step at or after
// each T ticks of game time (loading runs many short steps at variable step),
// or before logic step N. From one RAM snapshot the task pass runs once with a
// 1/30 s step, then (restored) twice with 1/60 s steps; RAM is restored before
// the real step. Every changed word goes to mh3-step-probe-N.txt with both
// deltas: time-based state changes about equally, step-based state about twice
// as much over the two half steps. Host-side effects of the extra passes
// (audio, GX, DVD requests) are not undone, so probe in a steady scene.
const DevOptions& StepProbeOptions() {
    static const DevOptions options("MH3_STEP_PROBE", {"step", "settle", "all"}, DevOptions::Words::Allowed);
    return options;
}

struct Ram {
    std::vector<uint8_t> bytes;
    static uint8_t* Mem1() { return Memory::GetPointer(Memory::kMem1CachedBase, Memory::kMem1Size); }
    static uint8_t* Mem2() { return Memory::GetPointer(Memory::kMem2CachedBase, Memory::kMem2Size); }
    void Save() {
        bytes.resize(Memory::kMem1Size + Memory::kMem2Size);
        std::memcpy(bytes.data(), Mem1(), Memory::kMem1Size);
        std::memcpy(bytes.data() + Memory::kMem1Size, Mem2(), Memory::kMem2Size);
    }

    void Restore() const {
        std::memcpy(Mem1(), bytes.data(), Memory::kMem1Size);
        std::memcpy(Mem2(), bytes.data() + Memory::kMem1Size, Memory::kMem2Size);
    }

    uint32_t Word(size_t offset) const { return uint32_t(bytes[offset]) << 24 | bytes[offset + 1] << 16 | bytes[offset + 2] << 8 | bytes[offset + 3]; }
    static uint32_t Address(size_t offset) { return offset < Memory::kMem1Size ? Memory::kMem1CachedBase + uint32_t(offset) : Memory::kMem2CachedBase + uint32_t(offset - Memory::kMem1Size); }
};

// settle: both runs then take one more 1/30 s step, so words that keep the
// value from before the step (previous positions, frames, joint matrices)
// agree again; only amounts still applied per step differ.
template <typename RunTasks>
void ProbeStepDependence(uint32_t step, RunTasks runTasks) {
    static const bool settle = StepProbeOptions().Has("settle");
    const GameStep::State clock = GameStep::Save();
    Ram before, whole, halves;
    before.Save();
    GameStep::Begin(1.0);
    runTasks();

    if (settle) {
        GameStep::Begin(1.0);
        runTasks();
    }

    whole.Save();
    before.Restore();
    GameStep::Restore(clock);
    GameStep::Begin(0.5);
    runTasks();
    GameStep::Begin(0.5);
    runTasks();

    if (settle) {
        GameStep::Begin(1.0);
        runTasks();
    }

    halves.Save();
    before.Restore();
    GameStep::Restore(clock);

    const std::string name = "mh3-step-probe-" + std::to_string(step) + ".txt";
    FILE* out = std::fopen(name.c_str(), "w");

    if (!out)
        return;

    std::fprintf(out, settle ? "# address before whole(2x1/30) halves(2x1/60+1/30) as u32 and f32\n" : "# address before whole(1/30) halves(2x1/60) as u32 and f32\n");
    size_t changed = 0;

    for (size_t i = 0; i + 4 <= before.bytes.size(); i += 4) {
        const uint32_t a = before.Word(i), b = whole.Word(i), c = halves.Word(i);

        if (a == b && a == c)
            continue;

        float fa, fb, fc;
        std::memcpy(&fa, &a, 4);
        std::memcpy(&fb, &b, 4);
        std::memcpy(&fc, &c, 4);
        std::fprintf(out, "%08x %08x %08x %08x %g %g %g\n", Ram::Address(i), a, b, c, fa, fb, fc);
        ++changed;
    }

    std::fclose(out);
    std::printf("MH3 step probe: step %u, %llu words changed -> %s\n", step, static_cast<unsigned long long>(changed), name.c_str());
    std::fflush(stdout);
}
#endif

#ifdef MH3_DEVELOPMENT
// Calls per frame phase (2 logic, 3 draw) of calc entry points: world matrices,
// animation results, effect calc, view matrices.
uint32_t splitCalls[4][5];
double splitFamilies[MH3_SPLIT_FAMILIES];
uint32_t splitScopes[MH3_SPLIT_FAMILIES];
#endif
} // namespace
#ifdef MH3_DEVELOPMENT
uint8_t mh3SplitDepth[MH3_SPLIT_FAMILIES];
uint32_t mh3SplitFrame;

// Set at the first frame: the Vita applies runtime.env after static
// initialization.
bool mh3SplitDetail = false;
void MH3FrameSplitCall(unsigned kind) {
    // The guest worker's calls are not the frame's (the draw thread's are).
    if (PpcThread::CurrentThread() == PpcThread::Worker)
        return;

    const unsigned phase = Memory::Read8(kSystem + kFramePhase);
    ++splitCalls[kind][phase < 5 ? phase : 0];
}

void MH3FrameSplitTime(unsigned family, MH3SplitClock::duration elapsed) {
    splitFamilies[family] += std::chrono::duration<double, std::milli>(elapsed).count();
    ++splitScopes[family];
}
#endif
namespace {
#ifdef MH3_DEVELOPMENT
// Game-thread split of one frame body: logic (input, scene update, tasks,
// timers), draw (scene, fonts, fade, HUD, movie: what the render thread is to
// take over, including the G3D calc passes it runs today) and the rest.
// Printed once per second with MH3 stats (MH3_STATS=0 disables it).
struct FrameSplit {
    using Clock = MH3SplitClock;
    Clock::time_point start, mark;
    double logic = 0, draw = 0, other = 0;
    uint32_t frames = 0;
    Clock::time_point reported = Clock::now();
    static double Ms(Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); }
    void Begin() {
        if (!mh3SplitFrame)
            mh3SplitDetail = ProfileOptions().Has("split");

        ++mh3SplitFrame;
        start = mark = Clock::now();
    }

    void Lap(double& bucket) {
        const auto now = Clock::now();
        bucket += Ms(now - mark);
        mark = now;
    }

    void End() {
        Lap(other);
        ++frames;
        static const bool enabled = [] {
            const char* value = std::getenv("MH3_STATS");
            return !value || std::strcmp(value, "0") != 0;
        }();

        if (!enabled || mark - reported < std::chrono::seconds(1))
            return;

        std::printf("MH3 frame split: logic %.2f draw %.2f other %.2f ms/frame (%u frames); calls logic/draw: world %u/%u anim %u/%u effect %u/%u view %u/%u\n", logic / frames, draw / frames,
                    other / frames, frames, splitCalls[0][2], splitCalls[0][3], splitCalls[1][2], splitCalls[1][3], splitCalls[2][2], splitCalls[2][3], splitCalls[3][2], splitCalls[3][3]);
        const auto f = [this](unsigned family) { return splitFamilies[family] / frames; };
        const auto n = [this](unsigned family) { return splitScopes[family] / frames; };
        std::printf("MH3 draw split: traverse %.2f model %.2f effect %.2f node callback %.2f font %.2f fade %.2f hud %.2f movie %.2f layers %.2f ui %.2f ms/frame; "
                    "timed scopes/frame: traverse %u model %u effect %u node callback %u\n",
                    f(MH3_SPLIT_TRAVERSE), f(MH3_SPLIT_MODEL), f(MH3_SPLIT_EFFECT), f(MH3_SPLIT_NODE_CALLBACK), f(MH3_SPLIT_FONT), f(MH3_SPLIT_FADE), f(MH3_SPLIT_HUD),
                    f(MH3_SPLIT_MOVIE), f(MH3_SPLIT_LAYERS), f(MH3_SPLIT_UI), n(MH3_SPLIT_TRAVERSE), n(MH3_SPLIT_MODEL), n(MH3_SPLIT_EFFECT), n(MH3_SPLIT_NODE_CALLBACK));
        std::printf("MH3 scene split: scene %.2f buckets %.2f setup %.2f finish %.2f post %.2f ms/frame\n", f(MH3_SPLIT_SCENE), f(MH3_SPLIT_BUCKETS), f(MH3_SPLIT_SCENE_SETUP),
                    f(MH3_SPLIT_SCENE_FINISH), f(MH3_SPLIT_POST));
        // Deferred Effect::Calc calls run as the guest worker's jobs (its only
        // ones), so the game thread's calc is the rest.
        const auto worker = GuestWorker::TakeStatistics();
        std::printf("MH3 effect split: calc %.2f on the game thread, %.2f on the guest worker; draw %.2f ms/frame (game-thread calc scopes/frame %u)\n",
                    f(MH3_SPLIT_EFFECT_CALC), worker.jobNs / 1e6 / frames, f(MH3_SPLIT_EFFECT), n(MH3_SPLIT_EFFECT_CALC));
        std::printf("MH3 guest worker: %.2f jobs/frame, %.2f ms/frame of jobs, game waited %.2f ms/frame (%llu waits)\n", double(worker.jobs) / frames, worker.jobNs / 1e6 / frames,
                    worker.waitNs / 1e6 / frames, static_cast<unsigned long long>(worker.waits));

        // The draw thread's scenes, and the game thread's waits for them at the handoff.
        if (const auto draw = DrawThread::Thread().TakeStatistics(); draw.jobs)
            std::printf("MH3 draw thread: %.2f scenes/frame, %.2f ms/frame of scenes, game waited %.2f ms/frame (%llu waits)\n", double(draw.jobs) / frames, draw.jobNs / 1e6 / frames,
                        draw.waitNs / 1e6 / frames, static_cast<unsigned long long>(draw.waits));
        MH3PrintWorldSplit(frames);

        if (mh3SplitDetail) {
            std::printf("MH3 model split: material prep %.2f (%u) shape prep %.2f (%u) record %.2f (%u) ms/frame (scopes/frame)\n", f(MH3_SPLIT_MATERIAL_PREP), n(MH3_SPLIT_MATERIAL_PREP),
                        f(MH3_SPLIT_SHAPE_PREP), n(MH3_SPLIT_SHAPE_PREP), f(MH3_SPLIT_MODEL_RECORD), n(MH3_SPLIT_MODEL_RECORD));
            std::printf("MH3 g3d split: world %.2f (%u) material %.2f (%u) vertex %.2f (%u) view %.2f (%u) update %.2f (%u) other %.2f (%u) ms/frame (model scopes/frame)\n",
                        f(MH3_SPLIT_G3D_WORLD), n(MH3_SPLIT_G3D_WORLD), f(MH3_SPLIT_G3D_MATERIAL), n(MH3_SPLIT_G3D_MATERIAL), f(MH3_SPLIT_G3D_VERTEX), n(MH3_SPLIT_G3D_VERTEX),
                        f(MH3_SPLIT_G3D_VIEW), n(MH3_SPLIT_G3D_VIEW), f(MH3_SPLIT_G3D_UPDATE), n(MH3_SPLIT_G3D_UPDATE), f(MH3_SPLIT_G3D_OTHER), n(MH3_SPLIT_G3D_OTHER));
            MH3PrintFieldSplit(frames);
            MH3PrintSceneChanges(frames);
            MH3PrintLayerSplit(frames);
            MH3PrintHudSplit(frames);
            MH3PrintNodeViewSplit(frames);
        }

        uint32_t snapshots[3][6];
        TakeNativePacketSnapshotCounts(snapshots);
        const auto s = [&](unsigned row, unsigned component) { return snapshots[row][component] / frames; };
        std::printf("MH3 packet snapshots: %u records/frame (%.1f before the draw); captured/stored per frame: draw %u/%u material %u/%u lighting %u/%u post %u/%u textures %u/%u colors %u/%u\n",
                    s(0, 0), double(TakeNativeRecordsBeforeDraw()) / frames, s(1, 0), s(2, 0), s(1, 1), s(2, 1), s(1, 2), s(2, 2), s(1, 3), s(2, 3), s(1, 4), s(2, 4), s(1, 5), s(2, 5));
        uint32_t geometry, merged;
        TakeNativeGeometryBatchCounts(geometry, merged);
        std::printf("MH3 geometry batch: %.1f material geometry submissions/frame, %.1f joined the draw before them\n", double(geometry) / frames, double(merged) / frames);
        std::memset(splitCalls, 0, sizeof splitCalls);
        std::memset(splitFamilies, 0, sizeof splitFamilies);
        std::memset(splitScopes, 0, sizeof splitScopes);
        std::fflush(stdout);
        logic = draw = other = 0;
        frames = 0;
        reported = mark;
    }
} split;
#endif

// The draw section of the frame body: the scene, fonts and fade overlay, the
// fade timers (and the grabbed screen's restore), the HUD and the movie.
// Inline it runs in that order; with the draw thread (docs/pipelined-draw.md)
// it splits: GameSideSection on the game thread first, its records moved
// behind the scene's (BeginNativeFramePacketTail), then SceneSection on the
// draw thread.
void DrawSection(CpuContext* ctx, bool render) {
    auto call = [ctx](uint32_t fn) {
        InvokeIndirectCpu(fn, ctx);
        return ctx->gpr[3];
    };

#ifdef MH3_DEVELOPMENT
#define MH3_SPLIT(bucket) split.Lap(split.bucket)
#else
#define MH3_SPLIT(bucket)
#endif
    if (render) {
        DrawPages::DrawBegins(ctx->gpr[1]);
        call(DrawScene);
        {
            MH3_SPLIT_SCOPE(MH3_SPLIT_FONT);
            call(flfntFlush);
        }

        MH3_SPLIT_SCOPE(MH3_SPLIT_FADE);
        call(DrawFadeOverlay);
    }

    // Joined in DrawAll; this runs the calls of a frame that drew no scene.
    EffectStep::Join(ctx);
    MH3_SPLIT(draw);
    call(TickFades);

    if (render && Memory::Read8(kSystem + kScreenGrab)) {
        Memory::Write8(kSystem + kScreenGrab, 0);
        call(RestoreGrabbedScreen);
    }

    MH3_SPLIT(logic);

    if (render) {
        {
            MH3_SPLIT_SCOPE(MH3_SPLIT_HUD);
            call(DrawHud);
        }

        MH3_SPLIT_SCOPE(MH3_SPLIT_MOVIE);
        call(MovieDraw);
        DrawPages::DrawEnds();
    }
#undef MH3_SPLIT
}

// The draw thread's part: the 3D scene the frame draw left to it
// (frame_draw.cpp) and the effect calculation's join.
void SceneSection(CpuContext* ctx, bool render) {
    NativeFramePacketProducerChanged();
    ApplyDeferredModelInvalidate();

    if (render) {
        DrawPages::DrawBegins(ctx->gpr[1]);
        FrameDraw::RunDeferredScene(ctx);
        DrawPages::DrawEnds();
    }

    EffectStep::Join(ctx);
}

// The frame draw's handoff at the scene's place: the scene starts from the
// game's state there, and what the game side draws after it replays after it.
bool sceneHandedOff = false;

void SceneHandoff() {
    CopyNativeGameStateForDraw();
    BeginNativeFramePacketTail();
    sceneHandedOff = true;
}

// MH3_PROFILE=parallel-prepare (development, with the overlapped draw;
// docs/pipelined-draw.md): the single view's scene graph walk runs on the
// draw thread, from the handoff's state, while the game thread draws the
// layers after the scene; the game thread joins it before the scene's job.
bool PrepareOnDrawThread() {
#ifdef MH3_DEVELOPMENT
    static const bool enabled = ProfileOptions().Has("parallel-prepare");
    return enabled;
#else
    return false;
#endif
}

bool prepareLaunched = false;

void RunPrepareJob(CpuContext* cpu, void*) {
    SceneQueue::PrepareGraph(cpu);
}

void LaunchScenePrepare(CpuContext* cpu) {
    {
        // The camera's view first: the traverse and a layer's 3D primitive
        // both read its cached view matrix, which a read after logic makes.
        GuestFrame frame(cpu, 0x40);
        GuestCall{cpu}.Call<0x802BDCA4>({frame[0x10]}); // get_current_view_mtx
    }

    // Prepare's effect launch stays here (the draw thread's would not launch).
    EffectStep::Launch(*cpu);
    SetNativeFramePacketPrepareOnDraw(true);
    DrawThread::Thread().Submit(*cpu, RunPrepareJob, nullptr);
    prepareLaunched = true;
}

void JoinScenePrepare() {
    if (!std::exchange(prepareLaunched, false))
        return;

    DrawThread::Thread().Wait();
    SetNativeFramePacketPrepareOnDraw(false);
}

// The game thread's part, which reads gameplay state directly (the HUD layers,
// the font stack): the whole draw section but the 3D scene, in its order.
void GameSideSection(CpuContext* ctx, bool render, bool overlapped) {
    GuestFrame frame(ctx, 0x10);
    auto call = [ctx](uint32_t fn) { InvokeIndirectCpu(fn, ctx); };
    sceneHandedOff = false;

    if (render) {
        FrameDraw::SetSceneHandoff(SceneHandoff);
        FrameDraw::SetScenePrepareLaunch(overlapped && PrepareOnDrawThread() ? LaunchScenePrepare : nullptr);
        call(DrawScene);
        FrameDraw::SetScenePrepareLaunch(nullptr);
        FrameDraw::SetSceneHandoff(nullptr);
    }

    // No scene (none admitted, or the frame draw stopped early).
    if (!sceneHandedOff)
        SceneHandoff();

    if (render) {
        {
            MH3_SPLIT_SCOPE(MH3_SPLIT_FONT);
            call(flfntFlush);
        }

        MH3_SPLIT_SCOPE(MH3_SPLIT_FADE);
        call(DrawFadeOverlay);
    }

    call(TickFades);

    if (render && Memory::Read8(kSystem + kScreenGrab)) {
        Memory::Write8(kSystem + kScreenGrab, 0);
        call(RestoreGrabbedScreen);
    }

    if (render) {
        {
            MH3_SPLIT_SCOPE(MH3_SPLIT_HUD);
            call(DrawHud);
        }

        MH3_SPLIT_SCOPE(MH3_SPLIT_MOVIE);
        call(MovieDraw);
    }

    JoinScenePrepare();
    EndNativeFramePacketTail();
}

// MH3_PROFILE=draw-thread (development, docs/pipelined-draw.md): the scene
// runs on the draw thread while the game thread waits, after the game thread
// recorded the game-side part.
bool DrawOnThread() {
#ifdef MH3_DEVELOPMENT
    static const bool enabled = ProfileOptions().Has("draw-thread") && DrawThread::Thread().Available();
    return enabled;
#else
    return false;
#endif
}

struct DrawJob {
    bool render, publish;
};

void RunDrawJob(CpuContext* cpu, void* data) {
    const auto& job = *static_cast<DrawJob*>(data);
    SceneSection(cpu, job.render);

    // Overlapped, the draw thread ends the frame it drew.
    if (job.publish)
        EndNativeGraphicsFrame();
}

// The overlapped draw (docs/pipelined-draw.md stage 3), in every build where
// the draw thread can run: as draw-thread, but the game thread does not wait
// for the scene. At the handoff it waits for the previous scene (which
// published its packet), begins this frame's packet, records the game side,
// launches the effect calculation and hands the scene over; the next logic
// step runs beside it. The draw thread runs on core 3, the guest worker on
// core 0. MH3_PROFILE=draw-inline (development) draws on the game thread
// instead, draw-thread with the game thread waiting.
bool DrawOverlap() {
    static const bool enabled = [] {
#ifdef MH3_DEVELOPMENT
        if (ProfileOptions().Has("draw-inline") || ProfileOptions().Has("draw-thread"))
            return false;
#endif
        if (!DrawThread::Thread().Available())
            return false;

        DrawThread::Thread().SetCore(3);
        GuestWorker::Thread().SetCore(0);
        EffectStep::SetOverlap(true);
        SetNativeFrameOverlap(true);
        SetModelInvalidateDeferral(true);
        return true;
    }();
    return enabled;
}

DrawJob overlapJob{};
} // namespace

// Decided before the first frame begins (native_frame.cpp), so every frame's
// packet begins the same way.
bool MH3DrawOverlapMode() {
    return DrawOverlap();
}

namespace {

} // namespace

extern "C" void MH3_GameFrame(CpuContext* ctx) {
    const CpuContext saved = *ctx;
    ctx->gpr[1] = (saved.gpr[1] - 0x10) & ~0xFu;
    Memory::Write32(ctx->gpr[1], saved.gpr[1]);
    auto call = [ctx](uint32_t fn, std::initializer_list<uint32_t> ints = {}) {
        uint32_t r = 3;

        for (uint32_t v : ints)
            ctx->gpr[r++] = v;

        InvokeIndirectCpu(fn, ctx);
        return ctx->gpr[3];
    };

#ifdef MH3_DEVELOPMENT
    split.Begin();
#define MH3_SPLIT(bucket) split.Lap(split.bucket)
#else
#define MH3_SPLIT(bucket)
#endif
    DrawPages::LogicPhase(DrawPages::Phase::Input);
    call(WPADDisconnect, {2});
    call(WPADDisconnect, {3});
    call(PollControllers);
    Memory::Write8(kSystem + kFramePhase, 1);
    const uint8_t boot = Memory::Read8(kSystem + kBootState);

    // FrameSetup already latched admission before this logic step. Completing
    // a notification during logic does not reopen a skipped frame's payload.
    const bool render = NativeGraphicsFrameShouldRender();

    if (uint8_t(boot - 1) <= 1) {
        DrawPages::LogicPhase(DrawPages::Phase::Scene);
        call(flfntStackReset);
        call(UpdateScene);

        if (render)
            call(Setup2DProjection);

        call(MovieUpdate);
        call(NetworkResetCheck);
        Memory::Write8(kSystem + kFramePhase, 2);
#ifdef MH3_DEVELOPMENT
        static uint32_t logicSteps = 0;
        static const uint32_t probeAt = uint32_t(StepProbeOptions().Number("step", 0));
        static const std::vector<double> probeTicks = [] {
            std::vector<double> ticks;

            for (const std::string& word : StepProbeOptions().List()) {
                char* end = nullptr;
                const double tick = std::strtod(word.c_str(), &end);

                if (end == word.c_str() || *end)
                    std::fprintf(stderr, "MH3: MH3_STEP_PROBE: '%s' is not a tick count\n", word.c_str());
                else
                    ticks.push_back(tick);
            }

            return ticks;
        }();
        static size_t nextTickProbe = 0;
        ++logicSteps;
        const bool tickDue = nextTickProbe < probeTicks.size() && GameStep::Elapsed() >= probeTicks[nextTickProbe];

        if (tickDue)
            ++nextTickProbe;
        // all: probe the whole frame (scene update, tasks and drawing) instead
        // of the task pass alone.
        static const bool probeAll = StepProbeOptions().Has("all");

        if (logicSteps == probeAt || tickDue)
            ProbeStepDependence(logicSteps, [&] {
                if (probeAll) {
                    call(flfntStackReset);
                    call(UpdateScene);

                    if (render)
                        call(Setup2DProjection);

                    call(MovieUpdate);
                }

                call(RunTasks);

                if (!probeAll)
                    return;

                call(GrabScreen);
                call(ResetFade);
                call(TickSystemTimers);

                if (render) {
                    call(DrawScene);
                    call(flfntFlush);
                    call(DrawFadeOverlay);
                }

                call(TickFades);

                if (render) {
                    call(DrawHud);
                    call(MovieDraw);
                }

                call(PauseHook);
                call(OverlayHook);
            });
#endif
        GameTasks::SkipStrapScreen();
        StepVillageBoot(ctx);
        Cheats::Step(ctx);

        // The task pass's Effect::Calc calls run on the guest worker during
        // the scene's traverse and opaque draws (EffectStep::Launch in the
        // scene queue's DrawScene).
        EffectStep::Defer(render);
        DrawPages::LogicPhase(DrawPages::Phase::Tasks);
        call(RunTasks);
        EffectStep::Collected();
        DrawPages::LogicPhase(DrawPages::Phase::Timers);
        call(GrabScreen);
        call(ResetFade);
        call(TickSystemTimers);
        Memory::Write8(kSystem + kFramePhase, 3);
        MH3_SPLIT(logic);

        if (DrawOverlap()) {
            DrawThread::Thread().Wait();
            ApplyDeferredDrawCacheRetirements();
            BeginOverlappedNativeGraphicsFrame();
            GameSideSection(ctx, render, true);
            EffectStep::Launch(*ctx);
            overlapJob = {render, true};
            NativeFrameSubmittedToDraw();
            DrawThread::Thread().Submit(*ctx, RunDrawJob, &overlapJob);
        } else if (DrawOnThread()) {
            // Stage 1 of docs/pipelined-draw.md: the same section on the draw
            // thread, the game thread waiting.
            GameSideSection(ctx, render, false);
            DrawJob job{render, false};
            DrawThread::Thread().Submit(*ctx, RunDrawJob, &job);
            DrawThread::Thread().Wait(true);
        } else {
            DrawSection(ctx, render);
        }

        MH3_SPLIT(draw);
        call(PauseHook);
        call(OverlayHook);
    } else if (boot == 0) {
        call(StartBootTask, {BootTask, 0});
        Memory::Write8(kSystem + kBootState, 1);
    }

    Memory::Write8(kSystem + kFramePhase, 4);
#ifdef MH3_DEVELOPMENT
    split.End();
#endif
#undef MH3_SPLIT
    *ctx = saved;
    ctx->gpr[3] = 1;
}
REGISTER_NATIVE_FUNCTION_AS(0x80046D98, MH3_GameFrame, "GameFrame");
