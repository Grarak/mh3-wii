// Rumble timers, RMHP08. C reconstruction of the per-step rumble countdown
// (0x80044AF4, called by the game frame after the task pass), decompiled with
// Ghidra (scripts/dol2elf.py -> ELF import).
//
// Each of the four controllers has an s8 rumble countdown in system_w; while
// its motor is on and the countdown has run out, the motor is stopped
// (0x80044A94 -> WPADControlMotor). The countdown is in steps (the original:
// 1 per step); here it counts whole ticks of game time, so rumble lasts as
// long at any step length.
#include "game_step.h"
#include "port_check.h"
#include "guest_call.h"

#include <algorithm>

namespace {
constexpr uint32_t kSystem = 0x8065AA60;    // system_w
constexpr uint32_t kRumbleSteps = 0x33;     // s8[4]
constexpr uint32_t kRumbleOn = 0x37;        // u8[4]: 1 = motor running
constexpr uint32_t RumbleStop = 0x80044A94; // (channel): clears both, stops the motor
} // namespace

static void TickSystemTimersPort(CpuContext* ctx) {
    const CpuContext saved = *ctx;
    GuestCall game{ctx};
    const uint32_t ticks = GameStep::Ticks();

    for (uint32_t i = 0; i < 4; ++i) {
        const int8_t steps = int8_t(Memory::Read8(kSystem + kRumbleSteps + i));

        if (steps > 0)
            Memory::Write8(kSystem + kRumbleSteps + i, uint8_t(steps - int8_t(std::min<uint32_t>(ticks, steps))));

        if (Memory::Read8(kSystem + kRumbleOn + i) == 1 && int8_t(Memory::Read8(kSystem + kRumbleSteps + i)) < 1)
            game.Call<RumbleStop>({i});
    }

    *ctx = saved;
}

extern "C" void func_80044AF4(CpuContext* ctx);
extern "C" void MH3_TickSystemTimers(CpuContext* ctx) {
    PortCheck::Call(ctx, "TickSystemTimers", func_80044AF4, TickSystemTimersPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80044AF4, MH3_TickSystemTimers, "TickSystemTimers");
