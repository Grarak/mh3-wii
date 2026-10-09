// A field actor's turns, RMHP08. The actor class updated by 0x8038595C (state
// +3; the Tundra quest 10057 has one at 0x806C76E8, found by a step probe at
// the quest's start) walks on the ground (GetGroundHit) and, in its wandering
// action (0x803A44AC, from 0x803A433C), waits 100 to 163 steps (s32 +0x16C,
// tick site 0x803A451C) while it turns its goal heading (u16 in s32 +0x18C,
// copied to +0x1A8) toward a point by at most 256 a step (ActorTurnToward
// 0x803A4148). After every action 0x803A41D4 (ActorFaceGoal) turns the
// heading +0x18C toward +0x1A8 by 4004 a step, 1820 in action 0.4 (+0x19D 0,
// +0x19F 4). Both amounts are taken over this step's length (GameStep::Share);
// at the stock 1/30 s step these are the originals.
#include "game_step.h"
#include "memory.h"
#include "port_check.h"
#include "ppc_runtime.h"

#include <cstdint>

namespace {

constexpr uint32_t kAction = 0x19D, kSub = 0x19F, kHeading = 0x18C, kGoal = 0x1A8;

void FaceGoalPort(CpuContext* ctx) {
    const uint32_t actor = ctx->gpr[3];
    const bool slow = MemoryInline::FlatRead8(actor + kAction) == 0 && MemoryInline::FlatRead8(actor + kSub) == 4;
    const int32_t turn = GameStep::Share(slow ? 1820 : 4004);
    const uint32_t heading = MemoryInline::FlatRead32(actor + kHeading);
    const uint32_t goal = MemoryInline::FlatRead32(actor + kGoal);
    const uint32_t gap = (goal - (heading & 0xFFFF)) & 0xFFFF;

    // Within one step's turn of the goal (the short way round): on it.
    if (((gap + uint32_t(turn)) & 0xFFFF) < uint32_t(turn) * 2)
        MemoryInline::FlatWrite32(actor + kHeading, goal);
    else
        MemoryInline::FlatWrite32(actor + kHeading, (gap < 0x8000 ? heading + uint32_t(turn) : heading - uint32_t(turn)) & 0xFFFF);

    ctx->gpr[3] = (gap + uint32_t(turn)) & 0xFFFF;
    ctx->gpr[4] = uint32_t(turn);
}

} // namespace

extern "C" void func_803A4148(CpuContext* ctx);
extern "C" void MH3_ActorTurnToward(CpuContext* ctx) {
    ctx->gpr[5] = uint16_t(GameStep::Share(int32_t(uint16_t(ctx->gpr[5]))));
    func_803A4148(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x803A4148, MH3_ActorTurnToward, "ActorTurnToward");

extern "C" void func_803A41D4(CpuContext* ctx);
extern "C" void MH3_ActorFaceGoal(CpuContext* ctx) {
    PortCheck::Call(ctx, "ActorFaceGoal", func_803A41D4, FaceGoalPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x803A41D4, MH3_ActorFaceGoal, "ActorFaceGoal");
