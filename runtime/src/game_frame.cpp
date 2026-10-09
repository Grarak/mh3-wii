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
#include "memory.h"
#include <initializer_list>

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
