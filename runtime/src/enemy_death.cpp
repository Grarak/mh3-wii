// A monster's death and capture action, RMHP08. C reconstructions from the
// translated code.
//
//   0x80124CC0 EnemyDownAction(work, action, how): the action's start (its
//              motion, timers +0x8D0..+0x914) then each step its body
//              (EnemyDownStep) and, once done, the next state of +0x8D2 when
//              it fell (+0x1E5 11): 0 the carcass with its carve spot
//              (+0x888): it lies until carved out, or with a time limit
//              (+0x20: 2100 frames in the down actions 11.26..11.28, else
//              the enemy data's +0x5C), 1 a 100-frame wait, 2 the 60-frame fade (0x8012FD28),
//              3 gone (0x80128A94 / 0x801289FC) or left lying (+0x17 = 3);
//              its other actions end their own way. The effect pulses every
//              64 steps (the step counter)
//   0x801249F0 EnemyDownStep(work, action, how): one step of it; 1 when the
//              state is over (the carcass's time ran out or its spots were
//              all carved)
//   0x80124948 EnemyCarcassTimed(work): 1 when a fallen monster's carcass
//              gets a time limit instead of lying until carved out: the
//              field's stop mode, a small monster, 0x8012E76C, the marathon
//              flag, or with 0x02000000 / the timed flag (or as a quest
//              target, +0xD) when 0x803B55F8 holds (an intruder chance)
#include "abi_bridge.h"
#include "guest_call.h"
#include "port_check.h"
#include "ppc_runtime.h"

#include <cstdint>

namespace {
using MemoryInline::FlatRead16;
using MemoryInline::FlatRead32;
using MemoryInline::FlatRead8;
using MemoryInline::FlatWrite16;
using MemoryInline::FlatWrite32;
using MemoryInline::FlatWrite8;

constexpr uint32_t kZero = 0x80799100;     // f32 0 (r2 - 0x6E60)
constexpr uint32_t kMinusOne = 0x80799108; // f32 -1 (r2 - 0x6E58)
constexpr uint32_t kRestScale = 0x80799118; // f32 (r2 - 0x6E48)
constexpr uint32_t kSystem = 0x8065AA60;

enum : uint32_t {
    kAlpha = 0x1D4, // f32 visibility (faded out at 0: the work stops, +1 = 0)
    kCountdown = 0x20,
    kCarveSpot = 0x888,
    kState = 0x8D2,
};

float Zero() {
    return Memory::ReadFloat32(kZero);
}

uint32_t EnemyCarcassTimed(GuestCall& game, uint32_t work) {
    if (game.Call<0x800CF2E4>() == 1 || !(FlatRead32(work + 456) & 1) || game.Call<0x8012E76C>({work}) == 1 || game.Call<0x803B4DC4>({0}) == 1)
        return 1;

    if (game.Call<0x803B52F8>({0}) == 1 && game.Call<0x803B55F8>() == 1)
        return 1;

    return int8_t(FlatRead8(work + 13)) != 0 && game.Call<0x803B55F8>() == 1;
}

// 0x8012E30C, inlined: in the down actions 11.26..11.28.
bool DownLate(uint32_t work) {
    return FlatRead8(work + 485) == 11 && uint8_t(FlatRead8(work + 486) - 26) <= 2;
}

// 0x8012FD28, inlined: the fade's target and its steps.
void Fade(uint32_t work, float target, uint32_t steps) {
    Memory::WriteFloat32(work + 472, target);
    FlatWrite16(work + 476, uint16_t(steps));

    if (int16_t(steps) == 0)
        Memory::WriteFloat32(work + kAlpha, target);
}

uint32_t Countdown(uint32_t work, uint32_t site) {
    const uint32_t left = MH3_TickLoad(FlatRead32(work + kCountdown), site) - 1;
    FlatWrite32(work + kCountdown, left);
    return left;
}

uint32_t EnemyDownStep(GuestCall& game, uint32_t work, uint32_t action, uint32_t how) {
    bool over = false;

    switch (FlatRead8(work + kState)) {
    case 1:
        if (EnemyCarcassTimed(game, work) == 0 && !DownLate(work)) {
            if (int32_t(FlatRead32(work + kCarveSpot)) != -1 && int32_t(game.Call<0x803B9AA4>({FlatRead32(work + kCarveSpot)})) <= 0) {
                game.Call<0x803B99F8>({FlatRead32(work + kCarveSpot)});
                FlatWrite32(work + kCarveSpot, 0xFFFFFFFF);
            }

            break;
        }

        // A small monster's carve spots all carved ends it at once.
        if (!(FlatRead32(work + 456) & 1) && int32_t(FlatRead32(work + kCarveSpot)) != -1) {
            uint32_t done = int32_t(game.Call<0x803B9AA4>({FlatRead32(work + kCarveSpot)})) <= 0;

            if (done) {
                for (uint32_t i = 0; i < 2; ++i) {
                    const uint32_t spot = work + 1160 + i * 132;

                    if (int32_t(FlatRead32(spot + 128)) == -1)
                        continue;

                    if (int32_t(game.Call<0x803B9AA4>({FlatRead32(spot + 128)})) > 0) {
                        done = 0;
                        break;
                    }

                    done = 1;
                }
            }

            if (done == 1)
                over = true;
        }

        if (int32_t(Countdown(work, 0x80124B78)) <= 0)
            over = true;

        break;
    case 2:
        if (Memory::ReadFloat32(work + kAlpha) <= Zero())
            FlatWrite8(work + 1, 0);

        if (int32_t(Countdown(work, 0x80124BB0)) <= 0)
            over = true;

        break;
    case 3:
        if (int32_t(Countdown(work, 0x80124BCC)) <= 0) {
            Fade(work, Zero(), 0);
            over = true;
        }

        if (Memory::ReadFloat32(work + kAlpha) <= Zero())
            FlatWrite8(work + 1, 0);

        break;
    default:
        if (FlatRead8(work + 485) == 11)
            FlatWrite16(work + 2324, uint16_t(MH3_TickLoad(uint32_t(int32_t(int16_t(FlatRead16(work + 2324)))), 0x80124A3C) + 1));

        if (!(Memory::ReadFloat32(work + kAlpha) <= Zero())) {
            const uint32_t id = action && FlatRead8(action) == 21 ? FlatRead32(action + 4) & 0xFFFF : FlatRead16(FlatRead32(work + 2272) + 16);

            if (game.Call<0x80124604>({work, action, how & 0xFF, id}) == 1)
                over = true;
        } else {
            over = true;
        }

        break;
    }

    if (over) {
        if (FlatRead8(work + kState) == 0) {
            FlatWrite8(work + 2257, FlatRead8(work + 2257) & 0x99);
            Memory::WriteFloat32(work + 2284, Zero());
            Memory::WriteFloat32(work + 2288, Zero());
            FlatWrite32(work + 2292, 0);
        }

        return 1;
    }

    if ((FlatRead8(work + 2257) & 1) && game.Call<0x8012F9A0>({work}) == 1) {
        const uint32_t motion = FlatRead32(work + 2272);
        game.Call<0x8012F568>({work, FlatRead16(motion), 0, FlatRead32(motion + 8), FlatRead32(motion + 12)});

        if (Zero() < Memory::ReadFloat32(work + 2304))
            game.Call<0x8012F92C>({work});
    }

    return 0;
}

// The next state once a part of the down action ended (+0x1E5 11).
void DownNext(GuestCall& game, uint32_t work) {
    const uint8_t state = FlatRead8(work + kState);

    switch (state) {
    case 0:
        FlatWrite8(work + kState, 1);

        if (!(Memory::ReadFloat32(work + kAlpha) <= Zero()) || (FlatRead32(work + 456) & 0x800)) {
            if (EnemyCarcassTimed(game, work) == 0 && DownLate(work))
                FlatWrite32(work + kCountdown, 2100);
            else
                FlatWrite32(work + kCountdown, FlatRead32(game.Call<0x80126098>({work}) + 92));

            if ((game.Call<0x8012E8E8>({work}) & 0xFF) == 0) {
                if (int32_t(FlatRead32(work + kCarveSpot)) == -1)
                    game.Call<0x80123218>({work});
            } else if (int32_t(FlatRead32(work + kCarveSpot)) != -1) {
                game.Call<0x803B99F8>({FlatRead32(work + kCarveSpot)});
                FlatWrite32(work + kCarveSpot, 0xFFFFFFFF);
            }
        } else {
            FlatWrite32(work + kCountdown, 0);

            if (int32_t(FlatRead32(work + kCarveSpot)) != -1) {
                game.Call<0x803B99F8>({FlatRead32(work + kCarveSpot)});
                FlatWrite32(work + kCarveSpot, 0xFFFFFFFF);
            }
        }

        FlatWrite16(work + 24, uint16_t(game.Call<0x8012680C>({work})));
        break;
    case 1:
        FlatWrite8(work + kState, 2);
        FlatWrite32(work + kCountdown, Memory::ReadFloat32(work + kAlpha) <= Zero() ? 0 : 100);
        game.Call<0x803B9A24>({work});
        break;
    case 2:
        FlatWrite8(work + kState, 3);

        if (Memory::ReadFloat32(work + kAlpha) <= Zero()) {
            FlatWrite32(work + kCountdown, 0);
        } else {
            FlatWrite32(work + kCountdown, 60);
            Fade(work, Zero(), 60);
        }

        break;
    case 3:
        if (game.Call<0x8012E76C>({work}) == 1) {
            FlatWrite8(work + 487, 0);

            if (FlatRead32(work + 456) & 1)
                game.Call<0x801289FC>({work, 12, 255});
            else
                game.Call<0x80128A94>({work, 12, 255});
        } else {
            FlatWrite8(work + 23, 3);
        }

        break;
    default:
        break;
    }
}

void EnemyDownAction(GuestCall& game, uint32_t sp, uint32_t work, uint32_t action, uint32_t how) {
    if (FlatRead8(work + 5) == 0) {
        FlatWrite8(work + 5, uint8_t(FlatRead8(work + 5) + 1));

        if ((how & 0xFF) == 1)
            game.Call<0x801306A0>({work, FlatRead32(work + 516), how});
        else
            game.Call<0x801304DC>({work, how & 0xFF});

        game.Call<0x801232F8>({work});
        FlatWrite8(work + 2256, 0);
        FlatWrite32(work + 2260, action);
        FlatWrite32(work + 2264, 0);
        FlatWrite32(work + 2268, 0);
        FlatWrite32(work + 2272, 0);
        FlatWrite32(work + 2276, 0);
        Memory::WriteFloat32(work + 2280, Zero());
        FlatWrite8(work + 2257, 0);
        Memory::WriteFloat32(work + 2284, Zero());
        Memory::WriteFloat32(work + 2288, Zero());
        FlatWrite32(work + 2292, 0);
        Memory::WriteFloat32(work + 2312, Zero());
        Memory::WriteFloat32(work + 2316, Zero());
        FlatWrite32(work + 2296, FlatRead32(work + 448));
        FlatWrite8(work + kState, 0);
        game.Call<0x801302AC>({work});
        Memory::WriteFloat32(work + 2300, float(game.cpu->fpr[1].d));
        Memory::WriteFloat32(work + 2304, Memory::ReadFloat32(kMinusOne));
        Memory::WriteFloat32(work + 2308, Zero());
        FlatWrite16(work + 2324, 0);
        FlatWrite32(work + 2320, 0);
        game.Call<0x80123734>({work, FlatRead32(work + 2260), 1});
        game.Call<0x8013AB28>({work});
    } else {
        const uint32_t pose = game.Call<0x801241B4>({work}) & 0xFF;
        uint32_t over = game.Call<0x801249F0>({work, FlatRead32(work + 2260), pose});

        if (over == 0 && FlatRead32(work + 2276) != 0 && game.Call<0x801235E8>({work}) == 1)
            over = 1;

        if (over == 1) {
            Memory::WriteFloat32(work + 2308, Zero());

            if (FlatRead32(work + 2276) != 0)
                FlatWrite32(work + 2276, 0);

            const uint32_t next = game.Call<0x80123734>({work, FlatRead32(work + 2260), 0}) & 0xFF;

            if (next == 0) {
                FlatWrite8(work + 5, uint8_t(FlatRead8(work + 5) + 1));
            } else if (next != 2) {
                // 0x8013545C, inlined: the rest scales.
                for (uint32_t i = 0; i < 6; ++i)
                    Memory::WriteFloat32(work + 784 + i * 4, Memory::ReadFloat32(kRestScale));

                if (FlatRead8(work + 485) == 11) {
                    DownNext(game, work);
                } else {
                    switch (int8_t(FlatRead8(work + 482))) {
                    case 1:
                        if (int8_t(FlatRead8(work + 483)) == 1)
                            game.Call<0x801281F8>({work});
                        else if (int8_t(FlatRead8(work + 483)) == 2)
                            game.Call<0x801281A0>({work});
                        else
                            game.Call<0x80128048>({work});

                        break;
                    case 2:
                        game.Call<0x80128094>({work});
                        break;
                    case 3:
                        game.Call<0x80128110>({work});
                        break;
                    case 4:
                        game.Call<0x80128158>({work});
                        break;
                    default:
                        game.Call<0x80127FAC>({work}); // EnemyActionDone
                        break;
                    }
                }
            }
        }
    }

    if (FlatRead8(work + 482) == 1)
        game.Call<0x8012FE04>({work}, {double(Memory::ReadFloat32(work + 2300))});

    if (game.Call<0x8012D934>({work}) == 1 || game.Call<0x8012DBA0>({work}) == 1 || game.Call<0x8012DDBC>({work}) == 1)
        FlatWrite16(work + 2244, 1); // 0x80131E64, inlined

    if (const uint32_t callback = FlatRead32(work + 2320))
        game(callback, {work});

    if (game.Call<0x8012D0D8>({work}) != 1) // em_area_ck
        return;

    // em_act_ck, inlined: the actions 10.186 and 11.28.
    const uint8_t mode = FlatRead8(work + 485), sub = FlatRead8(work + 486);

    if (!((mode == 10 && sub == 186) || (mode == 11 && sub == 28)))
        return;

    const uint32_t phase = ((MH3_HookedLoad32(kSystem, 12, 0x8012518C) & 127) + FlatRead8(work + 2) * 31) & 127;
    const uint32_t which = phase == 0 ? 0 : phase == 64 ? 1 : 255;

    if (which == 255)
        return;

    uint32_t joint = FlatRead32(FlatRead32(FlatRead32(game.Call<0x80126098>({work}) + 156) + 12));

    if (joint == 0xFFFFFFFF)
        joint = 2;

    game.Call<0x8013599C>({work, joint, sp + 8}); // get_joint_wpos_em
    game.Call<0x800DCAFC>({FlatRead32(work + 2836), sp + 8, which});
}

// Ports.

void EnemyCarcassTimedPort(CpuContext* ctx) {
    const uint32_t work = ctx->gpr[3];
    uint32_t result;

    {
        GuestFrame frame(ctx, 0x10);
        GuestCall game{ctx};
        result = EnemyCarcassTimed(game, work);
    }

    ctx->gpr[3] = result;
}

void EnemyDownStepPort(CpuContext* ctx) {
    const uint32_t work = ctx->gpr[3], action = ctx->gpr[4], how = ctx->gpr[5];
    uint32_t result;

    {
        GuestFrame frame(ctx, 0x20);
        GuestCall game{ctx};
        result = EnemyDownStep(game, work, action, how);
    }

    ctx->gpr[3] = result;
}

void EnemyDownActionPort(CpuContext* ctx) {
    const uint32_t work = ctx->gpr[3], action = ctx->gpr[4], how = ctx->gpr[5];
    GuestFrame frame(ctx, 0x30);
    GuestCall game{ctx};
    EnemyDownAction(game, frame[0], work, action, how);
}
} // namespace

extern "C" void func_80124948(CpuContext* ctx);
extern "C" void MH3_EnemyCarcassTimed(CpuContext* ctx) {
    PortCheck::Call(ctx, "EnemyCarcassTimed", PortCheck::Returns::Gpr3, func_80124948, EnemyCarcassTimedPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80124948, MH3_EnemyCarcassTimed, "EnemyCarcassTimed");

extern "C" void func_801249F0(CpuContext* ctx);
extern "C" void MH3_EnemyDownStep(CpuContext* ctx) {
    PortCheck::Call(ctx, "EnemyDownStep", PortCheck::Returns::Gpr3, func_801249F0, EnemyDownStepPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x801249F0, MH3_EnemyDownStep, "EnemyDownStep");

extern "C" void func_80124CC0(CpuContext* ctx);
extern "C" void MH3_EnemyDownAction(CpuContext* ctx) {
    PortCheck::Call(ctx, "EnemyDownAction", func_80124CC0, EnemyDownActionPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80124CC0, MH3_EnemyDownAction, "EnemyDownAction");
