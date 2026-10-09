// Monster waits and repeats counted per call, RMHP08. C reconstructions
// (decompiled with Ghidra) of twenty-six enemy actions that keep a count at +0x20
// by one a call, which the enemy update calls once per step:
//
//   waits (a countdown that ends the action or a state; a gameplay change at
//   short steps: the monster recovered, gave up or moved on early):
//     0x8015611C  kind 3 (Qurupeco)    a stagger, 110 or 220 ticks over the rate
//     0x80168430  kind 10 (Jaggi) a turn, at most 60-123 ticks
//     0x8019474C  em018 (Gobul)      a lunge, 180 ticks
//     0x801ACA48  em027 (Aptonoth)   a rest, 30-93 ticks
//     0x801B2578  kinds 30/31 (Felyne, Melynx)  a charge, 80 ticks
//     0x801D91B4  em007 (Deviljho)   a charge (7.3, 7.18), at most 1000 ticks
//     0x801482AC  kind 1 (Rathian)   a wait (0.0, 0.4), 150 ticks
//     0x80159054  kind 3 (Qurupeco)  a move, at least 10 or 20 ticks
//     0x8015898C  kind 3 (Qurupeco)  a backward hop, 22 or 30 ticks
//     0x80171B7C  kind 12 (Great Jaggi) a retreat, 15 ticks before it may end
//     0x8037A8AC  kind 19 (Agnaktor) under the ground: 15, 60 or 90 ticks, a mark every 8
//     0x801AC674  kind 27 (Aptonoth) a walk, 256-511 ticks
//     0x801B1228  kind 30 (Felyne)   a rest, 150 or 250 ticks and up to 63 more
//     0x801B1708  kind 30 (Felyne)   a run, at most 60 or 150 ticks (15 when near)
//     0x801BD724  kind 6 (Diablos)   under the sand: 45 or 80 ticks, a burst every 32
//     0x801BD89C  kind 6 (Diablos)   a move under the sand, at least 20 ticks
//     0x801B795C, 0x801B867C  hops (the dispatcher 0x801B94B4, after kind
//                 34 Kelbi's code): a 10-tick hold on landing
//     0x8034FA1C  em024 (Alatreon)   a flight, 300 ticks (an effect at 20 left)
//     0x8039E9EC  em028 (Popo)       a rest, 150 or 300 ticks
//     0x801C7570  kind 4 (Gigginox)    an ambush from the sand, 90 ticks
//     0x80372994  em020 (Ceadeus)    a rise of 214 and an arch of 124 ticks,
//                 its pitch turning by the step's share of a tick's amount
//   repeats (an effect every N calls while the motion is in a range):
//     0x8017AA5C  em015 (Lagiacrus)  sparks every 5
//     0x8019F3DC  em025 (Jhen Mohran) its roar's steam every 10
//     0x803519B8  em024 (Alatreon)   bursts every 7
//     0x8035455C  em024 (Alatreon)   its charge's sparks every 6
//
// The counts take the step's whole ticks: the waits last as many ticks as
// on the Wii, the repeats come on the step whose ticks include a multiple of
// N (Em::Enemy::CountEvery; Alatreon's two set their counts back to 0 at
// the repeat, as the originals do). A test of a countdown's exact value (`== 20`) passes on
// the step that reaches or crosses it.
//
// At the stock 1/30 s step these are the originals (MH3_PORT_CHECK=Em003Stagger,
// ... MH3_VARIABLE_STEP=0; the ones of kinds no arena has through the enemy
// fuzz's A: entries).
//
// The kinds (work +3) were relabeled on 2026-10-06 by the dispatcher that
// reaches each function (docs/monster-control.md); the descriptions of what a
// monster does were written under the earlier labels, wrong for kinds 1-12.
#include "abi_bridge.h"
#include "enemy_work.h"
#include "game_step.h"
#include "guest_call.h"
#include "memory.h"
#include "port_check.h"

#include <algorithm>
#include <cstdint>

namespace {

enum : uint32_t {
    kVelocity = 0x310,   // VEC3 (then the acceleration, VEC3)
    kSpeed = 0x314,      // f32: the velocity's y, here the forward speed
    kSideSpeed = 0x318,  // f32
    kYaw = 0x1C0,        // s32 angle units
    kRandom = 0x1EC,     // u16: the monster's random word
    kMotionRate = 0x80,  // f32: motion channel 0's frames per step (MHchar +0x5C; 0x8012F948, inlined)
    kCharging = 0x330,   // u8: kinds 30/31's charge hit is live
    kGulp = 0x79B,       // u8: em018 (0x8012CF84, inlined)
    kJhenFlag = 0x116,   // u8: em025 (0x801321B8, inlined)
    kRestDone = 0x43F,   // u8: em028 (0x801376A0, inlined)
};

enum : uint32_t {
    RoarVoice = 0x801293A0,      // (work, channel, voice, length)
    RoarVoice0 = 0x801296CC,     // (work, channel, voice): RoarVoice with length 0
    SetMotionEx = 0x8012F568,    // (work, motion, blend, flags, 1)
    SetMotionLoop = 0x8012F690,  // (work, motion, flags, blend)
    SetMotionSpeed = 0x8012F92C, // (work; f1)
    SetUpCarried = 0x801302AC,   // (work) -> f1, then SetScale takes that f1
    SetScale = 0x80130628,       // (work; f1)
    Steer = 0x80133CB4,          // (work, rate)
    TurnSetUp = 0x80134068,      // (work, 0; f1)
    TurnStep = 0x80134178,       // (work, 0, 0) -> 1 when turned
    Mark = 0x80136BB4,           // (work, a, b)
    AddVelocity = 0x8013547C,    // EnemyAddVelocity (work): position += velocity · s
    RotateY = 0x800510C8,        // rotVecY (VEC3*, angle)
    NextAction = 0x80128A78,     // (work, group, action)
    ActionDone = 0x80127FAC,     // (work): back to the AI
    ActionDone94 = 0x80128094,   // a variant of it with another set-up
    JointEffect = 0x80104918,    // (work, kind, joint, angle; f1 scale)
    JointPosition = 0x8013599C,  // get_joint_wpos_em (work, joint, VEC3* out)
    Spark = 0x80306AFC,          // (work, kind)
    Discharge = 0x8010A838,      // (work, kind): 0/7 and 1 start an effect, others nothing
};

constexpr uint32_t kShellTable = 0x80797020; // shell_set_func_ptr: the shell functions' table

// The countdown at +0x20 by the step's ticks; true once it is below 1.
bool CountDown(const Em::Enemy& em) {
    em.Count(em.Count() - int32_t(GameStep::Ticks()));
    return em.Count() < 1;
}

// 0x8013545C (work), inlined: the velocity and acceleration cleared.
void ClearMotion(uint32_t work) {
    for (uint32_t i = 0; i < 6; ++i)
        Memory::WriteFloat32(work + kVelocity + 4 * i, 0.0f);
}

// 0x80135448 (work), inlined: the velocity cleared.
void ClearVelocity(uint32_t work) {
    for (uint32_t i = 0; i < 3; ++i)
        Memory::WriteFloat32(work + kVelocity + 4 * i, 0.0f);
}

// kind 3 0x8015611C (r4: 1 slowed): a stagger whose length is 110 ticks (220
// with a broken head) over the motion's rate, counted down in state 2.
void Em003StaggerPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x20);
    GuestCall game{ctx};
    const Em::Enemy em{game, frame.Arg(3)};
    const uint32_t work = em.work;
    const bool slowed = uint8_t(frame.Arg(4)) == 1;

    // 2 / the motion's rate (single precision, as the original's fdivs path).
    const auto perRate = [&](float amount) { return float(double(amount) / double(Memory::ReadFloat32(work + kMotionRate))); };

    const auto slow = [&] {
        if (slowed)
            game.Call<SetMotionSpeed>({work}, {2.0f * perRate(1.5f)});
    };

    switch (em.State()) {
    case 0: {
        em.State(1);
        game.Call<Em::SetUp>({work, 0});
        em.SetMotion(200, 4, 0);
        slow();
        em.Count(0x6E);

        if (game.Call<0x8015D674>({work}) == 1) // em003_atama_koware_act_ck: the head is broken
            em.Count(em.Count() << 1);

        em.Count(int32_t(float(em.Count()) * perRate(2.0f)));
        break;
    }
    case 1:
        if (em.At(68.0f, 1)) {
            em.NextState();
            Memory::Write8(work + Em::kSubState, 0);
            em.SetMotion(0xCA, 0, 0);
            slow();
        }

        break;
    case 2:
        if (CountDown(em))
            game.Call<NextAction>({work, 1, 0x16});

        break;
    }
}

// kind 10 0x80168430 (r4: 0 a timed turn, 1 one motion long).
void Em010TurnPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x10);
    GuestCall game{ctx};
    const Em::Enemy em{game, frame.Arg(3)};
    const uint32_t work = em.work;
    const bool timed = uint8_t(frame.Arg(4)) == 0;

    if (em.State() == 0) {
        em.State(1);
        game.Call<Em::SetUp>({work, 0});
        game.Call<SetMotionLoop>({work, 0xCB, 4, 0});
        game.Call<SetMotionSpeed>({work}, {1.7f});
        game.Call<TurnSetUp>({work, 0}, {timed ? -500.0f : -100.0f});
        em.Count(int16_t((Memory::Read16(work + kRandom) & 0x3F) + 0x3C));
        return;
    }

    if (em.State() != 1)
        return;

    if (em.At(70.0f, 3, 146.0f) || em.At(254.0f, 3, 350.0f))
        game.Call<Steer>({work, 0x200});
    else if (em.At(44.0f, 1))
        game.Call<Steer>({work, 0x40});

    if (timed) {
        em.Count(em.Count() - int32_t(GameStep::Ticks()));

        if (game.Call<TurnStep>({work, 0, 0}) == 1 || em.Count() < 0) {
            game.Call<SetMotionSpeed>({work}, {1.0f});
            game.Call<ActionDone>({work});
        }
    } else if (em.Ended()) {
        game.Call<SetMotionSpeed>({work}, {1.0f});
        game.Call<ActionDone>({work});
    }
}

// em018 0x8019474C (r4: 1 gulping): a lunge that moves on from frame 34 for
// at most 180 ticks.
void Em018LungePort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x10);
    GuestCall game{ctx};
    const Em::Enemy em{game, frame.Arg(3)};
    const uint32_t work = em.work;

    if (uint8_t(frame.Arg(4)) == 1)
        Memory::Write8(work + kGulp, 1);

    switch (em.State()) {
    case 0:
        em.State(1);
        game.Call<Em::SetUp>({work, 2});
        em.SetMotion(200, 10, 0);
        game.Call<SetMotionSpeed>({work}, {2.0f});
        ClearVelocity(work);
        Memory::WriteFloat32(work + kSpeed, -15.0f);
        em.Count(0xB4);
        break;
    case 1:
        if (em.At(34.0f))
            game.Call<SetMotionSpeed>({work}, {1.0f});

        if (!em.At(34.0f, 1))
            break;

        game.Call<AddVelocity>({work});
        game.Call<0x80130348>({work});

        if (game.Call<0x8013006C>({work}) == 1) {
            em.NextState();
            game.Call<Em::SetUp>({work, 0});
            em.SetMotion(0xC9, 0, 0);
        } else if (CountDown(em)) {
            game.Call<ActionDone94>({work});
        }

        break;
    case 2:
        if (em.Ended())
            game.Call<ActionDone>({work});

        break;
    }
}

// em027 0x801ACA48: a rest of 30 + (random & 63) ticks after frame 94.
void Em027RestPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x10);
    GuestCall game{ctx};
    const Em::Enemy em{game, frame.Arg(3)};
    const uint32_t work = em.work;

    switch (em.State()) {
    case 0:
        em.State(1);
        game.Call<Em::SetUp>({work, 0});
        em.SetMotion(0x1B, 4, 0);
        break;
    case 1:
        game.Call<Steer>({work, 0x100});

        if (em.At(94.0f, 1)) {
            em.NextState();
            em.SetMotion(4, 2, 0x14);
            game.Call<SetMotionSpeed>({work}, {1.2f});
            em.Count(int32_t((Memory::Read16(work + kRandom) & 0x3F) + 0x1E));
        }

        break;
    case 2:
        game.Call<Steer>({work, 0x100});

        if (CountDown(em))
            game.Call<ActionDone>({work});

        break;
    }
}

// kinds 30/31 0x801B2578 (r4: 1 the other horn): a charge of 80 ticks.
void Em030ChargePort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x10);
    GuestCall game{ctx};
    const Em::Enemy em{game, frame.Arg(3)};
    const uint32_t work = em.work;

    switch (em.State()) {
    case 0:
        em.State(1);
        game.Call<Em::SetUp>({work, 0});
        em.SetMotion(0x11, 2, 0);
        em.Count(0x50);
        break;
    case 1:
        if (em.At(20.0f)) {
            const uint32_t fn = Memory::Read32(Memory::Read32(kShellTable) + 0x58);
            game(fn, {work, uint8_t(frame.Arg(4)) == 1 ? 3u : 2u});
        }

        if (em.At(20.0f, 1))
            Memory::Write8(work + kCharging, 1);

        if (CountDown(em)) {
            em.NextState();
            em.SetMotion(0x12, 4, 0);
        }

        break;
    case 2:
        if (em.At(46.0f, 2))
            Memory::Write8(work + kCharging, 1);

        if (em.Ended())
            game.Call<ActionDone>({work});

        break;
    }
}

// kind 1 0x801482AC: Rathian's waiting actions 0.0 and 0.4, motion 1
// looped for 150 ticks (ended when the count goes below 0), then back to
// the AI's waiting (0x80127FAC).
void Em001WaitPort(CpuContext* ctx) {
    GuestCall game{ctx};
    const Em::Enemy em{game, ctx->gpr[3]};
    const uint32_t work = em.work;

    switch (em.State()) {
    case 0:
        em.State(1);
        game.Call<Em::SetUp>({work, 0});
        em.Count(0x96);
        game.Call<SetMotionLoop>({work, 1, 10, 0});
        break;
    case 1:
        em.Count(em.Count() - int32_t(GameStep::Ticks()));

        if (em.Count() < 0)
            game.Call<ActionDone>({work});

        break;
    }
}

// kind 3 0x80159054 (r4 0-5): Qurupeco's move of a set distance (300,
// -250, -100 with at most 800 left, -1600, -500, 1500 through 0x80134068),
// motion 8 and voice 4; it lasts at least 10 or 20 ticks (the count at
// +0x20, down to 0) and until the move is done (0x80134178, turning at
// 0x80 or 0x40), then its end motion or a follow-up (4.7, 7.7).
void Em003MovePort(CpuContext* ctx) {
    GuestCall game{ctx};
    const Em::Enemy em{game, ctx->gpr[3]};
    const uint32_t work = em.work;
    const uint8_t kind = uint8_t(ctx->gpr[4]);
    const auto setUp = [&](float distance, uint32_t flags, int32_t ticks) {
        game.Call<TurnSetUp>({work, flags}, {double(distance)});

        if (ticks)
            em.Count(ticks);
    };

    switch (em.State()) {
    case 0:
        em.State(1);
        game.Call<Em::SetUp>({work, 0});
        game.Call<0x80130D40>({work, uint32_t(-12)}); // stamina (+0x8A0) less 12
        em.SetMotion(8, 4, 0);
        game.Call<RoarVoice>({work, 0, 4, 3});
        em.Count(0);

        switch (kind) {
        case 1:
            setUp(-250.0f, 0x10, 10);
            break;
        case 2:
            setUp(-100.0f, 0, 0);

            if (Memory::ReadFloat32(work + 0x378) >= 800.0f)
                Memory::WriteFloat32(work + 0x378, 800.0f);

            break;
        case 3:
            setUp(-1600.0f, 0x10, 0);
            break;
        case 4:
            setUp(-500.0f, 0x10, 10);
            break;
        case 5:
            setUp(1500.0f, 0x10, 20);
            break;
        default:
            setUp(300.0f, 0x10, 20);
            break;
        }

        break;
    case 1:
        // The count down by the step's ticks, to 0 (the original's 1 a call).
        em.Count(std::max(em.Count() - int32_t(GameStep::Ticks()), 0));

        if (game.Call<TurnStep>({work, 0, kind == 0 ? 0x80u : 0x40u}) == 1 && em.Count() <= 0) {
            em.NextState();
            Memory::Write8(work + 6, 0);

            if (kind == 2)
                em.SetMotion(0x40, 4, 0);
            else if (kind == 3 || kind == 4) {
                game.Call<0x80129788>({work, 0});
                game.Call<0x80128AD4>({work, kind == 3 ? 4u : 7u, 7});
            } else {
                game.Call<0x80129788>({work, 0});
                em.SetMotion(9, 4, 0);
            }
        }

        break;
    case 2:
        if (kind == 2 && Memory::Read8(work + 6) == 0 && em.At(120.0f, 1)) {
            Memory::Write8(work + 6, 1);
            game.Call<0x80129788>({work, 0});
        }

        if (em.Ended())
            game.Call<ActionDone>({work});

        break;
    }
}

// kind 3 0x8015898C (r4 0-2): Qurupeco's backward hop, motion 0x1B: the
// speed -20 (-22 for 1 and 2) easing by 0.3 (0.15) a tick (0x8013562C, by
// the step already), at most 0, for 22 (30) ticks; then 4.1 or its end.
void Em003BackstepPort(CpuContext* ctx) {
    GuestCall game{ctx};
    const Em::Enemy em{game, ctx->gpr[3]};
    const uint32_t work = em.work;
    const uint8_t kind = uint8_t(ctx->gpr[4]);

    switch (em.State()) {
    case 0:
        em.State(1);
        game.Call<Em::SetUp>({work, 3});
        em.SetMotion(0x1B, 6, 0);
        game.Call<0x8013545C>({work});

        if (kind == 1 || kind == 2) {
            Memory::WriteFloat32(work + 0x318, -22.0f);
            Memory::WriteFloat32(work + 0x324, 0.15f);
            em.Count(0x1E);
        } else if (kind == 0) {
            Memory::WriteFloat32(work + 0x318, -20.0f);
            Memory::WriteFloat32(work + 0x324, 0.3f);
            em.Count(0x16);
        }

        break;
    case 1:
        game.Call<0x8013562C>({work, work + 0x1BC});

        if (Memory::ReadFloat32(work + 0x318) > 0.0f)
            Memory::WriteFloat32(work + 0x318, 0.0f);

        if (CountDown(em)) {
            if (kind == 0 || kind == 2)
                game.Call<0x80128AD4>({work, 4, 1});
            else
                game.Call<0x80128110>({work});
        }

        break;
    }
}

// kind 12 0x80171B7C: Great Jaggi backing away from its target (motion 10,
// half the distance at most its data's first value, 0x80134068), then
// motion 11 for 15 ticks (the original's 15 calls) before it ends with the
// motion or once nearer than that value.
void Em012RetreatPort(CpuContext* ctx) {
    GuestCall game{ctx};
    const Em::Enemy em{game, ctx->gpr[3]};
    const uint32_t work = em.work;
    const uint32_t data = game.Call<0x801264B8>({work}); // the kind's data: f32 the near distance
    const float near = Memory::ReadFloat32(data);

    switch (em.State()) {
    case 0: {
        em.State(1);
        game.Call<Em::SetUp>({work, 0});
        em.SetMotion(10, 6, 0);
        game.Call<0x80050F58>({work + 0x188, work + 0x36C});
        const float away = -0.5f * game.F1();
        game.Call<TurnSetUp>({work, 0}, {double(away > -near ? -near : away)});
        break;
    }
    case 1:
        if (game.Call<TurnStep>({work, 0, 0x80}) == 1 && game.Call<0x8012F9AC>({work}) == 0) {
            em.NextState();
            em.SetMotion(11, 4, 0);
            game.Call<TurnSetUp>({work, 0}, {0.0});
            em.Count(0xF);
        }

        break;
    case 2:
        game.Call<TurnStep>({work, 0, 0x80});

        if (em.Count() > 0)
            em.Count(em.Count() - int32_t(GameStep::Ticks()));
        else if (em.Ended()) {
            game.Call<ActionDone>({work});
        } else {
            game.Call<0x80050FE4>({work + 0x188, work + 0x36C});

            if (game.F1() < near)
                game.Call<ActionDone>({work});
        }

        break;
    }
}

// kind 30 0x801B1228 (r4: 1 the short one): a Felyne resting, motion 0x1D
// for 250 (150) ticks plus the random word's low 6 bits, then motion 0x1E
// and 0x1F (only 0x1F for the long one) to their ends.
void Em030RestPort(CpuContext* ctx) {
    GuestCall game{ctx};
    const Em::Enemy em{game, ctx->gpr[3]};
    const uint32_t work = em.work;
    const bool shortRest = uint8_t(ctx->gpr[4]) == 1;

    switch (em.State()) {
    case 0:
        em.State(1);
        game.Call<Em::SetUp>({work, 0});
        em.SetMotion(0x1D, 4, 0);
        em.Count(int32_t((Memory::Read16(work + kRandom) & 0x3F) + (shortRest ? 0x96 : 0xFA)));
        break;
    case 1:
        if (CountDown(em)) {
            em.State(shortRest ? 2 : 3);
            em.SetMotion(shortRest ? 0x1E : 0x1F, 4, 0);
        }

        break;
    case 2:
        if (em.Ended()) {
            em.NextState();
            em.SetMotion(0x1F, 4, 0);
        }

        break;
    case 3:
        if (em.Ended())
            game.Call<ActionDone>({work});

        break;
    }
}

// kind 30 0x801B1708 (r4 the run 0-3, r5: 1 the short one): a Felyne running
// toward its target (motion 2 or 3, 0x80134068 -100 or -300, at 0.7 times
// the speed for run 3), ending once there (0x80134178 at 0x800) or after
// 150 (60) ticks, only 15 more once 0x801B01CC holds.
void Em030RunPort(CpuContext* ctx) {
    GuestCall game{ctx};
    const Em::Enemy em{game, ctx->gpr[3]};
    const uint32_t work = em.work;
    const uint8_t run = uint8_t(ctx->gpr[4]);
    const bool shortRun = uint8_t(ctx->gpr[5]) == 1;

    switch (em.State()) {
    case 0:
        em.State(1);
        game.Call<Em::SetUp>({work, 0});
        em.SetMotion(run == 0 ? 2 : 3, 4, 0);

        if (run == 3)
            game.Call<SetMotionSpeed>({work}, {0.7f});

        game.Call<TurnSetUp>({work, 0}, {run >= 2 ? -300.0 : -100.0});
        em.Count(shortRun ? 0x3C : 0x96);
        break;
    case 1:
        if (game.Call<TurnStep>({work, 0, 0x800}) == 1 || CountDown(em))
            game.Call<ActionDone>({work});

        if (em.Count() > 15 && game.Call<0x801B01CC>({work}) == 1)
            em.Count(15);

        break;
    }
}

// kind 27 0x801AC674: Aptonoth walking (motion 3 looped, steering at
// 0x80) for 256 plus the low byte of its random word ticks.
void Em027WalkPort(CpuContext* ctx) {
    GuestCall game{ctx};
    const Em::Enemy em{game, ctx->gpr[3]};
    const uint32_t work = em.work;

    switch (em.State()) {
    case 0:
        em.State(1);
        game.Call<Em::SetUp>({work, 0});
        game.Call<SetMotionLoop>({work, 3, 4, 0});
        em.Count(int32_t((Memory::Read16(work + kRandom) & 0xFF) + 0x100));
        break;
    case 1: {
        em.Count(em.Count() - int32_t(GameStep::Ticks()));
        game.Call<Steer>({work, 0x80});

        if (em.Count() < 1)
            game.Call<ActionDone>({work});

        break;
    }
    }
}

// kind 19 0x8037A8AC (r4: 1 60 ticks marking every 8, 2 15 ticks, else
// 90): Agnaktor waiting under the ground, motion 1 looped; then 0x80128158.
// The original counts down a call at a time and ends at exactly 0.
void Em019BuriedPort(CpuContext* ctx) {
    GuestCall game{ctx};
    const Em::Enemy em{game, ctx->gpr[3]};
    const uint32_t work = em.work;
    const uint8_t kind = uint8_t(ctx->gpr[4]);
    game.Call<0x80131E64>({work});
    game.Call<0x803790E8>({work});

    switch (em.State()) {
    case 0:
        em.State(1);
        game.Call<Em::SetUp>({work, 4});
        game.Call<SetMotionLoop>({work, 1, 0, 0});
        game.Call<0x80130390>({work}); // f1 passes on
        game.Call<0x80130450>({work});
        game.Call<0x80379000>({work, 0xFF});
        em.Count(kind == 1 ? 0x3C : kind == 2 ? 0xF : 0x5A);
        break;
    case 1: {
        const uint32_t ticks = GameStep::Ticks();

        // The original's test of the count before its decrement, over the
        // counts this step's ticks pass.
        if (kind == 1 && Em::AnyOf(em.Count() - int32_t(ticks) + 1, ticks, 8, 0))
            game.Call<Mark>({work, uint32_t(-1), 200});

        if (ticks && em.Count() > 0 && em.Count() - int32_t(ticks) <= 0) {
            em.Count(0);
            game.Call<0x80128158>({work});
        } else
            em.Count(em.Count() - int32_t(ticks));

        break;
    }
    }
}

// kind 6 0x801BD724 (r4: 1 the long one): Diablos waiting under the sand,
// motion 1 looped: every 32 ticks a mark (0x80136BB4 -1, 5), a sand burst
// at joint 3 (effect 0x53, 0x54 while +0x228 & 6) and its sound in the
// hunter's area; after 45 (80) ticks 0x80128158. The original counts both in
// calls.
void Em006BuriedPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x20);
    GuestCall game{ctx};
    const Em::Enemy em{game, frame.Arg(3)};
    const uint32_t work = em.work, at = frame[0x08];
    const bool longer = uint8_t(frame.Arg(4)) == 1;
    game.Call<0x80043F0C>({at});
    game.Call<0x80131E64>({work});

    switch (em.State()) {
    case 0:
        em.State(1);
        game.Call<Em::SetUp>({work, 4});
        game.Call<SetMotionLoop>({work, 1, 0, 0});
        game.Call<0x80130390>({work}); // f1 passes on
        game.Call<0x80130450>({work});
        em.Count(0);
        break;
    case 1:
        if (em.CountEvery(32)) {
            game.Call<Mark>({work, uint32_t(-1), 5});
            game.Call<JointPosition>({work, 3, at});
            const bool other = (Memory::Read16(work + 0x228) & 6) != 0;
            Memory::WriteFloat32(at + 4, 5.0f + Memory::ReadFloat32(work + (other ? 0x210 : 0x20C)));
            game.Call<JointEffect>({work, 3, other ? 0x54u : 0x53u, 0}, {1.0});

            if ((game.Call<0x802AFCE8>() & 0xFF) == Memory::Read8(work + 0x1E1)) // get_now_areano
                game.Call<0x800D9F0C>({Memory::Read32(work + 0xB14), 0x27, 2, at});
        }

        em.CountTicks();

        if (em.Count() >= (longer ? 0x50 : 0x2D))
            game.Call<0x80128158>({work});

        break;
    }
}

// kind 6 0x801BD89C: Diablos moving under the sand, motion 8 at 1.5 times
// its speed toward a point 1000 away (0x80134068, 0x80134178 at 0x40), its
// digging sound each step in the hunter's area; it ends (0x80128158) once
// there and at least 20 ticks in (the original's 20 calls).
void Em006BurrowPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x20);
    GuestCall game{ctx};
    const Em::Enemy em{game, frame.Arg(3)};
    const uint32_t work = em.work, at = frame[0x08];
    game.Call<0x80043F0C>({at});
    game.Call<0x80131E64>({work});

    switch (em.State()) {
    case 0:
        em.State(1);
        Memory::Write8(work + 6, 0);
        game.Call<Em::SetUp>({work, 4});
        em.SetMotion(8, 0, 0);
        game.Call<SetMotionSpeed>({work}, {1.5});
        game.Call<RoarVoice>({work, 0, 0x21, 2});
        game.Call<TurnSetUp>({work, 0}, {1000.0});
        game.Call<0x80130390>({work}); // f1 passes on
        game.Call<0x80130450>({work});
        em.Count(0x14);
        break;
    case 1:
        if ((game.Call<0x802AFCE8>() & 0xFF) == Memory::Read8(work + 0x1E1)) { // get_now_areano
            game.Call<JointPosition>({work, 3, at});
            game.Call<0x800DE360>({Memory::Read32(work + 0xB14), at, 8, Memory::Read16(work + 0x1A)}); // shell_se_req
        }

        if (Memory::Read8(work + 6) == 0 && game.Call<TurnStep>({work, 0, 0x40}) == 1)
            Memory::Write8(work + 6, 1);

        if (CountDown(em) && Memory::Read8(work + 6) == 1)
            game.Call<0x80128158>({work});

        break;
    }
}

// 0x801B795C and 0x801B867C (`back`: the second, which also moves backward
// and steers first through 0x801B7108): hops (the pattern sweep's em034
// Kelbi; reached through 0x801B94B4, a dispatcher after kind 34's code, not
// Kelbi variant 1's), motion 5:
// from frame 6 to 16 its rise (+0x314) and the second's backward speed
// (+0x318) follow two curves (0x801356A8, tables 0x805B2948 and 0x805B29A8)
// through the scaled velocity add; then motion 1 and a hold of 10 ticks
// (the original's 10 calls) before 0x80128048.
void Em034Hop(CpuContext* ctx, bool back) {
    GuestCall game{ctx};
    const Em::Enemy em{game, ctx->gpr[3]};
    const uint32_t work = em.work;
    const auto move = [&] {
        game.Call<0x8013545C>({work});
        game.Call<0x801356A8>({work, 0x805B2948});
        Memory::WriteFloat32(work + 0x314, game.F1());

        if (back) {
            game.Call<0x801356A8>({work, 0x805B29A8});
            Memory::WriteFloat32(work + 0x318, -game.F1());
        }

        game.Call<AddVelocity>({work});
    };

    switch (em.State()) {
    case 0:
        em.State(1);

        if (back) {
            game.Call<0x801302AC>({work}); // f1: the carried scale, for SetScale
            game.Call<SetScale>({work});
        } else
            game.Call<Em::SetUp>({work, 0});

        em.SetMotion(5, 4, 0);
        break;
    case 1:
        if (back)
            game.Call<0x801B7108>({work});

        if (em.At(6.0f, 1)) {
            em.NextState();

            if (!back) {
                game.Call<0x801302AC>({work});
                game.Call<SetScale>({work});
            }

            move();
        }

        break;
    case 2:
        move();

        if (em.At(16.0f, 1)) {
            em.NextState();
            em.SetMotion(1, 10, 0);
            em.Count(10);
            game.Call<0x8013545C>({work});
        }

        break;
    case 3:
        if (CountDown(em))
            game.Call<0x80128048>({work});

        break;
    }
}

void Em034HopPort(CpuContext* ctx) {
    Em034Hop(ctx, false);
}

void Em034HopBackPort(CpuContext* ctx) {
    Em034Hop(ctx, true);
}

// em007 0x801D91B4 (r4: 1 enraged, other voices): Deviljho's charge,
// actions 7.3 and 7.18. Motion 0x16 with voices at frames 64, 108, 136 and
// 170 and 5% more speed at frames 92 and 158 (at most 5 times); it steers
// toward its target (0x80134178, 0x100: EnemyTurnToward's rate, by game
// time) until it is past it, then motion 0x30 to its end. The count at
// +0x20 (1000 ticks) ends the charge early when it never gets past.
void Em007ChargePort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x20);
    GuestCall game{ctx};
    const Em::Enemy em{game, frame.Arg(3)};
    const uint32_t work = em.work;
    const bool enraged = uint8_t(frame.Arg(4)) == 1;

    switch (em.State()) {
    case 0:
        em.State(1);
        Memory::Write8(work + 6, 0);
        game.Call<Em::SetUp>({work, 0});
        em.SetMotion(0x16, 2, 0);
        game.Call<RoarVoice>({work, 0, enraged ? 0x25u : 4u, 0x88});
        game.Call<RoarVoice>({work, 1, 0x39, 0x18});
        game.Call<0x80130D40>({work, uint32_t(-50)}); // stamina (+0x8A0) less 50
        em.Count(1000);
        game.Call<TurnSetUp>({work, 0}, {0.0});
        break;
    case 1: {
        if (em.At(64.0f))
            game.Call<RoarVoice>({work, 1, 0x20, 0x90});

        if (em.At(108.0f)) {
            game.Call<RoarVoice>({work, 0, enraged ? 0x26u : 5u, 0x88});
            game.Call<RoarVoice>({work, 1, 0x3A, 0x18});
        }

        if (em.At(136.0f))
            game.Call<RoarVoice>({work, 1, 0x21, 0x90});

        if (em.At(170.0f)) {
            game.Call<RoarVoice>({work, 0, enraged ? 0x27u : 6u, 0x88});
            game.Call<RoarVoice>({work, 1, 0x3B, 0x18});
        }

        if ((em.At(92.0f) || em.At(158.0f)) && Memory::Read8(work + 6) < 5) {
            const uint8_t faster = uint8_t(Memory::Read8(work + 6) + 1);
            Memory::Write8(work + 6, faster);
            game.Call<SetMotionSpeed>({work}, {1.0f + 0.05f * float(faster)});
        }

        // The count down by the step's ticks, not below 0 (the original's 1 a call).
        if (em.Count() > 0)
            em.Count(std::max(em.Count() - int32_t(GameStep::Ticks()), 0));

        const bool past = game.Call<TurnStep>({work, 0, 0x100}) == 1;

        if (em.Ended() && (past || em.Count() < 1)) {
            em.NextState();
            em.SetMotion(0x30, 2, 0);
            game.Call<RoarVoice>({work, 1, 0x22, 0x90});
        }

        break;
    }
    case 2:
        if (em.Ended())
            game.Call<ActionDone>({work});

        break;
    }
}

// em024 0x8034FA1C: a flight of 300 ticks forward (at 10 a tick in the
// motion's [16, 36]); 20 ticks before its end a mark (0x80136BB4 0x15, 3).
void Em024FlightPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x20);
    GuestCall game{ctx};
    const Em::Enemy em{game, frame.Arg(3)};
    const uint32_t work = em.work, at = frame[0x08];

    switch (em.State()) {
    case 0:
        em.State(1);
        game.Call<Em::SetUp>({work, 0});
        em.SetMotion(0x22, 4, 0);
        ClearVelocity(work);
        Memory::WriteFloat32(work + kSideSpeed, 10.0f);
        game.Call<RotateY>({work + kVelocity, Memory::Read32(work + kYaw)});
        em.Count(300);
        break;
    case 1: {
        if (em.At(16.0f, 3, 36.0f))
            game.Call<AddVelocity>({work});

        const int32_t before = em.Count();

        if (CountDown(em)) {
            em.NextState();
            em.SetMotion(0x14, 0xE, 0x30);
            game.Call<RoarVoice>({work, 0, 0x14, 0x88});
            game.Call<RoarVoice>({work, 1, 0x17, 0x10});
        } else if (game.Call<0x8034F1C8>({work}) == 0) {
            game.Call<NextAction>({work, 1, 5});
        } else if (before > 0x14 && em.Count() <= 0x14) {
            game.Call<Mark>({work, 0x15, 3});
        }

        break;
    }
    case 2:
        if (game.Call<0x8012F990>({work, 0}, {54.0f, 0.0f}) == 1) { // em_after_frame_check: the advance just made
            Memory::WriteFloat32(at, 0.0f);
            Memory::WriteFloat32(at + 4, 50.0f);
            Memory::WriteFloat32(at + 8, 150.0f);
            em.Spawn(1.0f, 0xB5, 0x15, at);
        }

        if (em.Ended())
            game.Call<ActionDone>({work});

        break;
    }
}

// em028 0x8039E9EC (r4: 1 the long one): a rest of 150 or 300 ticks.
void Em028RestPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x10);
    GuestCall game{ctx};
    const Em::Enemy em{game, frame.Arg(3)};
    const uint32_t work = em.work;

    switch (em.State()) {
    case 0:
        em.State(1);
        game.Call<Em::SetUp>({work, 0});
        em.SetMotion(0xCF, 2, 0);
        em.Count(uint8_t(frame.Arg(4)) == 1 ? 300 : 0x96);
        game.Call<Mark>({work, uint32_t(-1), 7});
        break;
    case 1:
        if (em.At(4.0f, 1))
            Memory::Write8(work + kRestDone, 1);

        if (CountDown(em)) {
            em.NextState();
            em.SetMotion(0xD0, 6, 0);
        }

        break;
    case 2:
        if (em.Ended())
            game.Call<ActionDone>({work});

        break;
    }
}

// em015 0x8017AA5C (r4: 0 a charged roar, else a discharge): sparks
// (0x80306AFC) every 5 ticks while the motion is in [120, 230].
void Em015SparksPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x20);
    GuestCall game{ctx};
    const Em::Enemy em{game, frame.Arg(3)};
    const uint32_t work = em.work, at = frame[0x08];

    if (em.State() == 0) {
        em.State(1);
        game.Call<Em::SetUp>({work, 2});
        em.SetMotion(0xCE, 0x14, 0);
        Memory::Write8(work + Em::kSubState, 0);
        em.Count(0);
        return;
    }

    if (em.State() != 1)
        return;

    const auto sub = [&] { return Memory::Read8(work + Em::kSubState); };
    const auto nextSub = [&] { Memory::Write8(work + Em::kSubState, uint8_t(sub() + 1)); };

    if (uint8_t(frame.Arg(4)) == 0) {
        if (sub() == 0) {
            if (em.At(130.0f, 1)) {
                nextSub();
                game.Call<RoarVoice>({work, 0, 5, 3});
            }
        } else if (sub() == 1 && em.At(220.0f, 1)) {
            nextSub();

            // 0x80129788 (work, 0), inlined: the voice channel's flags cleared.
            Memory::Write8(work + 0xA0D, 0);
            Memory::Write8(work + 0xAEC, 0);
        }

        if (em.At(260.0f))
            game.Call<0x8011D6F4>({work, 7, 3}, {1.0f});

        // The sparks: every 5 ticks while the motion is in [120, 230].
        if (em.At(120.0f, 3, 230.0f)) {
            if (em.CountEvery(5))
                game.Call<Spark>({work, 1});

            game.Call<Discharge>({work, 5});
            em.CountTicks();
        }
    } else {
        if (em.At(120.0f))
            game.Call<Spark>({work, 1});

        if (em.At(120.0f, 3, 230.0f)) {
            if (em.CountEvery(5))
                game.Call<Spark>({work, 0});

            em.CountTicks();
        }

        if (em.At(144.0f)) {
            Memory::WriteFloat32(at, 0.0f);
            Memory::WriteFloat32(at + 4, 150.0f);
            Memory::WriteFloat32(at + 8, 0.0f);
            game.Call<0x801E2D68>({work, 3, 0x15, at, 0x35}, {1.0f});
        }
    }

    if (em.Ended())
        game.Call<ActionDone94>({work});
}

// em025 0x8019F3DC (r4: 1 in the sandship fight): its roar, the steam
// (kind 1, scale 3) every 10 ticks while the motion is in [388, 528].
void Em025RoarPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x20);
    GuestCall game{ctx};
    const Em::Enemy em{game, frame.Arg(3)};
    const uint32_t work = em.work, at = frame[0x08];
    const bool ship = uint8_t(frame.Arg(4)) == 1;

    if (ship)
        Memory::Write8(work + kJhenFlag, 1);

    if (em.State() == 0) {
        em.State(1);
        game.Call<Em::SetUp>({work, 0});
        em.SetMotion(0x18, 0x1E, 0);
        em.Count(0);
        return;
    }

    if (em.State() != 1)
        return;

    if (em.At(350.0f)) {
        game.Call<RoarVoice>({work, 0, 0x1A, 5});

        Em::Rumble(work, 0x19, 0x78);
    }

    if (em.At(550.0f)) {
        game.Call<RoarVoice0>({work, 0, 0x1D});
        game.Call<RoarVoice0>({work, 1, 0x20});
    }

    if (ship)
        game.Call<0x8019EC10>({work});

    Memory::WriteFloat32(at, 0.0f);
    Memory::WriteFloat32(at + 4, 0.0f);
    Memory::WriteFloat32(at + 8, 1000.0f);

    if (em.At(352.0f))
        em.Spawn(1.3f, 0, 0x19, at);

    // The steam: every 10 ticks while the motion is in [388, 528].
    if (em.At(388.0f, 3, 528.0f)) {
        if (em.CountEvery(10))
            em.Spawn(3.0f, 1, 0x19, at);

        em.CountTicks();
    }

    if (em.Ended())
        game.Call<ActionDone>({work});
}

// em024 0x803519B8 (r4: 1 with a shell): bursts (kind 0xA5 on joints 0x30
// and 0x31) every 7 ticks while the motion is in [30, 102]; with r4 1 a
// shell at a random spot ahead at frame 170.
void Em024BurstsPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x40);
    GuestCall game{ctx};
    const Em::Enemy em{game, frame.Arg(3)};
    const uint32_t work = em.work, at = frame[0x08], spot = frame[0x14];

    const auto flash = [&] {
        em.Spawn(1.0f, 0xA6, 0x30);
        em.Spawn(1.0f, 0xA6, 0x31);
    };

    if (em.State() == 0) {
        em.State(1);
        game.Call<Em::SetUp>({work, 0});
        em.SetMotion(0x2E, 4, 0);
        game.Call<RoarVoice>({work, 0, 2, 8});
        game.Call<RoarVoice>({work, 1, 3, 0x10});
        game.Call<0x80358854>({work, uint32_t(-20)});
        return;
    }

    if (em.State() != 1)
        return;

    if (em.At(10.0f))
        flash();

    // The bursts: every 7 ticks while the motion is in [30, 102] (the
    // original counted calls 1-7 and spawned at 7, back to 0; a step of
    // several ticks drops those past 7).
    if (em.At(30.0f, 3, 102.0f)) {
        em.CountTicks();

        if (em.Count() > 6) {
            Memory::WriteFloat32(at, 0.0f);
            Memory::WriteFloat32(at + 4, -30.0f);
            Memory::WriteFloat32(at + 8, 0.0f);
            em.Spawn(1.0f, 0xA5, 0x30, at);
            em.Spawn(1.0f, 0xA5, 0x31, at);
            em.Count(0);
        }
    }

    if (em.At(124.0f))
        game.Call<JointEffect>({work, 0x30, 0x4C, 0x8000}, {1.0f});

    if (em.At(162.0f))
        game.Call<JointEffect>({work, 0x30, 0x4C, 0}, {1.0f});

    if (uint8_t(frame.Arg(4)) == 1 && em.At(170.0f)) {
        // A spot 8 · (random low byte − 128) across and 200 + 5 · (random
        // high byte) ahead, turned to the monster's yaw.
        const uint32_t random = Memory::Read16(work + kRandom);
        Memory::WriteFloat32(spot, 8.0f * (float(random & 0xFF) - 128.0f));
        Memory::WriteFloat32(spot + 4, 0.0f);
        Memory::WriteFloat32(spot + 8, -200.0f - 5.0f * float(random >> 8));
        game.Call<RotateY>({spot, Memory::Read32(work + kYaw)});

        for (uint32_t i = 0; i < 3; ++i)
            Memory::WriteFloat32(spot + 4 * i, Memory::ReadFloat32(spot + 4 * i) + Memory::ReadFloat32(work + Em::kPosition + 4 * i));

        const uint32_t fn = Memory::Read32(Memory::Read32(kShellTable) + 0x8C);
        game(fn, {work, 0, spot, 0x3C}, {1.0f});
    }

    if (em.At(180.0f))
        flash();

    if (em.Ended())
        game.Call<ActionDone>({work});
}

// em024 0x8035455C (r4: 0 charged, 1, 2 fast): a charge that sheds sparks
// (0x80306AFC 0x11, with a sound) every 6 ticks once the motion is past
// frame 58 and while it flies on (state 2), steering until it has turned.
void Em024ChargePort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x20);
    GuestCall game{ctx};
    const Em::Enemy em{game, frame.Arg(3)};
    const uint32_t work = em.work, at = frame[0x08];
    const uint8_t kind = uint8_t(frame.Arg(4));
    constexpr uint32_t kCurrentArea = 0x806BB806; // get_now_areano (0x802AFCE8, inlined)
    constexpr uint32_t kSounds = 0xB14, kSoundId = 0x1A, kDrive = 0x324;

    const auto here = [&] { return Memory::Read8(work + 0x1E1) == Memory::Read8(kCurrentArea); };

    // The sound of a part of the body (shell_se_req 0x800DE360).
    const auto partSound = [&](uint32_t joint) {
        game.Call<JointPosition>({work, joint, at});
        game.Call<0x800DE360>({Memory::Read32(work + kSounds), at, 0x1D, Memory::Read16(work + kSoundId)});
    };

    // The sparks: every 6 ticks (the original: every 6 calls, its count
    // back to 0 past 5; a step of several ticks drops those past 6).
    const auto sparks = [&] {
        if (em.Count() > 5) {
            em.Count(0);
            game.Call<Spark>({work, 0x11});
        }

        em.CountTicks();

        if (here())
            partSound(3);
    };

    // Steering on; true once turned (the next state set).
    const auto turned = [&] {
        if (game.Call<TurnStep>({work, 0, 0}) == 1)
            return true;

        game.Call<SetUpCarried>({work});
        game.Call<0x80135664>({work, work + 0x1BC});
        const float limit = kind == 2 ? 240.0f : 180.0f;

        if (limit < Memory::ReadFloat32(work + kSideSpeed)) {
            Memory::WriteFloat32(work + kSideSpeed, limit);
            Memory::WriteFloat32(work + kDrive, 0.0f);
        }

        return false;
    };

    const auto land = [&](uint32_t state) {
        em.State(uint8_t(state));
        em.Count(0);
        em.SetMotion(0xB, 2, 0);
    };

    // The charge's flashes: (frame, scale, joint) of kind 0xA9.
    struct Flash {
        float frame, scale;
        uint32_t joint;
    };

    static constexpr Flash kFlashes[] = {
        {4.0f, 0.6f, 0x2B}, {4.0f, 1.0f, 0x31}, {6.0f, 2.0f, 0x21}, {8.0f, 1.2f, 0x30}, {8.0f, 0.6f, 0x25},
        {10.0f, 0.6f, 0x2A}, {12.0f, 1.4f, 0x2F}, {14.0f, 2.0f, 0x1B}, {14.0f, 0.6f, 0x24}, {16.0f, 1.6f, 0x2E},
        {16.0f, 0.6f, 0x29}, {20.0f, 2.0f, 0x1F}, {20.0f, 1.8f, 0x2D}, {20.0f, 0.6f, 0x23}, {22.0f, 0.6f, 0x28},
        {26.0f, 2.0f, 0x12}, {26.0f, 2.0f, 0x22},
    };

    switch (em.State()) {
    case 0:
        em.State(1);
        game.Call<Em::SetUp>({work, 3});
        em.SetMotion(10, 0, 0);
        game.Call<RoarVoice>({work, 0, 0xE, 0xB});
        game.Call<RoarVoice>({work, 1, 0xF, 0x1B});
        game.Call<TurnSetUp>({work, 1}, {800.0f});
        ClearMotion(work);

        if (kind == 2) {
            game.Call<SetMotionSpeed>({work}, {0.7f});
            Memory::WriteFloat32(work + kSideSpeed, 150.0f);
            Memory::WriteFloat32(work + kDrive, 10.0f);
        } else {
            Memory::WriteFloat32(work + kSideSpeed, 80.0f);
            Memory::WriteFloat32(work + kDrive, 5.0f);
        }

        em.Count(0);
        break;
    case 1:
        if (kind == 0) {
            if (em.At(2.0f)) {
                em.Spawn(2.0f, 0xA9, 0x1D);
                em.Spawn(0.6f, 0xA9, 0x26);

                if (here())
                    partSound(0x1D);
            }

            // The frames' flashes, in the original's order of frames (each
            // frame's test once, its spawns in turn).
            for (size_t i = 0; i < sizeof kFlashes / sizeof kFlashes[0];) {
                const float frameAt = kFlashes[i].frame;
                const bool met = em.At(frameAt);
                size_t j = i;

                for (; j < sizeof kFlashes / sizeof kFlashes[0] && kFlashes[j].frame == frameAt; ++j)
                    if (met)
                        em.Spawn(kFlashes[j].scale, 0xA9, kFlashes[j].joint);

                i = j;
            }

            if (em.At(58.0f, 1))
                sparks();
        }

        if (kind == 2 && em.At(36.0f))
            game.Call<SetMotionSpeed>({work}, {1.0f});

        if (em.At(64.0f, 1) && turned()) {
            land(3);
            return;
        }

        if (em.Ended()) {
            em.NextState();
            em.Count(0);
            em.SetMotion(9, 4, 0);
        }

        break;
    case 2:
        sparks();

        if (turned())
            land(3);

        break;
    case 3:
        if (em.At(2.0f) && here()) {
            game.Call<JointPosition>({work, 3, at});
            game.Call<0x800D9F0C>({Memory::Read32(work + kSounds), 0x91, 2, at}); // se_req_pos_ps
        }

        if (em.At(10.0f)) {
            // 0x80129788 (work, 0 / 1), inlined: the voice channels' flags cleared.
            for (uint32_t channel = 0; channel < 2; ++channel) {
                Memory::Write8(work + channel * 92 + 0xA0D, 0);
                Memory::Write8(work + channel + 0xAEC, 0);
            }

            game.Call<RoarVoice>({work, 0, 0x11, 0x10});
        }

        if (em.Ended())
            game.Call<0x80128110>({work}); // ActionDone with set-up 3

        break;
    }
}

// kind 4 0x801C7570: buried in the sand, it waits 90 ticks for a hunter to
// come within 160 of joint 0x1C, then bursts out at one it can hit, or gives
// up once the wait is out and none was in reach.
void Em004AmbushPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x60);
    GuestCall game{ctx};
    const Em::Enemy em{game, frame.Arg(3)};
    const uint32_t work = em.work, at = frame[0x08];
    constexpr uint32_t kPlayerMask = 0x382, kDug = 0x1F5, kBurstHold = 0x81C, kMoveFlags = 0x836;

    const auto burst = [&] {
        if (em.At(50.0f, 1))
            Memory::Write16(work + kBurstHold, 2); // 0x80131F24, inlined
    };

    game.Call<0x801C2C9C>({work});
    Memory::Write16(work + kMoveFlags, Memory::Read16(work + kMoveFlags) & 0x7FFF);

    switch (em.State()) {
    case 0:
        em.State(1);
        game.Call<SetUpCarried>({work});
        game.Call<SetScale>({work}); // f1: SetUpCarried's
        game.Call<0x80130724>({work, 2});
        game.Call<SetMotionEx>({work, 0x40, 4, 0, 1});
        Memory::Write8(work + 0x8AE, 1); // 0x80132228, inlined
        break;
    case 1:
        burst();

        if (em.At(44.0f))
            game.Call<Mark>({work, 0x1C, 1});

        if (em.Ended()) {
            em.NextState();
            em.Count(0x5A);
        }

        break;
    case 2: {
        burst();

        // The wait: by the step's ticks, down to 0.
        if (em.Count() > 0)
            em.Count(std::max(em.Count() - int32_t(GameStep::Ticks()), 0));

        if (Memory::Read8(work + kDug) != 1) // 0x8012D204, inlined
            break;

        // The players (get_move_work_adrs / _max(2), inlined) in reach.
        game.Call<JointPosition>({work, 0x1C, at});
        const uint32_t table = Memory::Read32(0x8065AB04);
        uint32_t player = table ? Memory::Read32(table + 16 + 2 * 4) : 0;
        const uint32_t players = table ? Memory::Read16(table + 2 * 2) : 0;
        const uint32_t mask = Memory::Read8(work + kPlayerMask);
        uint32_t hits = 0;
        bool seen = false;

        for (uint32_t i = 0; i < players; ++i, player += 0xB20) {
            if ((mask & (1u << i)) == 0 || Memory::Read8(player) == 0)
                continue;

            game.Call<0x80050FAC>({player + 0x3C, at}); // calcDistanceSqXZ

            if (game.F1() > 25600.0f)
                continue;

            if (game.Call<0x8012D2A0>({work, 1, i}) == 1)
                hits |= 1u << i;
            else if (game.Call<0x8012D2A0>({work, 0, i}) == 1)
                seen = true;
        }

        if (hits == 0) {
            if (em.Count() < 1 || !seen)
                game.Call<NextAction>({work, 3, 0x21});
        } else {
            game.Call<0x8012B3E4>({work, 1, 10});
            Memory::Write8(work + 0x1E7, 6);
            game.Call<NextAction>({work, 9, 0});
        }

        break;
    }
    }
}

// em020 0x80372994: rises from the deep (214 ticks, two bursts on frames 178
// and 282), arches back (124 ticks, its pitch turning 45.512 × the motion's
// rate a tick while the motion is in [110, 360)), then dives.
void Em020DivePort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x30);
    GuestCall game{ctx};
    const Em::Enemy em{game, frame.Arg(3)};
    const uint32_t work = em.work, at = frame[0x08];
    Memory::Write8(work + 0x785, 1); // 0x80133CA0, inlined

    const auto burst = [&](float frameAt, float scale) {
        if (game.Call<0x8012F990>({work, 0}, {frameAt, 0.0f}) != 1) // em_after_frame_check
            return;

        Memory::WriteFloat32(at, 0.0f);
        Memory::WriteFloat32(at + 4, 0.0f);
        Memory::WriteFloat32(at + 8, 0.0f);
        game.Call<0x801E2D68>({work, 0x17, 0x20, at, 100}, {scale});
    };

    const auto face = [&](float pitch) { game.Call<Em::SetAngles>({work}, {pitch, 0.0f, 0.0f}); };

    switch (em.State()) {
    case 0:
        em.State(1);
        game.Call<Em::SetUp>({work, 2});
        em.SetMotion(0x65, 0, 0);
        game.Call<0x802BC82C>({0x3E, work});
        game.Call<Em::SetPosition>({work}, {-4991.0f, -20390.0f, -3126.0f});
        face(0.0f);
        em.Count(0);
        break;
    case 1:
        burst(178.0f, 1.0f);
        burst(282.0f, 1.5f);
        em.CountTicks();

        if (em.Count() > 0xD6) {
            em.NextState();
            em.SetMotion(0xCF, 0, 0x6E);
            face(-27.5f);
            em.Count(0);
        }

        break;
    case 2:
        // The pitch: 45.512 × the motion's rate (whole units) a tick, the
        // step's share of it.
        if (em.At(110.0f, 5, 360.0f)) {
            const int32_t perTick = int32_t(45.512f * Memory::ReadFloat32(work + kMotionRate));
            Memory::Write32(work + 0x1BC, uint32_t(int32_t(Memory::Read32(work + 0x1BC)) - int32_t(GameStep::Share(perTick))));
        }

        em.CountTicks();

        if (em.Count() > 0x7C) {
            em.NextState();
            em.SetMotion(0x2C, 0, 0);
            face(-90.0f);
        }

        break;
    case 3:
        if (em.Ended()) {
            em.NextState();
            game.Call<SetMotionEx>({work, 0x29, 0x28, 0, 1});
        }

        break;
    case 4:
        Memory::Write8(work + 0x30F, 1); // 0x80131E0C, inlined
        break;
    }
}

} // namespace

extern "C" void func_801C7570(CpuContext* ctx);
extern "C" void MH3_Em006Ambush(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em004Ambush", func_801C7570, Em004AmbushPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x801C7570, MH3_Em006Ambush, "Em004Ambush");

extern "C" void func_80372994(CpuContext* ctx);
extern "C" void MH3_Em020Dive(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em020Dive", func_80372994, Em020DivePort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80372994, MH3_Em020Dive, "Em020Dive");

extern "C" void func_8035455C(CpuContext* ctx);
extern "C" void MH3_Em024Charge(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em024Charge", func_8035455C, Em024ChargePort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8035455C, MH3_Em024Charge, "Em024Charge");

extern "C" void func_8015611C(CpuContext* ctx);
extern "C" void MH3_Em005Stagger(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em003Stagger", func_8015611C, Em003StaggerPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8015611C, MH3_Em005Stagger, "Em003Stagger");

extern "C" void func_80168430(CpuContext* ctx);
extern "C" void MH3_Em012Turn(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em010Turn", func_80168430, Em010TurnPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80168430, MH3_Em012Turn, "Em010Turn");

extern "C" void func_8019474C(CpuContext* ctx);
extern "C" void MH3_Em018Lunge(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em018Lunge", func_8019474C, Em018LungePort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8019474C, MH3_Em018Lunge, "Em018Lunge");

extern "C" void func_801ACA48(CpuContext* ctx);
extern "C" void MH3_Em027Rest(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em027Rest", func_801ACA48, Em027RestPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x801ACA48, MH3_Em027Rest, "Em027Rest");

extern "C" void func_801B2578(CpuContext* ctx);
extern "C" void MH3_Em029Charge(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em030Charge", func_801B2578, Em030ChargePort);
}
REGISTER_NATIVE_FUNCTION_AS(0x801B2578, MH3_Em029Charge, "Em030Charge");

extern "C" void func_801482AC(CpuContext* ctx);
extern "C" void MH3_Em001Wait(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em001Wait", func_801482AC, Em001WaitPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x801482AC, MH3_Em001Wait, "Em001Wait");

extern "C" void func_80159054(CpuContext* ctx);
extern "C" void MH3_Em003Move(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em003Move", func_80159054, Em003MovePort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80159054, MH3_Em003Move, "Em003Move");

extern "C" void func_8015898C(CpuContext* ctx);
extern "C" void MH3_Em003Backstep(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em003Backstep", func_8015898C, Em003BackstepPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8015898C, MH3_Em003Backstep, "Em003Backstep");

extern "C" void func_801B795C(CpuContext* ctx);
extern "C" void MH3_Em034Hop(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em034Hop", func_801B795C, Em034HopPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x801B795C, MH3_Em034Hop, "Em034Hop");

extern "C" void func_801B867C(CpuContext* ctx);
extern "C" void MH3_Em034HopBack(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em034HopBack", func_801B867C, Em034HopBackPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x801B867C, MH3_Em034HopBack, "Em034HopBack");

extern "C" void func_80171B7C(CpuContext* ctx);
extern "C" void MH3_Em012Retreat(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em012Retreat", func_80171B7C, Em012RetreatPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80171B7C, MH3_Em012Retreat, "Em012Retreat");

extern "C" void func_801B1228(CpuContext* ctx);
extern "C" void MH3_Em030Rest(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em030Rest", func_801B1228, Em030RestPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x801B1228, MH3_Em030Rest, "Em030Rest");

extern "C" void func_801B1708(CpuContext* ctx);
extern "C" void MH3_Em030Run(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em030Run", func_801B1708, Em030RunPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x801B1708, MH3_Em030Run, "Em030Run");

extern "C" void func_801AC674(CpuContext* ctx);
extern "C" void MH3_Em027Walk(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em027Walk", func_801AC674, Em027WalkPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x801AC674, MH3_Em027Walk, "Em027Walk");

extern "C" void func_8037A8AC(CpuContext* ctx);
extern "C" void MH3_Em019Buried(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em019Buried", func_8037A8AC, Em019BuriedPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8037A8AC, MH3_Em019Buried, "Em019Buried");

extern "C" void func_801BD724(CpuContext* ctx);
extern "C" void MH3_Em006Buried(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em006Buried", func_801BD724, Em006BuriedPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x801BD724, MH3_Em006Buried, "Em006Buried");

extern "C" void func_801BD89C(CpuContext* ctx);
extern "C" void MH3_Em006Burrow(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em006Burrow", func_801BD89C, Em006BurrowPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x801BD89C, MH3_Em006Burrow, "Em006Burrow");

extern "C" void func_801D91B4(CpuContext* ctx);
extern "C" void MH3_Em007Charge(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em007Charge", func_801D91B4, Em007ChargePort);
}
REGISTER_NATIVE_FUNCTION_AS(0x801D91B4, MH3_Em007Charge, "Em007Charge");

extern "C" void func_8034FA1C(CpuContext* ctx);
extern "C" void MH3_Em024Flight(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em024Flight", func_8034FA1C, Em024FlightPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8034FA1C, MH3_Em024Flight, "Em024Flight");

extern "C" void func_8039E9EC(CpuContext* ctx);
extern "C" void MH3_Em028Rest(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em028Rest", func_8039E9EC, Em028RestPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8039E9EC, MH3_Em028Rest, "Em028Rest");

extern "C" void func_8017AA5C(CpuContext* ctx);
extern "C" void MH3_Em015Sparks(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em015Sparks", func_8017AA5C, Em015SparksPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8017AA5C, MH3_Em015Sparks, "Em015Sparks");

extern "C" void func_8019F3DC(CpuContext* ctx);
extern "C" void MH3_Em025Roar(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em025Roar", func_8019F3DC, Em025RoarPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8019F3DC, MH3_Em025Roar, "Em025Roar");

extern "C" void func_803519B8(CpuContext* ctx);
extern "C" void MH3_Em024Bursts(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em024Bursts", func_803519B8, Em024BurstsPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x803519B8, MH3_Em024Bursts, "Em024Bursts");
