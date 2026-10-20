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
#include "monster_control.h"
#include "fatal.h"
#include "frame_split.h"
#include "game_step.h"
#include "game_tasks.h"
#include "guest_call.h"
#include "guest_worker.h"
#include "memory.h"
#include "native_frame_packet.h"
#include "native_graphics.h"
#include "native_owner.h"
#include "port_check.h"
#include "village_boot.h"
#include <initializer_list>
#include <stdexcept>
#include <utility>
#ifdef MH3_DEVELOPMENT
#include "dev_options.h"
#ifndef __vita__
#include "debug_port.h"
#endif
#ifdef __EMSCRIPTEN__
#include <emscripten/heap.h>
#include <unistd.h>
#endif
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
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
void ApplyDeferredHeapFrees(CpuContext* cpu);
#ifdef MH3_DEVELOPMENT
extern uintptr_t mh3GameStackProbe;                           // model_catalog.cpp
void MH3TakeCatalogDrawWaits(uint64_t waits[2], uint64_t ns[2]); // model_catalog.cpp
extern "C" uint64_t MH3TakeEffectParticlesDrawn();                // native_effect_draw.cpp
extern "C" void MH3TakeEffectQuadCounts(uint32_t out[5]);         // native_effect_draw.cpp
#endif

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
    DiscErrorPause = 0x80435B54,    // pauses the game while the disc reports an error
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
// MH3_STEP_PROBE=T[,T...][,step=N][,settle][,all]: find state
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

// Every draw thread handoff wait since the start, in microseconds.
uint64_t handoffTotalUs = 0;
#endif
} // namespace
#ifdef MH3_DEVELOPMENT
uint8_t mh3SplitDepth[MH3_SPLIT_FAMILIES];
uint32_t mh3SplitFrame;

// Set at the first frame, after static initialization.
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

    // The totals at the frame's start, for the long frame report.
    double logicStart = 0, drawStart = 0, otherStart = 0, familiesStart[MH3_SPLIT_FAMILIES]{};
    uint64_t pacingStartNs = 0, handoffStartUs = 0;
    static double Ms(Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); }
    void Begin() {
        if (!mh3SplitFrame)
            mh3SplitDetail = ProfileOptions().Has("split");

        ++mh3SplitFrame;
        start = mark = Clock::now();
        logicStart = logic;
        drawStart = draw;
        otherStart = other;
        std::memcpy(familiesStart, splitFamilies, sizeof familiesStart);
        pacingStartNs = NativePacingWaitTotalNs();
        handoffStartUs = handoffTotalUs;
    }

    // A game frame over 40 ms (a stall the per-second averages hide): its own
    // logic, draw and other time and the split families that took the most.
    void ReportLong() const {
        const double wall = Ms(mark - start);

        if (wall < 40)
            return;

        static constexpr const char* names[MH3_SPLIT_FAMILIES] = {"traverse", "model", "effect", "node callback", "font", "fade", "hud", "movie", "material prep", "shape prep",
                                                                  "model record", "scene setup", "scene finish", "post", "scene", "buckets", "effect calc", "g3d world",
                                                                  "g3d material", "g3d vertex", "g3d view", "g3d update", "g3d other", "layers", "ui"};
        unsigned order[MH3_SPLIT_FAMILIES];

        for (unsigned i = 0; i < MH3_SPLIT_FAMILIES; ++i)
            order[i] = i;

        std::partial_sort(order, order + 4, order + MH3_SPLIT_FAMILIES,
                          [this](unsigned a, unsigned b) { return splitFamilies[a] - familiesStart[a] > splitFamilies[b] - familiesStart[b]; });
        std::printf("MH3 long frame %u: %.1f ms (logic %.1f draw %.1f other %.1f; pacing wait %.1f, draw thread handoff %.1f); most: %s %.1f, %s %.1f, %s %.1f, %s %.1f ms\n",
                    mh3SplitFrame, wall, logic - logicStart, draw - drawStart, other - otherStart, (NativePacingWaitTotalNs() - pacingStartNs) / 1e6,
                    (handoffTotalUs - handoffStartUs) / 1e3, names[order[0]], splitFamilies[order[0]] - familiesStart[order[0]], names[order[1]],
                    splitFamilies[order[1]] - familiesStart[order[1]], names[order[2]], splitFamilies[order[2]] - familiesStart[order[2]], names[order[3]],
                    splitFamilies[order[3]] - familiesStart[order[3]]);
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

        if (enabled)
            ReportLong();

        if (!enabled || mark - reported < std::chrono::seconds(1))
            return;

        std::printf("MH3 frame split: logic %.2f draw %.2f other %.2f ms/frame (%u frames); calls logic/draw: world %u/%u anim %u/%u effect %u/%u view %u/%u\n", logic / frames, draw / frames,
                    other / frames, frames, splitCalls[0][2], splitCalls[0][3], splitCalls[1][2], splitCalls[1][3], splitCalls[2][2], splitCalls[2][3], splitCalls[3][2], splitCalls[3][3]);
#ifdef __EMSCRIPTEN__
        // The web build's heap is fixed (-sINITIAL_MEMORY): how much of it the
        // allocator has claimed, which it never gives back.
        std::printf("MH3 web heap: %.0f of %.0f MiB claimed\n", double(reinterpret_cast<uintptr_t>(sbrk(0))) / 1048576, double(emscripten_get_heap_size()) / 1048576);
#endif
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
        std::printf("MH3 effect split: calc %.2f on the game thread, %.2f on the guest worker; draw %.2f ms/frame (game-thread calc scopes/frame %u, particles drawn/frame %.1f)\n",
                    f(MH3_SPLIT_EFFECT_CALC), worker.jobNs / 1e6 / frames, f(MH3_SPLIT_EFFECT), n(MH3_SPLIT_EFFECT_CALC), double(MH3TakeEffectParticlesDrawn()) / frames);
        std::printf("MH3 guest worker: %.2f jobs/frame, %.2f ms/frame of jobs, game waited %.2f ms/frame (%llu waits)\n", double(worker.jobs) / frames, worker.jobNs / 1e6 / frames,
                    worker.waitNs / 1e6 / frames, static_cast<unsigned long long>(worker.waits));

        // The draw thread's scenes, and the game thread's waits for them at the handoff.
        if (const auto draw = DrawThread::Thread().TakeStatistics(); draw.jobs) {
            uint64_t catalogWaits[2], catalogNs[2];
            MH3TakeCatalogDrawWaits(catalogWaits, catalogNs);
            std::printf("MH3 draw thread: %.2f scenes/frame, %.2f ms/frame of scenes, game waited %.2f ms/frame (%llu waits, %u of them effect retirements; catalog writes: "
                        "game thread %llu waits %.2f ms/frame, other threads %llu waits %.2f ms/frame)\n",
                        double(draw.jobs) / frames, draw.jobNs / 1e6 / frames, draw.waitNs / 1e6 / frames, static_cast<unsigned long long>(draw.waits), EffectStep::TakeRetireDrawWaits(),
                        static_cast<unsigned long long>(catalogWaits[1]), catalogNs[1] / 1e6 / frames, static_cast<unsigned long long>(catalogWaits[0]), catalogNs[0] / 1e6 / frames);
        }

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
        uint32_t geometry, merged, unmerged[6];
        TakeNativeGeometryBatchCounts(geometry, merged, unmerged);
        const auto per = [&](uint32_t count) { return double(count) / frames; };
        std::printf("MH3 geometry batch: %.1f material geometry submissions/frame, %.1f joined the draw before them; apart after another kind %.1f, "
                    "differing draw %.1f material %.1f lighting %.1f textures %.1f colors %.1f\n",
                    per(geometry), per(merged), per(unmerged[0]), per(unmerged[1]), per(unmerged[2]), per(unmerged[3]), per(unmerged[4]), per(unmerged[5]));
        uint32_t packets, runs, runRecords, refused;
        TakeNativeQuadRunCounts(packets, runs, runRecords, refused);

        if (packets)
            std::printf("MH3 quad runs: %.1f per drawn packet drew %.1f quad records as one; %.1f refused\n", double(runs) / packets, double(runRecords) / packets,
                        double(refused) / packets);

        uint32_t quads[5];
        MH3TakeEffectQuadCounts(quads);
        std::printf("MH3 effect quads: compact %.1f (strips %.1f points %.1f lines %.1f), built on the CPU %.1f per frame\n", per(quads[0]), per(quads[1]), per(quads[2]),
                    per(quads[3]), per(quads[4]));
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

// The draw section of the frame body is the scene, fonts and fade overlay,
// the fade timers (and the grabbed screen's restore), the HUD and the movie.
// With the draw thread (docs/optimizations/threading/pipelined-draw.md) it splits: GameSideSection on
// the game thread first, its records moved behind the scene's
// (BeginNativeFramePacketTail), then SceneSection on the draw thread.

// The draw thread's part: the 3D scene the frame draw left to it
// (frame_draw.cpp) and the effect calculation's join.
void SceneSection(CpuContext* ctx, bool render) {
    NativeFramePacketProducerChanged();
    ApplyDeferredModelInvalidate();

    if (render)
        FrameDraw::RunDeferredScene(ctx);

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

// With the overlapped draw (docs/optimizations/threading/pipelined-draw.md), the single view's scene
// graph walk runs on the draw thread, from the handoff's state, while the
// game thread draws the layers after the scene; the game thread joins it
// before the scene's job.
bool prepareLaunched = false;

#ifdef MH3_DEVELOPMENT
// The walk's job on the draw thread, the game thread's part from the launch
// to the join, and the join's wait (MH3 scene prepare line, once a second;
// a few clock reads a frame, so every development build).
struct PrepareTiming {
    MH3SplitClock::time_point launched{};
    std::atomic<int64_t> jobUs{0}, lastLatencyUs{0};
    int64_t latencyUs[2] = {}, launches[2] = {}; // by whether the launch woke the parked draw thread
    bool woke = false;
    int64_t gameUs = 0, waitUs = 0, handoffUs = 0, frames = 0;
    MH3SplitClock::time_point printed{};
} prepareTiming;
#endif

void RunPrepareJob(CpuContext* cpu, void*) {
#ifdef MH3_DEVELOPMENT
    const auto start = MH3SplitClock::now();
    prepareTiming.lastLatencyUs.store(std::chrono::duration_cast<std::chrono::microseconds>(start - prepareTiming.launched).count(), std::memory_order_relaxed);
#endif
    SceneQueue::PrepareGraph(cpu);
#ifdef MH3_DEVELOPMENT
    prepareTiming.jobUs.fetch_add(std::chrono::duration_cast<std::chrono::microseconds>(MH3SplitClock::now() - start).count(), std::memory_order_relaxed);
#endif
}

void LaunchScenePrepare(CpuContext* cpu) {
    {
        // The camera's view first: the traverse and a layer's 3D primitive
        // both read its cached view matrix, which a read after logic makes.
        GuestFrame frame(cpu, 0x40);
        GuestCall{cpu}.Call<0x802BDCA4>({frame[0x10]}); // get_current_view_mtx
        EffectStep::HoldView(frame[0x10]);
    }

    // Prepare's effect launch stays here (the draw thread's would not launch).
    EffectStep::Launch(*cpu);
    SetNativeFramePacketPrepareOnDraw(true);
#ifdef MH3_DEVELOPMENT
    prepareTiming.launched = MH3SplitClock::now();
#endif
    DrawThread::Thread().Submit(*cpu, RunPrepareJob, nullptr);
#ifdef MH3_DEVELOPMENT
    prepareTiming.woke = DrawThread::Thread().LastSubmitWoke();
#endif
    prepareLaunched = true;
}

void JoinScenePrepare() {
    if (!std::exchange(prepareLaunched, false))
        return;

#ifdef MH3_DEVELOPMENT
    const auto joined = MH3SplitClock::now();
#endif
    DrawThread::Thread().Wait();
    SetNativeFramePacketPrepareOnDraw(false);
#ifdef MH3_DEVELOPMENT
    {
        using std::chrono::duration_cast;
        using std::chrono::microseconds;
        auto& t = prepareTiming;
        const auto now = MH3SplitClock::now();
        t.latencyUs[t.woke] += t.lastLatencyUs.load(std::memory_order_relaxed);
        ++t.launches[t.woke];
        t.gameUs += duration_cast<microseconds>(joined - t.launched).count();
        t.waitUs += duration_cast<microseconds>(now - joined).count();
        ++t.frames;

        if (now - t.printed >= std::chrono::seconds(1)) {
            const double n = double(t.frames);
            std::fprintf(stderr,
                         "MH3 scene prepare: walk %.2f ms on the draw thread, starting %.2f ms after its launch (%.2f ms in %lld launches waking it, %.2f ms in %lld with it spinning); "
                         "game side to the join %.2f ms, join wait %.2f ms, handoff wait %.2f ms per frame (%lld frames)\n",
                         t.jobUs.exchange(0, std::memory_order_relaxed) / 1000.0 / n, (t.latencyUs[0] + t.latencyUs[1]) / 1000.0 / n,
                         t.launches[1] ? t.latencyUs[1] / 1000.0 / double(t.launches[1]) : 0.0, (long long)t.launches[1],
                         t.launches[0] ? t.latencyUs[0] / 1000.0 / double(t.launches[0]) : 0.0, (long long)t.launches[0], t.gameUs / 1000.0 / n, t.waitUs / 1000.0 / n,
                         t.handoffUs / 1000.0 / n, (long long)t.frames);
            t.gameUs = t.waitUs = t.handoffUs = t.frames = 0;
            t.latencyUs[0] = t.latencyUs[1] = t.launches[0] = t.launches[1] = 0;
            t.printed = now;
        }
    }
#endif
}

// The game thread's part, which reads gameplay state directly (the HUD layers,
// the font stack): the whole draw section but the 3D scene, in its order.
void GameSideSection(CpuContext* ctx, bool render) {
    GuestFrame frame(ctx, 0x10);
    auto call = [ctx](uint32_t fn) { InvokeIndirectCpu(fn, ctx); };
    sceneHandedOff = false;

    if (render) {
        FrameDraw::SetSceneHandoff(SceneHandoff);

        // Port checks compare and restore whole RAM around a call: nothing
        // may run beside the game thread, so the walk stays on it.
        FrameDraw::SetScenePrepareLaunch(PortCheck::Set() ? nullptr : LaunchScenePrepare);
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

// The draw thread's job: the scene of the frame the game thread handed over
// (its render flag), then the frame's end, which publishes its packet.
void RunDrawJob(CpuContext* cpu, void* data) {
    SceneSection(cpu, *static_cast<bool*>(data));
    EndNativeGraphicsFrame();
}

bool overlapRender = false;
} // namespace

// The overlapped draw (docs/optimizations/threading/pipelined-draw.md stage 3), in every build: at the
// handoff the game thread waits for the previous scene (which published its
// packet), begins this frame's packet, records the game side, launches the
// effect calculation and hands the scene over to the draw thread; the next
// logic step runs beside it. The draw thread runs on core 1, the guest worker
// on core 3. Started before the first frame begins (native_frame.cpp), so
// every frame's packet begins the same way.
void MH3StartDrawOverlap() {
    static bool started = false;

    if (std::exchange(started, true))
        return;

    if (!DrawThread::Thread().Available())
        MH3_THROW(std::runtime_error("Draw thread without a guest stack or work area"));

    // The draw thread spins 1 ms after a job, which catches the next of a
    // burst (the scene draw shortly after the walk), then parks: spinning
    // through all its idle time kept its core busy for nothing and starved
    // other threads (a parked wake costs 0.1-1.9 ms on the devkit).
    DrawThread::Thread().SetIdleSpin(1000);
    EffectStep::SetOverlap(true);
    SetNativeFrameOverlap(true);
    SetModelInvalidateDeferral(true);
}

extern "C" void MH3_GameFrame(CpuContext* ctx) {
    const CpuContext saved = *ctx;
    UpdateNativeCameraAspect();
#ifdef MH3_DEVELOPMENT
    mh3GameStackProbe = uintptr_t(&saved);
#endif
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
    call(WPADDisconnect, {2});
    call(WPADDisconnect, {3});
    call(PollControllers);
    Memory::Write8(kSystem + kFramePhase, 1);
    const uint8_t boot = Memory::Read8(kSystem + kBootState);

    // FrameSetup already latched admission before this logic step. Completing
    // a notification during logic does not reopen a skipped frame's payload.
    const bool render = NativeGraphicsFrameShouldRender();

    if (uint8_t(boot - 1) <= 1) {
        call(flfntStackReset);
        call(UpdateScene);

        if (render)
            call(Setup2DProjection);

        call(DiscErrorPause);
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

                    call(DiscErrorPause);
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
        MonsterControl::Step(ctx);
#if defined(MH3_DEVELOPMENT) && !defined(__vita__)
        DebugPort::Step(ctx);
#endif

        // The task pass's Effect::Calc calls run on the guest worker during
        // the scene's traverse and opaque draws (EffectStep::Launch in the
        // scene queue's DrawScene).
        EffectStep::Defer(render);
        call(RunTasks);
        EffectStep::Collected();

        // The calculation starts here when the previous frame's draw, which
        // draws the effects it moves, is done; the next task pass joins it,
        // and launching at the handoff leaves little time for it. The rest of
        // the logic step and the frame's draw section do not use the effects
        // but for the scene, which joins first.
        if (!DrawThread::Thread().Pending() && !GuestWorker::Pending() && !PortCheck::Set())
            EffectStep::Launch(*ctx);

        call(GrabScreen);
        call(ResetFade);
        call(TickSystemTimers);
        Memory::Write8(kSystem + kFramePhase, 3);
        MH3_SPLIT(logic);

        // The overlapped draw started before the first frame began
        // (MH3StartDrawOverlap).
#ifdef MH3_DEVELOPMENT
        const auto handoff = MH3SplitClock::now();
#endif
        DrawThread::Thread().Wait();
#ifdef MH3_DEVELOPMENT
        {
            const auto waited = std::chrono::duration_cast<std::chrono::microseconds>(MH3SplitClock::now() - handoff).count();
            prepareTiming.handoffUs += waited;
            handoffTotalUs += uint64_t(waited);
        }
#endif
        ApplyDeferredDrawCacheRetirements();
        ApplyDeferredHeapFrees(ctx);
        BeginOverlappedNativeGraphicsFrame();
        GameSideSection(ctx, render);
        EffectStep::Launch(*ctx);
        overlapRender = render;

        // A scene that walks the graph on the draw thread (the armor
        // preview's, the two players'): the next logic step adds and removes
        // the models it walks (a preview's equipment change left a node
        // without its model, and the draw read through it), so it waits for
        // the draw. So it does under port checks (see the walk above).
        const bool serial = render && FrameDraw::DeferredSceneWalksGraph();
        NativeFrameSubmittedToDraw();
        DrawThread::Thread().Submit(*ctx, RunDrawJob, &overlapRender);

        if (serial || PortCheck::Set())
            DrawThread::Thread().Wait();

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
