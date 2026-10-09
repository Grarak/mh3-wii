// Enemy turning and awareness, RMHP08. Monsters turn toward a target angle a
// step at a time through three helpers (decompiled from the disassembly), and
// spin by an angle over a motion's frames (0x80133FB0, 0x80133EA0, below):
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
#include "guest_call.h"
#include "memory.h"
#include "port_check.h"
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

// 0x80133FB0 (enemy, s32 angle; f1 first, f2 last frame), decompiled with
// Ghidra: while the motion is in [first, last] (em_frame_check mode 4), the
// yaw (s32 +0x1C0, 65536 a turn) turns by 2 * angle / (last - first) a call,
// truncated: the angle spread over the window at the motion's 2 frames a
// tick. Six kinds' spinning attacks call it once a step: Deviljho's tail spin
// (0x801D8F00, action 7.1: half a turn over frames 24-100, 862 a call), and
// 0x8014BE5C, 0x80158CAC, 0x801616C4, 0x801725E8, 0x801BDB28. At 60 or 120
// steps a second they spun two or four times as fast as the motion. Here the
// yaw turns by the step's share of the per-tick amount; at the stock 1/30 s
// step this is the original (MH3_PORT_CHECK=EnemyTurnOverMotion).
namespace {
void TurnOverMotionPort(CpuContext* ctx) {
    GuestCall game{ctx};
    const uint32_t enemy = ctx->gpr[3];
    const int32_t angle = int32_t(ctx->gpr[4]);
    const float first = float(ctx->fpr[1].d), last = float(ctx->fpr[2].d);

    if (game.Call<0x8012F980>({enemy, 4}, {first, last}) != 1) // em_frame_check
        return;

    const int32_t perTick = int32_t(2.0f * (float(angle) / (last - first)));
    Memory::Write32(enemy + 0x1C0, uint32_t(int32_t(Memory::Read32(enemy + 0x1C0)) + GameStep::Share(perTick)));
}

// 0x80133EA0 (enemy, s32 angle; f1 first, f2 last frame), decompiled with
// Ghidra: while the motion is in [first, last + rate], the yaw turns by the
// angle times the frames the motion advanced this step inside [first, last],
// over last - first (truncated). The original takes the advance as the
// motion's rate (0x8012F948, frames a tick), so at 60 or 120 steps a second
// every turn of its 75 callers (spins and turns within an attack's frames,
// most monsters) was two or four times as fast; the advance is the rate
// times the step's length (GameStep::Scale) here. At the stock step this is
// the original (MH3_PORT_CHECK=EnemyTurnWhile).
void TurnWhilePort(CpuContext* ctx) {
    GuestCall game{ctx};
    const uint32_t enemy = ctx->gpr[3];
    const int32_t angle = int32_t(ctx->gpr[4]);
    const float first = float(ctx->fpr[1].d), last = float(ctx->fpr[2].d);
    game.Call<0x8012F948>({enemy});
    float advance = game.F1() * float(GameStep::Scale());

    if (game.Call<0x8012F980>({enemy, 4}, {first, last + advance}) != 1) // em_frame_check
        return;

    game.Call<0x8012F950>({enemy});
    const float frame = game.F1(), past = frame - first;

    if (past < 0.0f)
        advance += past;
    else if (const float over = frame + advance - last; over > 0.0f)
        advance -= over;

    const int32_t turn = int32_t(advance * (float(angle) / (last - first)));
    Memory::Write32(enemy + 0x1C0, uint32_t(int32_t(Memory::Read32(enemy + 0x1C0)) + turn));
}

// 0x80137E34 (enemy), decompiled with Ghidra: the pitch (+0x1BC) and the roll
// (+0x1C4, u16 angles) brought back toward level by 0x100 a call (an angle
// within that set to 0) while +0x1E2 is 1-4: the pitch unless 0x80133CAC
// (enemy) says it is held, the roll unless +0x784 is set or +0x1E2 and +0x1E3
// are both 1; at other +0x1E2 values both are set to 0. The enemies' movement
// step (0x801382A0) calls it once a step, so at 60 or 120 steps a second a
// swimming or flying monster (Gobul after a lunge, ...) levelled two or four
// times as fast. Here by the step's share of 0x100; at the stock step this is
// the original (MH3_PORT_CHECK=EnemyLevel).
uint32_t Level(uint32_t angle, uint32_t amount) {
    const uint32_t low = angle & 0xFFFF;

    if (low < 0x8000)
        return low < amount ? 0 : angle - amount;

    return low + amount <= 0x10000 ? angle + amount : 0;
}

void LevelPort(CpuContext* ctx) {
    GuestCall game{ctx};
    const uint32_t enemy = ctx->gpr[3];
    const uint32_t mode = Memory::Read8(enemy + 0x1E2);

    if (mode - 1 > 3) {
        Memory::Write32(enemy + 0x1BC, 0);
        Memory::Write32(enemy + 0x1C4, 0);
        return;
    }

    const uint32_t amount = uint32_t(GameStep::Share(0x100));

    if (game.Call<0x80133CAC>({enemy}) == 0)
        Memory::Write32(enemy + 0x1BC, Level(Memory::Read32(enemy + 0x1BC), amount));

    if (Memory::Read8(enemy + 0x784) == 0 && (mode != 1 || Memory::Read8(enemy + 0x1E3) != 1))
        Memory::Write32(enemy + 0x1C4, Level(Memory::Read32(enemy + 0x1C4), amount));
}
} // namespace

extern "C" void func_80133EA0(CpuContext* ctx);
extern "C" void MH3_EnemyTurnWhile(CpuContext* ctx) {
    PortCheck::Call(ctx, "EnemyTurnWhile", func_80133EA0, TurnWhilePort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80133EA0, MH3_EnemyTurnWhile, "EnemyTurnWhile");

extern "C" void func_80133FB0(CpuContext* ctx);
extern "C" void MH3_EnemyTurnOverMotion(CpuContext* ctx) {
    PortCheck::Call(ctx, "EnemyTurnOverMotion", func_80133FB0, TurnOverMotionPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80133FB0, MH3_EnemyTurnOverMotion, "EnemyTurnOverMotion");

extern "C" void func_80137E34(CpuContext* ctx);
extern "C" void MH3_EnemyLevel(CpuContext* ctx) {
    PortCheck::Call(ctx, "EnemyLevel", func_80137E34, LevelPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80137E34, MH3_EnemyLevel, "EnemyLevel");

// Awareness (0x8012B66C: enemy, player, amount): the enemy's value for a
// player (s32 +0x428 + 4 * player) += amount, clamped to 0..20000. 0x8012B6B0
// adds each player's rate every step (its call returns to 0x8012B898): that
// amount becomes its share of this step. The other caller (0x8011F39C) adds on
// an event and is left as is. Three more gauges work the same way, each with
// one per-step caller in 0x8012BE58 and one event caller near 0x8011F3xx:
//   0x8012B9A8 (enemy, player, amount): +0x390 + 4 * player, from 0x8012BFB0
//   0x8012B9EC (enemy, amount): +0x3A0, from 0x8012C028
//   0x8012BA20 (enemy, player, amount): +0x3A4 + 4 * player, from 0x8012C0FC
namespace {
void ShareAt(CpuContext* ctx, uint32_t perStepReturn, int reg) {
    if (ctx->lr == perStepReturn)
        ctx->gpr[reg] = uint32_t(GameStep::Share(int32_t(ctx->gpr[reg])));
}
} // namespace

extern "C" void func_8012B66C(CpuContext* ctx);
extern "C" void MH3_EnemyAwarenessAdd(CpuContext* ctx) {
    ShareAt(ctx, 0x8012B898u, 5);
    func_8012B66C(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x8012B66C, MH3_EnemyAwarenessAdd, "EnemyAwarenessAdd");

extern "C" void func_8012B9A8(CpuContext* ctx);
extern "C" void MH3_EnemyGaugeAdd390(CpuContext* ctx) {
    ShareAt(ctx, 0x8012BFB4u, 5);
    func_8012B9A8(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x8012B9A8, MH3_EnemyGaugeAdd390, "EnemyGaugeAdd390");

extern "C" void func_8012B9EC(CpuContext* ctx);
extern "C" void MH3_EnemyGaugeAdd3A0(CpuContext* ctx) {
    ShareAt(ctx, 0x8012C02Cu, 4);
    func_8012B9EC(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x8012B9EC, MH3_EnemyGaugeAdd3A0, "EnemyGaugeAdd3A0");

extern "C" void func_8012BA20(CpuContext* ctx);
extern "C" void MH3_EnemyGaugeAdd3A4(CpuContext* ctx) {
    ShareAt(ctx, 0x8012C100u, 5);
    func_8012BA20(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x8012BA20, MH3_EnemyGaugeAdd3A4, "EnemyGaugeAdd3A4");
