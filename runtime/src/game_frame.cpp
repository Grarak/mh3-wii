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
#include "audio/ax_dsp.h"
#include "game_step.h"
#include "game_tasks.h"
#include "memory.h"
#include "village_boot.h"
#include <initializer_list>
#ifdef MH3_DEVELOPMENT
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#endif

namespace {

constexpr uint32_t kSystem = 0x8065AA60; // game system flags
enum : uint32_t {
    kBootState = 0x22,     // u8 0x8065AA82: 0 boot, 1-2 running
    kFramePhase = 0x23,    // u8 0x8065AA83
    kScreenGrab = 0x872,   // u8 0x8065B2D2: copy the grabbed screen back to the EFB
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
// MH3_STEP_PROBE=N: before logic step N, find state that counts steps rather
// than time. From one RAM snapshot the task pass runs once with a 1/30 s step,
// then (restored) twice with 1/60 s steps; RAM is restored before the real
// step. Every changed word goes to mh3-step-probe-N.txt with both deltas:
// time-based state changes about equally, step-based state about twice as much
// over the two half steps. Host-side effects of the extra passes (audio, GX,
// DVD requests) are not undone, so probe in a steady scene.
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
    uint32_t Word(size_t offset) const {
        return uint32_t(bytes[offset]) << 24 | bytes[offset + 1] << 16 | bytes[offset + 2] << 8 | bytes[offset + 3];
    }
    static uint32_t Address(size_t offset) {
        return offset < Memory::kMem1Size ? Memory::kMem1CachedBase + uint32_t(offset)
                                          : Memory::kMem2CachedBase + uint32_t(offset - Memory::kMem1Size);
    }
};

template <typename RunTasks> void ProbeStepDependence(uint32_t step, RunTasks runTasks) {
    const GameStep::State clock = GameStep::Save();
    AxDspHle::JoinMixWorker(); // its writes to guest RAM must not race the restores
    Ram before, whole, halves;
    before.Save();
    GameStep::Begin(1.0);
    runTasks();
    whole.Save();
    before.Restore();
    GameStep::Restore(clock);
    GameStep::Begin(0.5);
    runTasks();
    GameStep::Begin(0.5);
    runTasks();
    halves.Save();
    before.Restore();
    GameStep::Restore(clock);

    const std::string name = "mh3-step-probe-" + std::to_string(step) + ".txt";
    FILE* out = std::fopen(name.c_str(), "w");
    if (!out) return;
    std::fprintf(out, "# address before whole(1/30) halves(2x1/60) as u32 and f32\n");
    size_t changed = 0;
    for (size_t i = 0; i + 4 <= before.bytes.size(); i += 4) {
        const uint32_t a = before.Word(i), b = whole.Word(i), c = halves.Word(i);
        if (a == b && a == c) continue;
        float fa, fb, fc;
        std::memcpy(&fa, &a, 4); std::memcpy(&fb, &b, 4); std::memcpy(&fc, &c, 4);
        std::fprintf(out, "%08x %08x %08x %08x %g %g %g\n", Ram::Address(i), a, b, c, fa, fb, fc);
        ++changed;
    }
    std::fclose(out);
    std::printf("MH3 step probe: step %u, %zu words changed -> %s\n", step, changed, name.c_str());
    std::fflush(stdout);
}
#endif

} // namespace

extern "C" void MH3_GameFrame(CpuContext* ctx) {
    const CpuContext saved = *ctx;
    ctx->gpr[1] = (saved.gpr[1] - 0x10) & ~0xFu;
    Memory::Write32(ctx->gpr[1], saved.gpr[1]);
    auto call = [ctx](uint32_t fn, std::initializer_list<uint32_t> ints = {}) {
        uint32_t r = 3;
        for (uint32_t v : ints) ctx->gpr[r++] = v;
        InvokeIndirectCpu(fn, ctx);
        return ctx->gpr[3];
    };

    call(WPADDisconnect, {2});
    call(WPADDisconnect, {3});
    call(PollControllers);
    Memory::Write8(kSystem + kFramePhase, 1);
    const uint8_t boot = Memory::Read8(kSystem + kBootState);
    if (uint8_t(boot - 1) <= 1) {
        call(flfntStackReset);
        call(UpdateScene);
        call(Setup2DProjection);
        call(MovieUpdate);
        call(NetworkResetCheck);
        Memory::Write8(kSystem + kFramePhase, 2);
#ifdef MH3_DEVELOPMENT
        static uint32_t logicSteps = 0;
        static const uint32_t probeAt = [] {
            const char* v = std::getenv("MH3_STEP_PROBE");
            return v ? uint32_t(std::strtoul(v, nullptr, 10)) : 0u;
        }();
        // MH3_STEP_PROBE_TICK=N[,N...] probes the first step at or after each N
        // ticks of game time instead (loading runs many short steps at
        // variable step).
        static const std::vector<double> probeTicks = [] {
            std::vector<double> ticks;
            for (const char* v = std::getenv("MH3_STEP_PROBE_TICK"); v && *v;) {
                char* end = nullptr;
                ticks.push_back(std::strtod(v, &end));
                if (end == v) break;
                v = *end == ',' ? end + 1 : end;
            }
            return ticks;
        }();
        static size_t nextTickProbe = 0;
        ++logicSteps;
        const bool tickDue = nextTickProbe < probeTicks.size() && GameStep::Elapsed() >= probeTicks[nextTickProbe];
        if (tickDue) ++nextTickProbe;
        // MH3_STEP_PROBE_ALL: probe the whole frame (scene update, tasks and
        // drawing) instead of the task pass alone.
        static const bool probeAll = std::getenv("MH3_STEP_PROBE_ALL") != nullptr;
        if (logicSteps == probeAt || tickDue) ProbeStepDependence(logicSteps, [&] {
            if (probeAll) { call(flfntStackReset); call(UpdateScene); call(Setup2DProjection); call(MovieUpdate); }
            call(RunTasks);
            if (!probeAll) return;
            call(GrabScreen); call(ResetFade); call(TickSystemTimers); call(DrawScene); call(flfntFlush);
            call(DrawFadeOverlay); call(TickFades); call(DrawHud); call(MovieDraw); call(PauseHook); call(OverlayHook);
        });
#endif
        GameTasks::SkipStrapScreen();
#ifdef MH3_DEVELOPMENT
        StepVillageBoot(ctx);
#endif
        call(RunTasks);
        call(GrabScreen);
        call(ResetFade);
        call(TickSystemTimers);
        Memory::Write8(kSystem + kFramePhase, 3);
        call(DrawScene);
        call(flfntFlush);
        call(DrawFadeOverlay);
        call(TickFades);
        if (Memory::Read8(kSystem + kScreenGrab)) {
            Memory::Write8(kSystem + kScreenGrab, 0);
            call(RestoreGrabbedScreen);
        }
        call(DrawHud);
        call(MovieDraw);
        call(PauseHook);
        call(OverlayHook);
    } else if (boot == 0) {
        call(StartBootTask, {BootTask, 0});
        Memory::Write8(kSystem + kBootState, 1);
    }
    Memory::Write8(kSystem + kFramePhase, 4);
    *ctx = saved;
    ctx->gpr[3] = 1;
}
REGISTER_NATIVE_FUNCTION_AS(0x80046D98, MH3_GameFrame, "GameFrame");
