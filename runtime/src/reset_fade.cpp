// Reset fade, RMHP08. C reconstruction of the per-frame soft-reset handler
// (0x800D2230), decompiled with Ghidra (scripts/dol2elf.py -> ELF import).
//
// HOME (pad bit 0x8000, or classic-controller switch bit 2) on either player
// requests a reset. The state machine then fades the screen out, waits a
// second, fades back in, and returns to the title through the game's reset
// hooks. Its fade steps are counted in 60 Hz retraces: 1 per frame at a
// 1-retrace frame budget (unless a movie frame was drawn), 2 otherwise, so
// the fade takes the same time at 30 and 60 fps.
#include "abi_bridge.h"
#include "memory.h"
#include <initializer_list>

namespace {

// Game flags (0x8065AA60 block).
constexpr uint32_t kResetting = 0x8065AA65;      // u8: reset in progress
constexpr uint32_t kResetDisabled = 0x8065AA91;  // u8: 1 = ignore HOME
constexpr uint32_t kQuestActive = 0x8065B2C5;    // u8
constexpr uint32_t kResetRequested = 0x8065B2CC; // u8
constexpr uint32_t kResetConfirmed = 0x8065B2CD; // u8
constexpr uint32_t kResetPhase = 0x8065B2CF;     // u8: 0 idle, 1 running, 2 waiting on hook
constexpr uint32_t kSaving = 0x8065B2D0;         // u8
constexpr uint32_t kSkipFlag = 0x8065B2D2;       // u8
constexpr uint32_t kResetActive = 0x8065B2D3;    // u8
constexpr uint32_t kOnResetHook = 0x8065B32C;    // void (*)()
constexpr uint32_t kResetReadyHook = 0x8065B330; // int (*)(): 1 = not ready yet
constexpr uint32_t kOnlineMode = 0x8065AA84;     // u8 (0x800CF26C, inlined)
// Controller state (0x8065B7D0 block).
constexpr uint32_t kPad0Buttons = 0x8065B89E; // u16
constexpr uint32_t kPad1Channel = 0x8065BB50; // s32: -1 = no second player
constexpr uint32_t kPad1Buttons = 0x8065BBEE; // u16
// Fade state (0x806978E8).
constexpr uint32_t kFade = 0x806978E8;
enum : uint32_t {
    kState = 0x00,  // u8
    kClear1 = 0x01, // u8 (0x800D26C4 clears +1 and +6)
    kHold = 0x04,   // s16: frames (in retraces) to hold black
    kClear6 = 0x06, // u16
    kLevel = 0x20,  // s32: 0..255 fade alpha
    kStep = 0x24,   // s32: alpha per retrace
    kActive = 0x28, // u8
};
constexpr uint32_t kSaveBusy = 0x806970EC;        // u32 (0x800CEE20, inlined: == 0)
constexpr uint32_t kEventBusy = 0x806977A0;       // u8
constexpr uint32_t kDemoState = 0x806D4FA0;       // u8 (event_demo_ck, inlined: == 4)
constexpr uint32_t kNetworkBusy = 0x806D6D4A;     // u8 (0x804360A0, inlined)
constexpr uint32_t kMovieFrameDrawn = 0x806D6D24; // u8 (0x80434688, inlined)

enum : int32_t {
    kRetracesPerFrame = -0x4684
}; // r13 (0x8003F588, inlined)

enum : uint32_t {
    get_rcSwitch_on = 0x8004392C,
    get_ControlType = 0x80043BAC,
    ResetScene = 0x80044AB8,
    FadeHold = 0x800D2154,
    FadeOutDone = 0x800D26DC,
    ResetFinished = 0x800D2810,
    ResetTitle = 0x800E4B30,
    ResetAudio = 0x800E874C,
    ResetOnline = 0x803A91F0,
};

// 0x800D26C4, inlined.
void ClearFadeCounters() {
    Memory::Write8(kFade + kClear1, 0);
    Memory::Write16(kFade + kClear6, 0);
}

} // namespace

extern "C" void MH3_ResetFade(CpuContext* ctx) {
    const CpuContext saved = *ctx;
    ctx->gpr[1] = (saved.gpr[1] - 0x20) & ~0xFu;
    Memory::Write32(ctx->gpr[1], saved.gpr[1]);
    auto call = [ctx](uint32_t fn, std::initializer_list<uint32_t> ints = {}) {
        uint32_t r = 3;
        for (uint32_t v : ints)
            ctx->gpr[r++] = v;
        InvokeIndirectCpu(fn, ctx);
        return ctx->gpr[3];
    };
    auto homePressed = [&](uint32_t pad, uint32_t buttons) {
        if (Memory::Read16(buttons) & 0x8000)
            return true;
        return (call(get_ControlType, {pad}) & 0xFF) == 2 && (call(get_rcSwitch_on, {pad}) & 2);
    };
    // A single step per frame only at a 1-retrace budget without a movie frame.
    auto oneRetrace = [&] {
        return int32_t(Memory::Read32(saved.gpr[13] + uint32_t(kRetracesPerFrame))) == 1 && Memory::Read8(kMovieFrameDrawn) == 0;
    };
    auto level = [] {
        return int32_t(Memory::Read32(kFade + kLevel));
    };
    auto step = [] {
        return int32_t(Memory::Read32(kFade + kStep));
    };
    auto nextState = [] {
        Memory::Write8(kFade + kState, uint8_t(Memory::Read8(kFade + kState) + 1));
    };

    if (Memory::Read8(kResetDisabled) != 1) {
        if (homePressed(0, kPad0Buttons) && !Memory::Read8(kResetRequested))
            Memory::Write8(kResetRequested, 1);
        if (int32_t(Memory::Read32(kPad1Channel)) != -1 && homePressed(1, kPad1Buttons) && !Memory::Read8(kResetRequested))
            Memory::Write8(kResetRequested, 1);

        switch (Memory::Read8(kFade + kState)) {
        case 0:
            if (!Memory::Read8(kResetRequested))
                break;
            if (Memory::Read8(kNetworkBusy) == 1) {
                Memory::Write8(kResetRequested, 0);
            } else if (Memory::Read32(kSaveBusy) == 0 || Memory::Read8(kEventBusy) == 1 || Memory::Read8(kSaving) || Memory::Read8(kQuestActive) || Memory::Read8(kDemoState) == 4) {
                nextState();
                Memory::Write32(kFade + kLevel, 0);
                Memory::Write8(kFade + kActive, 1);
            } else if (Memory::Read8(kResetConfirmed)) {
                Memory::Write8(kResetRequested, 0);
                Memory::Write8(kResetting, 1);
                Memory::Write8(kResetActive, 1);
                if (Memory::Read8(kOnlineMode) != 1)
                    Memory::Write8(kSkipFlag, 1);
                call(Memory::Read32(kOnResetHook));
                ClearFadeCounters();
                Memory::Write8(kFade + kState, 10);
                call(ResetScene);
            }
            break;
        case 1: // fade out
            if (level() == 0xFF) {
                nextState();
                Memory::Write16(kFade + kHold, 60);
            } else {
                int32_t v = level() + (oneRetrace() ? step() : step() * 2);
                Memory::Write32(kFade + kLevel, uint32_t(v > 0xFF ? 0xFF : v));
            }
            break;
        case 2: { // hold black
            const int16_t hold = int16_t(Memory::Read16(kFade + kHold));
            if (hold <= 0)
                nextState();
            else
                Memory::Write16(kFade + kHold, uint16_t(hold - (oneRetrace() ? 1 : 2)));
            break;
        }
        case 3: // fade in
            if (level() == 0) {
                Memory::Write8(kFade + kState, 0);
                Memory::Write8(kFade + kActive, 0);
                Memory::Write8(kResetRequested, 0);
            } else {
                int32_t v = level() - (oneRetrace() ? step() : step() * 2);
                Memory::Write32(kFade + kLevel, uint32_t(v < step() ? 0 : v));
            }
            break;
        case 10:
            Memory::Write8(kResetPhase, 1);
            if (call(FadeOutDone) == 1)
                nextState();
            break;
        case 11:
            call(FadeHold, {0});
            call(ResetTitle);
            if (call(Memory::Read32(kResetReadyHook)) == 1) {
                Memory::Write8(kResetPhase, 2);
            } else {
                ClearFadeCounters();
                Memory::Write8(kResetPhase, 1);
                call(ResetAudio, {0});
                nextState();
            }
            break;
        case 12:
            if (call(ResetFinished) == 1) {
                Memory::Write8(kResetting, 0);
                call(ResetOnline);
                nextState();
            }
            break;
        case 13:
            Memory::Write8(kFade + kState, 0);
            Memory::Write8(kFade + kActive, 0);
            Memory::Write8(kResetRequested, 0);
            Memory::Write8(kResetActive, 0);
            Memory::Write8(kResetPhase, 0);
            break;
        default:
            break;
        }
    }
    *ctx = saved;
}
REGISTER_NATIVE_FUNCTION_AS(0x800D2230, MH3_ResetFade, "ResetFade");
