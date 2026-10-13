// The HUD's condition icon cycler, RMHP08: 0x802ED758(mask, count, state),
// reached from the nameplates (0x802ED848, 0x802ED898). State is {u8 index,
// s8 countdown}: a mask of 0 hides the icon (-1); a hidden icon shows the
// lowest set bit for 20 steps; a shown one counts down while its bit stays
// set, then (or once its bit clears) moves on to the next set bit, wrapping at
// count. The countdown ran once per step, so a 60 fps cheat patches it; here
// it runs once per whole tick of game time (GameStep::Ticks), and a step
// without a whole tick only picks up mask changes. At the stock 1/30 s step
// (one tick) this is the original.
#include "abi_bridge.h"
#include "game_step.h"
#include "memory.h"
#include "port_check.h"

#include <cstdint>

namespace {

constexpr int8_t kShowSteps = 20;

enum : uint32_t {
    kIndex = 0,     // u8, the bit shown
    kCountdown = 1, // s8, -1 when nothing shows
};

bool Set(uint32_t mask, uint32_t bit) {
    return bit < 32 && (mask >> bit & 1) != 0;
}

// One step of the original; `tick` false holds a shown icon whose bit is set.
void Cycle(uint32_t mask, int32_t count, uint32_t state, bool tick) {
    if (mask == 0) {
        Memory::Write8(state + kCountdown, uint8_t(-1));
        return;
    }

    const int8_t countdown = int8_t(Memory::Read8(state + kCountdown));

    if (countdown < 0) {
        for (int32_t i = 0; i < count; ++i) {
            if (Set(mask, i)) {
                Memory::Write8(state + kIndex, uint8_t(i));
                Memory::Write8(state + kCountdown, kShowSteps);
                return;
            }
        }

        return;
    }

    if (Set(mask, Memory::Read8(state + kIndex))) {
        if (!tick)
            return;

        if (countdown > 0) {
            Memory::Write8(state + kCountdown, uint8_t(countdown - 1));
            return;
        }
    }

    Memory::Write8(state + kCountdown, uint8_t(-1));

    for (int32_t i = 0; i < count; ++i) {
        uint8_t index = uint8_t(Memory::Read8(state + kIndex) + 1);

        if (index >= count)
            index = 0;

        Memory::Write8(state + kIndex, index);

        if (Set(mask, index)) {
            Memory::Write8(state + kCountdown, kShowSteps);
            return;
        }
    }
}

void CyclePort(CpuContext* ctx) {
    const uint32_t mask = ctx->gpr[3], state = ctx->gpr[5];
    const int32_t count = int32_t(ctx->gpr[4]);
    const uint32_t ticks = GameStep::Ticks();

    if (ticks == 0) {
        Cycle(mask, count, state, false);
        return;
    }

    for (uint32_t t = 0; t < ticks; ++t)
        Cycle(mask, count, state, true);
}

} // namespace

extern "C" void func_802ED758(CpuContext* ctx);
extern "C" void MH3_HudIconCycle(CpuContext* ctx) {
    PortCheck::Call(ctx, "HudIconCycle", func_802ED758, CyclePort);
}
REGISTER_NATIVE_FUNCTION_AS(0x802ED758, MH3_HudIconCycle, "HudIconCycle");
