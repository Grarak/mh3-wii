// Enemy step timers, RMHP08. Small per-step counters and periodic checks of
// the enemy classes, each taking the enemy in r3 (the fields are named for
// what the counters do; their users are the enemy AI):
//   0x801298C8  steps (s16 +0x452), counting up to 27000
//   0x80129A3C  two countdowns (s16 +0x200, +0x202), clamped at 0
//   0x801307C4  steps (s16 +0x440), counting up to 0x7FFF
//   0x801556C8  a 50-step cycle (u16 +0x33C) and a countdown (s16 +0x338)
//   0x80133B64  while a countdown (s16 +0x94C) runs: 0x80133A24 every 32 steps
//   0x80133920  while a countdown (s16 +0x812) runs: 0x80133874 every 32 steps
//               (at 8 mod 32); once it is out, +0x814 = 0
//   0x80132D50  when 0x8012E13C(+485, +486) is 1: 0x80135A54 and 0x80107868
//               (+516) every 32 steps
//   0x8011E750  (enemy, r4, s16 n, f1 scale): every n steps 0x8011E6BC(enemy,
//               r4, s32 +0x7A4 * scale)
//   0x80130D4C  (enemy, s16 r4, s16 n, f1 scale): every n steps
//               0x80130BD0(enemy, r4, u16 +0x8A0 * scale)
//   0x801C8978, 0x8038CC84  every 32 steps while 0x8012ECC4 is 1: effect
//               (23, 28) 50 up, (24, 20) 80 up (0x80105690)
//   0x8015574C  an action (state +5): first 0x801304DC and 0x8012F690(1, 10),
//               then after 150 steps (countdown +0x20) 0x80127FAC ends it
//   0x801311B4  sight: on every other step (at the parity of u8 +2) an awake
//               enemy (+485 not 11 or 12) marks the players it sees in +910
//               (0x80131514 from its eye joint), then may notice a target
//               of the table at 0x806A6AA0
//   0x8035BFC8  every 32 steps, at the enemy's own phase (u8 +2), a living
//               enemy may pick a new target (+827); then the per-step
//               bookkeeping of +829/+831 from +1083..+1085
// The originals move the counters by 1 per step and test the step counter
// (counter % n == k). Here the counters move by the whole ticks of game time
// in the step (GameStep::Ticks()) and the tests pass once every n ticks
// (StepCounter::Every), so they keep time at any step length; at the stock
// 1/30 s step (one tick) these are the originals. A phase the enemy holds
// (counter % n == its field) is out of reach of the step counter's load hooks
// (the phase is read after the counter), so only a port can test it in ticks. (The countdowns of
// 0x80133B64 and 0x80133920 were still counting steps before these ports.)
#include "cheats.h"
#include "game_step.h"
#include "guest_call.h"
#include "memory.h"
#include "port_check.h"
#include "step_counter.h"

#include <algorithm>

namespace {

int32_t S16(uint32_t address) {
    return int16_t(Memory::Read16(address));
}

void S16(uint32_t address, int32_t value) {
    Memory::Write16(address, uint16_t(value));
}

// Counts up by the step's ticks while below `cap`, stopping at it.
void CountUp(uint32_t address, int32_t cap) {
    const int32_t steps = S16(address);

    if (steps < cap)
        S16(address, std::min(steps + int32_t(GameStep::Ticks()), cap));
}

// Counts down by the step's ticks, stopping at 0 (a count below 0 becomes 0).
void CountDown(uint32_t address) {
    S16(address, std::max(S16(address) - int32_t(GameStep::Ticks()), 0));
}

void StepsTo27000(CpuContext* ctx) {
    CountUp(ctx->gpr[3] + 0x452, 27000);
}

void TwoCountdowns(CpuContext* ctx) {
    CountDown(ctx->gpr[3] + 0x200);
    CountDown(ctx->gpr[3] + 0x202);
}

void StepsTo7FFF(CpuContext* ctx) {
    CountUp(ctx->gpr[3] + 0x440, 0x7FFF);
}

void CycleAndCountdown(CpuContext* ctx) {
    const uint32_t self = ctx->gpr[3];

    // The original: + 1, back to 0 at 50.
    const uint32_t cycle = Memory::Read16(self + 0x33C), next = cycle + GameStep::Ticks();
    Memory::Write16(self + 0x33C, uint16_t(next < 50 ? next : cycle < 50 ? next % 50 : 0));

    // The original: - 1 while above 0.
    const int32_t countdown = S16(self + 0x338);

    if (countdown > 0)
        S16(self + 0x338, std::max(countdown - int32_t(GameStep::Ticks()), 0));
}

constexpr uint32_t kSystem = 0x8065AA60; // system_w (its address is in r4 at some calls)
constexpr uint32_t CopyVec = 0x80041EA4; // (dst, src)

void SlowCheckWhileCounting(CpuContext* ctx) {
    GuestFrame frame(ctx, 16);
    GuestCall game{ctx};
    const uint32_t self = frame.Arg(3);

    if (S16(self + 0x94C) <= 0)
        return;

    if (StepCounter::Every(32))
        game.Call<0x80133A24>({self, kSystem});
    // Felvine lasts forever (cheats.h): the countdown's store dropped.
    if (Cheats::On(Cheats::Id::FelvineForever))
        return;

    // Read again: the call may change it (and leave it at 0, which the
    // original then takes 1 from as well).
    const int32_t left = S16(self + 0x94C), ticks = int32_t(GameStep::Ticks());
    S16(self + 0x94C, left > 0 ? std::max(left - ticks, 0) : left - ticks);
}

void CheckWhileCounting(CpuContext* ctx) {
    GuestFrame frame(ctx, 16);
    GuestCall game{ctx};
    const uint32_t self = frame.Arg(3);
    const int32_t countdown = S16(self + 0x812);

    if (countdown > 0) {
        S16(self + 0x812, std::max(countdown - int32_t(GameStep::Ticks()), 0));

        if (StepCounter::Every(32, 8))
            game.Call<0x80133874>({self, kSystem});
    }

    if (S16(self + 0x812) <= 0)
        S16(self + 0x814, 0);
}

void PeriodicCheck(CpuContext* ctx) {
    GuestFrame frame(ctx, 16);
    GuestCall game{ctx};
    const uint32_t self = frame.Arg(3);

    if (game.Call<0x8012E13C>({Memory::Read8(self + 485), Memory::Read8(self + 486)}) != 1)
        return;

    if (!StepCounter::Every(32))
        return;

    game.Call<0x80135A54>({self});
    game.Call<0x80107868>({self, Memory::Read32(self + 516)});
}

void EveryNScaled(CpuContext* ctx) {
    GuestFrame frame(ctx, 32);
    GuestCall game{ctx};
    const uint32_t self = frame.Arg(3), arg = frame.Arg(4);

    if (!StepCounter::Every(uint32_t(int32_t(int16_t(frame.Arg(5))))))
        return;

    const float amount = float(float(int32_t(Memory::Read32(self + 0x7A4))) * ctx->fpr[1].d);
    game.Call<0x8011E6BC>({self, arg, uint32_t(int32_t(amount))});
}

void EveryNScaledU16(CpuContext* ctx) {
    GuestFrame frame(ctx, 32);
    GuestCall game{ctx};
    const uint32_t self = frame.Arg(3), arg = uint32_t(int32_t(int16_t(frame.Arg(4))));

    if (!StepCounter::Every(uint32_t(int32_t(int16_t(frame.Arg(5))))))
        return;

    const float amount = float(float(Memory::Read16(self + 0x8A0)) * ctx->fpr[1].d);
    game.Call<0x80130BD0>({self, arg, uint32_t(int32_t(amount)) & 0xFFFF});
}

// Every 32 steps while 0x8012ECC4 is 1: the effect (kind, id) `height` above
// the enemy, at rate 1 (0x80105690).
template <uint32_t Kind, uint32_t Id, int Height>
void PeriodicEffect(CpuContext* ctx) {
    GuestFrame frame(ctx, 32);
    GuestCall game{ctx};
    const uint32_t self = frame.Arg(3), offset = frame[8];

    if (game.Call<0x8012ECC4>({self}) != 1 || !StepCounter::Every(32))
        return;

    Memory::WriteFloat32(offset, 0.0f);
    Memory::WriteFloat32(offset + 4, 0.0f);
    Memory::WriteFloat32(offset + 8, float(Height));
    game.Call<0x80105690>({self, Kind, Id, offset}, {1.0});
}

void TimedAction(CpuContext* ctx) {
    GuestFrame frame(ctx, 16);
    GuestCall game{ctx};
    const uint32_t self = frame.Arg(3);

    switch (Memory::Read8(self + 5)) {
    case 0:
        Memory::Write8(self + 5, 1);
        game.Call<0x801304DC>({self, 0});
        Memory::Write32(self + 0x20, 150);
        game.Call<0x8012F690>({self, 1, 10, 0});
        break;
    case 1: {
        // The original: - 1, the action ends once it is below 0. Here it ends
        // once the ticks pass 0 (the count stops at -1).
        const int32_t left = int32_t(Memory::Read32(self + 0x20));
        const int32_t now = std::max(left - int32_t(GameStep::Ticks()), std::min(left - 1, -1));
        Memory::Write32(self + 0x20, uint32_t(now));

        if (now < 0)
            game.Call<0x80127FAC>({self, uint32_t(left)});

        break;
    }
    }
}

// 0x8035BFC8.
void RetargetEvery32(CpuContext* ctx) {
    GuestFrame frame(ctx, 16);
    GuestCall game{ctx};
    const uint32_t self = frame.Arg(3);
    const auto u8 = [&](uint32_t offset) {
        return Memory::Read8(self + offset);
    };

    const auto set = [&](uint32_t offset, uint32_t value) {
        Memory::Write8(self + offset, uint8_t(value));
    };

    const auto alive = [&] {
        return game.Call<0x8012D090>({self}) == 0;
    };                                                         // em_die_ck
    const auto roll = [&] {                                    // 0x8012CF18 (enemy, s16 0..15, 0)
        const uint32_t pick = game.Call<0x800CEF20>({0}) & 15; // ran_suu
        game.Call<0x8012CF18>({self, pick, 0});
    };

    const auto newTarget = [&] {
        set(827, 255);
        roll();
        game.Call<0x80130790>({self, 0, 0});
    };

    game.Call<0x8035ED3C>({self});

    if (u8(10) != 1) {
        // The original: (steps & 31) == +2, never for +2 of 32 or more.
        if (alive() && u8(484) != 1 && u8(2) < 32 && StepCounter::Every(32, u8(2))) {
            if (u8(831) == 2) {
                if (game.Call<0x803BE058>({u8(827), u8(481)}) == 0 && u8(826) == 0) {
                    newTarget();
                    set(484, 0);
                    set(831, 0);
                }
            } else if (u8(1083) == 5 && u8(1084) == 1 && game.Call<0x803BE058>({u8(827), u8(481)}) == 0) {
                if (game.Call<0x8035E880>({u8(481)}) == 1)
                    game.Call<0x8035EB04>({self, 1});
                else
                    newTarget();
            }
        }

        if (u8(2099) == 1)
            game.Call<0x8012C364>({33, u8(481)});

        if (alive()) {
            if (u8(1085) == 1 && u8(829) != 1) {
                roll();

                if (u8(831) < 3)
                    set(831, 0);
            }

            if ((u8(1083) == 0 || u8(1083) == 5) && u8(1084) == 0 && u8(484) != 1 && u8(485) != 12) {
                if (game.Call<0x8035E880>({u8(481)}) == 1) {
                    set(831, 0);

                    if (u8(1083) == 0)
                        roll();

                    game.Call<0x8035EB04>({self, 0});
                } else {
                    game.Call<0x80130790>({self, 0, 0});
                }
            }
        }
    }

    set(829, u8(1085));
}

// 0x801311B4.
void Sight(CpuContext* ctx) {
    GuestFrame frame(ctx, 176);
    GuestCall game{ctx};
    const uint32_t self = frame.Arg(3);
    const auto u8 = [&](uint32_t offset) {
        return Memory::Read8(self + offset);
    };

    const uint32_t eye = frame[8], target = frame[20], look = frame[32], mtx = frame[56];
    constexpr uint32_t kPlayerSize = 2848;
    game.Call<0x80050570>({mtx});
    const uint32_t players = game.Call<0x800CFB34>({2}) & 0xFFFF; // get_move_work_max
    uint32_t player = game.Call<0x800CFAF4>({2});                 // get_move_work_adrs
    Memory::Write8(self + 910, 0);

    if (uint8_t(u8(485) - 11) <= 1)
        return;

    if (game.Call<0x8012F368>({self, 0}) == 1 || u8(504) == 1)
        return; // em_sleep_ck
    // The eye: the joint 0x80126478 names (its world matrix's position and
    // facing), or 50 above +392 with the facing at +448.
    const uint32_t joint = game.Call<0x80126478>({self});
    uint32_t facing;

    if (Memory::Read32(joint) == 0xFFFFFFFF) {
        game.Call<CopyVec>({eye, self + 392});
        Memory::WriteFloat32(eye + 4, Memory::ReadFloat32(eye + 4) + 50.0f);
        facing = Memory::Read32(self + 448);
    } else {
        game.Call<0x80135994>({self, Memory::Read32(joint), mtx}); // get_joint_wmat_em
        for (uint32_t k = 0; k < 3; ++k)
            Memory::Write32(eye + 4 * k, Memory::Read32(mtx + 12 + 16 * k));

        facing = game.Call<0x80050EC4>({mtx});
    }

    if (int16_t(Memory::Read16(self + 2326)) > 0)
        return;
    // The original: on the steps where (steps & 1) == (+2 & 1).
    if ((u8(908) & 1) && StepCounter::Every(2, u8(2) & 1)) {
        for (uint32_t i = 0; i < players; ++i, player += kPlayerSize) {
            if (Memory::Read8(player) == 0 || game.Call<0x8012D20C>({Memory::Read8(player + 8)}) != 0)
                continue;

            if (game.Call<0x8012D118>({self, player}) == 0)
                continue;

            const float range = Memory::ReadFloat32(self + 1108 + 4 * i);

            if (!(range >= 0.0f))
                continue;

            game.Call<CopyVec>({target, player + 60});
            Memory::WriteFloat32(target + 4, Memory::ReadFloat32(target + 4) + 100.0f);

            if (!(Memory::Read32(self + 456) & 0x8000) && game.Call<0x80131514>({self, joint, eye, target, facing}, {range}) != 1)
                continue;

            Memory::Write8(self + 910, uint8_t(u8(910) | (1u << i)));
            game.Call<0x80131908>({self, i & 0xFF});
        }
    }

    if (game.Call<0x8011E6A4>({self, 4}) == 0 || game.Call<0x8012D204>({self}) == 0)
        return;

    if (game.Call<0x8011E6A4>({self, 8}) == 0 && u8(482) == 4)
        return;

    if (!(u8(908) & 2))
        return;
    // The joint's 22-byte record, with 10012 in its last half-word.
    for (uint32_t k = 0; k < 20; k += 4)
        Memory::Write32(look + k, Memory::Read32(joint + k));

    Memory::Write16(look + 20, 10012);

    for (uint32_t entry = 0x806A6AA0, j = 0; j < 8; ++j, entry += 20) {
        if (Memory::Read8(entry + 19) != 1 || Memory::Read8(entry + 18) != u8(481))
            continue;

        if (game.Call<0x80131688>({look, eye, entry, facing}, {Memory::ReadFloat32(entry + 12)}) == 1) {
            game.Call<0x80131770>({self, 1});
            return;
        }
    }
}

} // namespace

extern "C" void func_801298C8(CpuContext* ctx);
extern "C" void MH3_EnemyStepsTo27000(CpuContext* ctx) {
    PortCheck::Call(ctx, "EnemyStepsTo27000", func_801298C8, StepsTo27000);
}
REGISTER_NATIVE_FUNCTION_AS(0x801298C8, MH3_EnemyStepsTo27000, "EnemyStepsTo27000");

extern "C" void func_80129A3C(CpuContext* ctx);
extern "C" void MH3_EnemyTwoCountdowns(CpuContext* ctx) {
    PortCheck::Call(ctx, "EnemyTwoCountdowns", func_80129A3C, TwoCountdowns);
}
REGISTER_NATIVE_FUNCTION_AS(0x80129A3C, MH3_EnemyTwoCountdowns, "EnemyTwoCountdowns");

extern "C" void func_801307C4(CpuContext* ctx);
extern "C" void MH3_EnemyStepsTo7FFF(CpuContext* ctx) {
    PortCheck::Call(ctx, "EnemyStepsTo7FFF", func_801307C4, StepsTo7FFF);
}
REGISTER_NATIVE_FUNCTION_AS(0x801307C4, MH3_EnemyStepsTo7FFF, "EnemyStepsTo7FFF");

extern "C" void func_801556C8(CpuContext* ctx);
extern "C" void MH3_EnemyCycleAndCountdown(CpuContext* ctx) {
    PortCheck::Call(ctx, "EnemyCycleAndCountdown", func_801556C8, CycleAndCountdown);
}
REGISTER_NATIVE_FUNCTION_AS(0x801556C8, MH3_EnemyCycleAndCountdown, "EnemyCycleAndCountdown");

extern "C" void func_80133B64(CpuContext* ctx);
extern "C" void MH3_EnemySlowCheckWhileCounting(CpuContext* ctx) {
    PortCheck::Call(ctx, "EnemySlowCheckWhileCounting", func_80133B64, SlowCheckWhileCounting);
}
REGISTER_NATIVE_FUNCTION_AS(0x80133B64, MH3_EnemySlowCheckWhileCounting, "EnemySlowCheckWhileCounting");

extern "C" void func_80133920(CpuContext* ctx);
extern "C" void MH3_EnemyCheckWhileCounting(CpuContext* ctx) {
    PortCheck::Call(ctx, "EnemyCheckWhileCounting", func_80133920, CheckWhileCounting);
}
REGISTER_NATIVE_FUNCTION_AS(0x80133920, MH3_EnemyCheckWhileCounting, "EnemyCheckWhileCounting");

extern "C" void func_80132D50(CpuContext* ctx);
extern "C" void MH3_EnemyPeriodicCheck(CpuContext* ctx) {
    PortCheck::Call(ctx, "EnemyPeriodicCheck", func_80132D50, PeriodicCheck);
}
REGISTER_NATIVE_FUNCTION_AS(0x80132D50, MH3_EnemyPeriodicCheck, "EnemyPeriodicCheck");

extern "C" void func_8011E750(CpuContext* ctx);
extern "C" void MH3_EnemyEveryNScaled(CpuContext* ctx) {
    PortCheck::Call(ctx, "EnemyEveryNScaled", func_8011E750, EveryNScaled);
}
REGISTER_NATIVE_FUNCTION_AS(0x8011E750, MH3_EnemyEveryNScaled, "EnemyEveryNScaled");

extern "C" void func_80130D4C(CpuContext* ctx);
extern "C" void MH3_EnemyEveryNScaledU16(CpuContext* ctx) {
    PortCheck::Call(ctx, "EnemyEveryNScaledU16", func_80130D4C, EveryNScaledU16);
}
REGISTER_NATIVE_FUNCTION_AS(0x80130D4C, MH3_EnemyEveryNScaledU16, "EnemyEveryNScaledU16");

extern "C" void func_801C8978(CpuContext* ctx);
extern "C" void MH3_EnemyPeriodicEffect50(CpuContext* ctx) {
    PortCheck::Call(ctx, "EnemyPeriodicEffect50", func_801C8978, PeriodicEffect<23, 28, 50>);
}
REGISTER_NATIVE_FUNCTION_AS(0x801C8978, MH3_EnemyPeriodicEffect50, "EnemyPeriodicEffect50");

extern "C" void func_8038CC84(CpuContext* ctx);
extern "C" void MH3_EnemyPeriodicEffect80(CpuContext* ctx) {
    PortCheck::Call(ctx, "EnemyPeriodicEffect80", func_8038CC84, PeriodicEffect<24, 20, 80>);
}
REGISTER_NATIVE_FUNCTION_AS(0x8038CC84, MH3_EnemyPeriodicEffect80, "EnemyPeriodicEffect80");

extern "C" void func_8015574C(CpuContext* ctx);
extern "C" void MH3_EnemyTimedAction(CpuContext* ctx) {
    PortCheck::Call(ctx, "EnemyTimedAction", func_8015574C, TimedAction);
}
REGISTER_NATIVE_FUNCTION_AS(0x8015574C, MH3_EnemyTimedAction, "EnemyTimedAction");

extern "C" void func_8035BFC8(CpuContext* ctx);
extern "C" void MH3_EnemyRetargetEvery32(CpuContext* ctx) {
    PortCheck::Call(ctx, "EnemyRetargetEvery32", func_8035BFC8, RetargetEvery32);
}
REGISTER_NATIVE_FUNCTION_AS(0x8035BFC8, MH3_EnemyRetargetEvery32, "EnemyRetargetEvery32");

extern "C" void func_801311B4(CpuContext* ctx);
extern "C" void MH3_EnemySight(CpuContext* ctx) {
    PortCheck::Fuzz(ctx, 'E', ctx->gpr[3]);
    PortCheck::Call(ctx, "EnemySight", func_801311B4, Sight);
}
REGISTER_NATIVE_FUNCTION_AS(0x801311B4, MH3_EnemySight, "EnemySight");
