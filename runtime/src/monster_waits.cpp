// Monster waits and repeats counted per call, RMHP08. C reconstructions
// (decompiled with Ghidra) of the enemy actions that keep a count at +0x20 by
// one a call, which the enemy update calls once per step:
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
//     0x8035F378, 0x8035F560  kind 35 (Bnahabra) waits, 90 ticks
//     0x8014888C  kinds 1/2 (Rathian, Rathalos) a walk, 300 ticks
//     0x8014A8C4  kinds 1/2 a hover, 60 ticks
//     0x80388D80, 0x80387D08, 0x803878A8  kind 9 (Uragaan) under the lava 90,
//                 dozing 150 and its sleep 1800 ticks
//     0x80184DFC  kinds 16/17 (the Ludroth kinds) a swim, at most 200, 150+ or 90+ ticks
//     0x8018599C  kinds 16/17 a rest underwater, up to 1800 ticks, hunters checked every 30
//     sleeps of 1800 ticks: 0x80148ED0 kinds 1/2, 0x80155EBC 3, 0x8015E39C 8,
//                 0x80170BB0 12/14, 0x801777D8 15, 0x80183F00 16/17, 0x801BC230 6,
//                 0x801CB6F8 5, 0x80379A0C 19, 0x801D7C5C 7, 0x80195DE4 18,
//                 0x801C6014 4
//     0x801B1228  kind 30 (Felyne)   a rest, 150 or 250 ticks and up to 63 more
//     0x801B1708  kind 30 (Felyne)   a run, at most 60 or 150 ticks (15 when near)
//     0x801BD724  kind 6 (Diablos)   under the sand: 45 or 80 ticks, a burst every 32
//     0x801BD89C  kind 6 (Diablos)   a move under the sand, at least 20 ticks
//     0x801BD2F0, 0x801BC4D8  kind 6 (Diablos) under the sand 90 ticks, a sink of 240
//     0x80195004, 0x80195278, 0x80194B48, 0x801958C4, 0x8019771C, 0x80197F68
//                 kind 18 (Gobul): a lurk of 240 ticks, open mouths of 60, lying in
//                 wait (counting up to 1800, looking every 16), a leap's and a hop's
//                 wind-up of 24 and 12
//     the timed actions (a motion for a count of ticks, Timed below), 31 of them
//     from the static list of per-call counts, each described at its
//     port: Qurupeco's settle, staggers and display; Barroth's hold and track;
//     Jaggi's wander and turn; Great Jaggi's pause; Gigginox' gas, drop, leap
//     and spin; Barioth's climb and pounce; Deviljho's and Rathian's ledge
//     leaps, Rathian's rise, turn and landing; Lagiacrus' surfacing, path walks,
//     rest, circling and swim to its mark; Gobul's swim to its mark; Ceadeus'
//     path and circling; Jhen Mohran's holds and breach; Alatreon's landing;
//     Kelbi's hops, skips, jumps and climb; Felyne's naps, looks, turns and
//     circling; Aptonoth's turns; Giggi's crawls, hangs, latch and pounce;
//     Altaroth's turns, lifts and climbs; Bnahabra's rests
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
// The kinds (work +3) are labeled by the dispatcher that reaches each
// function; the descriptions of what a monster does
// were written under earlier labels, wrong for kinds 1-12.
#include "abi_bridge.h"
#include "enemy_work.h"
#include "game_step.h"
#include "guest_call.h"
#include "memory.h"
#include "port_check.h"
#include "step_counter.h"

#include <algorithm>
#include <cmath>
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

// The large monsters' sleep, one function per kind with the same code: lie
// down (motion `lie`), sleep (`sleep`, 1800 ticks, the original's 1800
// calls; +0x8AB set through 0x80132288, and 0x80132280, the periodic
// EnemyEveryNScaled, each step), wake (`wake`), then back to the AI. At 60
// steps a second the original woke after half the game time.
struct SleepMotions {
    uint8_t lie, lieFlags, sleep, sleepFlags, wake, wakeFlags;
    float rate;               // 0x80132280's f1
    uint32_t every;           // and its r5
    uint32_t done = ActionDone; // what ends it
    void (*start)(GuestCall&, uint32_t) = nullptr; // the set-up, before the lying motion (else SetUp 0)
    void (*after)(GuestCall&, uint32_t) = nullptr; // more set-up after it
};

void Sleep(CpuContext* ctx, const SleepMotions& m) {
    GuestCall game{ctx};
    const Em::Enemy em{game, ctx->gpr[3]};
    const uint32_t work = em.work;

    switch (em.State()) {
    case 0:
        em.State(1);

        if (m.start)
            m.start(game, work);
        else
            game.Call<Em::SetUp>({work, 0});

        em.SetMotion(m.lie, m.lieFlags, 0);

        if (m.after)
            m.after(game, work);

        break;
    case 1:
        if (em.Ended()) {
            em.NextState();
            em.SetMotion(m.sleep, m.sleepFlags, 0);
            em.Count(1800);
            game.Call<0x80132288>({work});
        }

        break;
    case 2:
        game.Call<0x80132280>({work, 1, m.every}, {double(m.rate)});

        if (CountDown(em)) {
            em.NextState();
            em.SetMotion(m.wake, m.wakeFlags, 0);
            game.Call<0x801322C8>({work});
        }

        break;
    case 3:
        if (em.Ended())
            game(m.done, {work});

        break;
    }
}

void Em001SleepPort(CpuContext* ctx) {
    Sleep(ctx, {0x17, 6, 0x18, 0, 0x19, 4, 0.6f, 10});
}

void Em003SleepPort(CpuContext* ctx) {
    Sleep(ctx, {0x17, 6, 0x18, 0, 0x19, 4, 0.5f, 15});
}

void Em008SleepPort(CpuContext* ctx) {
    Sleep(ctx, {0x28, 2, 0x6E, 4, 0x70, 4, 0.6f, 10});
}

void Em012SleepPort(CpuContext* ctx) {
    Sleep(ctx, {0x18, 2, 0x6E, 4, 0x71, 4, 0.4f, 15});
}

void Em015SleepPort(CpuContext* ctx) {
    Sleep(ctx, {8, 6, 4, 0, 5, 4, 0.6f, 15});
}

void Em016SleepPort(CpuContext* ctx) {
    Sleep(ctx, {8, 6, 4, 0, 5, 4, 0.4f, 15});
}

void Em006SleepPort(CpuContext* ctx) {
    Sleep(ctx, {0x17, 6, 0x18, 0, 0x19, 4, 0.6f, 10});
}

void Em005SleepPort(CpuContext* ctx) {
    Sleep(ctx, {0x17, 6, 0x18, 0, 0x19, 4, 0.6f, 20});
}

void Em019SleepPort(CpuContext* ctx) {
    Sleep(ctx, {8, 6, 4, 0, 5, 4, 0.6f, 15});
}

void Em007SleepPort(CpuContext* ctx) {
    Sleep(ctx, {0x28, 2, 0x6E, 4, 0x70, 4, 0.6f, 5});
}

// kind 18 0x80195DE4: Gobul's, flags +0x359 and +0x35B set and 0x8012CF84
// each step, set up with kind 4, 0x80130390/0x80130450 and 0x801321FC, ended
// through 0x80128158.
void Em018SleepPort(CpuContext* ctx) {
    GuestCall game{ctx};
    const uint32_t work = ctx->gpr[3];
    Memory::Write8(work + 0x359, 1);
    Memory::Write8(work + 0x35B, 1);
    game.Call<0x8012CF84>({work});
    ctx->gpr[3] = work;
    Sleep(ctx, {0xCC, 0, 0xCF, 0, 0xCD, 0, 0.4f, 15, 0x80128158, [](GuestCall& g, uint32_t w) { g.Call<Em::SetUp>({w, 4}); },
                [](GuestCall& g, uint32_t w) {
                    g.Call<0x80130390>({w}); // f1 passes on
                    g.Call<0x80130450>({w});
                    g.Call<0x801321FC>({w});
                }});
}

// kind 4 0x801C6014: Gigginox', set up with its carried scale and 0x80130724
// (work, 1), ended through 0x801281F8.
void Em004SleepPort(CpuContext* ctx) {
    Sleep(ctx, {0xEB, 4, 0xDD, 10, 0xDF, 6, 0.6f, 10, 0x801281F8, [](GuestCall& g, uint32_t w) {
                    g.Call<0x801302AC>({w}); // f1: the carried scale, for SetScale
                    g.Call<SetScale>({w});
                    g.Call<0x80130724>({w, 1});
                }});
}

// kinds 1/2 0x8014888C (r4: 0 steering through 0x8012CF84 each step, 1 with
// its dust): Rathian's and Rathalos' walk, motion 0x13 for 300 ticks (the
// original's 300 calls), then 1.5 (1.8 for walk 1). Walk 1 kicks up dust
// (0x33, 0x34 at joint 0x1A) and a puff (0x80104A34) on its motion's frames,
// the 0x34 dust every 4 steps of the step counter in two frame ranges.
void Em001WalkPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x20);
    GuestCall game{ctx};
    const Em::Enemy em{game, frame.Arg(3)};
    const uint32_t work = em.work, at = frame[0x08];
    const bool dusty = uint8_t(frame.Arg(4)) == 1;
    game.Call<0x80043F0C>({at});

    if (uint8_t(frame.Arg(4)) == 0)
        game.Call<0x8012CF84>({work});

    const auto dust = [&](uint32_t kind, float scale) {
        game.Call<Em::Vec3Set>({at}, {0.0, -30.0, 70.0});
        game.Call<Em::Spawn>({work, kind, 0x1A, at}, {double(scale)});
    };

    const auto puff = [&] {
        game.Call<Em::Vec3Set>({at}, {0.0, 0.0, 50.0});
        game.Call<0x80104A34>({work, 0x1A, 0x2F, 0, at}, {1.4});
    };

    switch (em.State()) {
    case 0:
        em.State(1);
        game.Call<Em::SetUp>({work, 0});
        em.SetMotion(0x13, 6, 0);
        em.Count(300);
        break;
    case 1:
        if (dusty) {
            if (em.At(92.0f) || em.At(262.0f))
                dust(0x33, 1.0f);

            if (em.At(96.0f))
                puff();

            if ((em.At(136.0f, 3, 152.0f) || em.At(344.0f, 3, 368.0f)) && StepCounter::Every(4))
                dust(0x34, 1.0f);

            if (em.At(156.0f) || em.At(190.0f) || em.At(386.0f))
                dust(0x33, 0.6f);

            if (em.At(262.0f))
                puff();

            if ((em.At(398.0f, 3, 426.0f) || em.At(458.0f, 3, 478.0f)) && StepCounter::Every(4))
                dust(0x34, 0.8f);

            if (em.At(434.0f))
                dust(0x33, 0.9f);
        }

        if (CountDown(em))
            game.Call<NextAction>({work, 1, dusty ? 8u : 5u});

        break;
    }
}

// kinds 1/2 0x8014A8C4: Rathian and Rathalos hovering, motion 0x1F looped for
// 60 ticks (the original's 60 calls), then 0x80148260.
void Em001HoverPort(CpuContext* ctx) {
    GuestCall game{ctx};
    const Em::Enemy em{game, ctx->gpr[3]};
    const uint32_t work = em.work;

    switch (em.State()) {
    case 0:
        em.State(1);
        game.Call<0x801302AC>({work}); // f1: the carried scale, for SetScale
        game.Call<SetScale>({work});
        game.Call<SetMotionLoop>({work, 0x1F, 4, 0});
        em.Count(0x3C);
        break;
    case 1:
        if (CountDown(em))
            game.Call<0x80148260>({work});

        break;
    }
}

// kinds 16/17 0x80184DFC (r4 the swim 0-9, r5: 0 motion 0x37, else 0x30): the
// Ludroth kinds swimming toward a point (0x80134068 at -360 or 0, flags
// 0x12); swim 3 rises to +0x210's height, giving up after 200 ticks; swims
// 5 and 9 end after 150 or 90 plus up to 31 ticks or once there; the others
// once there. Then motion 0x2F to its end (0x80128094). The counts were
// calls.
void Em016SwimPort(CpuContext* ctx) {
    GuestCall game{ctx};
    const Em::Enemy em{game, ctx->gpr[3]};
    const uint32_t work = em.work;
    const uint8_t swim = uint8_t(ctx->gpr[4]), other = uint8_t(ctx->gpr[5]);
    const auto way = [&](float distance) { game.Call<TurnSetUp>({work, 0x12}, {double(distance)}); };

    if (swim == 8) {
        game.Call<0x80131DE8>({work});
        game.Call<0x8012CF84>({work});
        game.Call<0x80131ED8>({work});
    }

    switch (em.State()) {
    case 0:
        em.State(1);
        game.Call<Em::SetUp>({work, 2});
        game.Call<SetMotionEx>({work, other == 0 ? 0x37u : 0x30u, 0x14, 0, 1});

        if (swim == 3) {
            Memory::Write32(work + 0x370, Memory::Read32(work + 0x210));
            em.Count(200);
        }

        switch (swim) {
        case 1:
            way(0.0f);
            Memory::WriteFloat32(work + 0x378, 700.0f);
            break;
        case 2:
            way(0.0f);
            Memory::WriteFloat32(work + 0x378, 1500.0f);
            break;
        case 4:
            way(0.0f);

            if (Memory::ReadFloat32(work + 0x378) > 700.0f)
                Memory::WriteFloat32(work + 0x378, 700.0f);

            break;
        case 5:
            way(0.0f);
            em.Count(int32_t((Memory::Read16(work + kRandom) & 0x1F) + 0x96));
            break;
        case 7:
            game.Call<SetMotionSpeed>({work}, {3.0f});
            way(-360.0f);
            break;
        case 8:
            way(0.0f);
            break;
        case 9:
            em.Count(int32_t((Memory::Read16(work + kRandom) & 0x1F) + 0x5A));
            [[fallthrough]];
        case 6:
            game.Call<SetMotionSpeed>({work}, {2.0f});
            way(-360.0f);
            break;
        default:
            way(-360.0f);
            break;
        }

        break;
    case 1: {
        bool there;

        if (swim == 3) {
            game.Call<TurnStep>({work, 0, 0x80});
            game.Call<0x80136DB0>({work}, {3.0});
            there = CountDown(em);

            if (!there) {
                game.Call<0x801359B4>({work}); // get_em_chg_scale
                const float size = game.F1();
                game.Call<0x801302D0>({work});
                const float below = Memory::ReadFloat32(work + 0x370) - (10.0f + game.F1()) * size;
                there = Memory::ReadFloat32(work + 0x18C) >= below;
            }
        } else if (swim == 5 || swim == 9)
            there = CountDown(em) || game.Call<TurnStep>({work, 0, 0x80}) == 1;
        else
            there = game.Call<TurnStep>({work, 0, 0x80}) == 1;

        if (there && game.Call<0x8012F9AC>({work}) == 0) {
            em.NextState();
            game.Call<SetMotionEx>({work, 0x2F, 0x28, 0, 1});
        }

        break;
    }
    case 2:
        if (em.Ended())
            game.Call<ActionDone94>({work});

        break;
    }
}

// kinds 16/17 0x8018599C: the Ludroth kinds resting underwater (motion 0x2A,
// then 0x2B), counting up to 1800 ticks; while it may act, every 30 ticks a
// hunter nearer than 3000 (+0x454 f32[4]) wakes it (0x8012B3E4 0, 2, then
// 5.17), and so does the count's end (0x8012B3E4 5, 2, 5). The original
// counted calls (and tested the count's multiples of 30 after each +1).
void Em016RestPort(CpuContext* ctx) {
    GuestCall game{ctx};
    const Em::Enemy em{game, ctx->gpr[3]};
    const uint32_t work = em.work;
    game.Call<0x8012CF84>({work});

    switch (em.State()) {
    case 0:
        em.State(1);
        game.Call<Em::SetUp>({work, 2});
        game.Call<SetMotionEx>({work, 0x2A, 10, 0, 1});
        game.Call<0x801321D0>({work});
        break;
    case 1:
        if (em.Ended()) {
            em.NextState();
            em.SetMotion(0x2B, 0, 0);
            em.Count(0);
        }

        break;
    case 2: {
        game.Call<0x8011E750>({work, 5, 0x1E}, {1.0}); // EnemyEveryNScaled
        const uint32_t ticks = GameStep::Ticks();
        const int32_t before = em.Count();
        em.Count(std::min(before + int32_t(ticks), 0x708));

        if (Memory::Read8(work + 0x1F5) != 1)
            break;

        // The counts this step's ticks pass, as the original tested each.
        if (ticks && Em::AnyOf(before + 1, ticks, 30, 0)) {
            for (uint32_t i = 0; i < 4; ++i) {
                const float distance = Memory::ReadFloat32(work + 0x454 + 4 * i);

                if (distance > 0.0f && distance <= 3000.0f) {
                    game.Call<0x8012B3E4>({work, 0, 2});
                    game.Call<NextAction>({work, 5, 0x11});
                    return;
                }
            }
        }

        if (em.Count() >= 0x708) {
            game.Call<0x8012B3E4>({work, 5, 2, 5});
            game.Call<0x801307DC>({work, 0});
            game.Call<NextAction>({work, 5, 0x11});
        }

        break;
    }
    }
}

// Timed actions, one per kind with the same code: a motion played for a
// count of ticks (a constant, or plus the random word's low bits), then the
// next action. The originals take one from the count each call and then
// test it, in one of these ways:
enum class Spent : uint8_t {
    BelowOne,  // below 1
    BelowZero, // below 0, the count then set back to 0
    AtZero,    // at exactly 0: here the step that reaches or crosses it
    Before,    // below 1 tested before a call takes one (a call longer)
    Never,     // not counted
};

using WorkCall = void (*)(GuestCall&, uint32_t work);

// The count less the step's ticks; whether it ran out.
bool RunOut(const Em::Enemy& em, Spent spent) {
    const int32_t ticks = int32_t(GameStep::Ticks());

    switch (spent) {
    case Spent::BelowOne:
        return CountDown(em);
    case Spent::BelowZero:
        if (CountDown(em) && em.Count() < 0) {
            em.Count(0);
            return true;
        }

        return false;
    case Spent::AtZero:
        if (ticks && em.Count() > 0 && em.Count() - ticks <= 0) {
            em.Count(0);
            return true;
        }

        em.Count(em.Count() - ticks);
        return false;
    case Spent::Before:
        if (em.Count() < 1)
            return true;

        em.Count(em.Count() - ticks);
        return false;    case Spent::Never:
        return false;
    }

    return false;
}

// The ends: the next action Group.Action, or a call (work).
template <uint32_t Group, uint32_t Action>
void Next(GuestCall& game, uint32_t work) {
    game.Call<NextAction>({work, Group, Action});
}

template <uint32_t Address>
void Then(GuestCall& game, uint32_t work) {
    game.Call<Address>({work});
}

// The height above the ground (+0x1AC) set again: 0x80130390 (work) -> f1,
// which 0x80130450 (work; f1) takes.
void KeepHeight(GuestCall& game, uint32_t work) {
    game.Call<0x80130390>({work});
    game.Call<0x80130450>({work});
}

using WorkTest = bool (*)(GuestCall&, uint32_t work);

struct TimedAction {
    uint32_t steps[3] = {};    // called (work) each step first, in order
    WorkCall each = nullptr;   // each step after them
    int32_t setUp = 0;         // SetUp's kind, -1 the carried scale (0x801302AC, SetScale)
    bool loop = false;         // the motion through SetMotionLoop
    uint32_t motion = 0, flags = 0;
    int32_t ticks = 0;         // the count
    uint16_t randomBits = 0;   // plus the random word (+0x1EC) & these
    WorkCall extra = nullptr;  // after the count is set
    WorkCall during = nullptr; // each step of the count, before it
    WorkTest first = nullptr;  // ends it before the count is taken (a turn done)
    Spent spent = Spent::BelowOne;
    WorkTest early = nullptr;  // ends it too, asked each step after the count
    WorkCall end = nullptr;
    WorkCall finish = nullptr; // state 2 (that `end` starts): at the motion's end
};

void Timed(CpuContext* ctx, const TimedAction& t) {
    GuestCall game{ctx};
    const Em::Enemy em{game, ctx->gpr[3]};
    const uint32_t work = em.work;

    for (const uint32_t step : t.steps)
        if (step)
            game(step, {work});

    if (t.each)
        t.each(game, work);

    switch (em.State()) {
    case 0:
        em.State(1);

        if (t.setUp < 0) {
            game.Call<0x801302AC>({work}); // f1: the carried scale, for SetScale
            game.Call<SetScale>({work});
        } else
            game.Call<Em::SetUp>({work, uint32_t(t.setUp)});

        if (t.loop)
            game.Call<SetMotionLoop>({work, t.motion, t.flags, 0});
        else
            em.SetMotion(t.motion, t.flags, 0);

        em.Count(t.ticks + (Memory::Read16(work + kRandom) & t.randomBits));

        if (t.extra)
            t.extra(game, work);

        break;
    case 1: {
        if (t.during)
            t.during(game, work);

        if (t.first && t.first(game, work)) {
            t.end(game, work);
            break;
        }

        const bool out = RunOut(em, t.spent);

        if ((t.early && t.early(game, work)) || out)
            t.end(game, work);

        break;
    }
    case 2:
        if (t.finish && em.Ended())
            t.finish(game, work);

        break;
    }
}

// Ended too when the float at +0x1D4 (0x8012FD28's, set up with 0.0) is 0.
bool Settled(GuestCall&, uint32_t work) {
    return Memory::ReadFloat32(work + 0x1D4) <= 0.0f;
}

// TurnStep (work, 0, Max) turned all the way.
template <uint32_t Max>
bool Turned(GuestCall& game, uint32_t work) {
    return game.Call<TurnStep>({work, 0, Max}) == 1;
}

// The end that starts state 2: Motion with Flags (blend Blend) played to its end.
template <uint32_t Motion, uint32_t Flags, uint32_t Blend = 0>
void ThenMotion(GuestCall& game, uint32_t work) {
    Memory::Write8(work + Em::kState, 2);
    game.Call<Em::SetMotion>({work, Motion, Flags, Blend});
}

// The timed actions found by their per-call counts (+0x20 - 1 each step):
//   0x80177A00 kind 15 (Lagiacrus) motion 6, 300 ticks, then 1.6
//   0x80379C34 kind 19 (Agnaktor) the same; 0x8037F414 too, with 0x803B9C04
//              (work, its place, 60) and then 13.8
//   0x801AD4FC, 0x801AD58C kind 27 (Aptonoth) motion 3 / 4 (looped), 120 /
//              60 ticks plus up to 127
//   0x801AC888 kind 27 (Aptonoth) motion 4 looped at 1.2, steering (0x100)
//              each step, 30 ticks plus up to 63
//   0x801B1198, 0x801B1378 kinds 30/31 (Felyne, Melynx) motion 0x1C / 0x24,
//              60 / 150 ticks plus up to 63
//   0x80157FA8 kind 3 (Qurupeco) hovering, motion 0x1F for 60 ticks, then
//              0x80155700
//   0x801BC10C kind 6 (Diablos), 0x801CB5E0 kind 5 (Barioth) walking (motion
//              0x13, steering each step) for 300 ticks, then 1.4 / 1.3
//   0x801C40B0 kind 4 (Gigginox) sub 4, 0x801CDDF8 kind 5 (Barioth) sub 0xB:
//              motion 0x15 / 0x1F looped at the carried scale for 60 ticks
//   0x80194D2C kind 18 (Gobul) sub 0x1F: motion 0x80 for 190 ticks, then 5.32
//   0x8035DA78 kind 33 (Altaroth): motion 3 looped for 31 ticks
//   0x80170CCC kinds 12/14 (Great Jaggi, Baggi) sub 4: motion 200 for 133
//              ticks, then state 2 and 1.5
//   0x801A14B8 em025 (Jhen Mohran) sub 0x10: motion 1 looped for 150 ticks
//   0x80167904 kind 10 (Jaggi) sub 8, 0x8016DB84 kind 11 (Jaggia) sub 0xB:
//              motion 0xE / 10 for 32 / 30 ticks or until +0x1D4 is 0
//   0x8015FB24 kind 8 (Barroth) sub 0xC: motion 0x25 looped for 150 ticks
//   under the ground (set up with kind 4, motion 1 looped) for 90 ticks, the
//   original's 90 calls ended at exactly 0, then 0x80128158: 0x80388D80 kind
//   9 (Uragaan, under the lava), 0x801D8B28 kind 7 (Deviljho), 0x8015F400
//   kind 8 (Barroth) sub 1
void Em015PausePort(CpuContext* ctx) {
    Timed(ctx, {.motion = 0x6, .flags = 10, .ticks = 300, .end = Next<1, 6>});
}

void Em019PausePort(CpuContext* ctx) {
    Timed(ctx, {.motion = 0x6, .flags = 10, .ticks = 300, .end = Next<1, 6>});
}

void Em019Pause2Port(CpuContext* ctx) {
    Timed(ctx, {.motion = 0x6, .flags = 10, .ticks = 300,
                .extra = [](GuestCall& g, uint32_t w) { g.Call<0x803B9C04>({w, w + 0x188, 0x3C}); },
                .end = Next<13, 8>});
}

void Em027GrazePort(CpuContext* ctx) {
    Timed(ctx, {.motion = 0x3, .flags = 4, .ticks = 120, .randomBits = 0x7F, .end = Then<ActionDone>});
}

void Em027Graze2Port(CpuContext* ctx) {
    Timed(ctx, {.loop = true, .motion = 0x4, .flags = 4, .ticks = 60, .randomBits = 0x7F, .end = Then<ActionDone>});
}

void Em027TrotPort(CpuContext* ctx) {
    Timed(ctx, {.loop = true, .motion = 0x4, .flags = 4, .ticks = 30, .randomBits = 0x3F,
                .extra = [](GuestCall& g, uint32_t w) { g.Call<SetMotionSpeed>({w}, {1.2f}); },
                .during = [](GuestCall& g, uint32_t w) { g.Call<Steer>({w, 0x100}); },
                .end = Then<ActionDone>});
}

void Em030IdlePort(CpuContext* ctx) {
    Timed(ctx, {.motion = 0x1C, .flags = 4, .ticks = 60, .randomBits = 0x3F, .end = Then<ActionDone>});
}

void Em030Idle2Port(CpuContext* ctx) {
    Timed(ctx, {.motion = 0x24, .flags = 6, .ticks = 150, .randomBits = 0x3F, .end = Then<ActionDone>});
}

void Em003HoverPort(CpuContext* ctx) {
    Timed(ctx, {.setUp = -1, .loop = true, .motion = 0x1F, .flags = 4, .ticks = 60, .end = Then<0x80155700>});
}

void Em006WalkPort(CpuContext* ctx) {
    Timed(ctx, {.steps = {0x8012CF84}, .motion = 0x13, .flags = 6, .ticks = 300, .end = Next<1, 4>});
}

void Em005WalkPort(CpuContext* ctx) {
    Timed(ctx, {.steps = {0x8012CF84}, .motion = 0x13, .flags = 6, .ticks = 300, .end = Next<1, 3>});
}

void Em004WaitPort(CpuContext* ctx) {
    Timed(ctx, {.steps = {0x80131E00}, .setUp = -1, .loop = true, .motion = 0x15, .flags = 4, .ticks = 60, .end = Then<0x801C2BEC>});
}

void Em005WaitPort(CpuContext* ctx) {
    Timed(ctx, {.steps = {0x8012CF84, 0x80131E00}, .setUp = -1, .loop = true, .motion = 0x1F, .flags = 4, .ticks = 60,
                .end = Then<0x801CAFD4>});
}

void Em018WaitPort(CpuContext* ctx) {
    Timed(ctx, {.steps = {0x8012CF84, 0x80131ED8}, .setUp = 2, .motion = 0x80, .flags = 4, .ticks = 190, .end = Next<5, 0x20>});
}

void Em033WaitPort(CpuContext* ctx) {
    Timed(ctx, {.steps = {0x80131E58, 0x80131DE8}, .loop = true, .motion = 3, .flags = 2, .ticks = 30, .spent = Spent::BelowZero,
                .end = Then<ActionDone>});
}

void Em012PausePort(CpuContext* ctx) {
    Timed(ctx, {.motion = 200, .flags = 4, .ticks = 0x84, .spent = Spent::Before,
                .end = [](GuestCall& g, uint32_t w) { Memory::Write8(w + Em::kState, 2); g.Call<NextAction>({w, 1, 5}); }});
}

void Em025RestPort(CpuContext* ctx) {
    Timed(ctx, {.setUp = 4, .loop = true, .motion = 1, .flags = 0x1E, .ticks = 150, .extra = KeepHeight,
                .end = [](GuestCall& g, uint32_t w) { KeepHeight(g, w); g.Call<0x8019F190>({w}); }});
}

void Em010BackPort(CpuContext* ctx) {
    Timed(ctx, {.steps = {0x80131DE8, 0x80131ED8, 0x80131E58}, .motion = 0xE, .flags = 8, .ticks = 0x20,
                .extra = [](GuestCall& g, uint32_t w) { g.Call<0x8012FD28>({w, 0x1E}, {0.0f}); },
                .early = Settled, .end = Then<0x8012E6F8>});
}

void Em011BackPort(CpuContext* ctx) {
    Timed(ctx, {.steps = {0x80131DE8, 0x80131ED8, 0x80131E58}, .motion = 10, .flags = 4, .ticks = 0x1E,
                .extra = [](GuestCall& g, uint32_t w) { g.Call<0x8012FD28>({w, 0x1C}, {0.0f}); },
                .early = Settled, .end = Then<0x8012E6F8>});
}

void Em008WaitPort(CpuContext* ctx) {
    Timed(ctx, {.steps = {0x80131ED8, 0x80136D78}, .setUp = 4, .loop = true, .motion = 0x25, .flags = 0, .ticks = 150,
                .extra = [](GuestCall& g, uint32_t w) {
                    g.Call<0x80130450>({w}, {0.0f});
                    g.Call<0x8015D8C4>({w});
                    g.Call<0x801321FC>({w});
                },
                .end = Then<0x8015DE64>});
}

void Em009BuriedPort(CpuContext* ctx) {
    Timed(ctx, {.steps = {0x8012CF84, 0x80131ED8}, .setUp = 4, .loop = true, .motion = 1, .ticks = 0x5A, .extra = KeepHeight,
                .spent = Spent::AtZero, .end = Then<0x80128158>});
}

void Em007BuriedPort(CpuContext* ctx) {
    Timed(ctx, {.steps = {0x8012CF84, 0x80131ED8}, .setUp = 4, .loop = true, .motion = 1, .ticks = 0x5A, .extra = KeepHeight,
                .spent = Spent::AtZero, .end = Then<0x80128158>});
}

void Em008BuriedPort(CpuContext* ctx) {
    Timed(ctx, {.steps = {0x80131ED8, 0x80131E64, 0x80136D78}, .setUp = 4, .loop = true, .motion = 1, .ticks = 0x5A,
                .extra = KeepHeight, .spent = Spent::AtZero, .end = Then<0x80128158>});
}

// More timed actions (the static list of per-call counts):
//   0x8019F5D0 em025 (Jhen Mohran) motion 0xD looped for 150 ticks; 0x801A13E8
//              motion 1 looped at the height 630, 240 ticks (r4 0) or 150
//   0x8032D89C kind 26 (Giggi) motion 0x72 for 120 ticks; 0x8032D53C,
//              0x8032D628 crawls (motion 5 / 7 at 1 / 4 plus 0.1 / 0.2 times
//              the random word & 3) until turned (TurnStep 0x80) or 150 / 120
//              ticks; 0x8032DC10 a turn (0xCD) of at most 120 ticks
//   0x8035C620 kind 33 (Altaroth), the same as Jaggi's 0x80167904 (motion 3,
//              30 ticks); 0x8035C410 motion 0xC for 118 ticks, then 1.3 (r4
//              0) or 1.12
//   0x8035F400, 0x8035F6A8 kind 35 (Bnahabra) resting upside down (+0x1C4
//              0x8000) 90 ticks
//   0x803796F8, 0x8037A56C kind 19 (Agnaktor) in the lava (its height 2000)
//              90 ticks (or 30 with r4 1), ended at exactly 0
//   0x801AC714, 0x801AC930 kind 27 (Aptonoth) turning walks of 30 ticks plus
//              up to 31 or 63
//   0x801B0F64, 0x801B1020, 0x801B10DC kinds 30/31 (Felyne, Melynx) motion
//              0x16 / 0x17 / 0x18 for 120 / 150 / 150 ticks, then 0x19 to its
//              end; 0x801B19BC a turn (6) of at most 300 ticks, then 0x19
//   0x801685D8 kind 10 (Jaggi) a turn (0xE at 0.8) of at most 60 ticks
void Em025WaitPort(CpuContext* ctx) {
    Timed(ctx, {.loop = true, .motion = 0xD, .flags = 0x1E, .ticks = 150, .end = Then<ActionDone>});
}

void Em025Rest2Port(CpuContext* ctx) {
    const bool shorter = uint8_t(ctx->gpr[4]) != 0;
    Timed(ctx, {.setUp = 4, .loop = true, .motion = 1, .flags = 0x1E, .ticks = shorter ? 150 : 240,
                .extra = [](GuestCall& g, uint32_t w) { g.Call<0x80130450>({w}, {630.0}); }, .end = Then<0x80128158>});
}

void Em026WaitPort(CpuContext* ctx) {
    Timed(ctx, {.motion = 0x72, .flags = 4, .ticks = 120, .end = Then<ActionDone>});
}

// Giggi's crawl speed: Base plus Step times the random word & 3, then a turn
// set up toward -200.
template <int Base10, int Step10>
void Em026Crawl(GuestCall& game, uint32_t work) {
    const float step = float(Step10) / 100.0f * float(Memory::Read16(work + kRandom) & 3); // fmuls, then fadds: no fused multiply-add
    const float speed = float(Base10) / 10.0f + step;
    game.Call<SetMotionSpeed>({work}, {speed});
    game.Call<TurnSetUp>({work, 0}, {-200.0});
}

void Em026CrawlPort(CpuContext* ctx) {
    Timed(ctx, {.motion = 5, .flags = 4, .ticks = 150, .extra = Em026Crawl<10, 10>, .first = Turned<0x80>, .end = Then<ActionDone>});
}

void Em026Crawl2Port(CpuContext* ctx) {
    Timed(ctx, {.motion = 7, .flags = 4, .ticks = 120, .extra = Em026Crawl<40, 20>, .first = Turned<0x80>, .end = Then<ActionDone>});
}

void Em026TurnPort(CpuContext* ctx) {
    Timed(ctx, {.setUp = -1, .motion = 0xCD, .flags = 4, .ticks = 120,
                .extra = [](GuestCall& g, uint32_t w) {
                    g.Call<TurnSetUp>({w, 0}, {-100.0});
                    g.Call<0x80136E58>({w});
                },
                .during = Then<0x80136E58>, .first = Turned<0x40>, .end = Then<0x80128048>});
}

void Em033BackPort(CpuContext* ctx) {
    Timed(ctx, {.steps = {0x80131DE8, 0x80131ED8, 0x80131E58}, .motion = 3, .flags = 4, .ticks = 0x1E,
                .extra = [](GuestCall& g, uint32_t w) { g.Call<0x8012FD28>({w, 0x1E}, {0.0}); }, .early = Settled, .end = Then<0x8012E6F8>});
}

void Em033PausePort(CpuContext* ctx) {
    const bool second = uint8_t(ctx->gpr[4]) != 0;
    Timed(ctx, {.steps = {0x8012CF84}, .motion = 0xC, .flags = 4, .ticks = 0x76, .end = second ? Next<1, 0xC> : Next<1, 3>});
}

void Em035Rest(CpuContext* ctx, uint32_t flags) {
    Timed(ctx, {.each = [](GuestCall&, uint32_t w) { Memory::Write32(w + 0x1C4, 0x8000); }, .setUp = -1, .loop = true, .motion = 1,
                .flags = flags, .ticks = 0x5A, .extra = Then<0x80136E58>, .during = Then<0x80136E58>, .end = Then<0x80128048>});
}

void Em035RestPort(CpuContext* ctx) {
    Em035Rest(ctx, 4);
}

void Em035Rest2Port(CpuContext* ctx) {
    Em035Rest(ctx, 0);
}

// Agnaktor in the lava: its steps (0x80131ED8, 0x80136E58, the height 2000
// through 0x80130404, 0x80136D78, 0x803790E8) and 0x80379000 (work, 0xFF).
void Em019Lava(CpuContext* ctx, int32_t ticks, WorkCall end) {
    Timed(ctx, {.steps = {0x80131ED8, 0x80136E58},
                .each = [](GuestCall& g, uint32_t w) {
                    g.Call<0x80130404>({w}, {2000.0});
                    g.Call<0x80136D78>({w});
                    g.Call<0x803790E8>({w});
                },
                .setUp = -1, .loop = true, .motion = 1, .ticks = ticks, .extra = [](GuestCall& g, uint32_t w) { g.Call<0x80379000>({w, 0xFF}); },
                .spent = Spent::AtZero, .end = end});
}

void Em019LavaPort(CpuContext* ctx) {
    Em019Lava(ctx, 0x5A, Then<0x80128048>);
}

void Em019Lava2Port(CpuContext* ctx) {
    const uint8_t how = uint8_t(ctx->gpr[4]);
    Em019Lava(ctx, how == 0 ? 0x5A : how == 1 ? 0x1E : int32_t(Memory::Read32(ctx->gpr[3] + Em::kCount)), Then<0x80379510>);
}

void Em027TurnPort(CpuContext* ctx) {
    Timed(ctx, {.motion = 4, .flags = 4, .ticks = 0x1E, .randomBits = 0x1F,
                .extra = [](GuestCall& g, uint32_t w) {
                    if (Memory::Read8(w + 10) == 2)
                        g.Call<SetMotionSpeed>({w}, {1.5});

                    g.Call<TurnSetUp>({w, 0x10}, {0.0});
                },
                .during = [](GuestCall& g, uint32_t w) { g.Call<TurnStep>({w, 0, 0x80}); }, .end = Then<ActionDone>});
}

void Em027Turn2Port(CpuContext* ctx) {
    const uint8_t how = uint8_t(ctx->gpr[4]);
    const int32_t count = int32_t(Memory::Read32(ctx->gpr[3] + Em::kCount));
    Timed(ctx, {.loop = true, .motion = 4, .flags = 4, .ticks = how <= 1 ? 0x1E : count, .randomBits = uint16_t(how == 0 ? 0x3F : how == 1 ? 0x1F : 0),
                .extra = how == 0 ? WorkCall([](GuestCall& g, uint32_t w) {
                    g.Call<SetMotionSpeed>({w}, {1.2f});
                    g.Call<TurnSetUp>({w, 0x10}, {0.0});
                })
                                  : WorkCall([](GuestCall& g, uint32_t w) { g.Call<TurnSetUp>({w, 0x10}, {0.0}); }),
                .during = [](GuestCall& g, uint32_t w) {
                    if (Memory::Read8(w + 6) == 0 && g.Call<TurnStep>({w, 0, 0x100}) == 1)
                        Memory::Write8(w + 6, uint8_t(Memory::Read8(w + 6) + 1));
                },
                .end = Then<ActionDone>});
}

void Em030Idle3Port(CpuContext* ctx) {
    Timed(ctx, {.motion = 0x16, .flags = 4, .ticks = 120, .end = ThenMotion<0x19, 6>, .finish = Then<ActionDone>});
}

void Em030Idle4Port(CpuContext* ctx) {
    Timed(ctx, {.motion = 0x17, .flags = 4, .ticks = 150, .end = ThenMotion<0x19, 6, 4>, .finish = Then<ActionDone>});
}

void Em030Idle5Port(CpuContext* ctx) {
    Timed(ctx, {.motion = 0x18, .flags = 4, .ticks = 150, .end = ThenMotion<0x19, 4>, .finish = Then<ActionDone>});
}

void Em030TurnPort(CpuContext* ctx) {
    Timed(ctx, {.motion = 6, .flags = 4, .ticks = 300, .extra = [](GuestCall& g, uint32_t w) { g.Call<TurnSetUp>({w, 0}, {-100.0}); },
                .first = Turned<0x800>, .end = ThenMotion<0x19, 4>, .finish = Then<ActionDone>});
}

void Em010Turn2Port(CpuContext* ctx) {
    const uint8_t how = uint8_t(ctx->gpr[4]);
    WorkCall setUp = [](GuestCall& g, uint32_t w) { g.Call<SetMotionSpeed>({w}, {0.8f}); };

    if (how == 0)
        setUp = [](GuestCall& g, uint32_t w) {
            g.Call<SetMotionSpeed>({w}, {0.8f});
            g.Call<TurnSetUp>({w, 0}, {-300.0});
        };
    else if (how == 1)
        setUp = [](GuestCall& g, uint32_t w) {
            g.Call<SetMotionSpeed>({w}, {0.8f});
            g.Call<TurnSetUp>({w, 0}, {-600.0});
        };

    Timed(ctx, {.loop = true, .motion = 0xE, .flags = 8, .ticks = 0x3C, .extra = setUp, .early = Turned<0x200>,
                .end = [](GuestCall& g, uint32_t w) {
                    g.Call<SetMotionSpeed>({w}, {1.0});
                    g.Call<ActionDone>({w});
                }});
}

// kinds 30/31 0x801B27F0: a charge (motion 7, +0x330 set each step,
// steering 0x400) of 450 ticks, cut to 15 when 0x801B01CC (work) is 1.
void Em030Charge2Port(CpuContext* ctx) {
    Timed(ctx, {.each = [](GuestCall&, uint32_t w) { Memory::Write8(w + kCharging, 1); }, .motion = 7, .flags = 4, .ticks = 0x1C2,
                .during = [](GuestCall& g, uint32_t w) { g.Call<Steer>({w, 0x400}); },
                .early = [](GuestCall& g, uint32_t w) {
                    if (int32_t(Memory::Read32(w + Em::kCount)) > 15 && g.Call<0x801B01CC>({w}) == 1)
                        Memory::Write32(w + Em::kCount, 15);

                    return false;
                },
                .end = Then<ActionDone>});
}

// kind 8 0x8015FA60: Barroth turning toward its target (motion 0x25,
// steering 0x5B a step, 0x80131ED8 and 0x80136D78 each step) while it counts
// up to 180 ticks, then 0x8015DE64.
void Em008TrackPort(CpuContext* ctx) {
    GuestCall game{ctx};
    const Em::Enemy em{game, ctx->gpr[3]};
    const uint32_t work = em.work;
    game.Call<0x80131ED8>({work});
    game.Call<0x80136D78>({work});

    switch (em.State()) {
    case 0:
        em.State(1);
        game.Call<Em::SetUp>({work, 4});
        game.Call<SetMotionLoop>({work, 0x25, 0, 0});
        game.Call<0x80130450>({work}, {0.0});
        em.Count(0);
        game.Call<0x8015D8C4>({work});
        game.Call<0x801321FC>({work});
        break;
    case 1:
        game.Call<Steer>({work, 0x5B});
        em.CountTicks();

        if (em.Count() > 0xB3)
            game.Call<0x8015DE64>({work});

        break;
    }
}

// kind 3 0x8015814C: Qurupeco settling to the height +0x370 (motion 0x1A at
// its carried scale), its y (+0x18C) moved 0.95 of the way a tick, at most
// 60, until within 50 or after 100 ticks; then state 2 and 0x80128048. The
// original moved by a tick's amount each call; here by the step's (1 - 0.05^s
// of the way, at most 60 s).
void Em003SettlePort(CpuContext* ctx) {
    GuestCall game{ctx};
    const Em::Enemy em{game, ctx->gpr[3]};
    const uint32_t work = em.work;

    switch (em.State()) {
    case 0:
        em.State(1);
        game.Call<0x801302AC>({work});
        game.Call<SetScale>({work});
        em.SetMotion(0x1A, 6, 0);
        em.Count(100);
        break;
    case 1: {
        const double s = GameStep::Scale();
        const float rate = float(1.0 - std::pow(1.0 - double(0.95f), s)), limit = float(60.0 * s);
        const float target = Memory::ReadFloat32(work + 0x370), y0 = Memory::ReadFloat32(work + 0x18C);
        const float y = y0 + std::clamp(rate * (target - y0), -limit, limit);
        Memory::WriteFloat32(work + 0x18C, y);

        if (CountDown(em) || (y - 50.0f <= target && target <= 50.0f + y)) {
            em.NextState();
            game.Call<0x80128048>({work});
        }

        break;
    }
    }
}

// kind 34 0x801B7C40 (r4 1: a short, fast one): Kelbi's hop back (motion 3,
// its velocity 5 or 40 backward along its yaw) steering 0x1000 a step, moving
// for 100 ticks or 10; then 0x80128048. 0x801B70AC after every step.
void Em034HopAwayPort(CpuContext* ctx) {
    GuestCall game{ctx};
    const Em::Enemy em{game, ctx->gpr[3]};
    const uint32_t work = em.work;
    const bool fast = uint8_t(ctx->gpr[4]) == 1;

    switch (em.State()) {
    case 0:
        em.State(1);
        game.Call<0x801302AC>({work});
        game.Call<SetScale>({work});
        em.SetMotion(3, 0x14, 0);
        game.Call<0x8013545C>({work});
        Memory::WriteFloat32(work + kSideSpeed, -5.0f);
        em.Count(100);

        if (fast) {
            Memory::WriteFloat32(work + kSideSpeed, -5.0f * 8.0f);
            em.Count(10);
        }

        game.Call<RotateY>({work + kVelocity, Memory::Read32(work + kYaw)});
        break;
    case 1:
        game.Call<Steer>({work, 0x1000});

        if (em.Count() < 1)
            game.Call<0x80128048>({work});
        else {
            game.Call<AddVelocity>({work});
            em.Count(em.Count() - int32_t(GameStep::Ticks()));
        }

        break;
    }

    game.Call<0x801B70AC>({work});
}

// kind 34 0x801B81F0: Kelbi's leap to its mark (+0x36C, motion 1), a tenth
// of the way a tick for 10 ticks, 0x801B7108 each step; then its yaw from
// +0x32E and 3.10.
void Em034LeapToPort(CpuContext* ctx) {
    GuestCall game{ctx};
    const Em::Enemy em{game, ctx->gpr[3]};
    const uint32_t work = em.work;

    switch (em.State()) {
    case 0:
        em.State(1);
        game.Call<0x801302AC>({work});
        game.Call<SetScale>({work});
        em.SetMotion(1, 4, 0);
        game.Call<0x8013545C>({work});

        for (uint32_t i = 0; i < 3; ++i) {
            const float way = Memory::ReadFloat32(work + 0x36C + 4 * i) - Memory::ReadFloat32(work + 0x188 + 4 * i);
            Memory::WriteFloat32(work + kVelocity + 4 * i, way * 0.1f);
        }

        em.Count(10);
        break;
    case 1:
        game.Call<0x801B7108>({work});

        if (em.Count() < 1) {
            Memory::Write32(work + kYaw, Memory::Read16(work + 0x32E));
            game.Call<NextAction>({work, 3, 10});
        } else {
            game.Call<AddVelocity>({work});
            em.Count(em.Count() - int32_t(GameStep::Ticks()));
        }

        break;
    }
}

// kind 34 0x801B5D88 (r4 0: then back to the AI): Kelbi's motion 10 for 40
// ticks plus 15 times the random word & 3, then (r4 not 0) motion 0x25 (+10
// bit 0) or 0xD to its end.
void Em034GrazePort(CpuContext* ctx) {
    const bool then = uint8_t(ctx->gpr[4]) != 0;
    Timed(ctx, {.motion = 10, .flags = 4,
                .extra = [](GuestCall&, uint32_t w) { Memory::Write32(w + Em::kCount, (Memory::Read16(w + kRandom) & 3) * 15 + 0x28); },
                .end = then ? WorkCall([](GuestCall& g, uint32_t w) {
                    Memory::Write8(w + Em::kState, 2);
                    g.Call<Em::SetMotion>({w, Memory::Read8(w + 10) & 1 ? 0x25u : 0xDu, 4, 0});
                })
                            : Then<ActionDone>,
                .finish = Then<ActionDone>});
}

// kind 8 0x8015F948: Barroth's motion 0x25 for 300 ticks (as 0x8015FB24),
// then 6.9 (the random word & 15 0) or 6.8; 6.9 at once when +0x44E is below
// +0x452.
bool Em008Hurt(GuestCall&, uint32_t work) {
    return int16_t(Memory::Read16(work + 0x44E)) < int16_t(Memory::Read16(work + 0x452));
}

void Em008HoldPort(CpuContext* ctx) {
    Timed(ctx, {.steps = {0x80131ED8, 0x80136D78}, .setUp = 4, .loop = true, .motion = 0x25, .ticks = 300,
                .extra = [](GuestCall& g, uint32_t w) {
                    g.Call<0x80130450>({w}, {0.0});
                    g.Call<0x8015D8C4>({w});
                    g.Call<0x801321FC>({w});
                },
                .first = Em008Hurt,
                .end = [](GuestCall& g, uint32_t w) {
                    const bool nine = Em008Hurt(g, w) || (Memory::Read16(w + kRandom) & 0xF) == 0;
                    Memory::Write8(w + Em::kState, 2);
                    g.Call<NextAction>({w, 6, nine ? 9u : 8u});
                }});
}

// kinds 30/31 0x801B1AA4: Felyne's motion 6 for 120 ticks, turning toward
// (+6 & 15) sixteenths of a turn (the random word's, 0x800 a step), then 0x19
// to its end.
void Em030LookPort(CpuContext* ctx) {
    Timed(ctx, {.motion = 6, .flags = 4, .ticks = 120,
                .extra = [](GuestCall&, uint32_t w) { Memory::Write8(w + 6, uint8_t(Memory::Read16(w + kRandom) & 0xF)); },
                .early = [](GuestCall& g, uint32_t w) {
                    if (int32_t(Memory::Read32(w + Em::kCount)) >= 1) {
                        const uint32_t goal = uint32_t(Memory::Read8(w + 6) & 0xF) << 12;
                        Memory::Write32(w + kYaw, g.Call<0x80133E14>({goal, Memory::Read32(w + kYaw) & 0xFFFF, 0x800}) & 0xFFFF); // EnemyTurnToward
                    }

                    return false;
                },
                .end = ThenMotion<0x19, 4>, .finish = Then<ActionDone>});
}

// kinds 30/31 0x801B1878: Felyne's motion 0x18 to its end, then a turn (5,
// toward -200) of at most 300 ticks, cut to 15 when 0x801B01CC (work) is 1,
// then 0x19 to its end.
void Em030Turn2Port(CpuContext* ctx) {
    GuestCall game{ctx};
    const Em::Enemy em{game, ctx->gpr[3]};
    const uint32_t work = em.work;

    switch (em.State()) {
    case 0:
        em.State(1);
        game.Call<Em::SetUp>({work, 0});
        em.SetMotion(0x18, 4, 0);
        break;
    case 1:
        if (em.Ended()) {
            em.NextState();
            em.SetMotion(5, 4, 0);
            game.Call<TurnSetUp>({work, 0}, {-200.0});
            em.Count(300);
        }

        break;
    case 2:
        if (game.Call<TurnStep>({work, 0, 0x100}) == 1 || CountDown(em)) {
            em.NextState();
            em.SetMotion(0x19, 4, 0);
        }

        if (em.Count() > 15 && game.Call<0x801B01CC>({work}) == 1)
            em.Count(15);

        break;
    case 3:
        if (em.Ended())
            game.Call<ActionDone>({work});

        break;
    }
}

// kind 26 0x8032D304: Giggi hanging (motion 1 at the height -100, 0x80131E18,
// 0x80131E58 and 0x80136D78 each step) counting up; every 32 ticks from the
// start it looks for a hunter (present, +0x3B0 set) whose distance (+0x454 +
// 4 * i) is within 0-1000: then 1.1.
void Em026HangPort(CpuContext* ctx) {
    GuestCall game{ctx};
    const Em::Enemy em{game, ctx->gpr[3]};
    const uint32_t work = em.work;
    game.Call<0x80131E18>({work});
    game.Call<0x80131E58>({work});
    game.Call<0x80136D78>({work});

    switch (em.State()) {
    case 0:
        em.State(1);
        game.Call<Em::SetUp>({work, 0});
        em.SetMotion(1, 0, 0);
        game.Call<0x80130450>({work}, {-100.0});
        em.Count(0);
        break;
    case 1:
        game.Call<0x80130450>({work}, {-100.0});

        if (em.CountEvery(32)) {
            const uint32_t hunters = game.Call<0x800CFB34>({2}) & 0xFFFF; // get_move_work_max
            uint32_t hunter = game.Call<0x800CFAF4>({2});                 // get_move_work_adrs

            for (uint32_t i = 0; (i & 0xFF) < hunters; ++i, hunter += 0xB20) {
                if (!hunter || !Memory::Read8(hunter) || !Memory::Read32(hunter + 0x3B0))
                    continue;

                const float distance = Memory::ReadFloat32(work + (i & 0xFF) * 4 + 0x454);

                if (distance <= 1000.0f && 0.0f <= distance) {
                    game.Call<NextAction>({work, 1, 1});
                    return;
                }
            }
        }

        em.CountTicks();
        break;
    }
}

// kind 3 0x80155FD8: Qurupeco's other stagger (motion 200, 0x80432230):
// 150 ticks, 300 with a broken head, sped up by 2 * 1.2 / its rate (and the
// count divided by 1.2) when +0x7C8 is over 40; 0x80432244 (work, +0x334,
// head broken) each step; then 1.21.
void Em003Stagger2Port(CpuContext* ctx) {
    GuestCall game{ctx};
    const Em::Enemy em{game, ctx->gpr[3]};
    const uint32_t work = em.work;
    constexpr uint32_t HeadBroken = 0x8015D674; // em003_atama_koware_act_ck

    switch (em.State()) {
    case 0:
        em.State(1);
        Memory::Write8(work + Em::kSubState, 0);
        game.Call<Em::SetUp>({work, 0});
        em.SetMotion(200, 4, 0);
        em.Count(game.Call<HeadBroken>({work}) == 1 ? 300 : 0x96);

        if (Memory::Read8(work + 0x7C8) > 0x28) {
            game.Call<0x8012F948>({work}); // the motion's rate
            const float perRate = 1.2f / game.F1();
            game.Call<SetMotionSpeed>({work}, {2.0f * perRate});
            em.Count(int32_t(float(em.Count()) / 1.2f));
        }

        game.Call<0x80432230>({work});
        break;
    case 1: {
        const bool broken = game.Call<HeadBroken>({work}) == 1;
        game.Call<0x80432244>({work, Memory::Read8(work + 0x334), broken ? 1u : 0u});

        if (CountDown(em))
            game.Call<NextAction>({work, 1, 0x15});

        break;
    }
    }
}

// kind 34 0x801B7E98 (r4 0: steering 0x1000 a step) and 0x801B7ACC (r4 1: a
// turn toward -450, else -80, ended when done): Kelbi's jump toward its mark
// (+0x36C, 60 higher, motion 2) at 10 a tick for 150 ticks, or 8 for 240
// steering each step; then 0x80128048. 0x801B70AC after every step.
void Em034Jump(CpuContext* ctx, bool turning) {
    GuestFrame frame(ctx, 0x30);
    GuestCall game{ctx};
    const Em::Enemy em{game, frame.Arg(3)};
    const uint32_t work = em.work, way = frame[0x14], velocity = frame[0x08];
    const uint8_t how = uint8_t(frame.Arg(4));

    switch (em.State()) {
    case 0:
        em.State(1);
        game.Call<0x801302AC>({work});
        game.Call<SetScale>({work});
        em.SetMotion(2, 0x14, 0);

        if (turning)
            game.Call<TurnSetUp>({work, 8}, {how == 1 ? -450.0 : -80.0});

        game.Call<0x8013545C>({work});
        game.Call<0x80041EA4>({way, work + 0x36C}); // Vec3Copy
        Memory::WriteFloat32(way + 4, Memory::ReadFloat32(way + 4) + 60.0f);
        game.Call<0x800B0BF4>({way, work + 0x188}); // way -= position
        game.Call<0x80050F40>({way});               // VECMag

        if (game.F1() > 0.001f) {
            game.Call<0x800508B4>({way, way}); // VecNormalizeTo
            game.Call<0x80051F44>({velocity, way}, {turning ? 8.0 : 10.0});
            game.Call<0x80041EA4>({work + kVelocity, velocity});
        }

        em.Count(turning ? 0xF0 : 0x96);
        break;
    case 1:
        if (turning || how == 0)
            game.Call<Steer>({work, 0x1000});

        if ((turning && game.Call<TurnStep>({work, 0, 0}) == 1) || em.Count() < 1)
            game.Call<0x80128048>({work});
        else {
            game.Call<AddVelocity>({work});
            em.Count(em.Count() - int32_t(GameStep::Ticks()));
        }

        break;
    }

    game.Call<0x801B70AC>({work});
}

void Em034JumpPort(CpuContext* ctx) {
    Em034Jump(ctx, false);
}

void Em034JumpTurnPort(CpuContext* ctx) {
    Em034Jump(ctx, true);
}

// kind 34 0x801B82F4: Kelbi's look up (motion 7 looped, its pitch turned
// toward 0xC000 by 0xC00 a step, +0x38B set) counting up; every 30 ticks from
// the start a hunter (0x80131974) whose 0x8012C150 is 200 or more starts
// 3.12. 0x801B7108 after every step.
void Em034LookUpPort(CpuContext* ctx) {
    GuestCall game{ctx};
    const Em::Enemy em{game, ctx->gpr[3]};
    const uint32_t work = em.work;

    const auto lookUp = [&] {
        Memory::Write32(work + 0x1BC, game.Call<0x80133E14>({0xC000, Memory::Read32(work + 0x1BC) & 0xFFFF, 0xC00}) & 0xFFFF); // EnemyTurnToward
        game.Call<0x80133CA0>({work});
    };

    switch (em.State()) {
    case 0:
        em.State(1);
        game.Call<0x801302AC>({work});
        game.Call<SetScale>({work});
        game.Call<SetMotionLoop>({work, 7, 10, 0});
        game.Call<0x801321D0>({work});
        lookUp();
        Memory::Write8(work + 0x38B, 1);
        em.Count(0);
        break;
    case 1:
        lookUp();

        if (em.CountEvery(30))
            for (uint32_t i = 0; i < 4; ++i)
                if (game.Call<0x80131974>({work, i}) != 0 && int32_t(game.Call<0x8012C150>({work, i})) > 199) {
                    game.Call<NextAction>({work, 3, 0xC});
                    return;
                }

        em.CountTicks();
        break;
    }

    game.Call<0x801B7108>({work});
}

// kind 15 0x801783DC and em020 0x8036E928 (r4 1: first a push away from the
// mark): Lagiacrus and Ceadeus following a path (0x80570610, 0x805711E0;
// 0x801349C8, 0x80134B70) at the speed 10, or 5 with +0x482 (0x80136DB0);
// with r4 1 pushed 5 a tick away from +0x36C for 40 ticks first; then
// ActionDone94. Ceadeus calls 0x80133CA0 each step first and turns
// (0x80133D2C: 0x40) each step on the path.
void PathWalk(CpuContext* ctx, uint32_t path, bool ceadeus) {
    GuestCall game{ctx};
    const Em::Enemy em{game, ctx->gpr[3]};
    const uint32_t work = em.work;
    const bool push = uint8_t(ctx->gpr[4]) == 1;

    if (ceadeus)
        game.Call<0x80133CA0>({work});

    switch (em.State()) {
    case 0:
        em.State(1);
        Memory::Write8(work + Em::kSubState, 0);
        game.Call<Em::SetUp>({work, 2});
        game.Call<0x801349C8>({work, path, 0, 1, 0});
        game.Call<0x80135448>({work});

        if (push) {
            const uint32_t angle = game.Call<0x80050AF4>({work + 0x188, work + 0x36C}); // calcVecAng2
            game.Call<Em::Vec3Set>({work + kVelocity}, {0.0, 0.0, -5.0});
            game.Call<RotateY>({work + kVelocity, angle});
            em.Count(0x28);
        }

        break;
    case 1:
        if (push && Memory::Read8(work + Em::kSubState) == 0) {
            game.Call<AddVelocity>({work});

            if (CountDown(em))
                Memory::Write8(work + Em::kSubState, uint8_t(Memory::Read8(work + Em::kSubState) + 1));
        }

        if (game.Call<0x80134B70>({work, path}) == 1)
            game.Call<ActionDone94>({work});
        else {
            game.Call<0x80136DB0>({work}, {Memory::Read8(work + 0x482) ? 5.0 : 10.0});

            if (ceadeus)
                game.Call<0x80133D2C>({work, 0x40, 0});
        }

        break;
    }
}

void Em015PathPort(CpuContext* ctx) {
    PathWalk(ctx, 0x80570610, false);
}

void Em020PathPort(CpuContext* ctx) {
    PathWalk(ctx, 0x805711E0, true);
}

// kinds 16/17 0x801849A0 (r4 1-3): the Ludroth kinds' turn (motion 0x59,
// toward -300 with r4 1 or 3, else 0) until turned (0x40 a step) or 150 ticks
// plus up to 31 (90 with r4 3; not counted with r4 2).
void Em016TurnPort(CpuContext* ctx) {
    const uint8_t how = uint8_t(ctx->gpr[4]);
    Timed(ctx, {.motion = 0x59, .flags = 4, .ticks = how == 3 ? 0x5A : 0x96, .randomBits = 0x1F,
                .extra = how == 1 || how == 3 ? WorkCall([](GuestCall& g, uint32_t w) { g.Call<TurnSetUp>({w, 0}, {-300.0}); })
                                              : WorkCall([](GuestCall& g, uint32_t w) { g.Call<TurnSetUp>({w, 0}, {0.0}); }),
                .spent = how == 2 ? Spent::Never : Spent::BelowOne, .early = Turned<0x40>, .end = Then<ActionDone>});
}

// kind 26 0x8032E038 and 0x8032DA08: Giggi waiting (motion 0xC9 at its
// carried scale; 0x8032DA08 with 0x80136E58 each step) counting up; every 16
// ticks from the start a hunter within 800 (VECDistance 0x80050FE4 of its
// +0x3C and +0x188) starts 7.6 (7.2).
void Em026Wait(CpuContext* ctx, uint32_t flags, bool hold, uint32_t next) {
    GuestCall game{ctx};
    const Em::Enemy em{game, ctx->gpr[3]};
    const uint32_t work = em.work;

    switch (em.State()) {
    case 0:
        em.State(1);
        game.Call<0x801302AC>({work});
        game.Call<SetScale>({work});
        em.SetMotion(0xC9, flags, 0);

        if (hold)
            game.Call<0x80136E58>({work});

        em.Count(0);
        break;
    case 1:
        if (hold)
            game.Call<0x80136E58>({work});

        if (em.CountEvery(16)) {
            const uint32_t hunters = game.Call<0x800CFB34>({2}) & 0xFFFF; // get_move_work_max
            uint32_t hunter = game.Call<0x800CFAF4>({2});                 // get_move_work_adrs

            for (uint32_t i = 0; (i & 0xFF) < hunters; ++i, hunter += 0xB20) {
                if (!hunter || !Memory::Read8(hunter))
                    continue;

                game.Call<0x80050FE4>({hunter + 0x3C, work + 0x188});

                if (game.F1() <= 800.0f) {
                    game.Call<NextAction>({work, 7, next});
                    return;
                }
            }
        }

        em.CountTicks();
        break;
    }
}

void Em026WaitNearPort(CpuContext* ctx) {
    Em026Wait(ctx, 4, false, 6);
}

void Em026HoldNearPort(CpuContext* ctx) {
    Em026Wait(ctx, 0, true, 2);
}

// kind 33 0x8035C988 (r4 0 / 1 / other: the turn toward -120, -250 or 0; r5
// 0 / 1: the speed 1 / 1.6): Altaroth's turn (motion 3) until turned (0x100
// a step, or 0x200 with r5 1, which also counts 130 ticks); then the count 0
// and ActionDone.
template <uint32_t Max>
bool TurnedOrSpent(GuestCall& game, uint32_t work) {
    const bool turned = game.Call<TurnStep>({work, 0, Max}) == 1;
    return turned || int32_t(Memory::Read32(work + Em::kCount)) < 1;
}

template <int Speed10, int Aim>
void Em033TurnSetUp(GuestCall& game, uint32_t work) {
    if (Speed10)
        game.Call<SetMotionSpeed>({work}, {float(Speed10) / 10.0f});

    game.Call<TurnSetUp>({work, 0}, {double(Aim)});
}

void Em033TurnPort(CpuContext* ctx) {
    static constexpr WorkCall kSetUps[3][3] = {
        {Em033TurnSetUp<10, -120>, Em033TurnSetUp<10, -250>, Em033TurnSetUp<10, 0>},
        {Em033TurnSetUp<16, -120>, Em033TurnSetUp<16, -250>, Em033TurnSetUp<16, 0>},
        {Em033TurnSetUp<0, -120>, Em033TurnSetUp<0, -250>, Em033TurnSetUp<0, 0>},
    };

    const uint8_t aim = std::min<uint8_t>(uint8_t(ctx->gpr[4]), 2), pace = std::min<uint8_t>(uint8_t(ctx->gpr[5]), 2);
    Timed(ctx, {.motion = 3, .flags = 2, .ticks = 0x82, .extra = kSetUps[pace][aim], .spent = pace == 1 ? Spent::BelowOne : Spent::Never,
                .early = pace == 1 ? TurnedOrSpent<0x200> : TurnedOrSpent<0x100>,
                .end = [](GuestCall& g, uint32_t w) {
                    Memory::Write32(w + Em::kCount, 0);
                    g.Call<ActionDone>({w});
                }});
}

// kind 33 0x8035C6E8: Altaroth's 0x8035C620 (motion 3 for 30 ticks or until
// +0x1D4 is 0) lifting itself by 0x801303B4 (work, {0, -2 * +6, 0}) a step,
// +6 counting its ticks.
void Em033RisePort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x30);
    GuestCall game{ctx};
    const Em::Enemy em{game, frame.Arg(3)};
    const uint32_t work = em.work, lift = frame[0x08];
    game.Call<0x80131DE8>({work});
    game.Call<0x80131ED8>({work});
    game.Call<0x80131E58>({work});

    switch (em.State()) {
    case 0:
        em.State(1);
        game.Call<Em::SetUp>({work, 0});
        em.SetMotion(3, 4, 0);
        game.Call<0x8012FD28>({work, 0x1E}, {0.0});
        em.Count(0x1E);
        Memory::Write8(work + Em::kSubState, 0);
        break;
    case 1: {
        const uint8_t ticks = Memory::Read8(work + Em::kSubState);
        Memory::Write8(work + Em::kSubState, uint8_t(ticks + GameStep::Ticks()));
        Memory::WriteFloat32(lift, 0.0f);
        Memory::WriteFloat32(lift + 4, -1.0f * 2.0f * float(ticks));
        Memory::WriteFloat32(lift + 8, 0.0f);
        game.Call<0x801303B4>({work, lift});

        if (CountDown(em) || Memory::ReadFloat32(work + 0x1D4) <= 0.0f)
            game.Call<0x8012E6F8>({work});

        break;
    }
    }
}

// kinds 1/2 0x8014F654: Rathian and Rathalos rising toward the height +0x370
// (motion 0x1A at the carried scale, 0x80134EF0): 10 a tick while more than
// 2000 below it (0x80134F7C when more than 3500 below) for at most 300
// ticks; 13.1 once it is 2500-4000 below and steering (0x180) is done. The
// original rose 10 a call; here 10 a tick by the step's length.
void Em001RisePort(CpuContext* ctx) {
    GuestCall game{ctx};
    const Em::Enemy em{game, ctx->gpr[3]};
    const uint32_t work = em.work;

    const auto below = [&] { return Memory::ReadFloat32(work + 0x370) - Memory::ReadFloat32(work + 0x18C); };

    switch (em.State()) {
    case 0:
        em.State(1);
        game.Call<0x801302AC>({work});
        game.Call<SetScale>({work});
        em.SetMotion(0x1A, 10, 0);
        game.Call<0x80134EF0>({work});
        em.Count(300);
        break;
    case 1: {
        const float gap = below();

        if (gap < -3500.0f)
            game.Call<0x80134F7C>({work});
        else if (gap > -2000.0f)
            Memory::WriteFloat32(work + 0x18C, Memory::ReadFloat32(work + 0x18C) + float(10.0 * GameStep::Scale()));

        const float after = below();

        if (CountDown(em) || (game.Call<Steer>({work, 0x180}) == 1 && -4000.0f <= after && after <= -2500.0f))
            game.Call<NextAction>({work, 0xD, 1});

        break;
    }
    }
}

// em025 0x801A0B34 (r4 0-3): Jhen Mohran's motion 1 (set up with kind 4,
// 0x8012CF84 and 0x80131ED8 each step) to its end, or for 1, 30, 150 or 60
// ticks (counted with r4 1-3; with 2 and 3 the status message 0x29 shown to
// the hunter once, +0x358); then its height kept and 0x8019F190.
void Em025HoldPort(CpuContext* ctx) {
    GuestCall game{ctx};
    const Em::Enemy em{game, ctx->gpr[3]};
    const uint32_t work = em.work;
    const uint32_t how = ctx->gpr[4] & 0xFF;
    game.Call<0x8012CF84>({work});
    game.Call<0x80131ED8>({work});

    switch (em.State()) {
    case 0:
        em.State(1);
        game.Call<Em::SetUp>({work, 4});
        em.SetMotion(1, 0, 0);
        em.Count(0);

        if (how == 2 || how == 3) {
            em.Count(how == 2 ? 0x96 : 0x3C);

            if (Memory::Read8(work + 0x358) == 0) {
                const uint32_t hunters = game.Call<0x800CFAF4>({2});                      // get_move_work_adrs
                const int32_t me = int8_t(game.Call<0x800CF3E8>());                       // MyPlayerIndex
                game.Call<0x802672D4>({hunters + uint32_t(me * 0xB20), 1, 0x29, 0}); // PlShowStatusMessage
                game.Call<0x800F6484>();
                Memory::Write8(work + 0x358, 1);
            }
        } else if (how == 0)
            em.Count(1);
        else if (how == 1)
            em.Count(0x1E);

        break;
    case 1:
        if (how - 1 < 3)
            em.Count(em.Count() - int32_t(GameStep::Ticks()));

        if (em.Ended() || em.Count() < 1) {
            KeepHeight(game, work);
            game.Call<0x8019F190>({work});
        }

        break;
    }
}

// kinds 30/31 0x801B1408 (r4 0-2): Felyne's motion 0x25 for 150, 90 or 50
// ticks plus up to 63; then (r4 1) 0x26 (0x803B9C04: work, its place, 100) to
// its end, or (r4 2 clearing +0x331); then 0x67 to its end.
void Em030NapPort(CpuContext* ctx) {
    GuestCall game{ctx};
    const Em::Enemy em{game, ctx->gpr[3]};
    const uint32_t work = em.work;
    const uint8_t how = uint8_t(ctx->gpr[4]);

    switch (em.State()) {
    case 0:
        em.State(1);
        game.Call<Em::SetUp>({work, 0});
        em.SetMotion(0x25, 0, 0);

        if (how <= 2)
            em.Count((Memory::Read16(work + kRandom) & 0x3F) + (how == 0 ? 0x96 : how == 1 ? 0x5A : 0x32));

        break;
    case 1:
        if (!CountDown(em))
            break;

        if (how == 1) {
            em.State(2);
            em.SetMotion(0x26, 0, 0);
            game.Call<0x803B9C04>({work, work + 0x188, 100});
        } else {
            if (how == 2)
                Memory::Write8(work + 0x331, 0);

            em.State(3);
            em.SetMotion(0x67, 4, 0);
        }

        break;
    case 2:
        if (em.Ended()) {
            em.NextState();
            em.SetMotion(0x67, 4, 0);
        }

        break;
    case 3:
        if (em.Ended())
            game.Call<ActionDone>({work});

        break;
    }
}

// kinds 30/31 0x801B1D58 (r4 1: to the other side): Felyne circling its mark
// (+0x36C, motion 6) for 120 ticks plus up to 63: turning (0x800 a step)
// toward the mark's angle plus 0x2000 less 16 per unit past 1000 away; then
// 0x19 to its end.
void Em030CirclePort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x40);
    GuestCall game{ctx};
    const Em::Enemy em{game, frame.Arg(3)};
    const uint32_t work = em.work;
    const bool otherSide = uint8_t(frame.Arg(4)) == 1;

    switch (em.State()) {
    case 0:
        em.State(1);
        game.Call<Em::SetUp>({work, 0});
        em.SetMotion(6, 4, 0);
        em.Count((Memory::Read16(work + kRandom) & 0x3F) + 0x78);
        break;
    case 1: {
        game.Call<0x80050FE4>({work + 0x36C, work + 0x188}); // VECDistance
        const float past = std::max(game.F1() - 1000.0f, 0.0f);
        int32_t offset = std::max(0x2000 - (int32_t(past) << 4), 0);
        game.Call<0x80050D04>({frame[0x10], work + 0x36C, work + 0x188}); // VectorSubtractWrapper
        game.Call<0x80041EA4>({frame[0x1C], frame[0x10]});                 // Vec3Copy
        game.Call<0x80050D7C>({frame[0x1C], frame[0x0C], frame[0x08]});    // calcVecAngXY

        if (otherSide)
            offset = -offset;

        const uint32_t goal = (Memory::Read32(frame[0x08]) + uint32_t(offset)) & 0xFFFF;
        Memory::Write32(work + kYaw, game.Call<0x80133E14>({goal, Memory::Read32(work + kYaw) & 0xFFFF, 0x800}) & 0xFFFF); // EnemyTurnToward

        if (CountDown(em)) {
            em.NextState();
            em.SetMotion(0x19, 4, 0);
        }

        break;
    }
    case 2:
        if (em.Ended())
            game.Call<ActionDone>({work});

        break;
    }
}

// em024 0x80350B34: Alatreon's motion 0x24 (a voice) to its end, then 0x25
// moving (+0x314 194 times its scale, then 0x801356A8's speed by the table
// 0x805EC410; voice 0 cleared at frame 32) until 0x8013006C (work; its carried
// scale) says it is down, then 8 (set up with kind 3) for 22 ticks; then
// 0x80128110. 0x8012CF84 and 0x80131ED8 each step.
void Em024LandPort(CpuContext* ctx) {
    GuestCall game{ctx};
    const Em::Enemy em{game, ctx->gpr[3]};
    const uint32_t work = em.work;
    game.Call<0x8012CF84>({work});
    game.Call<0x80131ED8>({work});

    switch (em.State()) {
    case 0:
        em.State(1);
        game.Call<Em::SetUp>({work, 0});
        em.SetMotion(0x24, 4, 0);
        game.Call<RoarVoice>({work, 0, 0x1E, 3});
        break;
    case 1:
        if (!em.Ended())
            break;

        em.NextState();
        game.Call<0x801302AC>({work});
        game.Call<SetScale>({work});
        em.SetMotion(0x25, 0, 0);
        game.Call<0x80135448>({work});
        game.Call<0x801359A4>({work}); // get_em_scale
        Memory::WriteFloat32(work + kSpeed, 194.0f * game.F1());
        game.Call<AddVelocity>({work});
        break;
    case 2:
        game.Call<0x801356A8>({work, 0x805EC410});
        Memory::WriteFloat32(work + kSpeed, game.F1());
        game.Call<AddVelocity>({work});

        if (em.At(32.0f))
            game.Call<0x80129788>({work, 0});

        game.Call<0x801302AC>({work}); // f1 for 0x8013006C

        if (game.Call<0x8013006C>({work}) == 1) {
            em.NextState();
            game.Call<Em::SetUp>({work, 3});
            game.Call<SetMotionEx>({work, 8, 0x14, 0, 1});
            em.Count(0x16);
        }

        break;
    case 3:
        if (CountDown(em))
            game.Call<0x80128110>({work});

        break;
    }
}

// kind 3 0x80155C0C (r4 0: with its effects): Qurupeco's display (motion
// 0x13) for 300 ticks, then 1.5; with r4 0 sparks at frames 94 and 258
// (joint effect 0x29), 146 and 314 (0x2A), feathers at 150, 318 and 350
// (0x8030456C 0x35 at 0, -10, 80) and calls (0x80306B74 6) at 96 and 264.
void Em003DisplayPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x20);
    GuestCall game{ctx};
    const Em::Enemy em{game, frame.Arg(3)};
    const uint32_t work = em.work, at = frame[0x08];
    const bool effects = uint8_t(frame.Arg(4)) == 0;

    switch (em.State()) {
    case 0:
        em.State(1);
        game.Call<Em::SetUp>({work, 0});
        em.SetMotion(0x13, 6, 0);
        em.Count(300);
        break;
    case 1:
        if (effects) {
            if (em.At(94.0f) || em.At(258.0f))
                game.Call<JointEffect>({work, 0x18, 0x29, 0}, {1.0});

            if (em.At(96.0f))
                game.Call<0x80306B74>({work, 6, 0x30});

            if (em.At(146.0f) || em.At(314.0f))
                game.Call<JointEffect>({work, 0x18, 0x2A, 0}, {1.0});

            if (em.At(150.0f) || em.At(318.0f) || em.At(350.0f)) {
                game.Call<Em::Vec3Set>({at}, {0.0, -10.0, 80.0});
                game.Call<Em::Spawn>({work, 0x35, 0x1A, at}, {1.0});
            }

            if (em.At(264.0f))
                game.Call<0x80306B74>({work, 6, 0x58});
        }

        if (CountDown(em))
            game.Call<NextAction>({work, 1, 5});

        break;
    }
}

// kind 4 0x801C67AC: Gigginox' motion 0x32 (+0x8A0 less 12): a voice at
// frame 176 (4, or 0x16 with part 2 damaged past level 1), from frame 190 a
// gas puff (0x80304574 0x1B, turned 0x2000 every other one) every 13 ticks,
// at frame 264 0x34 to its end.
void Em004GasPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x20);
    GuestCall game{ctx};
    const Em::Enemy em{game, frame.Arg(3)};
    const uint32_t work = em.work, at = frame[0x08];

    switch (em.State()) {
    case 0:
        em.State(1);
        game.Call<Em::SetUp>({work, 0});
        em.SetMotion(0x32, 4, 0);
        game.Call<0x80130D40>({work, uint32_t(-12)});
        em.Count(0);
        Memory::Write8(work + Em::kSubState, 0);
        break;
    case 1:
        if (em.At(176.0f)) {
            const uint8_t level = uint8_t(game.Call<0x8011EA40>({work, 2})); // em_parts_damage_level_get
            game.Call<RoarVoice>({work, 0, level > 1 ? 0x16u : 4u, 2});
        }

        if (game.Call<0x8012F990>({work, 1}, {190.0, 0.0}) == 1) { // em_after_frame_check
            em.Count(em.Count() - int32_t(GameStep::Ticks()));

            if (em.Count() < 0) {
                const bool turned = Memory::Read8(work + Em::kSubState) != 0;
                game.Call<Em::Vec3Set>({at}, {0.0, 0.0, 0.0});
                Memory::Write8(work + Em::kSubState, turned ? 0 : 1);
                game.Call<0x80304574>({work, 0x1B, 3, at, turned ? 0x2000u : 0u}, {1.0});
                em.Count(12);
            }
        }

        if (em.At(264.0f)) {
            em.NextState();
            em.Count(0);
            em.SetMotion(0x34, 4, 0);
        }

        break;
    case 2:
        if (em.Ended())
            game.Call<ActionDone>({work});

        break;
    }
}

// kind 4 0x801C432C: Gigginox dropping from the ceiling (motion 0x1B, a
// shake 0x80136A04 at frame 50), then 0x1C falling (+0x314 110) for the
// global count at 0x80793FB4 ticks until it lands (0x8013036C, 0x8012FF18;
// 3.21), or after them 0x80128048; 0x80131E58 each step.
void Em004DropPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x20);
    GuestCall game{ctx};
    const Em::Enemy em{game, frame.Arg(3)};
    const uint32_t work = em.work, at = frame[0x08];
    game.Call<0x80131E58>({work});

    switch (em.State()) {
    case 0:
        em.State(1);
        game.Call<Em::SetUp>({work, 0});
        em.SetMotion(0x1B, 4, 0);
        break;
    case 1:
        if (em.At(50.0f)) {
            game.Call<Em::Vec3Set>({at}, {0.0, 0.0, 50.0});
            game.Call<0x80136A04>({work, 2, 1, at}, {2.5});
        }

        if (em.Ended()) {
            em.NextState();
            Memory::Write8(work + Em::kSubState, 0);
            em.Count(int16_t(Memory::Read16(0x80793FB4)));
            game.Call<0x801302AC>({work});
            game.Call<SetScale>({work});
            em.SetMotion(0x1C, 0, 0);
            game.Call<0x80135448>({work});
            Memory::Write32(work + kSpeed, 0x42DC0000); // 110
        }

        break;
    case 2:
        if (Memory::Read8(work + Em::kSubState) == 0)
            game.Call<AddVelocity>({work});

        if (CountDown(em)) {
            if (game.Call<0x8012D204>({work}) == 1)
                game.Call<0x80128048>({work});
        } else {
            game.Call<0x8013036C>({work});

            if (game.Call<0x8012FF18>({work}) == 1) {
                Memory::Write8(work + Em::kSubState, 1);

                if (game.Call<0x8012D204>({work}) == 1)
                    game.Call<NextAction>({work, 3, 0x15});
            }
        }

        break;
    }
}

// kind 26 0x8032EA58: Giggi latched onto its hunter (+0x1F6) at +0x328 in
// the hunter's joint +0x334 (0x8026A3F8), following it while within 100
// (squared 10000), its hunter's shake-off (0x8027D580) taking 360 ticks off
// its 420; it lets go (0x8032C9F8, 7.5) when they are spent, the hunter is
// free (0x8027D560) or too far. After 150 ticks +0x338 and +0xB12 are set.
// EnemyEveryNScaled (5, 30) each step; 0x80131E58 and 0x80131E18 first.
void Em026LatchPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x70);
    GuestCall game{ctx};
    const Em::Enemy em{game, frame.Arg(3)};
    const uint32_t work = em.work, place = frame[0x08], joint = frame[0x18];

    game.Call<0x80050570>({joint});
    game.Call<0x80131E58>({work});
    game.Call<0x80131E18>({work});

    const auto hunter = [&] { return game.Call<0x800CFAF4>({2}) + uint32_t(int32_t(int8_t(Memory::Read8(work + 0x1F6))) * 0xB20); };

    // The latch point in the world: +0x328 through the hunter's joint matrix.
    const auto latch = [&](uint32_t pl) {
        game.Call<0x8026A3F8>({pl, Memory::Read32(work + 0x334), joint});
        game.Call<0x80041EA4>({place, work + 0x328});                   // Vec3Copy
        game.Call<0x80051510>({place, joint});                          // mulVecMatAddTrans
    };

    switch (em.State()) {
    case 0:
        em.State(1);
        Memory::Write16(work + 0x33A, 0);
        game.Call<0x801302AC>({work});
        game.Call<SetScale>({work});
        em.SetMotion(0x32, 0, 0);
        game.Call<0x80132254>({work});
        game.Call<0x80132228>({work});
        em.Count(0x1A4);
        latch(hunter());
        game.Call<0x80041EA4>({work + 0x188, place});
        break;
    case 1: {
        const uint32_t pl = hunter();
        latch(pl);
        game.Call<0x80050F10>({work + 0x188, place});
        const float apart = game.F1();

        if (10000.0f > apart)
            game.Call<0x80041EA4>({work + 0x188, place});

        game.Call<0x8011E750>({work, 5, 0x1E}, {1.0}); // EnemyEveryNScaled

        if (game.Call<0x8027D580>({pl}) == 1)
            em.Count(em.Count() - 0x168);

        if (CountDown(em) || game.Call<0x8027D560>({pl}) == 1 || 10000.0f <= apart) {
            game.Call<0x8032C9F8>({work});
            game.Call<NextAction>({work, 7, 5});
        }

        if (Memory::Read8(work + 0x338) == 0) {
            const int16_t held = int16_t(Memory::Read16(work + 0x33A));

            if (held < 0x96)
                Memory::Write16(work + 0x33A, uint16_t(std::min<int32_t>(held + int32_t(GameStep::Ticks()), 0x96)));
            else {
                Memory::Write8(work + 0x338, 1);
                Memory::Write8(work + 0xB12, 1);
            }
        }

        break;
    }
    }
}

// kind 15 0x80179660 (r4 1: 600 ticks, else 1800): Lagiacrus resting
// (motion 0x2A to its end, then 0x2B; EnemyEveryNScaled 5, 30 each step) while
// its count rises to the limit; every 30 ticks a hunter within 0-3000 (+0x454
// + 4 * i) wakes it (0x8012B3E4 0, 2; 5.19), the limit too (0x8012B3E4 5, 2,
// 5, 0x801307DC 0; 5.19). 0x8012CF84 each step.
void Em015RestPort(CpuContext* ctx) {
    GuestCall game{ctx};
    const Em::Enemy em{game, ctx->gpr[3]};
    const uint32_t work = em.work;
    const int32_t limit = uint8_t(ctx->gpr[4]) == 1 ? 600 : 0x708;
    game.Call<0x8012CF84>({work});

    switch (em.State()) {
    case 0:
        em.State(1);
        game.Call<Em::SetUp>({work, 2});
        game.Call<SetMotionEx>({work, 0x2A, 10, 0, 1});
        game.Call<0x801321D0>({work});
        break;
    case 1:
        if (em.Ended()) {
            em.NextState();
            em.SetMotion(0x2B, 0, 0);
            em.Count(0);
        }

        break;
    case 2: {
        game.Call<0x8011E750>({work, 5, 0x1E}, {1.0}); // EnemyEveryNScaled
        const uint32_t ticks = GameStep::Ticks();
        em.Count(std::min(em.Count() + int32_t(ticks), limit));

        // No game time passed: nothing to look at (the original's next call does).
        if (ticks == 0 || game.Call<0x8012D204>({work}) != 1)
            break;

        const int32_t count = em.Count();

        if (Em::AnyOf(count - int32_t(ticks) + 1, ticks, 30, 0))
            for (uint32_t i = 0; i < 4; ++i) {
                const float distance = Memory::ReadFloat32(work + 0x454 + 4 * i);

                if (0.0f < distance && distance <= 3000.0f) {
                    em.NextState();
                    game.Call<0x8012B3E4>({work, 0, 2});
                    game.Call<NextAction>({work, 5, 0x13});
                    return;
                }
            }

        if (limit <= count) {
            game.Call<0x8012B3E4>({work, 5, 2, 5});
            game.Call<0x801307DC>({work, 0});
            game.Call<NextAction>({work, 5, 0x13});
        }

        break;
    }
    }
}

// kind 3 0x801562BC (r4 0-2: the feathers' colour): Qurupeco's third
// stagger (motion 0xCB) of 210 ticks, 315 with a broken head (sped up as in
// 0x80155FD8): a burst of feathers (0x8030456C 0x2D-0x2F at 0, 50, 150) at
// frame 204 and more (0x15-0x17) every 16 steps in frames 236-254; then 1.18.
void Em003Stagger3Port(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x30);
    GuestCall game{ctx};
    const Em::Enemy em{game, frame.Arg(3)};
    const uint32_t work = em.work, at = frame[0x08];
    const uint8_t colour = uint8_t(frame.Arg(4));
    constexpr uint32_t HeadBroken = 0x8015D674; // em003_atama_koware_act_ck

    const auto feathers = [&](uint32_t first) {
        game.Call<Em::Vec3Set>({at}, {0.0, 50.0, 150.0});

        if (colour <= 2)
            game.Call<Em::Spawn>({work, first + colour, 0x18, at}, {1.0});
    };

    switch (em.State()) {
    case 0:
        em.State(1);
        game.Call<Em::SetUp>({work, 0});
        em.SetMotion(0xCB, 6, 0);
        em.Count(game.Call<HeadBroken>({work}) == 1 ? 0x13B : 0xD2);

        if (Memory::Read8(work + 0x7C8) > 0x28) {
            game.Call<0x8012F948>({work}); // the motion's rate
            const float perRate = 1.2f / game.F1();
            game.Call<SetMotionSpeed>({work}, {2.0f * perRate});
            em.Count(int32_t(float(em.Count()) / 1.2f));
        }

        break;
    case 1:
        if (em.At(204.0f))
            feathers(0x2D);

        if (em.At(236.0f, 3, 254.0f) && StepCounter::Every(16))
            feathers(0x15);

        if (CountDown(em))
            game.Call<NextAction>({work, 1, 0x12});

        break;
    }
}

// kind 4 0x801C56F0: Gigginox' leap (motion 0xEC at its carried scale, then
// 0xED: velocity 54 forward and up, -3 a tick down, its count rising) until
// it lands (0x8013006C; then 0xD5 to its end), clinging (0x8013036C,
// 0x8012FF18: 3.26) in frames 14-40 or curling up (0xD4, a voice) at 46.
void Em004LeapPort(CpuContext* ctx) {
    GuestCall game{ctx};
    const Em::Enemy em{game, ctx->gpr[3]};
    const uint32_t work = em.work;

    switch (em.State()) {
    case 0:
        em.State(1);
        game.Call<0x801302AC>({work});
        game.Call<SetScale>({work});
        game.Call<0x80130724>({work, 1});
        em.SetMotion(0xEC, 4, 0);
        break;
    case 1:
        if (!em.Ended())
            break;

        em.NextState();
        game.Call<0x801302AC>({work});
        game.Call<SetScale>({work});
        em.SetMotion(0xED, 0, 0);
        game.Call<0x801C2ABC>({work});
        game.Call<0x8013545C>({work});
        Memory::Write32(work + kSpeed, 0x42580000);     // 54
        Memory::Write32(work + kSideSpeed, 0x42580000); // 54
        Memory::Write32(work + 0x320, 0xC0400000);      // -3
        game.Call<RotateY>({work + kVelocity, Memory::Read32(work + kYaw)});
        em.Count(0);
        break;
    case 2:
        game.Call<0x8013548C>({work});
        em.CountTicks();
        game.Call<0x801302AC>({work}); // f1 for 0x8013006C

        if (game.Call<0x8013006C>({work}) == 1) {
            em.NextState();
            game.Call<Em::SetUp>({work, 0});
            em.SetMotion(0xD5, 0, 0);
        } else if (game.Call<0x8012D204>({work}) == 1) {
            if (em.At(14.0f, 3, 40.0f)) {
                game.Call<0x8013036C>({work});

                if (game.Call<0x8012FF18>({work}) == 1)
                    game.Call<NextAction>({work, 3, 0x1A});
            }

            if (em.At(46.0f)) {
                em.SetMotion(0xD4, 6, 0);
                game.Call<RoarVoice>({work, 0, 5, 3});
            }
        }

        break;
    case 3:
        if (em.Ended())
            game.Call<ActionDone>({work});

        break;
    }
}

// kind 15 0x8017E000 and kind 18 0x80199248: Lagiacrus and Gobul swimming to
// their mark (+0x36C) over 50 ticks (the speed the way less 900 / 600 times
// the change scale over 50, clamped to -50..50) along the path 0x80570610 /
// 0x80570850, at the speed 10 (or 5 with +0x482); at the path's end, when
// 0x8012D204, 13.4 if 0x80182494 / 0x8019E500 (work, 1), else 5.0 / 5.10.
struct Approach {
    uint32_t path;
    float margin;
    uint32_t near, other;
};

void SwimTo(CpuContext* ctx, const Approach& a) {
    GuestFrame frame(ctx, 0x40);
    GuestCall game{ctx};
    const Em::Enemy em{game, frame.Arg(3)};
    const uint32_t work = em.work, way = frame[0x10], copy = frame[0x20];

    switch (em.State()) {
    case 0: {
        em.State(1);
        game.Call<Em::SetUp>({work, 2});
        game.Call<0x801349C8>({work, a.path, 0, 1, 0});
        em.Count(0x32);
        game.Call<0x80050D04>({way, work + 0x36C, work + 0x188}); // VectorSubtractWrapper
        game.Call<0x80041EA4>({copy, way});                        // Vec3Copy
        const uint32_t angle = game.Call<0x80050B08>({copy}) & 0xFFFF; // calcVecAng3
        game.Call<0x80050F88>({copy});                                  // VecMagWrapper
        const float distance = game.F1();
        game.Call<0x801359B4>({work}); // get_em_chg_scale
        const float reach = a.margin * game.F1(); // fmuls, then fsubs: no fused multiply-add
        const float speed = std::clamp((distance - reach) / 50.0f, -50.0f, 50.0f);
        game.Call<Em::Vec3Set>({work + kVelocity}, {0.0, 0.0, double(speed)});
        game.Call<RotateY>({work + kVelocity, angle});
        break;
    }
    case 1:
        if (em.Count() > 0) {
            em.Count(em.Count() - int32_t(GameStep::Ticks()));
            game.Call<AddVelocity>({work});
        }

        if (game.Call<0x80134B70>({work, a.path}) == 1) {
            if (game.Call<0x8012D204>({work}) == 1) {
                if (game(a.near, {work, 1}) == 1)
                    game.Call<NextAction>({work, 0xD, 4});
                else
                    game.Call<NextAction>({work, 5, a.other});
            }
        } else
            game.Call<0x80136DB0>({work}, {Memory::Read8(work + 0x482) ? 5.0 : 10.0});

        break;
    }
}

void Em015SwimToPort(CpuContext* ctx) {
    SwimTo(ctx, {0x80570610, 900.0f, 0x80182494, 0});
}

void Em018SwimToPort(CpuContext* ctx) {
    SwimTo(ctx, {0x80570850, 600.0f, 0x8019E500, 10});
}

// kind 33 0x8035DB1C: Altaroth's motion 2 (0x80131E58, 0x80131DE8 each
// step; 0x801321B8 while waiting) after a wait of 0-70 ticks (+2 & 7 times
// 10), then lifted by 0x801303B4 (work, {0, -2 * count, 0}) while its count
// runs down from 60, then 60 more ticks; then ActionDone.
void Em033ClimbPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x40);
    GuestCall game{ctx};
    const Em::Enemy em{game, frame.Arg(3)};
    const uint32_t work = em.work, lift = frame[0x10];
    const int32_t ticks = int32_t(GameStep::Ticks());
    game.Call<0x80131E58>({work});
    game.Call<0x80131DE8>({work});

    const auto raise = [&](int32_t count) {
        Memory::WriteFloat32(lift, 0.0f);
        Memory::WriteFloat32(lift + 4, -1.0f * 2.0f * float(count));
        Memory::WriteFloat32(lift + 8, 0.0f);
        game.Call<0x801303B4>({work, lift});
    };

    switch (em.State()) {
    case 0:
        game.Call<Em::SetUp>({work, 0});
        game.Call<SetMotionLoop>({work, 2, 4, 0});
        em.NextState();
        em.Count((Memory::Read8(work + 2) & 7) * 10);
        game.Call<0x801321B8>({work});
        break;
    case 1:
        em.Count(em.Count() - ticks);
        game.Call<0x801321B8>({work});

        if (em.Count() < 0) {
            em.NextState();
            em.Count(0x3C);
            raise(0x3C);
        }

        break;
    case 2:
        em.Count(em.Count() - ticks);

        if (em.Count() < 0) {
            em.Count(0x3C);
            em.State(3);
        } else
            raise(em.Count());

        break;
    case 3:
        em.Count(em.Count() - ticks);

        if (em.Count() < 0) {
            em.Count(0);
            game.Call<ActionDone>({work});
        }

        break;
    }
}

// kind 4 0x801C72D8 (r4 1 / 2: spinning left / right): Gigginox' turn
// (motion 8, a voice, toward 0) with its yaw also turned 0x80 a call, until
// turned (TurnStep 0); then 9 to its end. The original spun 0x80 a call; here
// the step's share of 0x80 a tick.
void Em004SpinPort(CpuContext* ctx) {
    GuestCall game{ctx};
    const Em::Enemy em{game, ctx->gpr[3]};
    const uint32_t work = em.work;
    const uint8_t way = uint8_t(ctx->gpr[4]);

    switch (em.State()) {
    case 0:
        em.State(1);
        game.Call<Em::SetUp>({work, 0});
        em.SetMotion(8, 4, 0);
        game.Call<RoarVoice>({work, 0, 1, 2});
        game.Call<TurnSetUp>({work, 0}, {0.0});
        break;
    case 1:
        if (way == 1 || way == 2) {
            const int32_t turn = GameStep::Share(0x80);
            Memory::Write32(work + kYaw, (Memory::Read32(work + kYaw) + uint32_t(way == 1 ? turn : -turn)) & 0xFFFF);
        }

        if (game.Call<TurnStep>({work, 0, 0}) == 1) {
            em.NextState();
            em.SetMotion(9, 4, 0);
        }

        break;
    case 2:
        if (em.Ended())
            game.Call<ActionDone>({work});

        break;
    }
}

// kind 10 0x80169528 (r4 0-2: then 12.0, 12.3 or 12.6): Jaggi waiting (motion
// 1) for up to 127 ticks (the random word's), then turning (0xE) toward its
// mark (+0x36C) until it is near (within 150), turned away or three turns
// are done; then along the path 0x80570450 (a call 0x80126388 in some areas)
// to its end.
void Em010WanderPort(CpuContext* ctx) {
    GuestCall game{ctx};
    const Em::Enemy em{game, ctx->gpr[3]};
    const uint32_t work = em.work;
    const uint8_t then = uint8_t(ctx->gpr[4]);
    constexpr uint32_t kPath = 0x80570450;

    switch (em.State()) {
    case 0:
        em.State(1);
        game.Call<Em::SetUp>({work, 0});
        game.Call<SetMotionLoop>({work, 1, 4, 0});
        em.Count(Memory::Read16(work + kRandom) & 0x7F);
        break;
    case 1:
        if (CountDown(em)) {
            em.State(2);
            Memory::Write8(work + Em::kSubState, 0);
            game.Call<Em::SetUp>({work, 0});
            game.Call<SetMotionLoop>({work, 0xE, 8, 0});
            game.Call<TurnSetUp>({work, 0}, {0.0});
        }

        break;
    case 2: {
        if (game.Call<TurnStep>({work, 0, 0xA0}) != 1)
            break;

        uint32_t off = (game.Call<0x80050AF4>({work + 0x188, work + 0x36C}) - Memory::Read32(work + kYaw)) & 0xFFFF; // calcVecAng2

        if (off > 0x7FFF)
            off = 0x10000 - off;

        game.Call<0x80050FAC>({work + 0x188, work + 0x36C}); // calcDistanceSqXZ

        if (game.F1() <= 22500.0f || Memory::Read8(work + Em::kSubState) > 2 || off > 0x4000) {
            em.NextState();
            const uint8_t map = uint8_t(game.Call<0x802B06CC>({Memory::Read8(work + 0x1E0)})); // MapKind
            const uint8_t area = Memory::Read8(work + 0x1E1);

            if (map == 2 && area == 5)
                game.Call<0x80126388>({work, 0xFF, 0xB}, {400.0});
            else if (map == 4 && (area == 5 || area == 6))
                game.Call<0x80126388>({work, 0xFF, area == 5 ? 0xCu : 10u}, {200.0});

            game.Call<0x801349C8>({work, kPath, 0, 0, 0});
        } else {
            Memory::Write8(work + Em::kSubState, uint8_t(Memory::Read8(work + Em::kSubState) + 1));
            game.Call<TurnSetUp>({work, 0}, {0.0});
        }

        break;
    }
    case 3:
        if (game.Call<0x80134B70>({work, kPath}) == 1 && then <= 2)
            game.Call<NextAction>({work, 0xC, then * 3u});

        break;
    }
}

// kind 34 0x801B7FDC and 0x801B8468: Kelbi's skip, a crouch of 3 (2) ticks,
// a hop of 4 (1) ticks plus up to 3 (its velocity 8 ahead or behind, -4..3
// up and aside), and a landing of 3 (2) ticks with the stored velocity
// (+0x31C) turned back; steering 0x1000 and 0x801B7084 each step, 0x801B70AC
// after every step.
struct Skip {
    int32_t crouch, hop, landing;
};

void Em034Skip(CpuContext* ctx, const Skip& k) {
    GuestCall game{ctx};
    const Em::Enemy em{game, ctx->gpr[3]};
    const uint32_t work = em.work;
    const uint16_t random = Memory::Read16(work + kRandom);

    const auto step = [&](uint32_t move) {
        game.Call<Steer>({work, 0x1000});
        game(move, {work});
        game.Call<0x801B7084>({work});
        return CountDown(em);
    };

    switch (em.State()) {
    case 0: {
        em.State(1);
        game.Call<0x801302AC>({work});
        game.Call<SetScale>({work});
        em.Count(k.crouch);
        game.Call<0x8013545C>({work});
        Memory::WriteFloat32(work + kVelocity, (random & 1) == 0 ? 8.0f * -1.0f : 8.0f);
        Memory::WriteFloat32(work + kSpeed, float(int32_t(random & 7) - 4));
        const float aside = float(int32_t(random >> 4 & 7) - 4);
        Memory::WriteFloat32(work + kSideSpeed, aside);
        em.SetMotion(aside < 0.0f ? 3 : 2, 0x14, 0);
        game.Call<RotateY>({work + kVelocity, Memory::Read32(work + kYaw)});
        game.Call<0x80041EA4>({work + 0x31C, work + kVelocity}); // Vec3Copy
        break;
    }
    case 1:
        if (step(0x8013548C)) {
            em.NextState();
            em.Count((Memory::Read16(work + kRandom) & 3) + k.hop);
        }

        break;
    case 2:
        if (step(AddVelocity)) {
            em.NextState();
            em.Count(k.landing);
            game.Call<0x80051454>({work + 0x31C}, {-1.0}); // VecScaleInPlace
        }

        break;
    case 3:
        if (step(0x8013548C))
            game.Call<0x80128048>({work});

        break;
    }

    game.Call<0x801B70AC>({work});
}

void Em034SkipPort(CpuContext* ctx) {
    Em034Skip(ctx, {3, 4, 3});
}

void Em034Skip2Port(CpuContext* ctx) {
    Em034Skip(ctx, {2, 1, 2});
}

// kind 34 0x801B8884 (r4 1: a turn toward -450, else 0): Kelbi's jump toward
// its mark as 0x801B7ACC (8 a tick for 240 ticks) climbing or dropping 1 a
// tick toward 60 above the mark's height (+0x370) until within 50 (+7);
// ended once both turn (+6) and height are done.
void Em034ClimbPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x40);
    GuestCall game{ctx};
    const Em::Enemy em{game, frame.Arg(3)};
    const uint32_t work = em.work, way = frame[0x14], velocity = frame[0x08];
    const uint8_t how = uint8_t(frame.Arg(4));

    switch (em.State()) {
    case 0:
        em.State(1);
        Memory::Write8(work + 6, 0);
        Memory::Write8(work + 7, 0);
        game.Call<0x801302AC>({work});
        game.Call<SetScale>({work});
        em.SetMotion(2, 0x14, 0);
        game.Call<TurnSetUp>({work, 8}, {how == 1 ? -450.0 : 0.0});
        game.Call<0x8013545C>({work});
        game.Call<0x80041EA4>({way, work + 0x36C}); // Vec3Copy
        Memory::WriteFloat32(way + 4, Memory::ReadFloat32(way + 4) + 60.0f);
        game.Call<0x800B0BF4>({way, work + 0x188}); // way -= position
        game.Call<0x80050F40>({way});               // VECMag

        if (game.F1() > 0.001f) {
            game.Call<0x800508B4>({way, way}); // VecNormalizeTo
            game.Call<0x80051F44>({velocity, way}, {8.0});
            game.Call<0x80041EA4>({work + kVelocity, velocity});
        }

        em.Count(0xF0);
        break;
    case 1: {
        game.Call<Steer>({work, 0x1000});

        if (Memory::Read8(work + 6) == 0 && game.Call<TurnStep>({work, 0, 0}) == 1)
            Memory::Write8(work + 6, 1);

        const float gap = (60.0f + Memory::ReadFloat32(work + 0x370)) - Memory::ReadFloat32(work + 0x18C);

        if (std::fabs(gap) >= 50.0f) {
            const float climb = float(1.0 * GameStep::Scale());
            Memory::WriteFloat32(work + 0x18C, Memory::ReadFloat32(work + 0x18C) + (gap >= 0.0f ? climb : -climb));
        } else
            Memory::Write8(work + 7, 1);

        if ((Memory::Read8(work + 6) == 1 && Memory::Read8(work + 7) == 1) || em.Count() < 1)
            game.Call<0x80128048>({work});
        else {
            game.Call<AddVelocity>({work});
            em.Count(em.Count() - int32_t(GameStep::Ticks()));
        }

        break;
    }
    }

    game.Call<0x801B70AC>({work});
}

// kinds 30/31 0x801B22FC (r4 1: steering at 1.2, for 120 ticks): Felyne
// facing its mark (+0x36C; motion 6, a voice) for 60 ticks, cut to 15 when
// 0x801B01CC is 1, while its voice plays (+0xA16 -1 or +0xA38 set); then
// 0x19 to its end (and 0x1C after a cut voice, not Melynx).
void Em030FacePort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x40);
    GuestCall game{ctx};
    const Em::Enemy em{game, frame.Arg(3)};
    const uint32_t work = em.work;
    const bool steer = uint8_t(frame.Arg(4)) == 1;

    const auto stop = [&] {
        game.Call<0x80129788>({work, 0});
        em.SetMotion(0x19, 4, 0);
    };

    switch (em.State()) {
    case 0:
        em.State(1);
        Memory::Write8(work + Em::kSubState, 0);
        game.Call<Em::SetUp>({work, 0});
        em.SetMotion(6, 4, 0);
        game.Call<RoarVoice>({work, 0, 2, 3});

        if (steer) {
            em.Count(0x78);
            game.Call<SetMotionSpeed>({work}, {1.2f});
        } else
            em.Count(0x3C);

        game.Call<0x80050D04>({frame[0x20], work + 0x36C, work + 0x188}); // VectorSubtractWrapper
        game.Call<0x80041EA4>({frame[0x2C], frame[0x20]});                 // Vec3Copy
        game.Call<0x80050D7C>({frame[0x2C], frame[0x1C], frame[0x18]});    // calcVecAngXY
        Memory::Write32(work + 0x37C, Memory::Read32(frame[0x18]));
        break;
    case 1:
        if (Memory::Read8(work + 0xA16) == 0xFF || Memory::Read8(work + 0xA38) != 0) {
            if (CountDown(em)) {
                em.NextState();
                stop();
            } else if (steer)
                game.Call<Steer>({work, 0x800});
            else
                Memory::Write32(work + kYaw, game.Call<0x80133E14>({Memory::Read32(work + 0x37C) & 0xFFFF, Memory::Read32(work + kYaw) & 0xFFFF, 0x800}) & 0xFFFF);

            if (em.Count() > 15 && game.Call<0x801B01CC>({work}) == 1)
                em.Count(15);
        } else {
            em.State(2);
            Memory::Write8(work + Em::kSubState, 1);
            stop();
        }

        break;
    case 2:
        if (!em.Ended())
            break;

        if (Memory::Read8(work + 3) != 0x1F && Memory::Read8(work + Em::kSubState) == 1) {
            em.NextState();
            em.SetMotion(0x1C, 4, 0);
        } else
            game.Call<ActionDone>({work});

        break;
    case 3:
        if (em.Ended())
            game.Call<ActionDone>({work});

        break;
    }
}

// kind 26 0x8032E5B0 (r4 1: without 0x80136E58 each step): Giggi's motion
// 0xC9 (a burst 0x8030456C 0x4F, its sound in its area) for 60 ticks, then
// 0x3C to its end, a leap (0x3D, a voice, velocity -10 / -3 a tick) until it
// lands (0x8013006C; 0x3E, a joint effect), and that to its end.
void Em026PouncePort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x30);
    GuestCall game{ctx};
    const Em::Enemy em{game, frame.Arg(3)};
    const uint32_t work = em.work, at = frame[0x08];
    const bool hold = uint8_t(frame.Arg(4)) != 1;

    if (hold && em.State() <= 2 && em.State() != 0)
        game.Call<0x80136E58>({work});

    switch (em.State()) {
    case 0:
        em.State(1);
        game.Call<0x801302AC>({work});
        game.Call<SetScale>({work});
        em.SetMotion(0xC9, 6, 0);

        if (hold)
            game.Call<0x80136E58>({work});

        em.Count(0x3C);
        game.Call<Em::Vec3Set>({at}, {0.0, 0.0, 0.0});
        game.Call<Em::Spawn>({work, 0x4F, 3, at}, {1.0});

        if ((game.Call<0x802AFCE8>() & 0xFF) == Memory::Read8(work + 0x1E1))                // get_now_areano
            game.Call<0x800D9F0C>({Memory::Read32(work + 0xB14), 0x16, 2, work + 0x188}); // se_req_pos_ps

        break;
    case 1:
        if (CountDown(em)) {
            em.NextState();
            em.SetMotion(0x3C, 2, 0);
        }

        break;
    case 2:
        if (!em.Ended())
            break;

        em.NextState();
        em.SetMotion(0x3D, 0, 0);
        game.Call<RoarVoice>({work, 0, 1, 2});
        game.Call<0x8013545C>({work});
        Memory::Write32(work + kSpeed, 0xC20C0000); // -35
        game.Call<0x8013548C>({work});
        game.Call<0x8013545C>({work});
        Memory::Write32(work + kSpeed, 0xC1200000); // -10
        Memory::WriteFloat32(work + kSideSpeed, 0.0f);
        Memory::Write32(work + 0x320, 0xC0400000); // -3
        game.Call<RotateY>({work + kVelocity, Memory::Read32(work + kYaw)});
        break;
    case 3:
        game.Call<0x8013548C>({work});
        game.Call<0x801302AC>({work}); // f1 for 0x8013006C

        if (game.Call<0x8013006C>({work}) == 1) {
            em.NextState();
            game.Call<Em::SetUp>({work, 0});
            em.SetMotion(0x3E, 0, 0);
            game.Call<JointEffect>({work, 3, (Memory::Read16(work + 0x228) & 6) == 0 ? 9u : 0x18u, 0}, {0.8f});
        }

        break;
    case 4:
        if (em.Ended())
            game.Call<ActionDone>({work});

        break;
    }
}

// kinds 1/2 0x8014C1D0 (r4 0-4): Rathian's and Rathalos' turn (motion 8, a
// voice; toward -300, -850 or -2000) for at least 20 / 10 ticks with r4 0 /
// 1 until turned (0x80 a step with r4 0, else 0x40); then 0x40 (r4 2, voice
// 0 cleared at frame 120), 0x30 (r4 3, a voice at 36), 4.7 (r4 4) or 9, to
// its end.
void Em001TurnPort(CpuContext* ctx) {
    GuestCall game{ctx};
    const Em::Enemy em{game, ctx->gpr[3]};
    const uint32_t work = em.work;
    const uint8_t how = uint8_t(ctx->gpr[4]);

    switch (em.State()) {
    case 0:
        em.State(1);
        game.Call<Em::SetUp>({work, 0});
        game.Call<0x80154CD8>({work});
        em.SetMotion(8, 4, 0);
        game.Call<RoarVoice>({work, 0, 5, 3});
        em.Count(0);

        if (how == 0) {
            game.Call<TurnSetUp>({work, 0x10}, {-300.0});
            em.Count(0x14);
        } else if (how == 1) {
            game.Call<TurnSetUp>({work, 0x10}, {-850.0});
            em.Count(10);
        } else if (how == 2) {
            game.Call<TurnSetUp>({work, 0}, {-300.0});

            if (1000.0f <= Memory::ReadFloat32(work + 0x378))
                Memory::WriteFloat32(work + 0x378, 1000.0f);
        } else if (how == 4)
            game.Call<TurnSetUp>({work, 0x10}, {-2000.0});
        else
            game.Call<TurnSetUp>({work, 0}, {-300.0});

        break;
    case 1:
        if (CountDown(em))
            em.Count(0);

        if (game.Call<TurnStep>({work, 0, how == 0 ? 0x80u : 0x40u}) == 1 && em.Count() < 1) {
            em.NextState();
            Memory::Write8(work + Em::kSubState, 0);

            if (how == 2)
                em.SetMotion(0x40, 4, 0);
            else if (how == 3)
                em.SetMotion(0x30, 4, 0);
            else if (how == 4) {
                game.Call<0x80129788>({work, 0});
                game.Call<0x80128AD4>({work, 4, 7});
            } else {
                game.Call<0x80129788>({work, 0});
                em.SetMotion(9, 4, 0);
            }
        }

        break;
    case 2:
        if (how == 2) {
            if (Memory::Read8(work + Em::kSubState) == 0 && em.At(120.0f, 1)) {
                Memory::Write8(work + Em::kSubState, 1);
                game.Call<0x80129788>({work, 0});
            }
        } else if (how == 3 && Memory::Read8(work + Em::kSubState) == 0 && em.At(36.0f, 1)) {
            Memory::Write8(work + Em::kSubState, 1);
            game.Call<RoarVoice0>({work, 0, 0xD});
        }

        if (em.Ended())
            game.Call<ActionDone>({work});

        break;
    }
}

// kind 15 0x80178534 (r4 0-5: how; r5 0 / 1: motion 0x37 / 0x30): Lagiacrus
// surfacing and turning (toward -360, or 0 holding +0x378 at 700 / 2000,
// 0x80182404 each step) until turned (0x80 a step); with r4 3 rising to its
// water line (+0x370 from +0x210) at the speed 3 for at most 200 ticks
// instead, with r4 5 waiting (0x80131DE8); then, unless 0x8012F9AC, 0x2F to
// its end and ActionDone94.
void Em015SurfacePort(CpuContext* ctx) {
    GuestCall game{ctx};
    const Em::Enemy em{game, ctx->gpr[3]};
    const uint32_t work = em.work;
    const uint8_t how = uint8_t(ctx->gpr[4]), motion = uint8_t(ctx->gpr[5]);
    game.Call<0x80182404>({work, 1});

    switch (em.State()) {
    case 0:
        em.State(1);
        game.Call<Em::SetUp>({work, 2});
        game.Call<SetMotionEx>({work, motion == 0 ? 0x37u : 0x30u, 0x14, 0, 1});

        if (how == 3) {
            Memory::Write32(work + 0x370, Memory::Read32(work + 0x210));
            em.Count(200);
        }

        if (how == 1 || how == 2 || how == 4) {
            game.Call<TurnSetUp>({work, 0x12}, {0.0});

            if (how == 1)
                Memory::WriteFloat32(work + 0x378, 700.0f);
            else if (how == 2)
                Memory::Write32(work + 0x378, 0x44FA0000); // 2000
            else if (700.0f < Memory::ReadFloat32(work + 0x378))
                Memory::WriteFloat32(work + 0x378, 700.0f);
        } else
            game.Call<TurnSetUp>({work, 0x12}, {-360.0});

        break;
    case 1: {
        bool done;

        if (how == 3) {
            game.Call<TurnStep>({work, 0, 0x80});
            game.Call<0x80136DB0>({work}, {3.0});
            done = true;

            if (!CountDown(em)) {
                game.Call<0x801359B4>({work}); // get_em_chg_scale
                const float scale = game.F1();
                game.Call<0x801302D0>({work});
                const float raised = 10.0f + game.F1();
                const float depth = raised * scale; // fmuls, then fsubs: no fused multiply-add
                done = !(Memory::ReadFloat32(work + 0x18C) < Memory::ReadFloat32(work + 0x370) - depth);
            }
        } else if (how == 5) {
            game.Call<0x80131DE8>({work});
            done = false;
        } else
            done = game.Call<TurnStep>({work, 0, 0x80}) == 1;

        if (done && game.Call<0x8012F9AC>({work}) == 0) {
            em.NextState();
            game.Call<SetMotionEx>({work, 0x2F, 0x28, 0, 1});
        }

        break;
    }
    case 2:
        if (em.Ended())
            game.Call<ActionDone94>({work});

        break;
    }
}

// kind 15 0x80178900 (r4 0 / 1: the path 0x80570690 / 0x805706D0; r5 0-3):
// Lagiacrus circling (0x80182404 each step). It swims the path (with r5 0 at
// its mark 2000 lower, r5 not 0 turning) to frame 180, then for at most 1000
// ticks: with r5 0 turning to its target (0x80133D2C 0x40, 0x80 in frames
// 80-130) for two loops, else spinning 160 times the motion's rate a call
// in frames 80-130 (the caller's r28 outside them, as the original) until
// turned (r5 1-2) or the motion ends (r5 3); then 0x13 or a lunge (0x23,
// 0x2F) and 7.31 / 7.37 / 7.32. The original counted and spun once a call;
// here the limit is in ticks and the spin takes the step's length.
void Em015CirclePort(CpuContext* ctx) {
    GuestCall game{ctx};
    const Em::Enemy em{game, ctx->gpr[3]};
    const uint32_t work = em.work, callerR28 = ctx->gpr[28];
    uint32_t flag = ctx->gpr[4];
    const uint8_t how = uint8_t(ctx->gpr[5]);
    const uint32_t path = (flag & 0xFF) == 0 ? 0x80570690 : 0x805706D0;
    game.Call<0x80182404>({work, 1});

    const auto spin = [&] {
        const int32_t amount = em.At(80.0f, 3, 130.0f) ? 0xA0 : int32_t(callerR28);
        game.Call<0x8012F948>({work}); // the motion's rate
        const float advance = float(game.F1() * GameStep::Scale());
        const int32_t turn = int32_t(float(amount) * advance);
        const bool left = (game.Call<0x8012F960>({work}) & 0xFFFF) == 0x29; // em_get_mot_no
        Memory::Write32(work + kYaw, Memory::Read32(work + kYaw) + uint32_t(left ? turn : -turn));
    };

    switch (em.State()) {
    case 0: {
        em.State(1);
        game.Call<Em::SetUp>({work, 2});
        int32_t span;

        if (how == 0) {
            span = 0x3000;
            game.Call<0x80041EA4>({work + 0x334, work + 0x36C}); // Vec3Copy
            const float lower = Memory::ReadFloat32(work + 0x370) + 2000.0f;
            Memory::WriteFloat32(work + 0x370, lower);

            if (Memory::ReadFloat32(work + 0x210) < lower)
                Memory::Write32(work + 0x370, Memory::Read32(work + 0x210));
        } else {
            span = 0x1800;
            game.Call<TurnSetUp>({work, 10}, {0.0});
        }

        if ((flag & 0xFF) == 0)
            game.Call<0x801349C8>({work, 0x80570690, 0, 5, uint32_t(-span)});
        else
            game.Call<0x801349C8>({work, 0x805706D0, 0, 3, uint32_t(span)});

        break;
    }
    case 1:
        game.Call<0x80134B70>({work, path});

        if (how != 0)
            game.Call<TurnStep>({work, 0, 0});

        if (em.At(180.0f, 1)) {
            em.NextState();
            Memory::Write8(work + Em::kSubState, 0);
            em.Count(1000);
        }

        break;
    case 2:
        if (how == 0) {
            game.Call<0x80133D2C>({work, 0x40, em.At(80.0f, 3, 130.0f) ? 0x80u : 0x40u});
            game.Call<0x80133CA0>({work});
            flag = 0;
        } else if (how == 3) {
            spin();

            if (em.Ended())
                flag = 1;
        } else {
            flag = game.Call<TurnStep>({work, 0, 0x20});
            spin();
        }

        if (em.Count() > 0)
            em.Count(std::max(em.Count() - int32_t(GameStep::Ticks()), 0));

        if (flag == 0 && em.Count() > 0) {
            if (!em.Ended())
                break;

            if (how == 0) {
                const uint8_t loops = uint8_t(Memory::Read8(work + Em::kSubState) + 1);
                Memory::Write8(work + Em::kSubState, loops);

                if (loops > 1) {
                    em.NextState();
                    Memory::Write8(work + Em::kSubState, 0);
                    game.Call<SetMotionEx>({work, 0x13, 0x14, 0, 1});
                    game.Call<0x80041EA4>({work + 0x36C, work + 0x334});
                    break;
                }
            }

            if (Memory::Read8(work + 7) != 1)
                game.Call<SetMotionEx>({work, (game.Call<0x8012F960>({work}) & 0xFFFF) == 0x29 ? 0x2Cu : 0x29u, 0x18, 0, 1});
        } else if (!em.At(50.0f, 3, 200.0f)) {
            em.NextState();

            if (how - 2u < 2u)
                game.Call<SetMotionEx>({work, 0x23, 0x14, 0, 1});
            else {
                Memory::Write8(work + 7, 1);
                game.Call<SetMotionEx>({work, 0x2F, 0x1E, 0, 1});
            }
        }

        break;
    case 3:
        if (Memory::Read8(work + 7) == 1) {
            if (em.Ended())
                game.Call<ActionDone94>({work});

            break;
        }

        if (em.At(20.0f, 1))
            game.Call<0x80133D2C>({work, 0x40, 0x200});

        game.Call<0x80133CA0>({work});

        if (em.Ended()) {
            if (how == 0) {
                em.SetMotion(0x43, 10, 0);
                game.Call<NextAction>({work, 7, uint8_t(game.Call<0x8018297C>({work})) == 0 ? 0x1Fu : 0x25u});
            } else {
                em.SetMotion(0x24, 10, 0);
                game.Call<NextAction>({work, 7, 0x20});
            }
        }

        break;
    }
}

// kind 7 0x801DA578 and kinds 1/2 0x8014E7D8: Deviljho's and Rathian's leap
// to a ledge (0x801DFDA0 / 0x80154B68 picks it within 250 / 300; the path
// 0x80570E40 / 0x80570120): moving by 0.02 (1/38) of the way times the
// motion's rate a tick in frames 12-62 (12-50), then motion 0x35 (0xCA) for
// at least 90 ticks while it looks for its target (+0x382: 0x801DA474 /
// 0x8014E6D4, 0x8012D2A0) to climb (9.3 / 9.1), else 1.7 / 2.6. 0x80131F24
// each step (Rathian's from frame 50).
struct Ledge {
    uint32_t pick;
    double reach;
    uint32_t path;
    float share, lastFrame;
    bool rathian;
    uint32_t waitMotion, waitFlags, target, giveUpGroup, giveUpAction;
};

void LeapToLedge(CpuContext* ctx, const Ledge& l) {
    GuestFrame frame(ctx, 0x70);
    GuestCall game{ctx};
    const Em::Enemy em{game, frame.Arg(3)};
    const uint32_t work = em.work;

    switch (em.State()) {
    case 0: {
        em.State(1);
        Memory::Write8(work + Em::kSubState, 0);
        game.Call<Em::SetUp>({work, 0});
        game(l.pick, {work, frame[0x3C], frame[0x08]}, {l.reach});
        game.Call<0x80050D04>({frame[0x24], frame[0x3C], work + 0x188}); // VectorSubtractWrapper
        game.Call<0x80041EA4>({frame[0x30], frame[0x24]});                // Vec3Copy
        game.Call<0x80128C5C>({work, frame[0x3C]});
        game.Call<0x801349C8>({work, l.path, 2, 1, Memory::Read16(frame[0x08])});
        game.Call<0x80135448>({work});
        game.Call<0x8012F948>({work}); // the motion's rate
        const double rate = game.cpu->fpr[1].d;
        game.Call<0x80051F44>({frame[0x0C], frame[0x30]}, {double(l.share)}); // VectorScaleWrapper
        game.Call<0x80051F44>({frame[0x18], frame[0x0C]}, {rate});
        game.Call<0x80041EA4>({work + kVelocity, frame[0x18]});
        Memory::WriteFloat32(work + kSpeed, 0.0f);
        break;
    }
    case 1:
        if (!l.rathian)
            game.Call<0x80131F24>({work});

        if (em.At(12.0f, 3, l.lastFrame)) {
            game.Call<AddVelocity>({work});
            game.Call<0x80134B70>({work, l.path});
        }

        if (l.rathian && em.At(50.0f, 1))
            game.Call<0x80131F24>({work});

        if (em.Ended()) {
            em.NextState();
            em.Count(0x5A);
            em.SetMotion(l.waitMotion, l.waitFlags, 0);
        }

        break;
    case 2: {
        game.Call<0x80131F24>({work});
        em.Count(std::max(em.Count() - int32_t(GameStep::Ticks()), 0));

        if (game.Call<0x8012D204>({work}) != 1)
            break;

        const uint32_t target = Memory::Read8(work + 0x382);

        if (game(l.target, {work, target}) != 1)
            game.Call<NextAction>({work, l.giveUpGroup, l.giveUpAction});
        else if (game.Call<0x8012D2A0>({work, 1, target}) == 1) {
            const bool high = game.Call<0x8012ECA0>({work}) == 1;

            if (l.rathian) {
                Memory::Write8(work + 0x1E7, 3);
                game.Call<0x80130D40>({work, high ? 300u : 0x96u});
                const int32_t every = int32_t(0.015f * float(int32_t(Memory::Read32(work + 0x7A4))));
                game.Call<0x8011E750>({work, uint32_t(every), 1}, {1.0}); // EnemyEveryNScaled
                game.Call<NextAction>({work, 9, 1});
            } else {
                Memory::Write8(work + 0x1E7, 5);
                game.Call<NextAction>({work, 9, high ? 3u : 1u});
            }
        } else if (em.Count() < 1 || game.Call<0x8012D2A0>({work, 0, target}) == 0)
            game.Call<NextAction>({work, l.giveUpGroup, l.giveUpAction});

        break;
    }
    }
}

void Em007LedgePort(CpuContext* ctx) {
    LeapToLedge(ctx, {0x801DFDA0, 250.0, 0x80570E40, 0.02f, 62.0f, false, 0x35, 2, 0x801DA474, 1, 7});
}

void Em001LedgePort(CpuContext* ctx) {
    LeapToLedge(ctx, {0x80154B68, 300.0, 0x80570120, 0.02631579f, 50.0f, true, 0xCA, 4, 0x8014E6D4, 2, 6});
}

// kind 5 0x801D2F34: Barioth's climb. On the Tundra's areas 1-3 it first
// glides (EnemyGlide, the path 0x80570C90) to a point 8000 up there and turns
// back (0x200); then it rises 10 a tick toward its height (+0x370) while more
// than 1500 below (0x80134F7C past 2000) for at most 300 ticks, until it is
// 1500-2000 below with its steering (0x180) done; then 13.5, or 13.6 with
// parts 2 and 3 damaged. The original rose 10 a call.
void Em005ClimbPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x30);
    GuestCall game{ctx};
    const Em::Enemy em{game, frame.Arg(3)};
    const uint32_t work = em.work, point = frame[0x08];

    const auto rise = [&] {
        game.Call<0x80134EF0>({work});
        em.Count(300);
    };

    switch (em.State()) {
    case 0: {
        bool glide = false;

        if (uint8_t(game.Call<0x802B06CC>({Memory::Read8(work + 0x1E0)})) == 4) { // MapKind
            static constexpr float kPoints[3][2] = {{-654.0f, 1598.0f}, {-2578.0f, -1253.0f}, {950.0f, -3978.0f}};
            const uint8_t area = Memory::Read8(work + 0x1E1);

            if (area >= 1 && area <= 3) {
                game.Call<Em::Vec3Set>({point}, {double(kPoints[area - 1][0]), 8000.0, double(kPoints[area - 1][1])});
                glide = true;
            }
        }

        if (glide) {
            em.NextState();
            game.Call<0x801302AC>({work});
            game.Call<SetScale>({work});
            game.Call<0x80041EA4>({work + 0x334, work + 0x36C}); // Vec3Copy
            game.Call<0x80041EA4>({work + 0x36C, point});
            game.Call<0x80134FD4>({work, 0x80570C90});
            game.Call<TurnSetUp>({work, 0x19}, {0.0});
            game.Call<0x801302AC>({work});
            game.Call<0x801355E8>({work, work + 0x1BC});
        } else {
            em.State(3);
            game.Call<0x801302AC>({work});
            game.Call<SetScale>({work});
            em.SetMotion(0x1A, 10, 0);
            rise();
        }

        break;
    }
    case 1:
        if (game.Call<TurnStep>({work, 0, 0}) == 1) {
            em.NextState();
            game.Call<0x80041EA4>({work + 0x36C, work + 0x334});
            em.SetMotion(0x1A, 10, 0);
        } else {
            game.Call<0x80135064>({work, 2, 0x80570C90}); // EnemyGlide
            game.Call<0x801302AC>({work});
            game.Call<0x801355E8>({work, work + 0x1BC});
        }

        break;
    case 2:
        if (game.Call<Steer>({work, 0x200}) == 1) {
            em.NextState();
            rise();
        }

        break;
    case 3: {
        const auto below = [&] { return Memory::ReadFloat32(work + 0x370) - Memory::ReadFloat32(work + 0x18C); };
        const float gap = below();

        if (gap < -2000.0f)
            game.Call<0x80134F7C>({work});
        else if (gap > -1500.0f)
            Memory::WriteFloat32(work + 0x18C, Memory::ReadFloat32(work + 0x18C) + float(10.0 * GameStep::Scale()));

        const float after = below();

        if (CountDown(em) || (game.Call<Steer>({work, 0x180}) == 1 && -2000.0f <= after && after <= -1500.0f)) {
            const bool whole = game.Call<0x8011EA40>({work, 2}) == 0 || game.Call<0x8011EA40>({work, 3}) == 0; // em_parts_damage_level_get
            game.Call<NextAction>({work, 0xD, whole ? 5u : 6u});
        }

        break;
    }
    }
}

// kind 26 0x8032DD68: Giggi on the ceiling (motion 0xD1 looped at its
// carried scale, +0x38B set) counting up; every 16 ticks from the start,
// unless a torch-carrying NPC (0x806BF7E0, ai_torch_ck) is in its area within
// 1000, it picks the nearest hunter it senses within 0-700 (0x80131974, then
// 0x8012B3E4 1, 2, i) to drop on (7.3), none when one is 0x8012D860.
// 0x8032C984 after every step.
void Em026CeilingPort(CpuContext* ctx) {
    GuestCall game{ctx};
    const Em::Enemy em{game, ctx->gpr[3]};
    const uint32_t work = em.work;
    constexpr uint32_t kNpc = 0x806BF7E0;

    switch (em.State()) {
    case 0:
        em.State(1);
        game.Call<0x801302AC>({work});
        game.Call<SetScale>({work});
        game.Call<SetMotionLoop>({work, 0xD1, 4, 0});
        em.Count(0);
        Memory::Write8(work + 0x38B, 1);
        break;
    case 1:
        if (em.CountEvery(16)) {
            bool torch = false, found = false;

            if (Memory::Read8(kNpc) != 0 && Memory::Read8(work + 0x1E1) == Memory::Read8(0x806BF984) &&
                game.Call<0x802D8478>({kNpc}) == 1) { // ai_torch_ck
                game.Call<0x80050F10>({work + 0x188, 0x806BF958});
                torch = game.F1() <= 1000000.0f;
            }

            if (!torch) {
                float nearest = 800.0f;
                const uint32_t hunters = game.Call<0x800CFB34>({2}); // get_move_work_max
                uint32_t hunter = game.Call<0x800CFAF4>({2});        // get_move_work_adrs

                for (uint8_t i = 0; i < hunters; ++i, hunter += 0xB20) {
                    if (!hunter || !Memory::Read8(hunter) || game.Call<0x8012D118>({work, hunter}) == 0) // EnemySensesPlayer
                        continue;

                    const float distance = Memory::ReadFloat32(work + 4 * i + 0x454);

                    if (distance > 700.0f || distance < 0.0f)
                        continue;

                    if (game.Call<0x8012D860>({hunter}) == 1) {
                        found = false;
                        break;
                    }

                    if (game.Call<0x80131974>({work, i}) != 0) {
                        const float again = Memory::ReadFloat32(work + 4 * i + 0x454);

                        if (again < nearest) {
                            game.Call<0x8012B3E4>({work, 1, 2, i});
                            nearest = again;
                        }

                        found = true;
                    }
                }
            }

            if (found)
                game.Call<NextAction>({work, 7, 3});
        }

        em.CountTicks();
        break;
    }

    game.Call<0x8032C984>({work});
}

// kinds 1/2 0x8014B730 (r4 0-2: the landing): Rathian and Rathalos dropping
// to the ground: motion 0xD1 sinking 6.67 a tick to 200 below (0x80130460),
// steering (0x100) until frame 50, then moving (0x8013562C, at most 110),
// 0xD0 to its end, a turn (TurnStep 0) and the landing: r4 1 0xDD, 0xDA (voice
// 0 cleared at 30); r4 2 0x35 rising back over 10 ticks, 0x3C; else 0xD2
// settling over 20 ticks and moving to a stop, then 0x80128110. The
// original sank by its tick's amount and ramped its height once a call;
// here by the step's length and ticks.
void Em001LandPort(CpuContext* ctx) {
    GuestCall game{ctx};
    const Em::Enemy em{game, ctx->gpr[3]};
    const uint32_t work = em.work;
    const uint8_t how = uint8_t(ctx->gpr[4]);
    const int32_t ticks = int32_t(GameStep::Ticks());

    const auto move = [&] {
        game.Call<0x8013562C>({work, work + 0x1BC});

        if (110.0f < Memory::ReadFloat32(work + kSideSpeed))
            Memory::WriteFloat32(work + kSideSpeed, 110.0f);
    };

    switch (em.State()) {
    case 0:
        em.State(1);
        Memory::Write8(work + Em::kSubState, 0);
        game.Call<Em::SetUp>({work, 3});
        game.Call<0x80154CD8>({work});
        em.SetMotion(0xD1, 6, 0);
        em.Count(0);
        break;
    case 1:
        game.Call<0x80130460>({work}, {double(-6.6666665f * float(GameStep::Scale()))});

        if (Memory::ReadFloat32(work + 0x1AC) < -200.0f)
            Memory::WriteFloat32(work + 0x1AC, -200.0f);

        if (Memory::Read8(work + Em::kSubState) == 0) {
            game.Call<Steer>({work, 0x100});

            if (em.At(50.0f, 1)) {
                Memory::Write8(work + Em::kSubState, 1);
                game.Call<0x8013545C>({work});
                Memory::Write32(work + 0x324, 0x40400000); // 3
                game.Call<TurnSetUp>({work, 0x10}, {0.0});
                game.Call<RoarVoice>({work, 0, 0xE, 3});
            }
        } else if (Memory::Read8(work + Em::kSubState) == 1) {
            move();

            if (em.Ended()) {
                em.NextState();
                em.SetMotion(0xD0, 6, 0);
            }
        }

        break;
    case 2:
        game.Call<0x80130450>({work}, {-200.0});
        move();

        if (uint8_t(game.Call<TurnStep>({work, 0, 0})) == 1) {
            em.NextState();
            Memory::Write8(work + Em::kSubState, 0);

            if (how == 1) {
                game.Call<Em::SetUp>({work, 0});
                em.SetMotion(0xDD, 0, 0);
                game.Call<0x80130450>({work}, {0.0});
            } else if (how == 2)
                em.SetMotion(0x35, 10, 0);
            else {
                em.SetMotion(0xD2, 6, 0);
                Memory::Write32(work + 0x324, 0xC0400000); // -3
                em.Count(0);
            }
        }

        break;
    case 3:
        if (how == 1) {
            if (Memory::Read8(work + Em::kSubState) == 0) {
                if (em.Ended()) {
                    Memory::Write8(work + Em::kSubState, 1);
                    em.SetMotion(0xDA, 0, 0);
                }
            } else if (Memory::Read8(work + Em::kSubState) == 1) {
                if (em.At(30.0f))
                    game.Call<0x80129788>({work, 0});

                if (em.Ended())
                    game.Call<ActionDone>({work});
            }
        } else if (how == 2) {
            if (Memory::Read8(work + Em::kSubState) == 0) {
                if (em.Count() < 10) {
                    em.Count(std::min(em.Count() + ticks, 10));
                    game.Call<0x80130450>({work}, {double(-200.0f * float(5 - em.Count()) / 5.0f)});
                }

                if (em.Ended()) {
                    Memory::Write8(work + Em::kSubState, 1);
                    game.Call<0x80129788>({work, 0});
                    game.Call<Em::SetUp>({work, 0});
                    em.SetMotion(0x3C, 0, 0);
                }
            } else if (Memory::Read8(work + Em::kSubState) == 1 && em.Ended())
                game.Call<ActionDone>({work});
        } else {
            if (em.Count() < 0x14) {
                em.Count(std::min(em.Count() + ticks, 0x14));
                game.Call<0x80130450>({work}, {double(-150.0f * float(0x14 - em.Count()) / 20.0f)});
            }

            if (em.At(30.0f))
                game.Call<0x80129788>({work, 0});

            game.Call<0x8013562C>({work, work + 0x1BC});

            if (Memory::ReadFloat32(work + kSideSpeed) < 0.0f)
                Memory::WriteFloat32(work + kSideSpeed, 0.0f);

            if (em.Ended())
                game.Call<0x80128110>({work});
        }

        break;
    }
}

// kind 5 0x801D1174 (r4 0 / 1): Barioth's pounce (motion 0xE7, +0x8A0 less
// 10; 0x8012CF84 each step): a crouch moving -21.4 times the motion's rate
// in frames 0-14, the leap at frame 28 (40 up, 60 times the rate scaled by
// 0x8013570C, -10 a tick down, a voice), its time in flight counted (times
// the rate: the frames flown; steering by 4.5 degrees of rate from 4 to 14
// of them) until it lands (0x8013006C), then 0xDB / 0xF6 sliding along its
// yaw at landing (TurnWhile; the tables 0x805B59D8 / 0x805B5A28) to its end.
void Em005PouncePort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x40);
    GuestCall game{ctx};
    const Em::Enemy em{game, frame.Arg(3)};
    const uint32_t work = em.work, slide = frame[0x10];
    const bool second = uint8_t(frame.Arg(4)) != 0;

    const auto rate = [&] {
        game.Call<0x8012F948>({work});
        return game.F1();
    };

    const auto glide = [&] {
        game.Call<0x80131ED8>({work});
        game.Call<0x80131DE8>({work});
    };

    game.Call<0x8012CF84>({work});

    switch (em.State()) {
    case 0:
        em.State(1);
        game.Call<0x801302AC>({work});
        game.Call<SetScale>({work});
        game.Call<0x80130D40>({work, uint32_t(-10)});
        em.SetMotion(0xE7, 6, 8);
        game.Call<0x8013545C>({work});
        game.Call<0x80131ED8>({work});
        break;
    case 1:
        glide();

        if (em.At(0.0f, 3, 14.0f)) {
            Memory::WriteFloat32(work + kSideSpeed, -21.428572f * rate());
            game.Call<0x80135558>({work, work + 0x1BC}); // EnemyAddTurnedVelocity
        }

        if (em.At(28.0f, 1)) {
            em.NextState();
            game.Call<0x8013545C>({work});
            Memory::Write32(work + kSpeed, 0x42200000); // 40
            Memory::WriteFloat32(work + kSideSpeed, 60.0f * rate());
            game.Call<0x8013570C>({work}, {2.0, 0.5, 2000.0});
            Memory::WriteFloat32(work + kSideSpeed, Memory::ReadFloat32(work + kSideSpeed) * game.F1());
            Memory::Write32(work + 0x320, 0xC1200000); // -10
            em.Count(0);
            game.Call<RoarVoice>({work, 0, 10, 0xB});
        }

        break;
    case 2: {
        em.CountTicks();
        const float flown = float(em.Count()) * rate();

        if (flown < 8.0f)
            glide();

        if (4.0f <= flown && flown < 14.0f) {
            const float turn = 4.5f * rate();
            const float units = 65536.0f * turn / 360.0f;
            game.Call<Steer>({work, uint32_t(int32_t(0.5f + units)) & 0xFFFF});
        }

        game.Call<0x8013562C>({work, work + 0x1BC});
        game.Call<0x801302AC>({work}); // f1 for 0x8013006C

        if (game.Call<0x8013006C>({work}) == 1) {
            em.NextState();
            game.Call<Em::SetUp>({work, 0});

            if (!second) {
                em.SetMotion(0xDB, 0, 0);
                game.Call<RoarVoice>({work, 1, 0xB, 0x18});
            } else {
                em.SetMotion(0xF6, 0, 0);
                game.Call<RoarVoice>({work, 1, 0x1C, 0x18});
            }

            em.Count(int16_t(Memory::Read32(work + kYaw)));
        }

        break;
    }
    case 3: {
        const float last = second ? 62.0f : 30.0f;

        if (em.At(20.0f)) {
            if (second)
                game.Call<0x80129788>({work, 0});
            else
                game.Call<RoarVoice>({work, 0, 0xC, 0x10});
        }

        game.Call<0x80133EA0>({work, second ? 0x10000u : 0x8000u}, {0.0, double(last)}); // EnemyTurnWhile

        if (em.At(0.0f, 3, last)) {
            game.Call<0x801356A8>({work, second ? 0x805B5A28u : 0x805B59D8u});
            Memory::WriteFloat32(work + kVelocity, second ? game.F1() / 3.0f : game.F1());
            game.Call<0x801356A8>({work, second ? 0x805B5A68u : 0x805B5A00u});
            Memory::WriteFloat32(work + kSideSpeed, game.F1());
            Memory::Write32(slide, 0);
            Memory::Write32(slide + 4, uint32_t(em.Count()) & 0xFFFF);
            Memory::Write32(slide + 8, 0);
            game.Call<0x80135558>({work, slide}); // EnemyAddTurnedVelocity
        }

        if (em.Ended())
            game.Call<ActionDone>({work});

        break;
    }
    }
}

// em020 0x8036EC58 (r4 0 / 1: the path 0x80571220 / 0x80571260; r5 0-3):
// Ceadeus circling, as Lagiacrus' 0x80178900: the path to its end (then
// 0x29 / 0x2C), then for at most 1000 ticks turning (r5 0, three loops) or
// spinning 80 times the motion's rate a call in frames 160-260 (outside them
// the caller's r27, and with r5 3 the caller's r30 as its done flag, as the
// original) until turned (0x10 a step; not while 0x8012F9AC) or the motion
// ends; then 0x13 or a lunge (0x23, 0x2F) and 7.18 / 7.19. 0x80128C5C (work,
// 0) first unless 0x8045169C. The limit is in ticks and the spin takes the
// step's length.
void Em020CirclePort(CpuContext* ctx) {
    GuestCall game{ctx};
    const Em::Enemy em{game, ctx->gpr[3]};
    const uint32_t work = em.work, callerR27 = ctx->gpr[27];
    uint32_t flag = ctx->gpr[30];
    const uint8_t side = uint8_t(ctx->gpr[4]), how = uint8_t(ctx->gpr[5]);
    const uint32_t path = side == 0 ? 0x80571220 : 0x80571260;

    if (game.Call<0x8045169C>() == 0)
        game.Call<0x80128C5C>({work, 0});

    const auto spin = [&] {
        const int32_t amount = em.At(160.0f, 3, 260.0f) ? 0x50 : int32_t(callerR27);
        game.Call<0x8012F948>({work}); // the motion's rate
        const float advance = float(game.F1() * GameStep::Scale());
        const int32_t turn = int32_t(float(amount) * advance);
        const bool left = (game.Call<0x8012F960>({work}) & 0xFFFF) == 0x29; // em_get_mot_no
        Memory::Write32(work + kYaw, Memory::Read32(work + kYaw) + uint32_t(left ? turn : -turn));
    };

    switch (em.State()) {
    case 0: {
        em.State(1);
        game.Call<Em::SetUp>({work, 2});
        int32_t span;

        if (how == 0) {
            span = 0x3000;
            game.Call<0x80041EA4>({work + 0x328, work + 0x36C}); // Vec3Copy
            const float lower = Memory::ReadFloat32(work + 0x370) + 2000.0f;
            Memory::WriteFloat32(work + 0x370, lower);

            if (Memory::ReadFloat32(work + 0x210) < lower)
                Memory::Write32(work + 0x370, Memory::Read32(work + 0x210));
        } else {
            span = 0x1800;
            game.Call<TurnSetUp>({work, 0x1A}, {0.0});
        }

        if (side == 0)
            game.Call<0x801349C8>({work, 0x80571220, 0, 0x45, uint32_t(-span)});
        else
            game.Call<0x801349C8>({work, 0x80571260, 0, 0x43, uint32_t(span)});

        break;
    }
    case 1: {
        const uint32_t arrived = game.Call<0x80134B70>({work, path});

        if (how != 0)
            game.Call<TurnStep>({work, 0, 0});

        if (arrived == 1) {
            em.NextState();
            Memory::Write8(work + Em::kSubState, 0);
            em.Count(1000);
            game.Call<SetMotionEx>({work, side == 0 ? 0x29u : 0x2Cu, 0x30, 0, 1});
        }

        break;
    }
    case 2:
        if (how == 0) {
            game.Call<0x80133D2C>({work, 0x40, em.At(160.0f, 3, 260.0f) ? 0x80u : 0x40u});
            game.Call<0x80133CA0>({work});
            flag = 0;
        } else if (how == 3) {
            spin();

            if (em.Ended())
                flag = 1;
        } else {
            flag = game.Call<TurnStep>({work, 0, 0x10});

            if (game.Call<0x8012F9AC>({work}) == 1)
                flag = 0;

            spin();
        }

        if (em.Count() > 0)
            em.Count(std::max(em.Count() - int32_t(GameStep::Ticks()), 0));

        if (flag == 0 && em.Count() > 0) {
            if (!em.Ended())
                break;

            if (how == 0) {
                const uint8_t loops = uint8_t(Memory::Read8(work + Em::kSubState) + 1);
                Memory::Write8(work + Em::kSubState, loops);

                if (loops > 2) {
                    em.NextState();
                    Memory::Write8(work + Em::kSubState, 0);
                    game.Call<SetMotionEx>({work, 0x13, 0x28, 0, 1});
                    game.Call<0x80041EA4>({work + 0x36C, work + 0x328});
                    break;
                }
            }

            if (Memory::Read8(work + 7) != 1)
                game.Call<SetMotionEx>({work, (game.Call<0x8012F960>({work}) & 0xFFFF) == 0x29 ? 0x2Cu : 0x29u, 0x30, 0, 1});
        } else if (!em.At(100.0f, 3, 400.0f)) {
            em.NextState();

            if (how - 2u < 2u)
                game.Call<SetMotionEx>({work, 0x23, 0x28, 0, 1});
            else {
                Memory::Write8(work + 7, 1);
                game.Call<SetMotionEx>({work, 0x2F, 0x3C, 0, 1});
            }
        }

        break;
    case 3:
        if (Memory::Read8(work + 7) == 1) {
            if (em.Ended())
                game.Call<ActionDone94>({work});

            break;
        }

        if (em.At(40.0f, 1))
            game.Call<0x80133D2C>({work, 0x20, 0x100});

        game.Call<0x80133CA0>({work});

        if (em.Ended()) {
            em.SetMotion(how == 0 ? 0x43 : 0x24, 10, 0);
            game.Call<NextAction>({work, 7, how == 0 ? 0x12u : 0x13u});
        }

        break;
    }
}

// kind 33 0x8035CC3C: Altaroth climbing out of a hole (0x80131E58,
// 0x8012CF84, 0x80131ED8 each step). It is moved by its carried joint's shift
// (+0x21C before and after 0x80130724 1), its lift (+0x32C, the shift back)
// shrinking by a count-th a tick over 11 ticks while it pitches down 0x38D
// a tick to 0xC000; it rises to its floor (0x8035EEA4), crawls forward 2 a
// tick pitching up 0x38E a tick, and once level waits 20 ticks (motion 3) to
// ActionDone. The original shrank, pitched and counted once a call; here
// a tick of game time each.
void Em033ClimbOutPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x80);
    GuestCall game{ctx};
    const Em::Enemy em{game, frame.Arg(3)};
    const uint32_t work = em.work, shift = frame[0x20], before = frame[0x30], floor = frame[0x40], part = frame[0x50], lift = frame[0x60];
    const uint32_t ticks = GameStep::Ticks();
    constexpr uint32_t kPitch = 0x1BC, kLift = 0x32C;

    const auto applyLift = [&] {
        game.Call<0x80041EA4>({lift, work + kLift}); // Vec3Copy
        game.Call<0x801303B4>({work, lift});
    };

    game.Call<0x80131E58>({work});
    game.Call<0x8012CF84>({work});
    game.Call<0x80131ED8>({work});

    switch (em.State()) {
    case 0:
        em.State(1);
        game.Call<0x80041EA4>({before, work + 0x21C});
        game.Call<0x801302AC>({work});
        game.Call<SetScale>({work});
        game.Call<0x80130724>({work, 1});
        game.Call<0x80050D04>({part, before, work + 0x21C}); // VectorSubtractWrapper
        game.Call<0x80041EA4>({shift, part});
        Memory::WriteFloat32(shift + 4, 0.0f);
        game.Call<RotateY>({shift, Memory::Read32(work + kYaw)});
        Memory::WriteFloat32(work + 0x188, Memory::ReadFloat32(work + 0x188) + Memory::ReadFloat32(shift));
        Memory::WriteFloat32(work + 0x190, Memory::ReadFloat32(work + 0x190) + Memory::ReadFloat32(shift + 8));
        game.Call<0x80051454>({shift}, {-1.0}); // VecScaleInPlace
        game.Call<0x80041EA4>({work + kLift, shift});
        game.Call<0x80041EA4>({lift, shift});
        game.Call<0x801303B4>({work, lift});
        game.Call<SetMotionLoop>({work, 3, 2, 0});
        em.Count(0xB);
        break;
    case 1: {
        for (uint32_t i = 0; i < ticks && em.Count() > 0; ++i) {
            game.Call<0x800ADA24>({part, work + kLift}, {double(float(em.Count()))}); // part = lift / count
            game.Call<0x800B0BF4>({work + kLift, part});                              // lift -= part
            applyLift();
            em.Count(em.Count() - 1);
        }

        const uint32_t pitch = (Memory::Read32(work + kPitch) - uint32_t(GameStep::Share(0x38D))) & 0xFFFF;
        Memory::Write32(work + kPitch, pitch);

        if (pitch < 0xC000) {
            Memory::Write32(work + kPitch, 0xC000);
            em.Count(0);
            em.NextState();
        }

        break;
    }
    case 2:
        game.Call<0x80131DE8>({work});
        Memory::Write32(work + kPitch, 0xC000);
        game.Call<0x8035EEA4>({work, 1, floor});

        if (Memory::ReadFloat32(floor + 4) < Memory::ReadFloat32(work + 0x18C)) {
            em.NextState();
            game.Call<0x80135448>({work});
        }

        break;
    case 3: {
        game.Call<0x80131DE8>({work});
        game.Call<0x801321B8>({work});
        game.Call<Em::Vec3Set>({shift}, {0.0, 0.0, 2.0});
        game.Call<RotateY>({shift, Memory::Read32(work + kYaw)});
        game.Call<0x80041EA4>({work + kVelocity, shift});
        game.Call<AddVelocity>({work});
        const uint32_t pitch = Memory::Read32(work + kPitch) + uint32_t(GameStep::Share(0x38E));
        Memory::Write32(work + kPitch, pitch);

        if (pitch < 0x1000) {
            em.NextState();
            game.Call<Em::SetUp>({work, 0});
            game.Call<0x80130724>({work, 0});
            game.Call<AddVelocity>({work});
            game.Call<0x8035EEA4>({work, 1, floor});
            Memory::WriteFloat32(work + 0x18C, Memory::ReadFloat32(floor + 4));
            Memory::WriteFloat32(work + 0x190, Memory::ReadFloat32(work + 0x190) + 10.0f);
            Memory::Write32(work + kPitch, 0);
            Memory::Write8(work + 0x33E, 3);
            em.SetMotion(3, 2, 0);
            em.Count(0x14);
        }

        break;
    }
    case 4:
        game.Call<0x80131DE8>({work});
        em.Count(em.Count() - int32_t(ticks));

        if (em.Count() < 0)
            game.Call<ActionDone>({work});

        break;
    }
}

// em025 0x801A314C: Jhen Mohran's breach in the sandship fight. Placed at
// -3515.6, 0, -2467.8 (0x802BC82C 0x39), it waits 160 ticks under the sand,
// plays motion 0x67 at half speed with its sand bursts (0x8030456C 0xB8 /
// 0x7E at frames 350-596) for 480 ticks more, is placed again at -13200, 0,
// 12000 and climbs (0x11, its height from the table 0x805B0908 a tick, at
// most 630) into its swim (motion 1, 12.5 forward). The original counted
// and climbed once a call; here by the step's ticks and length.
void Em025BreachPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x30);
    GuestCall game{ctx};
    const Em::Enemy em{game, frame.Arg(3)};
    const uint32_t work = em.work, at = frame[0x08];

    const auto burst = [&](uint32_t kind, uint32_t bone) { game.Call<Em::Spawn>({work, kind, bone, 0}, {1.0}); };

    switch (em.State()) {
    case 0:
        em.State(1);
        game.Call<Em::SetUp>({work, 4});
        em.SetMotion(0x67, 0, 0);
        game.Call<0x802BC82C>({0x39, work});
        game.Call<0x801460BC>({work}, {-3515.6f, 0.0, -2467.8f}); // SetPosition
        game.Call<0x80146170>({work}, {0.0, 240.0, 0.0});
        game.Call<0x80130450>({work}, {630.0});
        em.Count(0);
        break;
    case 1:
        em.CountTicks();

        if (em.Count() > 0x9F) {
            em.State(2);
            em.SetMotion(0x67, 0, 0x152);
            game.Call<SetMotionSpeed>({work}, {0.5});
        }

        break;
    case 2:
        if (em.At(350.0f)) {
            game.Call<Em::Vec3Set>({at}, {0.0, 0.0, 700.0});
            game.Call<Em::Spawn>({work, 0xB8, 0x14, at}, {1.0});
        }

        if (em.At(380.0f))
            burst(0xB8, 0x12);

        if (em.At(404.0f) || em.At(414.0f))
            burst(0xB8, 5);

        if (em.At(428.0f))
            burst(0xB8, 0x24);

        if (em.At(442.0f) || em.At(460.0f))
            burst(0xB8, 0x26);

        if (em.At(480.0f) || em.At(496.0f))
            burst(0xB8, 0x28);

        if (em.At(506.0f))
            burst(0xB8, 0x29);

        if (em.At(516.0f))
            burst(0xB8, 0x2A);

        if (em.At(540.0f) || em.At(556.0f))
            burst(0x7E, 0x2C);

        if (em.At(580.0f) || em.At(596.0f)) {
            game.Call<Em::Vec3Set>({at}, {0.0, -200.0, -300.0});
            game.Call<Em::Spawn>({work, 0x7E, 0x2D, at}, {1.0});
        }

        em.CountTicks();

        if (em.Count() > 0x1DF) {
            em.NextState();
            em.SetMotion(0x11, 0, 0);
            game.Call<0x801460BC>({work}, {-13200.0, 0.0, 12000.0});
            game.Call<0x80146170>({work}, {0.0, -35.0, 0.0});
            game.Call<0x80130450>({work}, {-1930.0});
            game.Call<0x8019E9AC>({work});
        }

        break;
    case 3:
        game.Call<0x801356A8>({work, 0x805B0908});
        game.Call<0x80130460>({work}, {double(game.F1() * float(GameStep::Scale()))}); // height += the table's climb
        game.Call<0x80135558>({work, work + 0x1BC});                                    // EnemyAddTurnedVelocity

        if (630.0f <= Memory::ReadFloat32(work + 0x1AC))
            Memory::WriteFloat32(work + 0x1AC, 630.0f);

        if (em.Ended()) {
            em.NextState();
            em.SetMotion(1, 0x1E, 0);
            game.Call<0x80130450>({work}, {630.0});
            game.Call<0x8013545C>({work});
            Memory::Write32(work + kSideSpeed, 0x41480000); // 12.5
            game.Call<0x8019E9AC>({work});
        }

        break;
    case 4:
        game.Call<0x80135558>({work, work + 0x1BC});
        break;
    }
}

// kind 6 0x801BD2F0 (r4 1: at once): Diablos under the sand (set up with
// kind 4, motion 1 looped, 0x80131E64 each step) for 90 ticks, or 1 with r4
// 1, then 0x80128158.
void Em006UnderPort(CpuContext* ctx) {
    const bool atOnce = uint8_t(ctx->gpr[4]) == 1;
    Timed(ctx, {.steps = {0x80131E64}, .setUp = 4, .loop = true, .motion = 1, .ticks = atOnce ? 1 : 0x5A, .extra = KeepHeight,
                .end = Then<0x80128158>});
}

// kind 6 0x801BC4D8: Diablos' motion 0xD6 (0x803B9C04: work, its place, 100)
// for 240 ticks, or 1.10 as soon as 0x801BB7BC (work) is 0; then 0xD7 to
// its end.
void Em006SinkPort(CpuContext* ctx) {
    GuestCall game{ctx};
    const Em::Enemy em{game, ctx->gpr[3]};
    const uint32_t work = em.work;

    switch (em.State()) {
    case 0:
        em.State(1);
        game.Call<Em::SetUp>({work, 0});
        em.SetMotion(0xD6, 6, 0);
        em.Count(240);
        game.Call<0x803B9C04>({work, work + 0x188, 100});
        break;
    case 1:
        if (CountDown(em)) {
            em.State(2);
            em.SetMotion(0xD7, 0, 0);
        } else if (game.Call<0x801BB7BC>({work}) == 0)
            game.Call<NextAction>({work, 1, 10});

        break;
    case 2:
        if (em.Ended())
            game.Call<ActionDone>({work});

        break;
    }
}

// The hunter a Gobul aims at (+0x1F6, s8) among the hunters' works
// (get_move_work_adrs(2), 0xB20 each).
uint32_t Em018Hunter(GuestCall& game, uint32_t work) {
    return game.Call<0x800CFAF4>({2}) + uint32_t(int32_t(int8_t(Memory::Read8(work + 0x1F6))) * 0xB20);
}

// Whether a hunter is there and in reach: present (+0), 0x8012D20C (its
// +8) 0, EnemySensesPlayer and 0x8027D594 (hunter) 1.
bool Em018Reaches(GuestCall& game, uint32_t work, uint32_t hunter) {
    return Memory::Read8(hunter) && game.Call<0x8012D20C>({Memory::Read8(hunter + 8)}) == 0 && game.Call<0x8012D118>({work, hunter}) == 1 &&
           game.Call<0x8027D594>({hunter}) == 1;
}

// The count at +0x20 less the step's ticks, not below 0 (the originals: one
// a call while above 0).
void CountDownToZero(const Em::Enemy& em) {
    if (em.Count() > 0)
        em.Count(std::max(em.Count() - int32_t(GameStep::Ticks()), 0));
}

// kind 18 0x80195004: Gobul's motion 0x7D (0x803B9C04: work, its place,
// 100), then 0x7E for at least 240 ticks, then 5.34 once 0x8012D204 (work)
// is 1; 0x8012CF84 each step.
void Em018LurkPort(CpuContext* ctx) {
    GuestCall game{ctx};
    const Em::Enemy em{game, ctx->gpr[3]};
    const uint32_t work = em.work;
    game.Call<0x8012CF84>({work});

    switch (em.State()) {
    case 0:
        em.State(1);
        game.Call<Em::SetUp>({work, 0});
        em.SetMotion(0x7D, 0, 0);
        game.Call<0x803B9C04>({work, work + 0x188, 100});
        break;
    case 1:
        if (em.Ended()) {
            em.NextState();
            em.SetMotion(0x7E, 0, 0);
            em.Count(240);
        }

        break;
    case 2:
        if (CountDown(em) && game.Call<0x8012D204>({work}) == 1)
            game.Call<NextAction>({work, 5, 0x22});

        break;
    }
}

// kind 18 0x80195278 and 0x80194B48: Gobul's open mouth (set up with kind
// 2, motion 0x28 through SetMotionEx) for at least 60 ticks while 0x8012D204
// (work) is 1: 5.30 (5.31) when its hunter is in reach, otherwise
// ActionDone94 once the 60 ticks are over. 0x80194B48 also steers
// (0x80131ED8), holds a hunter in reach at the start (+0x59A 60, 0x8027D5E8
// (hunter, work), 0x80275524 (hunter, 1)) when 0x801924AC (work) is 1, and
// asks that and Pl_act_ck (hunter, 0, 0x9A) too.
void Em018Gape(CpuContext* ctx, bool hold) {
    GuestCall game{ctx};
    const Em::Enemy em{game, ctx->gpr[3]};
    const uint32_t work = em.work;
    game.Call<0x8012CF84>({work});

    if (hold)
        game.Call<0x80131ED8>({work});

    switch (em.State()) {
    case 0:
        em.State(1);
        game.Call<Em::SetUp>({work, 2});
        game.Call<SetMotionEx>({work, 0x28, 0x14, 0, 3});
        em.Count(60);

        if (hold && game.Call<0x8012D204>({work}) == 1) {
            const uint32_t hunter = Em018Hunter(game, work);

            if (Em018Reaches(game, work, hunter) && game.Call<0x801924AC>({work}) == 1) {
                Memory::Write16(hunter + 0x59A, uint16_t(em.Count()));
                game.Call<0x8027D5E8>({hunter, work});
                game.Call<0x80275524>({hunter, 1});
            }
        }

        break;
    case 1: {
        CountDownToZero(em);

        if (game.Call<0x8012D204>({work}) != 1)
            break;

        const uint32_t hunter = Em018Hunter(game, work);
        bool reach;

        if (hold)
            reach = Memory::Read8(hunter) && game.Call<0x8012D20C>({Memory::Read8(hunter + 8)}) == 0 && game.Call<0x8012D118>({work, hunter}) == 1 &&
                    game.Call<0x801924AC>({work}) == 1 && game.Call<0x8026FECC>({hunter, 0, 0x9A}) != 0; // Pl_act_ck
        else
            reach = Em018Reaches(game, work, hunter);

        if (reach)
            game.Call<NextAction>({work, 5, hold ? 0x1Fu : 0x1Eu});
        else if (em.Count() < 1)
            game.Call<ActionDone94>({work});

        break;
    }
    }
}

void Em018GapePort(CpuContext* ctx) {
    Em018Gape(ctx, false);
}

void Em018BitePort(CpuContext* ctx) {
    Em018Gape(ctx, true);
}

// kind 18 0x801958C4: Gobul lying in wait (motions 0xCC, then 0xCF with its
// lure, 0x803B95EC: work, 2, +0x334, 4, 0), +0x359 and +0x35B set and
// 0x8012CF84 each step. While 0x8012D204 (work) is 1 it counts up; every 16
// ticks it looks at the four distances at +0x454: one within 300 starts state
// 3 (90 ticks, then 6.11), the first within 600 substate 1 (300 ticks, then
// 6.23); +0x833 set starts substate 2 (60 ticks, then 6.24); in substate 0 a
// hunter in reach or 1800 ticks start 6.4.
void Em018AmbushPort(CpuContext* ctx) {
    GuestCall game{ctx};
    const Em::Enemy em{game, ctx->gpr[3]};
    const uint32_t work = em.work;
    constexpr uint32_t kSub = 6, kLure = 0x334;
    Memory::Write8(work + 0x359, 1);
    Memory::Write8(work + 0x35B, 1);
    game.Call<0x8012CF84>({work});

    switch (em.State()) {
    case 0:
        em.State(1);
        game.Call<Em::SetUp>({work, 4});
        em.SetMotion(0xCC, 0, 0);
        KeepHeight(game, work);
        game.Call<0x801321FC>({work});
        break;
    case 1:
        if (!em.Ended())
            break;

        em.NextState();
        Memory::Write8(work + kSub, 0);
        em.SetMotion(0xCF, 0, 0);
        em.Count(0);

        if (Memory::Read32(work + kLure) != 0xFFFFFFFF) {
            game.Call<0x803B99F8>({Memory::Read32(work + kLure)});
            Memory::Write32(work + kLure, 0xFFFFFFFF);
        }

        game.Call<0x803B95EC>({work, 2, work + kLure, 4, 0});
        break;
    case 2: {
        em.CountTicks();

        if (game.Call<0x8012D204>({work}) != 1)
            break;

        // The original's look at a count (after its +1) that is a multiple of 16.
        if (Em::AnyOf(em.Count() - int32_t(GameStep::Ticks()) + 1, GameStep::Ticks(), 16, 0)) {
            for (uint32_t i = 0; i < 4; ++i) {
                const float distance = Memory::ReadFloat32(work + 0x454 + 4 * i);

                if (distance <= 0.0f)
                    continue;

                if (distance <= 300.0f) {
                    em.NextState();
                    em.Count(90);
                    return;
                }

                if (Memory::Read8(work + kSub) == 0 && distance <= 600.0f) {
                    Memory::Write8(work + kSub, 1);
                    em.Count(0);
                    break;
                }
            }
        }

        switch (Memory::Read8(work + kSub)) {
        case 0:
            if (em.Count() >= 1800) {
                game.Call<NextAction>({work, 6, 4});
                break;
            }

            if (Memory::Read8(work + 0x833)) {
                Memory::Write8(work + kSub, 2);
                em.Count(0);
                break;
            }

            if (Memory::Read8(work + 0x43D) == 1)
                break;

            for (uint32_t i = 0, n = game.Call<0x800CFB34>({2}) & 0xFFFF, hunter = game.Call<0x800CFAF4>({2}); i < n; ++i, hunter += 0xB20)
                if (Em018Reaches(game, work, hunter)) {
                    game.Call<NextAction>({work, 6, 4});
                    return;
                }

            break;
        case 1:
            if (em.Count() > 299)
                game.Call<NextAction>({work, 6, 0x17});

            break;
        case 2:
            if (em.Count() > 59)
                game.Call<NextAction>({work, 6, 0x18});

            break;
        }

        break;
    }
    case 3:
        CountDownToZero(em);

        if (game.Call<0x8012D204>({work}) == 1 && em.Count() < 1)
            game.Call<NextAction>({work, 6, 0xB});

        break;
    }
}

// kind 18 0x8019771C: Gobul's leap out of the water. Motion 0xCE for 24
// ticks (a splash, 0x8030456C 0x27, and in its area the sound 4 at joint 3
// raised to +0x20C + 5 on the first), then 0xDD (two voices; 0x80136D78 from
// frame 14), the jump 0xDF (+0x314 86.67) until it is 50 above the ground
// (0x801302D0, 0x801300EC), the fall to the water line (+0x210) and the dive
// 0xE0 (voices cleared, +0x314 86.67, +0x320 -3.47, splashes), then landing
// in the water (0xE1, a mark) or on land 7.18, then ActionDone94.
void Em018LeapPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x40);
    GuestCall game{ctx};
    const Em::Enemy em{game, frame.Arg(3)};
    const uint32_t work = em.work, at = frame[0x10], joint = frame[0x20];

    const auto splash = [&](float scale, uint32_t kind, uint32_t bone, float x, float y, float z) {
        game.Call<Em::Vec3Set>({at}, {double(x), double(y), double(z)});
        game.Call<Em::Spawn>({work, kind, bone, at}, {double(scale)});
    };

    const auto submerged = [&] {
        Memory::Write8(work + 0x359, 1);
        Memory::Write8(work + 0x35B, 1);
    };

    switch (em.State()) {
    case 0:
        em.State(1);
        Memory::Write8(work + 6, 0);
        game.Call<Em::SetUp>({work, 4});
        em.SetMotion(0xCE, 0, 0);
        KeepHeight(game, work);
        em.Count(0x18);
        game.Call<0x80130D40>({work, uint32_t(-12)});
        submerged();
        break;
    case 1:
        submerged();

        // The first step of the count (the original's first call).
        if (em.Count() == 0x18 && GameStep::Ticks() > 0) {
            splash(1.0f, 0x27, 3, 0.0f, 15.0f, 0.0f);

            if (Memory::Read8(work + 6) == 0) {
                if ((game.Call<0x802AFCE8>() & 0xFF) == Memory::Read8(work + 0x1E1)) { // get_now_areano
                    game.Call<JointPosition>({work, 3, joint});
                    Memory::WriteFloat32(joint + 4, 5.0f + Memory::ReadFloat32(work + 0x20C));
                    game.Call<0x800D9F0C>({Memory::Read32(work + 0xB14), 4, 2, joint}); // se_req_pos_ps
                }

                Memory::Write8(work + 6, 1);
            }
        }

        if (CountDown(em)) {
            em.NextState();
            game.Call<Em::SetUp>({work, 0});
            em.SetMotion(0xDD, 0, 0);
            game.Call<0x80130450>({work}, {0.0});
            game.Call<RoarVoice>({work, 0, 0xD, 3});
            game.Call<RoarVoice>({work, 1, 0x15, 3});
            game.Call<0x80136D78>({work});
        }

        break;
    case 2:
        if (em.At(14.0f, 2))
            game.Call<0x80136D78>({work});

        if (em.Ended()) {
            em.NextState();
            game.Call<Em::SetUp>({work, 2});
            em.SetMotion(0xDF, 0, 0);
            game.Call<0x80135448>({work});
            Memory::Write32(work + kSpeed, 0x42AD570A); // 86.67
        }

        break;
    case 3:
        game.Call<AddVelocity>({work});
        game.Call<0x801302D0>({work});

        if (game.Call<0x801300EC>({work}, {double(50.0f + game.F1())}) == 1) {
            em.NextState();
            game.Call<SetScale>({work}, {0.0});
        }

        break;
    case 4:
        game.Call<AddVelocity>({work});

        if (Memory::ReadFloat32(work + 0x210) <= Memory::ReadFloat32(work + 0x18C)) {
            em.NextState();
            game.Call<0x80129788>({work, 0});
            game.Call<0x80129788>({work, 1});
            game.Call<SetMotionEx>({work, 0xE0, 4, 0, 1});
            game.Call<RoarVoice>({work, 0, 0x1C, 2});
            game.Call<0x8013545C>({work});
            Memory::Write32(work + kSpeed, 0x42AD570A);  // 86.67
            Memory::Write32(work + 0x320, 0xC05E147B);   // -3.47
            game.Call<JointEffect>({work, 0xE, 0x2C, 0}, {1.0});
            splash(0.7f, 0x30, 0xE, 0.0f, 0.0f, 0.0f);
            splash(1.0f, 0x32, 3, 0.0f, -75.0f, 0.0f);
        }

        break;
    case 5: {
        const uint32_t moved = game.Call<0x801354CC>({work}, {200.0});

        if (game.Call<0x80130168>({work}) == 1) { // em_water_check
            if (Memory::ReadFloat32(work + 0x18C) <= Memory::ReadFloat32(work + 0x210)) {
                Memory::WriteFloat32(work + 0x18C, Memory::ReadFloat32(work + 0x210));
                em.NextState();
                game.Call<Em::SetUp>({work, 2});
                em.SetMotion(0xE1, 0, 0);
                game.Call<RoarVoice0>({work, 0, 0x1D});
                game.Call<0x8012FEA0>({work}, {300.0});
                game.Call<JointEffect>({work, 0xE, 0x2C, 0}, {1.0});
                splash(0.7f, 0x30, 0xE, 0.0f, 0.0f, 0.0f);
                splash(0.7f, 0x31, 3, 0.0f, 0.0f, 15.0f);
                game.Call<Mark>({work, uint32_t(-1), 7});
            }
        } else if (moved == 1 && Memory::ReadFloat32(work + kSpeed) < 0.0f) {
            game.Call<Em::SetUp>({work, 0});
            game.Call<NextAction>({work, 7, 0x12});
        }

        break;
    }
    case 6:
        if (em.Ended())
            game.Call<ActionDone94>({work});

        break;
    }
}

// kind 18 0x80197F68: Gobul's hop. Motion 0xCE for 12 ticks (+0x359 and
// +0x35B set), then 0xDC (a voice, +0x318 50): steering (0x300) from frame
// 34, moving (EnemyAddTurnedVelocity) in frames 38-50, a splash at 12, a mark
// from 28, a wave (0x801E2D68) at 50; ActionDone at its end.
void Em018HopPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x30);
    GuestCall game{ctx};
    const Em::Enemy em{game, frame.Arg(3)};
    const uint32_t work = em.work, at = frame[0x08];

    switch (em.State()) {
    case 0:
        em.State(1);
        game.Call<Em::SetUp>({work, 4});
        em.SetMotion(0xCE, 0, 0);
        KeepHeight(game, work);
        em.Count(12);
        game.Call<0x80130D40>({work, uint32_t(-12)});
        Memory::Write8(work + 0x359, 1);
        Memory::Write8(work + 0x35B, 1);
        break;
    case 1:
        Memory::Write8(work + 0x359, 1);
        Memory::Write8(work + 0x35B, 1);

        if (CountDown(em)) {
            em.State(2);
            game.Call<Em::SetUp>({work, 0});
            em.SetMotion(0xDC, 0, 0);
            game.Call<RoarVoice0>({work, 0, 0xC});
            game.Call<0x80130450>({work}, {0.0});
            game.Call<0x80136D78>({work});
            game.Call<0x80135448>({work});
            Memory::WriteFloat32(work + kSideSpeed, 50.0f);
        }

        break;
    case 2:
        if (em.At(34.0f, 2))
            game.Call<Steer>({work, 0x300});

        if (em.At(38.0f, 3, 50.0f))
            game.Call<0x80135558>({work, work + 0x1BC}); // EnemyAddTurnedVelocity

        if (em.At(12.0f)) {
            game.Call<Em::Vec3Set>({at}, {0.0, 0.0, 0.0});
            game.Call<Em::Spawn>({work, 0x1C, 3, at}, {1.0});
        }

        if (em.At(28.0f, 2))
            game.Call<0x80136D78>({work});

        if (em.At(50.0f)) {
            game.Call<Em::Vec3Set>({at}, {0.0, 0.0, 200.0});
            game.Call<0x801E2D68>({work, 0xF, 0xD, at, 0x3C}, {1.3f});
        }

        if (em.Ended())
            game.Call<ActionDone>({work});

        break;
    }
}

// kind 9 0x80387D08: Uragaan dozing (motion 0x6E looped for 150 ticks, the
// original's 150 calls, +0x8AB set and 0x80132280 each step), then it gets
// up (0x70, 0x26), follows the path 0x805712E0 (0x801349C8, 0x80134B70),
// turns to its target (motion 10, 0x80134178), digs in (0x27, effects at
// frames 200, 10 and 210) and ends (motion 1, 0x8012B3E4, then 1.7), steering
// each step (0x8012CF84, 0x80131ED8) before state 6.
void Em009DozePort(CpuContext* ctx) {
    GuestCall game{ctx};
    const Em::Enemy em{game, ctx->gpr[3]};
    const uint32_t work = em.work;
    constexpr uint32_t kPath = 0x805712E0;
    const auto steer = [&] {
        game.Call<0x8012CF84>({work});
        game.Call<0x80131ED8>({work});
    };

    switch (em.State()) {
    case 0:
        em.NextState();
        game.Call<SetMotionLoop>({work, 0x6E, 0, 0});
        em.Count(0x96);
        game.Call<0x80132288>({work});
        steer();
        break;
    case 1:
        steer();
        game.Call<0x80132280>({work, 1, 3}, {0.6f});

        if (CountDown(em)) {
            em.NextState();
            em.SetMotion(0x70, 4, 0);
            game.Call<0x801322C8>({work});
        }

        break;
    case 2:
        steer();

        if (em.Ended()) {
            em.NextState();
            game.Call<Em::SetUp>({work, 0});
            em.SetMotion(0x26, 4, 0);
        }

        break;
    case 3:
        steer();

        if (em.At(200.0f, 1))
            game.Call<0x80136D78>({work});

        if (em.Ended()) {
            em.NextState();
            game.Call<Em::SetUp>({work, 4});
            game.Call<0x801349C8>({work, kPath, 0, 0, 0});
            game.Call<0x80130390>({work}); // f1 passes on
            game.Call<0x80130450>({work});
        }

        break;
    case 4:
        steer();

        if (game.Call<0x80134B70>({work, kPath}) == 1) {
            em.NextState();
            game.Call<Em::SetUp>({work, 4});
            em.SetMotion(10, 0, 0);
            game.Call<TurnSetUp>({work, 0}, {0.0});
            game.Call<0x80130390>({work}); // f1 passes on
            game.Call<0x80130450>({work});
        }

        break;
    case 5:
        steer();
        game.Call<0x80131DE8>({work});

        if (game.Call<TurnStep>({work, 0, 0x100}) == 1) {
            em.NextState();
            game.Call<Em::SetUp>({work, 0});
            em.SetMotion(0x27, 0, 0);
            game.Call<0x80130450>({work}, {0.0});
            game.Call<0x80136D78>({work});
        }

        break;
    case 6:
        if (em.At(210.0f, 2))
            steer();

        if (em.At(10.0f, 2))
            game.Call<0x80136D78>({work});

        if (em.Ended()) {
            em.NextState();
            game.Call<SetMotionLoop>({work, 1, 6, 0});
        }

        break;
    case 7:
        if (game.Call<0x8012F9AC>({work}) == 1) {
            game.Call<0x8012B3E4>({work, 0, 0, 0});
            game.Call<NextAction>({work, 1, 7});
        }

        break;
    }
}

// kind 9 0x803878A8: Uragaan's sleep, steering each step (0x8012CF84,
// 0x80131ED8): lie down (0x28), sleep (0x6E) for 1800 ticks (the
// original's 1800 calls) or until, while it may act, it senses a hunter
// (EnemySensesPlayer over the player works), then 0x8012B3E4 (5, 8, 4) and
// 1.6. The original's flag is r28, left from the caller when the sleep goes
// on and it may not act; the port reads it the same way.
void Em009SleepPort(CpuContext* ctx) {
    GuestCall game{ctx};
    const uint32_t entryR28 = ctx->gpr[28];
    const Em::Enemy em{game, ctx->gpr[3]};
    const uint32_t work = em.work;
    game.Call<0x8012CF84>({work});
    game.Call<0x80131ED8>({work});

    switch (em.State()) {
    case 0:
        em.State(1);
        game.Call<Em::SetUp>({work, 0});
        em.SetMotion(0x28, 2, 0);
        break;
    case 1:
        if (em.Ended()) {
            em.NextState();
            em.SetMotion(0x6E, 4, 0);
            em.Count(0x708);
            game.Call<0x80132288>({work});
        }

        break;
    case 2: {
        game.Call<0x80132280>({work, 1, 0xF}, {0.6f});
        uint32_t wake = entryR28;

        if (CountDown(em))
            wake = 1;
        else if (Memory::Read8(work + 0x1F5) == 1) {
            const uint32_t players = game.Call<0x800CFB34>({2}) & 0xFFFF; // get_move_work_max(2)
            uint32_t player = game.Call<0x800CFAF4>({2});                 // get_move_work_adrs(2)
            wake = 0;

            for (uint32_t i = 0; i < players; ++i, player += 0xB20)
                if (Memory::Read8(player) != 0 && game.Call<0x8012D20C>({Memory::Read8(player + 8)}) == 0 && game.Call<0x8012D118>({work, player}) == 1)
                    wake = 1;
        }

        if (wake == 1) {
            game.Call<0x8012B3E4>({work, 5, 8, 4});
            game.Call<NextAction>({work, 1, 6});
        }

        break;
    }
    }
}

// kind 35 0x8035F378 and 0x8035F560 (motion flags 4 or 0): Bnahabra
// waiting, motion 1 looped for 90 ticks (the original's 90 calls).
void Em035Wait(CpuContext* ctx, uint32_t flags) {
    GuestCall game{ctx};
    const Em::Enemy em{game, ctx->gpr[3]};
    const uint32_t work = em.work;

    switch (em.State()) {
    case 0:
        em.State(1);
        game.Call<Em::SetUp>({work, 0});
        game.Call<SetMotionLoop>({work, 1, flags, 0});
        em.Count(0x5A);
        break;
    case 1:
        if (CountDown(em))
            game.Call<ActionDone>({work});

        break;
    }
}

void Em035WaitPort(CpuContext* ctx) {
    Em035Wait(ctx, 4);
}

void Em035HoverPort(CpuContext* ctx) {
    Em035Wait(ctx, 0);
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

extern "C" void func_80148ED0(CpuContext* ctx);
extern "C" void MH3_Em001Sleep(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em001Sleep", func_80148ED0, Em001SleepPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80148ED0, MH3_Em001Sleep, "Em001Sleep");

extern "C" void func_80155EBC(CpuContext* ctx);
extern "C" void MH3_Em003Sleep(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em003Sleep", func_80155EBC, Em003SleepPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80155EBC, MH3_Em003Sleep, "Em003Sleep");

extern "C" void func_8015E39C(CpuContext* ctx);
extern "C" void MH3_Em008Sleep(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em008Sleep", func_8015E39C, Em008SleepPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8015E39C, MH3_Em008Sleep, "Em008Sleep");

extern "C" void func_80170BB0(CpuContext* ctx);
extern "C" void MH3_Em012Sleep(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em012Sleep", func_80170BB0, Em012SleepPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80170BB0, MH3_Em012Sleep, "Em012Sleep");

extern "C" void func_801777D8(CpuContext* ctx);
extern "C" void MH3_Em015Sleep(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em015Sleep", func_801777D8, Em015SleepPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x801777D8, MH3_Em015Sleep, "Em015Sleep");

extern "C" void func_80183F00(CpuContext* ctx);
extern "C" void MH3_Em016Sleep(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em016Sleep", func_80183F00, Em016SleepPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80183F00, MH3_Em016Sleep, "Em016Sleep");

extern "C" void func_801BC230(CpuContext* ctx);
extern "C" void MH3_Em006Sleep(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em006Sleep", func_801BC230, Em006SleepPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x801BC230, MH3_Em006Sleep, "Em006Sleep");

extern "C" void func_801CB6F8(CpuContext* ctx);
extern "C" void MH3_Em005Sleep(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em005Sleep", func_801CB6F8, Em005SleepPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x801CB6F8, MH3_Em005Sleep, "Em005Sleep");

extern "C" void func_80379A0C(CpuContext* ctx);
extern "C" void MH3_Em019Sleep(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em019Sleep", func_80379A0C, Em019SleepPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80379A0C, MH3_Em019Sleep, "Em019Sleep");

extern "C" void func_801D7C5C(CpuContext* ctx);
extern "C" void MH3_Em007Sleep(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em007Sleep", func_801D7C5C, Em007SleepPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x801D7C5C, MH3_Em007Sleep, "Em007Sleep");

extern "C" void func_80195DE4(CpuContext* ctx);
extern "C" void MH3_Em018Sleep(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em018Sleep", func_80195DE4, Em018SleepPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80195DE4, MH3_Em018Sleep, "Em018Sleep");

extern "C" void func_801C6014(CpuContext* ctx);
extern "C" void MH3_Em004Sleep(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em004Sleep", func_801C6014, Em004SleepPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x801C6014, MH3_Em004Sleep, "Em004Sleep");

extern "C" void func_8014888C(CpuContext* ctx);
extern "C" void MH3_Em001Walk(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em001Walk", func_8014888C, Em001WalkPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8014888C, MH3_Em001Walk, "Em001Walk");

extern "C" void func_8014A8C4(CpuContext* ctx);
extern "C" void MH3_Em001Hover(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em001Hover", func_8014A8C4, Em001HoverPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8014A8C4, MH3_Em001Hover, "Em001Hover");

extern "C" void func_80184DFC(CpuContext* ctx);
extern "C" void MH3_Em016Swim(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em016Swim", func_80184DFC, Em016SwimPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80184DFC, MH3_Em016Swim, "Em016Swim");

extern "C" void func_8018599C(CpuContext* ctx);
extern "C" void MH3_Em016Rest(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em016Rest", func_8018599C, Em016RestPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8018599C, MH3_Em016Rest, "Em016Rest");

extern "C" void func_80177A00(CpuContext* ctx);
extern "C" void MH3_Em015Pause(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em015Pause", func_80177A00, Em015PausePort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80177A00, MH3_Em015Pause, "Em015Pause");

extern "C" void func_80379C34(CpuContext* ctx);
extern "C" void MH3_Em019Pause(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em019Pause", func_80379C34, Em019PausePort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80379C34, MH3_Em019Pause, "Em019Pause");

extern "C" void func_8037F414(CpuContext* ctx);
extern "C" void MH3_Em019Pause2(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em019Pause2", func_8037F414, Em019Pause2Port);
}
REGISTER_NATIVE_FUNCTION_AS(0x8037F414, MH3_Em019Pause2, "Em019Pause2");

extern "C" void func_801AD4FC(CpuContext* ctx);
extern "C" void MH3_Em027Graze(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em027Graze", func_801AD4FC, Em027GrazePort);
}
REGISTER_NATIVE_FUNCTION_AS(0x801AD4FC, MH3_Em027Graze, "Em027Graze");

extern "C" void func_801AD58C(CpuContext* ctx);
extern "C" void MH3_Em027Graze2(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em027Graze2", func_801AD58C, Em027Graze2Port);
}
REGISTER_NATIVE_FUNCTION_AS(0x801AD58C, MH3_Em027Graze2, "Em027Graze2");

extern "C" void func_801B1198(CpuContext* ctx);
extern "C" void MH3_Em030Idle(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em030Idle", func_801B1198, Em030IdlePort);
}
REGISTER_NATIVE_FUNCTION_AS(0x801B1198, MH3_Em030Idle, "Em030Idle");

extern "C" void func_801B1378(CpuContext* ctx);
extern "C" void MH3_Em030Idle2(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em030Idle2", func_801B1378, Em030Idle2Port);
}
REGISTER_NATIVE_FUNCTION_AS(0x801B1378, MH3_Em030Idle2, "Em030Idle2");

extern "C" void func_80157FA8(CpuContext* ctx);
extern "C" void MH3_Em003Hover(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em003Hover", func_80157FA8, Em003HoverPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80157FA8, MH3_Em003Hover, "Em003Hover");

extern "C" void func_801BC10C(CpuContext* ctx);
extern "C" void MH3_Em006Walk(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em006Walk", func_801BC10C, Em006WalkPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x801BC10C, MH3_Em006Walk, "Em006Walk");

extern "C" void func_801CB5E0(CpuContext* ctx);
extern "C" void MH3_Em005Walk(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em005Walk", func_801CB5E0, Em005WalkPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x801CB5E0, MH3_Em005Walk, "Em005Walk");

extern "C" void func_80388D80(CpuContext* ctx);
extern "C" void MH3_Em009Buried(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em009Buried", func_80388D80, Em009BuriedPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80388D80, MH3_Em009Buried, "Em009Buried");

extern "C" void func_80387D08(CpuContext* ctx);
extern "C" void MH3_Em009Doze(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em009Doze", func_80387D08, Em009DozePort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80387D08, MH3_Em009Doze, "Em009Doze");

extern "C" void func_803878A8(CpuContext* ctx);
extern "C" void MH3_Em009Sleep(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em009Sleep", func_803878A8, Em009SleepPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x803878A8, MH3_Em009Sleep, "Em009Sleep");

extern "C" void func_801AC888(CpuContext* ctx);
extern "C" void MH3_Em027Trot(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em027Trot", func_801AC888, Em027TrotPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x801AC888, MH3_Em027Trot, "Em027Trot");

extern "C" void func_801C40B0(CpuContext* ctx);
extern "C" void MH3_Em004Wait(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em004Wait", func_801C40B0, Em004WaitPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x801C40B0, MH3_Em004Wait, "Em004Wait");

extern "C" void func_801CDDF8(CpuContext* ctx);
extern "C" void MH3_Em005Wait(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em005Wait", func_801CDDF8, Em005WaitPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x801CDDF8, MH3_Em005Wait, "Em005Wait");

extern "C" void func_80194D2C(CpuContext* ctx);
extern "C" void MH3_Em018Wait(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em018Wait", func_80194D2C, Em018WaitPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80194D2C, MH3_Em018Wait, "Em018Wait");

extern "C" void func_8035DA78(CpuContext* ctx);
extern "C" void MH3_Em033Wait(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em033Wait", func_8035DA78, Em033WaitPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8035DA78, MH3_Em033Wait, "Em033Wait");

extern "C" void func_80170CCC(CpuContext* ctx);
extern "C" void MH3_Em012Pause(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em012Pause", func_80170CCC, Em012PausePort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80170CCC, MH3_Em012Pause, "Em012Pause");

extern "C" void func_801A14B8(CpuContext* ctx);
extern "C" void MH3_Em025Rest(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em025Rest", func_801A14B8, Em025RestPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x801A14B8, MH3_Em025Rest, "Em025Rest");

extern "C" void func_80167904(CpuContext* ctx);
extern "C" void MH3_Em010Back(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em010Back", func_80167904, Em010BackPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80167904, MH3_Em010Back, "Em010Back");

extern "C" void func_8016DB84(CpuContext* ctx);
extern "C" void MH3_Em011Back(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em011Back", func_8016DB84, Em011BackPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8016DB84, MH3_Em011Back, "Em011Back");

extern "C" void func_8015FB24(CpuContext* ctx);
extern "C" void MH3_Em008Wait(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em008Wait", func_8015FB24, Em008WaitPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8015FB24, MH3_Em008Wait, "Em008Wait");

extern "C" void func_801D8B28(CpuContext* ctx);
extern "C" void MH3_Em007Buried(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em007Buried", func_801D8B28, Em007BuriedPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x801D8B28, MH3_Em007Buried, "Em007Buried");

extern "C" void func_8015F400(CpuContext* ctx);
extern "C" void MH3_Em008Buried(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em008Buried", func_8015F400, Em008BuriedPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8015F400, MH3_Em008Buried, "Em008Buried");

extern "C" void func_801BD2F0(CpuContext* ctx);
extern "C" void MH3_Em006Under(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em006Under", func_801BD2F0, Em006UnderPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x801BD2F0, MH3_Em006Under, "Em006Under");

extern "C" void func_801BC4D8(CpuContext* ctx);
extern "C" void MH3_Em006Sink(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em006Sink", func_801BC4D8, Em006SinkPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x801BC4D8, MH3_Em006Sink, "Em006Sink");

extern "C" void func_80195004(CpuContext* ctx);
extern "C" void MH3_Em018Lurk(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em018Lurk", func_80195004, Em018LurkPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80195004, MH3_Em018Lurk, "Em018Lurk");

extern "C" void func_80195278(CpuContext* ctx);
extern "C" void MH3_Em018Gape(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em018Gape", func_80195278, Em018GapePort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80195278, MH3_Em018Gape, "Em018Gape");

extern "C" void func_80194B48(CpuContext* ctx);
extern "C" void MH3_Em018Bite(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em018Bite", func_80194B48, Em018BitePort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80194B48, MH3_Em018Bite, "Em018Bite");

extern "C" void func_801958C4(CpuContext* ctx);
extern "C" void MH3_Em018Ambush(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em018Ambush", func_801958C4, Em018AmbushPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x801958C4, MH3_Em018Ambush, "Em018Ambush");

extern "C" void func_8019771C(CpuContext* ctx);
extern "C" void MH3_Em018Leap(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em018Leap", func_8019771C, Em018LeapPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8019771C, MH3_Em018Leap, "Em018Leap");

extern "C" void func_80197F68(CpuContext* ctx);
extern "C" void MH3_Em018Hop(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em018Hop", func_80197F68, Em018HopPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80197F68, MH3_Em018Hop, "Em018Hop");

extern "C" void func_8019F5D0(CpuContext* ctx);
extern "C" void MH3_Em025Wait(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em025Wait", func_8019F5D0, Em025WaitPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8019F5D0, MH3_Em025Wait, "Em025Wait");

extern "C" void func_801A13E8(CpuContext* ctx);
extern "C" void MH3_Em025Rest2(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em025Rest2", func_801A13E8, Em025Rest2Port);
}
REGISTER_NATIVE_FUNCTION_AS(0x801A13E8, MH3_Em025Rest2, "Em025Rest2");

extern "C" void func_8032D89C(CpuContext* ctx);
extern "C" void MH3_Em026Wait(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em026Wait", func_8032D89C, Em026WaitPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8032D89C, MH3_Em026Wait, "Em026Wait");

extern "C" void func_8032D53C(CpuContext* ctx);
extern "C" void MH3_Em026Crawl(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em026Crawl", func_8032D53C, Em026CrawlPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8032D53C, MH3_Em026Crawl, "Em026Crawl");

extern "C" void func_8032D628(CpuContext* ctx);
extern "C" void MH3_Em026Crawl2(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em026Crawl2", func_8032D628, Em026Crawl2Port);
}
REGISTER_NATIVE_FUNCTION_AS(0x8032D628, MH3_Em026Crawl2, "Em026Crawl2");

extern "C" void func_8032DC10(CpuContext* ctx);
extern "C" void MH3_Em026Turn(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em026Turn", func_8032DC10, Em026TurnPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8032DC10, MH3_Em026Turn, "Em026Turn");

extern "C" void func_8035C620(CpuContext* ctx);
extern "C" void MH3_Em033Back(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em033Back", func_8035C620, Em033BackPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8035C620, MH3_Em033Back, "Em033Back");

extern "C" void func_8035C410(CpuContext* ctx);
extern "C" void MH3_Em033Pause(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em033Pause", func_8035C410, Em033PausePort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8035C410, MH3_Em033Pause, "Em033Pause");

extern "C" void func_8035F400(CpuContext* ctx);
extern "C" void MH3_Em035Rest(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em035Rest", func_8035F400, Em035RestPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8035F400, MH3_Em035Rest, "Em035Rest");

extern "C" void func_8035F6A8(CpuContext* ctx);
extern "C" void MH3_Em035Rest2(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em035Rest2", func_8035F6A8, Em035Rest2Port);
}
REGISTER_NATIVE_FUNCTION_AS(0x8035F6A8, MH3_Em035Rest2, "Em035Rest2");

extern "C" void func_803796F8(CpuContext* ctx);
extern "C" void MH3_Em019Lava(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em019Lava", func_803796F8, Em019LavaPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x803796F8, MH3_Em019Lava, "Em019Lava");

extern "C" void func_8037A56C(CpuContext* ctx);
extern "C" void MH3_Em019Lava2(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em019Lava2", func_8037A56C, Em019Lava2Port);
}
REGISTER_NATIVE_FUNCTION_AS(0x8037A56C, MH3_Em019Lava2, "Em019Lava2");

extern "C" void func_801AC714(CpuContext* ctx);
extern "C" void MH3_Em027Turn(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em027Turn", func_801AC714, Em027TurnPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x801AC714, MH3_Em027Turn, "Em027Turn");

extern "C" void func_801AC930(CpuContext* ctx);
extern "C" void MH3_Em027Turn2(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em027Turn2", func_801AC930, Em027Turn2Port);
}
REGISTER_NATIVE_FUNCTION_AS(0x801AC930, MH3_Em027Turn2, "Em027Turn2");

extern "C" void func_801B0F64(CpuContext* ctx);
extern "C" void MH3_Em030Idle3(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em030Idle3", func_801B0F64, Em030Idle3Port);
}
REGISTER_NATIVE_FUNCTION_AS(0x801B0F64, MH3_Em030Idle3, "Em030Idle3");

extern "C" void func_801B1020(CpuContext* ctx);
extern "C" void MH3_Em030Idle4(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em030Idle4", func_801B1020, Em030Idle4Port);
}
REGISTER_NATIVE_FUNCTION_AS(0x801B1020, MH3_Em030Idle4, "Em030Idle4");

extern "C" void func_801B10DC(CpuContext* ctx);
extern "C" void MH3_Em030Idle5(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em030Idle5", func_801B10DC, Em030Idle5Port);
}
REGISTER_NATIVE_FUNCTION_AS(0x801B10DC, MH3_Em030Idle5, "Em030Idle5");

extern "C" void func_801B19BC(CpuContext* ctx);
extern "C" void MH3_Em030Turn(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em030Turn", func_801B19BC, Em030TurnPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x801B19BC, MH3_Em030Turn, "Em030Turn");

extern "C" void func_801685D8(CpuContext* ctx);
extern "C" void MH3_Em010Turn2(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em010Turn2", func_801685D8, Em010Turn2Port);
}
REGISTER_NATIVE_FUNCTION_AS(0x801685D8, MH3_Em010Turn2, "Em010Turn2");

extern "C" void func_801B27F0(CpuContext* ctx);
extern "C" void MH3_Em030Charge2(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em030Charge2", func_801B27F0, Em030Charge2Port);
}
REGISTER_NATIVE_FUNCTION_AS(0x801B27F0, MH3_Em030Charge2, "Em030Charge2");

extern "C" void func_8015FA60(CpuContext* ctx);
extern "C" void MH3_Em008Track(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em008Track", func_8015FA60, Em008TrackPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8015FA60, MH3_Em008Track, "Em008Track");

extern "C" void func_8015814C(CpuContext* ctx);
extern "C" void MH3_Em003Settle(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em003Settle", func_8015814C, Em003SettlePort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8015814C, MH3_Em003Settle, "Em003Settle");

extern "C" void func_801B7C40(CpuContext* ctx);
extern "C" void MH3_Em034HopAway(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em034HopAway", func_801B7C40, Em034HopAwayPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x801B7C40, MH3_Em034HopAway, "Em034HopAway");

extern "C" void func_801B81F0(CpuContext* ctx);
extern "C" void MH3_Em034LeapTo(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em034LeapTo", func_801B81F0, Em034LeapToPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x801B81F0, MH3_Em034LeapTo, "Em034LeapTo");

extern "C" void func_801B5D88(CpuContext* ctx);
extern "C" void MH3_Em034Graze(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em034Graze", func_801B5D88, Em034GrazePort);
}
REGISTER_NATIVE_FUNCTION_AS(0x801B5D88, MH3_Em034Graze, "Em034Graze");

extern "C" void func_8015F948(CpuContext* ctx);
extern "C" void MH3_Em008Hold(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em008Hold", func_8015F948, Em008HoldPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8015F948, MH3_Em008Hold, "Em008Hold");

extern "C" void func_801B1AA4(CpuContext* ctx);
extern "C" void MH3_Em030Look(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em030Look", func_801B1AA4, Em030LookPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x801B1AA4, MH3_Em030Look, "Em030Look");

extern "C" void func_801B1878(CpuContext* ctx);
extern "C" void MH3_Em030Turn2(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em030Turn2", func_801B1878, Em030Turn2Port);
}
REGISTER_NATIVE_FUNCTION_AS(0x801B1878, MH3_Em030Turn2, "Em030Turn2");

extern "C" void func_8032D304(CpuContext* ctx);
extern "C" void MH3_Em026Hang(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em026Hang", func_8032D304, Em026HangPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8032D304, MH3_Em026Hang, "Em026Hang");

extern "C" void func_80155FD8(CpuContext* ctx);
extern "C" void MH3_Em003Stagger2(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em003Stagger2", func_80155FD8, Em003Stagger2Port);
}
REGISTER_NATIVE_FUNCTION_AS(0x80155FD8, MH3_Em003Stagger2, "Em003Stagger2");

extern "C" void func_801B7E98(CpuContext* ctx);
extern "C" void MH3_Em034Jump(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em034Jump", func_801B7E98, Em034JumpPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x801B7E98, MH3_Em034Jump, "Em034Jump");

extern "C" void func_801B82F4(CpuContext* ctx);
extern "C" void MH3_Em034LookUp(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em034LookUp", func_801B82F4, Em034LookUpPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x801B82F4, MH3_Em034LookUp, "Em034LookUp");

extern "C" void func_801783DC(CpuContext* ctx);
extern "C" void MH3_Em015Path(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em015Path", func_801783DC, Em015PathPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x801783DC, MH3_Em015Path, "Em015Path");

extern "C" void func_801849A0(CpuContext* ctx);
extern "C" void MH3_Em016Turn(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em016Turn", func_801849A0, Em016TurnPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x801849A0, MH3_Em016Turn, "Em016Turn");

extern "C" void func_8032E038(CpuContext* ctx);
extern "C" void MH3_Em026WaitNear(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em026WaitNear", func_8032E038, Em026WaitNearPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8032E038, MH3_Em026WaitNear, "Em026WaitNear");

extern "C" void func_801B7ACC(CpuContext* ctx);
extern "C" void MH3_Em034JumpTurn(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em034JumpTurn", func_801B7ACC, Em034JumpTurnPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x801B7ACC, MH3_Em034JumpTurn, "Em034JumpTurn");

extern "C" void func_8036E928(CpuContext* ctx);
extern "C" void MH3_Em020Path(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em020Path", func_8036E928, Em020PathPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8036E928, MH3_Em020Path, "Em020Path");

extern "C" void func_8032DA08(CpuContext* ctx);
extern "C" void MH3_Em026HoldNear(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em026HoldNear", func_8032DA08, Em026HoldNearPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8032DA08, MH3_Em026HoldNear, "Em026HoldNear");

extern "C" void func_8035C988(CpuContext* ctx);
extern "C" void MH3_Em033Turn(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em033Turn", func_8035C988, Em033TurnPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8035C988, MH3_Em033Turn, "Em033Turn");

extern "C" void func_8035C6E8(CpuContext* ctx);
extern "C" void MH3_Em033Rise(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em033Rise", func_8035C6E8, Em033RisePort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8035C6E8, MH3_Em033Rise, "Em033Rise");

extern "C" void func_8014F654(CpuContext* ctx);
extern "C" void MH3_Em001Rise(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em001Rise", func_8014F654, Em001RisePort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8014F654, MH3_Em001Rise, "Em001Rise");

extern "C" void func_801A0B34(CpuContext* ctx);
extern "C" void MH3_Em025Hold(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em025Hold", func_801A0B34, Em025HoldPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x801A0B34, MH3_Em025Hold, "Em025Hold");

extern "C" void func_801B1408(CpuContext* ctx);
extern "C" void MH3_Em030Nap(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em030Nap", func_801B1408, Em030NapPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x801B1408, MH3_Em030Nap, "Em030Nap");

extern "C" void func_801B1D58(CpuContext* ctx);
extern "C" void MH3_Em030Circle(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em030Circle", func_801B1D58, Em030CirclePort);
}
REGISTER_NATIVE_FUNCTION_AS(0x801B1D58, MH3_Em030Circle, "Em030Circle");

extern "C" void func_80350B34(CpuContext* ctx);
extern "C" void MH3_Em024Land(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em024Land", func_80350B34, Em024LandPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80350B34, MH3_Em024Land, "Em024Land");

extern "C" void func_80155C0C(CpuContext* ctx);
extern "C" void MH3_Em003Display(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em003Display", func_80155C0C, Em003DisplayPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80155C0C, MH3_Em003Display, "Em003Display");

extern "C" void func_801C67AC(CpuContext* ctx);
extern "C" void MH3_Em004Gas(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em004Gas", func_801C67AC, Em004GasPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x801C67AC, MH3_Em004Gas, "Em004Gas");

extern "C" void func_801C432C(CpuContext* ctx);
extern "C" void MH3_Em004Drop(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em004Drop", func_801C432C, Em004DropPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x801C432C, MH3_Em004Drop, "Em004Drop");

extern "C" void func_8032EA58(CpuContext* ctx);
extern "C" void MH3_Em026Latch(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em026Latch", func_8032EA58, Em026LatchPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8032EA58, MH3_Em026Latch, "Em026Latch");

extern "C" void func_80179660(CpuContext* ctx);
extern "C" void MH3_Em015Rest(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em015Rest", func_80179660, Em015RestPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80179660, MH3_Em015Rest, "Em015Rest");

extern "C" void func_801562BC(CpuContext* ctx);
extern "C" void MH3_Em003Stagger3(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em003Stagger3", func_801562BC, Em003Stagger3Port);
}
REGISTER_NATIVE_FUNCTION_AS(0x801562BC, MH3_Em003Stagger3, "Em003Stagger3");

extern "C" void func_801C56F0(CpuContext* ctx);
extern "C" void MH3_Em004Leap(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em004Leap", func_801C56F0, Em004LeapPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x801C56F0, MH3_Em004Leap, "Em004Leap");

extern "C" void func_8017E000(CpuContext* ctx);
extern "C" void MH3_Em015SwimTo(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em015SwimTo", func_8017E000, Em015SwimToPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8017E000, MH3_Em015SwimTo, "Em015SwimTo");

extern "C" void func_80199248(CpuContext* ctx);
extern "C" void MH3_Em018SwimTo(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em018SwimTo", func_80199248, Em018SwimToPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80199248, MH3_Em018SwimTo, "Em018SwimTo");

extern "C" void func_8035DB1C(CpuContext* ctx);
extern "C" void MH3_Em033Climb(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em033Climb", func_8035DB1C, Em033ClimbPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8035DB1C, MH3_Em033Climb, "Em033Climb");

extern "C" void func_801C72D8(CpuContext* ctx);
extern "C" void MH3_Em004Spin(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em004Spin", func_801C72D8, Em004SpinPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x801C72D8, MH3_Em004Spin, "Em004Spin");

extern "C" void func_80169528(CpuContext* ctx);
extern "C" void MH3_Em010Wander(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em010Wander", func_80169528, Em010WanderPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80169528, MH3_Em010Wander, "Em010Wander");

extern "C" void func_801B7FDC(CpuContext* ctx);
extern "C" void MH3_Em034Skip(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em034Skip", func_801B7FDC, Em034SkipPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x801B7FDC, MH3_Em034Skip, "Em034Skip");

extern "C" void func_801B8468(CpuContext* ctx);
extern "C" void MH3_Em034Skip2(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em034Skip2", func_801B8468, Em034Skip2Port);
}
REGISTER_NATIVE_FUNCTION_AS(0x801B8468, MH3_Em034Skip2, "Em034Skip2");

extern "C" void func_801B8884(CpuContext* ctx);
extern "C" void MH3_Em034Climb(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em034Climb", func_801B8884, Em034ClimbPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x801B8884, MH3_Em034Climb, "Em034Climb");

extern "C" void func_801B22FC(CpuContext* ctx);
extern "C" void MH3_Em030Face(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em030Face", func_801B22FC, Em030FacePort);
}
REGISTER_NATIVE_FUNCTION_AS(0x801B22FC, MH3_Em030Face, "Em030Face");

extern "C" void func_8032E5B0(CpuContext* ctx);
extern "C" void MH3_Em026Pounce(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em026Pounce", func_8032E5B0, Em026PouncePort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8032E5B0, MH3_Em026Pounce, "Em026Pounce");

extern "C" void func_8014C1D0(CpuContext* ctx);
extern "C" void MH3_Em001Turn(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em001Turn", func_8014C1D0, Em001TurnPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8014C1D0, MH3_Em001Turn, "Em001Turn");

extern "C" void func_80178534(CpuContext* ctx);
extern "C" void MH3_Em015Surface(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em015Surface", func_80178534, Em015SurfacePort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80178534, MH3_Em015Surface, "Em015Surface");

extern "C" void func_80178900(CpuContext* ctx);
extern "C" void MH3_Em015Circle(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em015Circle", func_80178900, Em015CirclePort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80178900, MH3_Em015Circle, "Em015Circle");

extern "C" void func_801DA578(CpuContext* ctx);
extern "C" void MH3_Em007Ledge(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em007Ledge", func_801DA578, Em007LedgePort);
}
REGISTER_NATIVE_FUNCTION_AS(0x801DA578, MH3_Em007Ledge, "Em007Ledge");

extern "C" void func_8032DD68(CpuContext* ctx);
extern "C" void MH3_Em026Ceiling(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em026Ceiling", func_8032DD68, Em026CeilingPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8032DD68, MH3_Em026Ceiling, "Em026Ceiling");

extern "C" void func_8014E7D8(CpuContext* ctx);
extern "C" void MH3_Em001Ledge(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em001Ledge", func_8014E7D8, Em001LedgePort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8014E7D8, MH3_Em001Ledge, "Em001Ledge");

extern "C" void func_801D2F34(CpuContext* ctx);
extern "C" void MH3_Em005Climb(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em005Climb", func_801D2F34, Em005ClimbPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x801D2F34, MH3_Em005Climb, "Em005Climb");

extern "C" void func_8014B730(CpuContext* ctx);
extern "C" void MH3_Em001Land(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em001Land", func_8014B730, Em001LandPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8014B730, MH3_Em001Land, "Em001Land");

extern "C" void func_801D1174(CpuContext* ctx);
extern "C" void MH3_Em005Pounce(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em005Pounce", func_801D1174, Em005PouncePort);
}
REGISTER_NATIVE_FUNCTION_AS(0x801D1174, MH3_Em005Pounce, "Em005Pounce");

extern "C" void func_8036EC58(CpuContext* ctx);
extern "C" void MH3_Em020Circle(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em020Circle", func_8036EC58, Em020CirclePort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8036EC58, MH3_Em020Circle, "Em020Circle");

extern "C" void func_8035CC3C(CpuContext* ctx);
extern "C" void MH3_Em033ClimbOut(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em033ClimbOut", func_8035CC3C, Em033ClimbOutPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8035CC3C, MH3_Em033ClimbOut, "Em033ClimbOut");

extern "C" void func_801A314C(CpuContext* ctx);
extern "C" void MH3_Em025Breach(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em025Breach", func_801A314C, Em025BreachPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x801A314C, MH3_Em025Breach, "Em025Breach");

extern "C" void func_8035F378(CpuContext* ctx);
extern "C" void MH3_Em035Wait(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em035Wait", func_8035F378, Em035WaitPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8035F378, MH3_Em035Wait, "Em035Wait");

extern "C" void func_8035F560(CpuContext* ctx);
extern "C" void MH3_Em035Hover(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em035Hover", func_8035F560, Em035HoverPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8035F560, MH3_Em035Hover, "Em035Hover");

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
