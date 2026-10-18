// Pointing at monsters for the log, RMHP08. C reconstructions of the
// functions that call into the Wii Remote pointer (pad_pointer.cpp),
// decompiled with Ghidra:
//   0x802DB354  PointerLogUpdate (): once a step from the village
//               (VillageSceneMove) and the field (0x8028CDA0). While the
//               remote's B is held the cursor opens; over a target A takes
//               it, and dropped on the log on the HUD it is logged: offline a
//               monster (its bit in the save, message 38), online an entry of
//               the HUD into an 8-entry list (message 45)
//   0x802DCDBC  PointerLogCursor (work): the cursor from the pointer
//               (PadPointerStart), and a trail of its last 4 positions
//   0x802DCB60  PointerLogPadCursor (work, u16 held): the cursor by the
//               remote's d-pad instead (option 4)
//   0x802DBB84  PointerLogIrReady () -> 1 once the IR camera is on
//   0x802DC954  PointerLogButtons (u16* held, u16* pressed): the remote's
//               buttons (the Nunchuk set with a Nunchuk)
//   0x802DC9F4  PointerLogOpenHeld (u16 held) -> 1: B (Z with a Nunchuk)
//   0x802DCA78  PointerLogGrab (work, u16 held, u16 pressed) -> 1: A taken
//               (held while dragging); with the d-pad cursor A toggles
//
// The hit tests (0x802DBBF4, the monsters' 0x802DC2CC), the log's box
// (0x802DC5CC) and the logging itself stay translated.
//
// The cursor's animation counters, its opening time (23 steps before it
// takes anything), the IR wait and the trail counted steps; here they count
// whole ticks (GameStep::Ticks()), and the d-pad cursor moves by
// GameStep::Share of its speed. At the stock 1/30 s step this is the
// original.
#include "game_step.h"
#include "guest_call.h"
#include "port_check.h"

#include <algorithm>
#include <cstdint>

namespace {

constexpr uint32_t kWork = 0x806BFE88;

// The pointing work.
enum : uint32_t {
    kEnabled = 0x00,     // u8: may open this step (0x802DB96C)
    kState = 0x01,       // u8: 0 closed, 1 open, 2 over a target, 3 over one it can't take, 4 dragging
    kCursorX = 0x04,     // s16 screen pixels
    kCursorY = 0x06,
    kTrail = 0x08,       // s16 x, y [4], newest first
    kTakeable = 0x19,    // u8: the target can be taken
    kOverLog = 0x1A,     // u8
    kDragFrame = 0x1B,   // u8: 30 outside the log, 60..79 over it
    kLoggedFrame = 0x1C, // u8: the logged animation, 0 when done
    kFrameA = 0x1D,      // u8, wraps after the layout's 0x467 frames
    kFade = 0x1E,        // u8, up to the layout's 0x465 frames
    kOpenTime = 0x1F,    // u8, up to 23
    kBlink = 0x20,       // u8, wraps after the layout's 0x460 frames; a sound at 7
    kFrameB = 0x21,      // u8, up to 5
    kFrameC = 0x22,      // u8, wraps after 15
    kCursorValid = 0x23, // u8: the pointer is on the screen
    kIrWait = 0x24,      // u8
    kActive = 0x26,      // u8: open this step
    kHovering = 0x27,    // u8
    kTarget = 0x28,      // u8 set, u8 kind (+1, 4 a monster), s16 id (+2), s16 x, y (+4), u8 index (+8, 0xFF)
    kPadSpeed = 0x36,    // s16: the d-pad cursor's speed ramp
    kBHeld = 0x38,       // u8
    kTargetRecord = 0x3C,
};

constexpr uint32_t kOptions = 0x8065B510;         // u8[]: get_option_cfg, inlined
constexpr uint32_t kPointerX = 0x8066B938;        // f32 screen pixels (PadPointerSmooth)
constexpr uint32_t kPointerY = 0x8066B93C;
constexpr uint32_t kPointerValid = 0x80796CD4;    // s32: 1 on screen, 2 appeared this step
constexpr uint32_t kRemoteSet = 0x8065B89A;       // Psw+0xCA: the Wii Remote's buttons, pressed at +4
constexpr uint32_t kNunchukSet = 0x8065B8CC;      // Psw+0xFC
constexpr uint32_t kLayoutTables = 0x806AF3A8;    // get_menu_lsp_tbl, inlined

enum : uint32_t {
    PadPointerStart = 0x800440BC,
    PadPointerRead = 0x80044DDC,
    PadPointerReset = 0x80043C10,
    GetControlType = 0x80043BAC,
    GetScreenSize = 0x80040354,    // (MH_VEC2*)
    WPADIsDpdEnabled = 0x804F1A30,
    SubTransSetPrio = 0x800490D4,  // (priority, draw, 0, 0)
    PlaySystemSound = 0x800DBC94,  // sysSE_req (id)
    PointerSound = 0x800DBC7C,     // ()
    MenuOpen = 0x8037695C,         // () -> 1: no remote buttons
    PointerLogAllowed = 0x802DB96C, // () sets kEnabled
    PointerLogHitTest = 0x802DBBF4, // () -> 1 over a target
    PointerLogOverLog = 0x802DC5CC, // (s16* cursor) -> 1
    PointerLogTargetLogged = 0x802DC8C0, // (target, record) -> 1, 2 refused; 0 takeable
    MonsterLogged = 0x802DE560,    // (u8 id) -> nonzero
    MonsterLog = 0x802DE510,       // (u8 id)
    ListAdd = 0x8034C618,          // (record) -> 0 added, 1 there already, 2 full
    ShowMessage = 0x8035B764,      // (1, message, 0)
    LayoutFrames = 0x802E0BB8,     // (layout) -> u16
    LayoutTableFrames = 0x802E0CE4, // (table) -> u16
    ClearHitTargets = 0x8034CA24,  // ()
    DrawCursor = 0x802DE2C4,
    DrawTarget = 0x802DD7C4,
    DrawLog = 0x802DD56C,
};

// Sound effects.
enum : uint32_t {
    kSoundClose = 1,
    kSoundRefused = 2,
    kSoundBlink = 21,
    kSoundTake = 22,
    kSoundLogged = 23,
};

uint8_t R8(uint32_t offset) {
    return Memory::Read8(kWork + offset);
}

void W8(uint32_t offset, uint32_t value) {
    Memory::Write8(kWork + offset, uint8_t(value));
}

uint8_t Option(uint32_t n) {
    const uint8_t value = Memory::Read8(kOptions + n);
    return n == 20 ? value ^ 1 : value;
}

bool PointerByPad() {
    return Option(4) != 0;
}

// Shifts the trail and puts (x, y) first, once per tick of the step.
void Trail(uint32_t work, uint16_t x, uint16_t y) {
    for (uint32_t tick = 0; tick < GameStep::Ticks(); ++tick) {
        for (uint32_t i = 3; i > 0; --i) {
            Memory::Write16(work + kTrail + i * 4, Memory::Read16(work + kTrail + (i - 1) * 4));
            Memory::Write16(work + kTrail + i * 4 + 2, Memory::Read16(work + kTrail + (i - 1) * 4 + 2));
        }

        Memory::Write16(work + kTrail, x);
        Memory::Write16(work + kTrail + 2, y);
    }
}

void ResetTrail(uint32_t work, uint16_t x, uint16_t y) {
    for (uint32_t i = 0; i < 4; ++i) {
        Memory::Write16(work + kTrail + i * 4, x);
        Memory::Write16(work + kTrail + i * 4 + 2, y);
    }
}

// Back to open without a target.
void Release() {
    W8(kState, 1);
    W8(kTarget, 0);
    W8(kTarget + 8, 0xFF);
    W8(kBlink, 0);
    W8(kFrameB, 0);
    W8(kOpenTime, 0);
}

} // namespace

static void PointerLogButtonsPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x10);
    GuestCall game{ctx};
    const uint32_t held = frame.Arg(3), pressed = frame.Arg(4);
    Memory::Write16(pressed, 0);
    Memory::Write16(held, 0);

    if (game.Call<MenuOpen>() == 1)
        return;

    const uint32_t type = game.Call<GetControlType>({0}) & 0xFF;
    const uint32_t set = type == 1 ? kNunchukSet : type == 2 ? kRemoteSet : 0;

    if (set) {
        Memory::Write16(held, Memory::Read16(set));
        Memory::Write16(pressed, Memory::Read16(set + 4));
    }
}

static void PointerLogOpenHeldPort(CpuContext* ctx) {
    bool open;

    // The frame puts the registers back when it closes: r3 after it.
    {
        GuestFrame frame(ctx, 0x10);
        GuestCall game{ctx};
        const uint32_t held = frame.Arg(3) & 0xFFFF;
        const uint32_t type = game.Call<GetControlType>({0}) & 0xFF;
        open = (type == 1 && (held & 4)) || (type == 2 && (held & 0x100));
    }

    ctx->gpr[3] = open;
}

// A, as held (dragging) or pressed (taking).
static void PointerLogGrabPort(CpuContext* ctx) {
    const uint32_t work = ctx->gpr[3], held = ctx->gpr[4] & 0xFFFF, pressed = ctx->gpr[5] & 0xFFFF;
    const bool dragging = uint8_t(Memory::Read8(work + kState) - 3) <= 1;

    if (!PointerByPad())
        ctx->gpr[3] = ((dragging ? held : pressed) & 0x200) != 0;
    else
        ctx->gpr[3] = ((pressed & 0x200) != 0) != dragging;
}

static void PointerLogIrReadyPort(CpuContext* ctx) {
    uint32_t ready = 0;

    if (PointerByPad()) {
        ready = 1;
    } else {
        GuestFrame frame(ctx, 0x10);
        GuestCall game{ctx};

        if (game.Call<WPADIsDpdEnabled>({0}) == 1) {
            // A tick for the camera to start.
            if (const uint8_t wait = R8(kIrWait))
                W8(kIrWait, wait - std::min<uint32_t>(wait, GameStep::Ticks()));
            else
                ready = 1;
        }
    }

    ctx->gpr[3] = ready;
}

static void PointerLogCursorPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x10);
    GuestCall game{ctx};
    const uint32_t work = frame.Arg(3);
    game.Call<PadPointerStart>();
    const uint16_t x = uint16_t(int32_t(Memory::ReadFloat32(kPointerX))), y = uint16_t(int32_t(Memory::ReadFloat32(kPointerY)));
    const uint32_t valid = Memory::Read32(kPointerValid);
    Memory::Write8(work + kCursorValid, valid != 0);

    if (Memory::Read8(work + kState) == 0) {
        if (valid != 0) {
            Memory::Write16(work + kCursorX, x);
            Memory::Write16(work + kCursorY, y);
            ResetTrail(work, x, y);
        }

        return;
    }

    if (valid == 1)
        Trail(work, x, y);
    else if (valid != 0)
        ResetTrail(work, x, y);

    Memory::Write16(work + kCursorX, x);
    Memory::Write16(work + kCursorY, y);
}

// The d-pad moves the cursor at 4 pixels a tick, speeding up to 12.
static void PointerLogPadCursorPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x20);
    GuestCall game{ctx};
    const uint32_t work = frame.Arg(3), held = frame.Arg(4) & 0xFFFF;
    const uint32_t size = frame[8];
    game.Call<GetScreenSize>({size});
    const float width = Memory::ReadFloat32(size), height = Memory::ReadFloat32(size + 4);

    if ((held & 0x3C00) == 0) {
        Memory::Write16(work + kPadSpeed, 0xFFF0);
    } else {
        for (uint32_t tick = 0; tick < GameStep::Ticks() && int16_t(Memory::Read16(work + kPadSpeed)) < 8; ++tick)
            Memory::Write16(work + kPadSpeed, uint16_t(Memory::Read16(work + kPadSpeed) + 2));
    }

    const int16_t speed = int16_t(Memory::Read16(work + kPadSpeed));
    const int32_t step = GameStep::Share(speed > 0 ? speed + 4 : 4);
    int32_t dx = 0, dy = 0;

    if (held & 0x2000)
        dy = -step;

    if (held & 0x1000)
        dy += step;

    if (held & 0x800)
        dx = -step;

    if (held & 0x400)
        dx += step;

    if (Memory::Read8(work + kState) == 0) {
        Memory::Write8(work + kCursorValid, 1);
        Memory::Write16(work + kPadSpeed, 0xFFF0);
        const uint16_t x = uint16_t(int32_t(width * 0.5f)), y = uint16_t(int32_t(height * 0.5f));
        Memory::Write16(work + kCursorX, x);
        Memory::Write16(work + kCursorY, y);
        ResetTrail(work, x, y);
        return;
    }

    const int16_t right = int16_t(int32_t(width)), bottom = int16_t(int32_t(height));
    int16_t x = int16_t(int16_t(Memory::Read16(work + kCursorX)) + dx);
    int16_t y = int16_t(int16_t(Memory::Read16(work + kCursorY)) + dy);
    x = std::clamp<int16_t>(x, 0, std::max<int16_t>(right, 0));
    y = std::clamp<int16_t>(y, 0, std::max<int16_t>(bottom, 0));
    Memory::Write16(work + kCursorX, uint16_t(x));
    Memory::Write16(work + kCursorY, uint16_t(y));

    if (Memory::Read8(work + kCursorValid) != 0)
        Trail(work, uint16_t(x), uint16_t(y));
}

// The cursor's animation counters, one tick. True when the blink reached 7.
static bool AdvanceFrames(GuestCall& game) {
    if (R8(kOpenTime) < 23)
        W8(kOpenTime, R8(kOpenTime) + 1);

    if (R8(kFrameB) < 5)
        W8(kFrameB, R8(kFrameB) + 1);

    if (int32_t(game.Call<LayoutFrames>({0x465}) & 0xFFFF) > int32_t(R8(kFade)))
        W8(kFade, R8(kFade) + 1);

    W8(kBlink, R8(kBlink) + 1);

    if (int32_t(R8(kBlink)) > int32_t(game.Call<LayoutFrames>({0x460}) & 0xFFFF))
        W8(kBlink, 0);

    const bool blink = R8(kBlink) == 7;
    W8(kFrameA, R8(kFrameA) + 1);

    if (int32_t(R8(kFrameA)) > int32_t(game.Call<LayoutFrames>({0x467}) & 0xFFFF))
        W8(kFrameA, 0);

    W8(kFrameC, R8(kFrameC) + 1);

    if (R8(kFrameC) > 15)
        W8(kFrameC, 0);

    if (R8(kHovering) == 0 && R8(kLoggedFrame) != 0) {
        W8(kLoggedFrame, R8(kLoggedFrame) + 1);
        const uint32_t table = Memory::Read32(kLayoutTables + 0x60 * 4);

        if (int32_t(R8(kLoggedFrame)) > int32_t(game.Call<LayoutTableFrames>({Memory::Read32(table + 12)}) & 0xFFFF))
            W8(kLoggedFrame, 0);
    }

    return blink;
}

static void PointerLogUpdatePort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x20);
    GuestCall game{ctx};
    const uint8_t wasState = R8(kState);
    W8(kEnabled, 0);
    W8(kActive, 0);
    game.Call<PointerLogAllowed>();
    game.Call<PadPointerRead>();

    if (R8(kEnabled) != 1) {
        W8(kBHeld, 0);
    } else {
        game.Call<SubTransSetPrio>({6, DrawCursor, 0, 0});
        game.Call<SubTransSetPrio>({7, DrawTarget, 0, 0});
        game.Call<SubTransSetPrio>({10, DrawLog, 0, 0});
        const uint32_t pressedAt = frame[8], heldAt = frame[10];
        game.Call<0x802DC954>({heldAt, pressedAt});
        const auto held = [&] {
            return uint32_t(Memory::Read16(heldAt));
        };

        const auto pressed = [&] {
            return uint32_t(Memory::Read16(pressedAt));
        };

        bool open = false;

        if (R8(kBHeld) == 0) {
            if (game.Call<0x802DC9F4>({held()}) == 1) {
                open = true;
                game.Call<PadPointerReset>();
                game.Call<PointerSound>();
                W8(kIrWait, 1);
                W8(kBHeld, 1);
            }
        } else {
            open = game.Call<0x802DC9F4>({held()}) == 1;
        }

        // It stays open while it opens.
        if (R8(kOpenTime) != 0 && R8(kOpenTime) < 23 && R8(kState) == 1)
            open = true;

        if (!open) {
            W8(kBHeld, 0);
        } else {
            if (PointerByPad())
                game.Call<0x802DCB60>({kWork, held()});
            else
                game.Call<0x802DCDBC>({kWork});

            if (game.Call<0x802DBB84>() == 1) {
                W8(kActive, 1);
                bool blink = false;

                for (uint32_t tick = 0; tick < GameStep::Ticks(); ++tick)
                    blink |= AdvanceFrames(game);

                const auto grab = [&] {
                    return game.Call<0x802DCA78>({kWork, held(), pressed()}) == 1;
                };

                switch (R8(kState)) {
                case 0:
                    W8(kState, 1);
                    W8(kFrameB, 0);
                    W8(kBlink, 0);
                    W8(kOpenTime, 0);
                    W8(kTarget, 0);
                    W8(kTarget + 8, 0xFF);
                    break;
                case 1:
                    if (R8(kOpenTime) >= 23 && game.Call<PointerLogHitTest>() == 1) {
                        W8(kState, 2);

                        if (R8(kTakeable) == 1) {
                            W8(kLoggedFrame, 0);
                            W8(kBlink, 0);
                            W8(kFrameB, 0);
                        }
                    }

                    break;
                case 2:
                    if (game.Call<PointerLogHitTest>() == 0) {
                        W8(kState, 1);
                        W8(kTarget, 0);
                        W8(kTarget + 8, 0xFF);
                        break;
                    }

                    if (blink)
                        game.Call<PlaySystemSound>({kSoundBlink});

                    if (!grab())
                        break;

                    if (R8(kTakeable) == 1) {
                        W8(kState, 4);
                        W8(kOpenTime, 0);
                        const uint32_t logged = game.Call<PointerLogTargetLogged>({kWork + kTarget, kWork + kTargetRecord}) & 0xFF;

                        if (logged - 1 <= 1)
                            game.Call<PlaySystemSound>({kSoundRefused});
                        else if (logged == 0)
                            game.Call<PlaySystemSound>({kSoundTake});
                    } else {
                        W8(kState, 3);
                        W8(kOpenTime, 0);
                        game.Call<PlaySystemSound>({kSoundTake});
                    }

                    break;
                case 3:
                    if (!grab()) {
                        W8(kHovering, 0);
                        Release();
                    }

                    break;
                case 4:
                    W8(kOverLog, game.Call<PointerLogOverLog>({kWork + kCursorX}));

                    if (grab()) {
                        if (R8(kOverLog) == 0) {
                            W8(kDragFrame, 30);
                        } else {
                            for (uint32_t tick = 0; tick < GameStep::Ticks(); ++tick) {
                                W8(kDragFrame, R8(kDragFrame) + 1);

                                if (uint8_t(R8(kDragFrame) - 60) > 19)
                                    W8(kDragFrame, 60);
                            }
                        }

                        break;
                    }

                    // Dropped on the log: logged, unless it already is.
                    if (R8(kOverLog) != 0) {
                        if (R8(kTarget + 1) == 4) {
                            const uint32_t id = Memory::Read16(kWork + kTarget + 2) & 0xFF;

                            if (game.Call<MonsterLogged>({id}) != 0) {
                                game.Call<PlaySystemSound>({kSoundRefused});
                            } else {
                                game.Call<MonsterLog>({id});
                                game.Call<ShowMessage>({1, 38, 0});
                                W8(kLoggedFrame, 1);
                                game.Call<PlaySystemSound>({kSoundLogged});
                            }
                        } else {
                            const int32_t added = int8_t(game.Call<ListAdd>({kWork + kTargetRecord}));

                            if (uint32_t(added - 1) <= 1) {
                                game.Call<PlaySystemSound>({kSoundRefused});
                            } else if (added == 0) {
                                game.Call<ShowMessage>({1, 45, 0});
                                W8(kLoggedFrame, 1);
                                game.Call<PlaySystemSound>({kSoundLogged});
                            }
                        }
                    }

                    W8(kHovering, 0);
                    Release();
                    game.Call<PointerSound>();
                    break;
                default:
                    W8(kActive, 0);
                    break;
                }
            }
        }
    }

    game.Call<ClearHitTargets>();

    if (R8(kActive) != 0)
        return;

    if (wasState != 0 || (R8(kState) != 0 && R8(kEnabled) == 0))
        game.Call<PlaySystemSound>({kSoundClose});

    W8(kState, 0);
    W8(kTakeable, 0);
    W8(kHovering, 0);
    W8(kTarget, 0);
    W8(kTarget + 8, 0xFF);
    W8(kLoggedFrame, 0);

    // The cursor fades out a frame a tick.
    W8(kFade, R8(kFade) - std::min<uint32_t>(R8(kFade), GameStep::Ticks()));
}

#define MH3_LOG_POINTER_PORT(address, name)                                                                                                                                                            \
    extern "C" void func_##address(CpuContext* ctx);                                                                                                                                                   \
    extern "C" void MH3_##name(CpuContext* ctx) {                                                                                                                                                      \
        PortCheck::Call(ctx, #name, func_##address, name##Port);                                                                                                                                       \
    }

MH3_LOG_POINTER_PORT(802DB354, PointerLogUpdate)
REGISTER_NATIVE_FUNCTION_AS(0x802DB354, MH3_PointerLogUpdate, "PointerLogUpdate");
MH3_LOG_POINTER_PORT(802DCDBC, PointerLogCursor)
REGISTER_NATIVE_FUNCTION_AS(0x802DCDBC, MH3_PointerLogCursor, "PointerLogCursor");
MH3_LOG_POINTER_PORT(802DCB60, PointerLogPadCursor)
REGISTER_NATIVE_FUNCTION_AS(0x802DCB60, MH3_PointerLogPadCursor, "PointerLogPadCursor");
MH3_LOG_POINTER_PORT(802DBB84, PointerLogIrReady)
REGISTER_NATIVE_FUNCTION_AS(0x802DBB84, MH3_PointerLogIrReady, "PointerLogIrReady");
MH3_LOG_POINTER_PORT(802DC954, PointerLogButtons)
REGISTER_NATIVE_FUNCTION_AS(0x802DC954, MH3_PointerLogButtons, "PointerLogButtons");
MH3_LOG_POINTER_PORT(802DC9F4, PointerLogOpenHeld)
REGISTER_NATIVE_FUNCTION_AS(0x802DC9F4, MH3_PointerLogOpenHeld, "PointerLogOpenHeld");
MH3_LOG_POINTER_PORT(802DCA78, PointerLogGrab)
REGISTER_NATIVE_FUNCTION_AS(0x802DCA78, MH3_PointerLogGrab, "PointerLogGrab");
