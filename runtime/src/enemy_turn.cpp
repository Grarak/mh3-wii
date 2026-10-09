// Enemy turning and awareness, RMHP08. Monsters turn toward a target angle a
// step at a time through three helpers (decompiled from the disassembly):
//   0x80133E14 (target, current, max): current moved toward target by at most
//     max (u16 angles, the short way round); the enemy AI calls it once a step
//     from 41 places (0x80133CB4 turning to face the player, ...);
//   0x80134884 (enemy, &angle, diff, step): *angle += or -= step toward diff,
//     or the enemy's goal (+0x37C) when |diff| < step (0x80134B70, 0x80134C44).
//   0x8012F8C4 (enemy, goal f1, step f2): the f32 at +0x464 (a blend weight
//     that 0x801A0CAC, a desert enemy's swim, turns its target angle by)
//     moved toward goal by at most step, then clamped to its range.
// Their amount is per step. Here it is taken over this step's length (the
// angles' GameStep::Share, the weight's step * GameStep::Scale), so monsters
// turn and lean at the same rate at any step length. At the stock 1/30 s step
// these are the originals.
#include "game_step.h"
#include "abi_bridge.h"
#include "ppc_runtime.h"

#include <cstdint>

extern "C" void func_80133E14(CpuContext* ctx);
extern "C" void MH3_EnemyTurnToward(CpuContext* ctx) {
    ctx->gpr[5] = uint16_t(GameStep::Share(int32_t(uint16_t(ctx->gpr[5]))));
    func_80133E14(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x80133E14, MH3_EnemyTurnToward, "EnemyTurnToward");

extern "C" void func_80134884(CpuContext* ctx);
extern "C" void MH3_EnemyTurnStep(CpuContext* ctx) {
    ctx->gpr[6] = uint16_t(GameStep::Share(int32_t(uint16_t(ctx->gpr[6]))));
    func_80134884(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x80134884, MH3_EnemyTurnStep, "EnemyTurnStep");

extern "C" void func_8012F8C4(CpuContext* ctx);
extern "C" void MH3_EnemyBlendToward(CpuContext* ctx) {
    ctx->fpr[2].d = float(ctx->fpr[2].d * GameStep::Scale());
    func_8012F8C4(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x8012F8C4, MH3_EnemyBlendToward, "EnemyBlendToward");

// Awareness (0x8012B66C: enemy, player, amount): the enemy's value for a
// player (s32 +0x428 + 4 * player) += amount, clamped to 0..20000. 0x8012B6B0
// adds each player's rate every step (its call returns to 0x8012B898): that
// amount becomes its share of this step. The other caller (0x8011F39C) adds on
// an event and is left as is.
extern "C" void func_8012B66C(CpuContext* ctx);
extern "C" void MH3_EnemyAwarenessAdd(CpuContext* ctx) {
    if (ctx->lr == 0x8012B898u) ctx->gpr[5] = uint32_t(GameStep::Share(int32_t(ctx->gpr[5])));
    func_8012B66C(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x8012B66C, MH3_EnemyAwarenessAdd, "EnemyAwarenessAdd");
