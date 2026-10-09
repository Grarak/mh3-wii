// Player stamina drains, RMHP08. C reconstructions of 0x8027695C and
// 0x80276AA0, decompiled with Ghidra.
//
// Stamina is the PLW's condition gauge (+0x378, changed by
// PlSetConditionState 0x802768CC). Actions that wear it down while they last
// (dashing, swimming, climbing, holding on) call 0x8027695C with a small
// drain (-1 .. -4) every step, and 0x80276AA0 drains 1 every step. Both adjust
// the drain for conditions and skills, some of which drain only on odd steps
// or double it every fifth step, by the player's step counter (+0x20). The
// drains therefore run once per whole tick of game time elapsed this step
// (GameStep::Ticks), each seeing the step counter of its tick, so stamina
// drains at the Wii's rate at any step length. At the stock 1/30 s step it
// runs once, as the original.
#include "game_step.h"
#include "guest_call.h"
#include "port_check.h"
#include "launcher_cheats.h"

#include <cstdint>

namespace {
bool noStaminaDrain = false;

enum : uint32_t {
    MasterCheck = 0x8026FE38,       // Pl_master_ck(PLW*) -> 1 for the local player
    SkillCheck = 0x80271374,        // Pl_Skill_ck(PLW*, skill) -> 1
    CatSkillCheck = 0x80272B14,     // Pl_cat_skill_ck(PLW*, skill) -> 1
    DamageCondition = 0x80279130,   // Pl_dm_condition_ck(PLW*, condition) -> 1
    SetConditionState = 0x802768CC, // PlSetConditionState(PLW*, s16 delta)
};

enum : uint32_t {
    kSteps = 0x20, // s32 player steps
};

// Conditions that triple a drain, and the skills that change it.
enum : uint32_t {
    kConditionA = 0x80,
    kConditionB = 0x40,
    kSkillHalve = 0xA8,     // drains halved (1: only on even steps)
    kSkillDouble = 0xA9,    // drains doubled every fifth step
    kCatSkillEvenOnly = 0x17,
};

bool Is(GuestCall& game, uint32_t fn, uint32_t plw, uint32_t arg) { return game(fn, {plw, arg}) == 1; }

// 0x8027695C: stamina changes by `delta`; a drain is adjusted.
void Change(GuestCall& game, uint32_t plw, int16_t delta) {
    if (noStaminaDrain && delta < 0) return;
    if (game.Call<MasterCheck>({plw}) == 0) return;
    if (delta < 0) {
        if (Is(game, DamageCondition, plw, kConditionA) || Is(game, DamageCondition, plw, kConditionB))
            delta = int16_t(delta * 3);
        const int32_t steps = int32_t(Memory::Read32(plw + kSteps));
        if (delta == -1) {
            if (Is(game, SkillCheck, plw, kSkillHalve) && (steps & 1) != 0) return;
        } else if (Is(game, SkillCheck, plw, kSkillHalve)) {
            delta = int16_t(delta / 2);
        }
        if (Is(game, SkillCheck, plw, kSkillDouble) && steps % 5 == 0) delta = int16_t(delta * 2);
    }
    game.Call<SetConditionState>({plw, uint32_t(int32_t(delta))});
}

// 0x80276AA0: a drain of 1 (3 in the conditions), on even steps only with the
// halving skills.
void Drain(GuestCall& game, uint32_t plw) {
    if (noStaminaDrain) return;
    int16_t delta = -1;
    if (Is(game, DamageCondition, plw, kConditionA) || Is(game, DamageCondition, plw, kConditionB)) delta = -3;
    uint16_t skipMask = Is(game, SkillCheck, plw, kSkillHalve) ? 1 : 0;
    if (Is(game, CatSkillCheck, plw, kCatSkillEvenOnly)) skipMask = skipMask != 0 ? 3 : 1;
    if (skipMask != 0 && (Memory::Read32(plw + kSteps) & skipMask) != 0) return;
    if (Is(game, SkillCheck, plw, kSkillDouble) && int32_t(Memory::Read32(plw + kSteps)) % 5 == 0)
        delta = int16_t(delta * 2);
    game.Call<SetConditionState>({plw, uint32_t(int32_t(delta))});
}

// Runs `once` for each tick of this step, with the step counter of that tick.
template <typename F> void PerTick(uint32_t plw, F once) {
    const uint32_t ticks = GameStep::Ticks();
    const uint32_t steps = Memory::Read32(plw + kSteps);
    for (uint32_t t = 0; t < ticks; ++t) {
        Memory::Write32(plw + kSteps, steps - (ticks - 1 - t));
        once();
    }
    Memory::Write32(plw + kSteps, steps);
}

} // namespace
void LauncherCheats::SetNoStaminaDrain(bool enabled) { noStaminaDrain = enabled; }

extern "C" void func_8027695C(CpuContext* ctx);
extern "C" void func_80276AA0(CpuContext* ctx);

static void StaminaChangePort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x20);
    GuestCall game{ctx};
    const uint32_t plw = frame.Arg(3);
    const int16_t delta = int16_t(frame.Arg(4));
    PerTick(plw, [&] { Change(game, plw, delta); });
}

static void StaminaDrainPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x20);
    GuestCall game{ctx};
    const uint32_t plw = frame.Arg(3);
    PerTick(plw, [&] { Drain(game, plw); });
}

extern "C" void MH3_StaminaChange(CpuContext* ctx) {
    PortCheck::Call(ctx, "StaminaChange", func_8027695C, StaminaChangePort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8027695C, MH3_StaminaChange, "StaminaChange");

extern "C" void MH3_StaminaDrain(CpuContext* ctx) {
    PortCheck::Call(ctx, "StaminaDrain", func_80276AA0, StaminaDrainPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80276AA0, MH3_StaminaDrain, "StaminaDrain");
