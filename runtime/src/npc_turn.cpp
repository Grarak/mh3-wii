// NPC turning, RMHP08: the village's and the city's people (NpcVisibilityUpdate
// 0x801FDC34 steps each one). They turn in two ways, both once a step:
//   eases: NpcYawTo (0x801FDEA0: the angle from the NPC (f32 x, y, z +0x10)
//     to a point, less its yaw, s32 +0x2C, 65536 a turn) then
//     yaw += (s16)gap / d, at seven places (kEases), d 10, 5, 4 or 2; the
//     step's amount is also kept at +0x1D4;
//   timed turns: NpcTimedTurnStart (0x801FDF48) spreads the gap over N steps
//     (s16 +0x1E4 = N, s16 +0x1E8 = gap / N), and NpcTimedTurnStep
//     (0x801FDED8, from NpcScriptStep 0x801FFD04) adds +0x1E8 to the yaw and
//     takes one from +0x1E4 each step until it is spent.
// At 60 steps a second they turned twice as fast. Here an ease closes the
// share of the gap that d steps of a tick close over this step's length s,
// 1 - (1 - 1/d)^s: the gap NpcYawTo returns to those callers is scaled so that
// their truncating divide by d gives it. A timed turn counts its N in ticks
// and turns by the step's share of the amount. At the stock 1/30 s step these
// are the originals.
#include "game_step.h"
#include "memory.h"
#include "port_check.h"
#include "ppc_runtime.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>

namespace {

constexpr uint32_t kYaw = 0x2C, kAction = 0x1CC, kTurnSteps = 0x1E4, kTurnAmount = 0x1E8, kTurning = 0x228, kTurnHeld = 0x229;

// NpcYawTo calls that ease the yaw by gap / d, by return address (d 0: by the
// NPC's action, below).
struct Ease {
    uint32_t lr;
    int32_t d;
};

constexpr Ease kEases[] = {
    {0x80201974u, 0},                    // 0x802014C8 (the village NPC's step)
    {0x80203070u, 10},                   // 0x80202F64
    {0x802052B0u, 5},                    // 0x80205110 (no +0x1D4)
    {0x802058E8u, 10}, {0x80205950u, 10}, // 0x802057C8
    {0x802078A8u, 10}, {0x80207910u, 10}, // 0x80207738
};

// 0x802014C8's divisor (r31), set by the action (s16 +0x1CC) it switches on:
// 4 turns in halves, 53 and 54 in quarters, the others in tenths.
int32_t StepDivisor(uint32_t npc) {
    const int16_t action = int16_t(MemoryInline::FlatRead16(npc + kAction));
    return action == 4 ? 2 : action == 53 || action == 54 ? 4 : 10;
}

int32_t EaseDivisor(uint32_t lr, uint32_t npc) {
    for (const Ease& ease : kEases)
        if (ease.lr == lr)
            return ease.d ? ease.d : StepDivisor(npc);

    return 0;
}

void TimedTurnStepPort(CpuContext* ctx) {
    const uint32_t npc = ctx->gpr[3];
    const int32_t steps = int16_t(MemoryInline::FlatRead16(npc + kTurnSteps));

    if (steps <= 0) {
        MemoryInline::FlatWrite16(npc + kTurnSteps, 0);
        MemoryInline::FlatWrite8(npc + kTurnHeld, 0);
        MemoryInline::FlatWrite8(npc + kTurning, 0);
        ctx->gpr[4] = uint32_t(steps);
        return;
    }

    const int32_t ticks = int32_t(GameStep::Ticks());
    const int32_t amount = int16_t(MemoryInline::FlatRead32(npc + kTurnAmount));
    int32_t turn = GameStep::Share(amount);

    // A step that spends the count turns by the ticks left at most.
    if (ticks >= steps)
        turn = std::clamp(turn, -std::abs(amount) * steps, std::abs(amount) * steps);

    const uint32_t yaw = MemoryInline::FlatRead32(npc + kYaw);
    MemoryInline::FlatWrite16(npc + kTurnSteps, uint16_t(std::max(steps - ticks, 0)));
    MemoryInline::FlatWrite32(npc + kYaw, yaw + uint32_t(turn));
    ctx->gpr[4] = yaw;
}

} // namespace

extern "C" void func_801FDEA0(CpuContext* ctx);
extern "C" void MH3_NpcYawTo(CpuContext* ctx) {
    const double s = GameStep::Scale();
    const int32_t d = s == 1.0 ? 0 : EaseDivisor(ctx->lr, ctx->gpr[3]);
    func_801FDEA0(ctx);

    if (d <= 1)
        return;

    const double closed = double(d) * (1.0 - std::pow(1.0 - 1.0 / d, s));
    const double gap = std::round(int16_t(ctx->gpr[3]) * closed);
    ctx->gpr[3] = uint32_t(int32_t(std::clamp(gap, -32768.0, 32767.0)));
}
REGISTER_NATIVE_FUNCTION_AS(0x801FDEA0, MH3_NpcYawTo, "NpcYawTo");

extern "C" void func_801FDED8(CpuContext* ctx);
extern "C" void MH3_NpcTimedTurnStep(CpuContext* ctx) {
    PortCheck::Call(ctx, "NpcTimedTurnStep", func_801FDED8, TimedTurnStepPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x801FDED8, MH3_NpcTimedTurnStep, "NpcTimedTurnStep");
