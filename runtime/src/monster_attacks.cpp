// Monster attacks with counts per call, RMHP08. C reconstructions (decompiled
// with Ghidra) of eight enemy actions that count their calls at +0x20 while a
// motion is before or past a frame, which the enemy update calls once per
// step:
//
//   0x8014BBA4, 0x8014C4C8  kinds 1/2 (Rathian, Rathalos) spits: a sound (0x801039C4 1)
//                           every 8 calls until the motion reaches frame 56
//   0x8017B7AC, 0x8017C6F4  em015 (Lagiacrus) discharges: a spark object
//                           (0x80306AFC 5) every 3 calls once the motion is
//                           past frame 65 / in [45, 150]
//   0x80188548              the Ludroth kinds' (em016/017/021) spit: the
//                           blend from motion channel 0 to 1 grows by 0.1 a
//                           call once the motion is past frame 114 / 174
//   0x8014DE50              Rathian's and Rathalos' dive (7.37): its moves
//                           spread over a count of calls (the watch pass of
//                           docs/monster-control.md found it)
//   0x8014CF5C              their flying charge (7.22-7.29): its give-up,
//                           braking and landing counted in calls
//   0x8038A81C              Uragaan's roll: its curves turned and its holds
//                           counted once a call
//
// At 120 steps a second the sounds and sparks came four times as often and
// the blend took a quarter of the time. The counts take the step's whole
// ticks; the sound and the spark come on the step whose ticks include a
// multiple of 8 or 3 (Em::Enemy::CountEvery), and the blend weight is the
// ticks plus the fraction of a tick past the last one, so it grows on every
// step at the Wii's rate. Their periodic tests of the global step counter
// (`system_w+0xC & 3`, hooked in the originals) go through StepCounter::Every.
//
// At the stock 1/30 s step these are the originals (MH3_PORT_CHECK=Em001Spit,
// ... MH3_VARIABLE_STEP=0; the Ludroth spit in the Royal Ludroth arena 9003,
// the others through the enemy fuzz's A: entries).
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
#include "step_counter.h"

#include <algorithm>
#include <cstdint>

namespace {

enum : uint32_t {
    kAngles = 0x1BC,      // s32[3]
    kArea = 0x1E1,        // u8: the area the monster is in
    kKind = 0x003,        // u8: the enemy kind
    kMotionFrame = 0x98,  // f32: motion channel 0's frame (MHchar +0x74; 0x8012F950, inlined)
    kChannelWeight = 0xF8, // f32[6]: the motion channels' blend weights (MHchar +0xD4; 0x800E2718, inlined)
    kSpitHold = 0x35A,    // u16: 300 after a spit
    kHold785 = 0x785,     // u8: 1 from frame 190 of em015's discharge (0x80133CA0, inlined)
    kVoiceFlags = 0xA0D,  // u8[n * 92]: cleared per voice channel (0x80129788, inlined)
    kVoiceFlags2 = 0xAEC, // u8[n]
};

enum : uint32_t {
    RoarVoice = 0x801293A0,      // (work, channel, voice, length)
    RoarVoice0 = 0x801296CC,     // (work, channel, voice): RoarVoice with length 0
    SetMotionSpeed = 0x8012F92C, // (work; f1)
    SetChannelMotion = 0x8012F7B0, // (work, channel, motion, flags, blend)
    Adjust8A0 = 0x80130D40,      // (work, s16 delta): 0x80130BD0 against +0x8A0
    TurnToward = 0x80133D2C,     // (work, 0x180, 0x280)
    TurnWhile = 0x80133EA0,      // (work, 0x8000; f1, f2 frames)
    TurnSetUp = 0x80134068,      // (work, 0; f1)
    TurnStep = 0x80134178,       // (work, 0, 0x40)
    Em001Prepare = 0x80154CD8,   // (work)
    Em016Prepare = 0x80182DA8,   // (work, s32)
    KeyData = 0x80052364,        // getKeyData (keys; f1 frame) -> f1
    Sound = 0x801039C4,          // (work, id)
    SoundAt = 0x80103A14,        // (work, id, u16, u16)
    Discharge = 0x8010A838,      // (work, kind): 0/7 and 1 start an effect, others nothing
    Spark = 0x80306AFC,          // (work, kind)
    SparkEffect = 0x8011D814,    // (kind, VEC3* at, angles*, u8 area, u8 enemy kind; f1 scale)
    ChestBurst = 0x80117EBC,     // (work, 2, VEC3*, 3; f1 scale)
    JointPosition = 0x8013599C,  // get_joint_wpos_em (work, joint, VEC3* out)
    ChangeScale = 0x801359B4,    // get_em_chg_scale (work) -> f1
    ShellParams = 0x801497EC,    // (params*): the shell parameters' constructor
    Em001ShellAim = 0x8015461C,  // (params*, 0, u16 angle, u16 angle2)
    Em016ShellAim = 0x8019216C,  // (params*, 0, u16 angle, u16 angle2)
    ActionDone = 0x80127FAC,     // (work): back to the AI
    ActionDone94 = 0x80128094,
};

constexpr uint32_t kShellTable = 0x80797020; // shell_set_func_ptr: the shell functions' table
constexpr uint32_t kShellThrow = 0x3C;       // (work, params*, kind)

void Throw(GuestCall& game, uint32_t work, uint32_t params, uint32_t kind) {
    game(Memory::Read32(Memory::Read32(kShellTable) + kShellThrow), {work, params, kind});
}

// The sound/effect at the spit's angles (the parameters' u16s +0x12, +0x14).
void SpitSound(GuestCall& game, uint32_t work, uint32_t params, uint32_t id) {
    game.Call<SoundAt>({work, id, Memory::Read16(params + 0x12), Memory::Read16(params + 0x14)});
}

// kinds 1/2's spits: the motion's speed from a table by its frame (kinds 3 and
// 1), the hiss (sound 1) every 8 ticks until frame 56.
void Em001Hiss(const Em::Enemy& em, GuestCall& game) {
    if (em.At(2.0f))
        game.Call<Sound>({em.work, 3});

    if (!em.At(56.0f, 1)) {
        if (em.CountEvery(8))
            game.Call<Sound>({em.work, 1});

        em.CountTicks();
    }
}

void Em001SpeedFromKeys(GuestCall& game, uint32_t work, uint32_t keys) {
    game.Call<KeyData>({keys}, {Memory::ReadFloat32(work + kMotionFrame)});
    game.Call<SetMotionSpeed>({work}); // f1: the key's speed
}

// kinds 1/2 0x8014BBA4 (r4: the spit's direction 0-2, or 3 the slow one).
void Em001SpitPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x30);
    GuestCall game{ctx};
    const Em::Enemy em{game, frame.Arg(3)};
    const uint32_t work = em.work, params = frame[0x08];
    const uint8_t kind = uint8_t(frame.Arg(4));
    game.Call<ShellParams>({params});

    if (em.State() == 0) {
        em.State(1);
        game.Call<Em::SetUp>({work, 0});
        em.Count(0);

        if (kind != 3)
            game.Call<Em001Prepare>({work});

        em.SetMotion(0x33, 6, 0);
        return;
    }

    if (em.State() != 1)
        return;

    if (kind == 3)
        Em001SpeedFromKeys(game, work, 0x805A2838);

    Em001Hiss(em, game);

    if (kind == 3) {
        if (em.At(78.0f)) {
            game.Call<Em001ShellAim>({params, 0, 0, 0});
            Throw(game, work, params, 1);
            game.Call<SoundAt>({work, 4, 0, 0});
        }
    } else {
        const uint32_t angle = kind == 1 ? 0xF8E4 : kind == 2 ? 0x71C : 0;

        if (em.At(78.0f)) {
            game.Call<Em001ShellAim>({params, 0, angle, 0});
            Throw(game, work, params, 0);
            SpitSound(game, work, params, 0);
            SpitSound(game, work, params, 2);
        }
    }

    if (em.Ended())
        game.Call<ActionDone>({work});
}

// kinds 1/2 0x8014C4C8 (r4: 1 the slow one): three spits at frames 78, 120 and
// 164.
void Em001Spit3Port(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x30);
    GuestCall game{ctx};
    const Em::Enemy em{game, frame.Arg(3)};
    const uint32_t work = em.work, params = frame[0x08];
    const uint8_t kind = uint8_t(frame.Arg(4));
    game.Call<ShellParams>({params});

    if (em.State() == 0) {
        em.State(1);
        game.Call<Em::SetUp>({work, 0});
        game.Call<Em001Prepare>({work});
        em.SetMotion(0xD0, 4, 0);
        return;
    }

    if (em.State() != 1)
        return;

    if (kind == 1)
        Em001SpeedFromKeys(game, work, 0x805A2860);

    Em001Hiss(em, game);

    const bool first = em.At(78.0f);

    if (first)
        game.Call<Em001ShellAim>({params, 0, 0x38E, 0});

    const bool second = em.At(120.0f);

    if (second)
        game.Call<Em001ShellAim>({params, 0, 0x38E, 0xF334});

    uint32_t spit = first || second ? 1 : 0;

    if (em.At(164.0f)) {
        game.Call<Em001ShellAim>({params, 0, 0x321, 0x105B});
        spit = 2;
    }

    if (spit != 0) {
        if (kind == 0) {
            Throw(game, work, params, 0);
            SpitSound(game, work, params, 0);

            if (spit == 2)
                SpitSound(game, work, params, 2);
        } else {
            Throw(game, work, params, 1);
            SpitSound(game, work, params, 4);
        }
    }

    if (em.Ended())
        game.Call<ActionDone>({work});
}

// 0x80129788 (work, channel), inlined: clears the voice channel's flags.
void ClearVoice(uint32_t work, uint32_t channel) {
    Memory::Write8(work + channel * 92 + kVoiceFlags, 0);
    Memory::Write8(work + channel + kVoiceFlags2, 0);
}

// em015 0x8017B7AC (r4: 1 charged): turns, and charged sparks every 3 ticks
// once the motion is past frame 65, then roars.
void Em015DischargePort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x10);
    GuestCall game{ctx};
    const Em::Enemy em{game, frame.Arg(3)};
    const uint32_t work = em.work;
    const bool charged = uint8_t(frame.Arg(4)) == 1;

    switch (em.State()) {
    case 0:
        em.State(1);
        game.Call<Em::SetUp>({work, 0});
        game.Call<Adjust8A0>({work, uint32_t(-5)});
        em.SetMotion(0x53, 10, 0);
        game.Call<TurnSetUp>({work, 0}, {0.0});
        em.Count(0);
        break;
    case 1:
        game.Call<TurnStep>({work, 0, 0x40});

        if (charged) {
            if (em.At(20.0f))
                game.Call<Discharge>({work, 0});

            // The sparks: every 3 ticks once the motion is past frame 65.
            if (em.At(65.0f, 1)) {
                if (em.CountEvery(3))
                    game.Call<Spark>({work, 5});

                game.Call<Discharge>({work, 5});
                em.CountTicks();
            }
        }

        if (em.At(78.0f)) {
            game.Call<RoarVoice>({work, 0, 8, 0xB});

            if (charged)
                game.Call<RoarVoice>({work, 1, 0x1E, 0x1B});
        }

        if (em.Ended()) {
            em.NextState();
            Memory::Write8(work + Em::kSubState, 0);
            em.SetMotion(0x55, 6, 0);
        }

        break;
    case 2:
        if (Memory::Read8(work + Em::kSubState) == 0 && em.At(14.0f, 1)) {
            Memory::Write8(work + Em::kSubState, uint8_t(Memory::Read8(work + Em::kSubState) + 1));
            ClearVoice(work, 0);
            ClearVoice(work, 1);
            game.Call<RoarVoice>({work, 0, 0x26, 0x10});
        }

        if (em.Ended())
            game.Call<ActionDone>({work});

        break;
    }
}

// em015 0x8017C6F4 (r4: 0 a chest burst at frame 70, 1 charged: sparks
// every 3 ticks while the motion is in [45, 150]).
void Em015ChargePort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x30);
    GuestCall game{ctx};
    const Em::Enemy em{game, frame.Arg(3)};
    const uint32_t work = em.work, at = frame[0x08];
    const uint8_t kind = uint8_t(frame.Arg(4));

    if (em.State() == 0) {
        em.State(1);
        game.Call<Em::SetUp>({work, 2});
        game.Call<Adjust8A0>({work, uint32_t(-10)});
        em.SetMotion(0x44, 4, 0);
        game.Call<RoarVoice>({work, 0, 0x16, 8});
        game.Call<RoarVoice>({work, 1, kind == 0 ? 0x17u : 0x1Du, 0x10});
        em.Count(0);
        return;
    }

    if (em.State() != 1)
        return;

    if (em.At(30.0f, 2))
        game.Call<TurnToward>({work, 0x180, 0x280});

    if (em.At(190.0f, 2))
        Memory::Write8(work + kHold785, 1);

    game.Call<TurnWhile>({work, 0x8000}, {190.0, 240.0});

    if (em.Ended())
        game.Call<ActionDone94>({work});

    if (kind == 0) {
        if (em.At(70.0f)) {
            game.Call<ChangeScale>({work});
            const float scale = game.F1();
            Memory::WriteFloat32(at, 0.0f);
            Memory::WriteFloat32(at + 4, 200.0f);
            Memory::WriteFloat32(at + 8, -1200.0f * scale);
            game.Call<ChestBurst>({work, 2, at, 3}, {1.0});
        }

        return;
    }

    if (em.At(2.0f))
        game.Call<Discharge>({work, 0});

    // The sparks: every 3 ticks while the motion is in [45, 150].
    if (em.At(45.0f, 3, 150.0f)) {
        if (em.CountEvery(3)) {
            game.Call<Spark>({work, 5});
            game.Call<JointPosition>({work, 3, at});
            game.Call<SparkEffect>({6, at, work + kAngles, Memory::Read8(work + kArea), Memory::Read8(work + kKind)}, {1.0});
        }

        game.Call<Discharge>({work, 5});
        em.CountTicks();
    }
}

// The Ludroth kinds' spit (0x80188548, r4: 1 or 2 the variants): two motions
// blended by +0x20 / 10, two spits per state.
void LudrothSpitPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x40);
    GuestCall game{ctx};
    const Em::Enemy em{game, frame.Arg(3)};
    const uint32_t work = em.work, params = frame[0x08];
    const uint8_t kind = uint8_t(frame.Arg(4));
    game.Call<ShellParams>({params});

    const auto spit = [&](uint32_t angle, uint32_t angle2) {
        game.Call<Em016ShellAim>({params, 0, angle, angle2});
        SpitSound(game, work, params, 0xE);
        Throw(game, work, params, 5);
        Memory::Write16(work + kSpitHold, 300);
    };

    // The blend from channel 0's motion to channel 1's: 0.1 a tick past the
    // frame, here with the fraction of a tick so it grows on every step.
    const auto blend = [&](float frameReached) {
        if (em.At(frameReached, 1))
            em.CountTicks();

        const float ticks = em.Count() > 0 ? float(double(em.Count()) + GameStep::Fraction()) : float(em.Count());
        const float weight = std::clamp(0.1f * ticks, 0.0f, 1.0f);
        Memory::WriteFloat32(work + kChannelWeight, 1.0f - weight);
        Memory::WriteFloat32(work + kChannelWeight + 4, weight);
    };

    switch (em.State()) {
    case 0: {
        em.State(1);
        game.Call<Em::SetUp>({work, 0});
        const uint32_t first = kind == 1 ? 99 : kind == 2 ? 0x56 : 0x55;
        const uint32_t second = kind == 1 || kind == 2 ? 0x55 : 99;
        em.SetMotion(first, 10, 0);
        game.Call<SetChannelMotion>({work, 1, second, 10, 0});
        game.Call<Em016Prepare>({work, uint32_t(-15)});
        em.Count(0);
        break;
    }
    case 1:
        if (em.At(12.0f, 3, 70.0f) && StepCounter::Every(4))
            game.Call<Sound>({work, 0xD});

        if (em.At(82.0f)) {
            if (kind == 1)
                spit(0, 0);
            else if (kind == 2)
                spit(0x800, 0xE000);
            else
                spit(0, 0x1800);
        }

        if (em.At(142.0f)) {
            if (kind == 1)
                spit(64000, 0x1600);
            else if (kind == 2)
                spit(0, 0x1800);
            else
                spit(64000, 0x400);
        }

        blend(114.0f);

        if (em.At(150.0f)) {
            em.NextState();
            const uint32_t first = kind == 1 || kind == 2 ? 0x55 : 99;
            const uint32_t second = kind == 2 ? 99 : 0x56;
            em.SetMotion(first, 0, 0x97);
            game.Call<SetChannelMotion>({work, 1, second, 0, 0x97});
            game.Call<RoarVoice0>({work, 0, 0x20});
            em.Count(0);
        }

        break;
    case 2:
        if (em.At(202.0f)) {
            if (kind == 1)
                spit(0x400, 0xE000);
            else if (kind == 2)
                spit(0, 0);
            else
                spit(0, 0xE400);
        }

        blend(174.0f);

        if (em.Ended())
            game.Call<ActionDone>({work});

        break;
    }
}

// kind 1/2 0x8014DE50: Rathian's and Rathalos' dive (motion 0xDB). It
// steers at the target before frame 110; over frames 60-100 it sets the
// sinking speed (+0x314, the target's height left, at most 0) and the
// forward speed (+0x318, the distance left less 100 × its size, at least
// 30) to what is left over the count (+0x20: 40 frames over the motion's
// rate, so the ticks the window lasts) and moves by them (0x801355E8); from
// frame 150 it flies back to where it started (+0x1B0) over the ticks to
// frame 200, facing its start yaw (+0x32C). The original counts the calls,
// so at short steps it covered the same way in fewer ticks: here the count
// goes down by the step's ticks (the moves are taken over the step's
// length: shell_motion.cpp).
void Em001DivePort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x40);
    GuestCall game{ctx};
    const Em::Enemy em{game, frame.Arg(3)};
    const uint32_t work = em.work, diff = frame[0x08], step = frame[0x14];
    const auto countDown = [&] { em.Count(em.Count() - int32_t(GameStep::Ticks())); };

    switch (em.State()) {
    case 0:
        em.State(1);
        Memory::Write8(work + 6, 0);
        game.Call<0x801302AC>({work}); // f1: the carried scale, for SetScale
        game.Call<0x80130628>({work});
        game.Call<Em001Prepare>({work});
        em.SetMotion(0xDB, 6, 0);
        game.Call<RoarVoice0>({work, 0, 0x1F});
        game.Call<0x80041EA4>({work + 0x1B0, work + 0x188}); // the start
        Memory::Write16(work + 0x32C, uint16_t(Memory::Read32(work + kAngles + 4)));
        game.Call<0x8013545C>({work});
        game.Call<0x8012F948>({work});
        em.Count(int16_t(int32_t(40.0f / game.F1())));
        break;
    case 1:
        game.Call<0x8012CF84>({work});
        game.Call<0x80128C5C>({work, 0});

        if (em.At(110.0f, 2))
            game.Call<0x80133CB4>({work, 0x300});

        if (em.At(60.0f, 3, 100.0f)) {
            if (em.Count() > 0) {
                const float count = float(em.Count());
                Memory::WriteFloat32(work + 0x310, 0.0f);
                game.Call<ChangeScale>({work});
                const float size = game.F1();
                game.Call<0x801302AC>({work});
                const float height = (game.F1() - 100.0f) * size;
                const float sink = (Memory::ReadFloat32(work + 0x370) - Memory::ReadFloat32(work + 0x18C) + height) / count;
                Memory::WriteFloat32(work + 0x314, sink > 0.0f ? 0.0f : sink);
                const uint32_t angle = game.Call<0x80050AF4>({work + 0x188, work + 0x36C}) & 0xFFFF;

                if (((angle - Memory::Read32(work + kAngles + 4) + 0x4000) & 0xFFFF) < 0x8000) {
                    game.Call<ChangeScale>({work});
                    const float near = 100.0f * game.F1();
                    game.Call<0x80050FE4>({work + 0x36C, work + 0x188});
                    const float forward = (game.F1() - near) / count;
                    Memory::WriteFloat32(work + 0x318, forward <= 30.0f ? 30.0f : forward);
                } else
                    Memory::WriteFloat32(work + 0x318, 30.0f);

                game.Call<0x801302AC>({work}); // f1 passes on
                game.Call<0x801355E8>({work, work + kAngles});
            }

            countDown();
        }

        if (em.At(150.0f, 1)) {
            em.NextState();
            game.Call<0x8012F948>({work});
            const float rate = game.F1();
            game.Call<0x8012F950>({work});
            em.Count(int16_t(int32_t((200.0f - game.F1()) / rate)));
        }

        break;
    case 2: {
        const uint32_t yaw = game.Call<0x80133E14>({Memory::Read16(work + 0x32C), Memory::Read32(work + kAngles + 4) & 0xFFFF, 0x300});
        Memory::Write32(work + kAngles + 4, yaw & 0xFFFF);
        game.Call<0x80135448>({work});

        if (const int32_t count = em.Count(); count > 0) {
            game.Call<0x80050D04>({diff, work + 0x1B0, work + 0x188});
            game.Call<0x800ADA24>({step, diff}, {double(float(count))});
            game.Call<0x80041EA4>({work + 0x310, step});
            game.Call<0x8013547C>({work});
        }

        countDown();

        if (em.Ended())
            game.Call<0x80128048>({work});

        break;
    }
    default:
        break;
    }
}

// kind 1/2 0x8014CF5C (r4 how it lands 0-2, r5 its climb 0-3): Rathian's and
// Rathalos' flying charge (7.22-7.29). State 1 climbs (pitch toward 0x2800,
// 0x2000, 0x1800 or the target's, at most 0x3000) and steers; state 2 flies
// at the target until it passes it (0x80135664 with the height less 200),
// giving up after 150 ticks; states 3-4 brake for 7 ticks; state 5 lands,
// the hover (+0x1AC through 0x80130450) ramping back from -150 over 20 ticks
// or -160..200 over 10. The original counts each of these in calls, so at
// short steps it gave up, braked and landed early: here the counts take the
// step's ticks (the moves through 0x80135558/0x8013562C are scaled by the
// step already: shell_motion.cpp).
void Em001FlyChargePort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x40);
    GuestCall game{ctx};
    const Em::Enemy em{game, frame.Arg(3)};
    const uint32_t work = em.work, landing = uint8_t(frame.Arg(4)), climb = uint8_t(frame.Arg(5));
    const uint32_t angles = work + kAngles, pitch = frame[0x0C], roll = frame[0x08], toTarget = frame[0x10], vector = frame[0x1C];
    const int32_t ticks = int32_t(GameStep::Ticks());
    const auto hover = [&](float value) { game.Call<0x80130450>({work}, {double(value)}); };
    const auto fly = [&] {
        game.Call<0x801302AC>({work});
        return game.Call<0x80135664>({work, angles}, {double(game.F1() - 200.0f)});
    };

    game.Call<0x80043F0C>({vector});
    game.Call<0x8012CF84>({work});

    switch (em.State()) {
    case 0:
        em.State(1);
        game.Call<0x801302AC>({work}); // f1: the carried scale, for SetScale
        game.Call<0x80130628>({work});
        game.Call<Em001Prepare>({work});
        em.SetMotion(0x45, 6, 0);
        Memory::Write8(work + 6, 0);
        game.Call<0x8013545C>({work});
        Memory::WriteFloat32(work + 0x314, -2.0f);
        Memory::WriteFloat32(work + 0x318, 50.0f);
        break;
    case 1: {
        game.Call<0x80133CA0>({work});
        game.Call<0x80136DB0>({work}, {5.0});
        uint32_t goal = climb == 0 ? 0x2800 : climb == 1 ? 0x2000 : 0x1800;

        if (climb > 2) {
            game.Call<0x80050D04>({toTarget, work + 0x36C, work + 0x188});
            game.Call<0x80041EA4>({vector, toTarget});
            game.Call<0x80050D7C>({vector, pitch, roll}); // calcVecAngXY
            goal = std::min<uint32_t>(Memory::Read32(pitch), 0x3000);
        }

        Memory::Write32(angles, game.Call<0x80133E14>({goal & 0xFFFF, Memory::Read32(angles) & 0xFFFF, 0x180}) & 0xFFFF);
        game.Call<0x80133CB4>({work, 0x280});
        fly();

        if (Memory::ReadFloat32(work + 0x318) > 120.0f)
            Memory::WriteFloat32(work + 0x318, 120.0f);

        if (em.Ended()) {
            em.NextState();
            em.SetMotion(0x3E, 6, 0);
            em.Count(0);
            game.Call<RoarVoice>({work, 0, 0xE, 3});
            Memory::WriteFloat32(work + 0x324, 3.0f);
        }

        break;
    }
    case 2:
        game.Call<0x80133CA0>({work});
        em.Count(em.Count() + ticks);

        if (fly() == 1) {
            em.NextState();
            game.Call<Em::SetUp>({work, 3});
            em.SetMotion(0x3F, 6, 0);
            hover(-200.0f);
        } else if (em.Count() > 0x96 && Memory::Read8(work + 0x1F5) == 1)
            game.Call<0x80128048>({work});

        if (Memory::ReadFloat32(work + 0x318) > 110.0f)
            Memory::WriteFloat32(work + 0x318, 110.0f);

        break;
    case 3:
        hover(-200.0f);
        game.Call<0x80135558>({work, angles});

        if (em.Ended()) {
            em.NextState();
            em.SetMotion(0xD0, 0, 0);
            em.Count(0);
        }

        break;
    case 4:
        hover(-200.0f);
        game.Call<0x80135558>({work, angles});
        em.Count(em.Count() + ticks);

        if (em.Count() > 6) {
            em.NextState();
            Memory::Write8(work + 6, 0);

            if (landing == 1) {
                game.Call<Em::SetUp>({work, 0});
                em.SetMotion(0xDD, 0, 0);
                hover(0.0f);
            } else if (landing == 2)
                em.SetMotion(0x35, 10, 0);
            else {
                em.SetMotion(0xD2, 6, 0);
                Memory::WriteFloat32(work + 0x324, -3.5f);
                em.Count(0);
            }
        }

        break;
    case 5:
        if (landing == 1) {
            if (Memory::Read8(work + 6) == 0) {
                if (em.Ended()) {
                    Memory::Write8(work + 6, 1);
                    em.SetMotion(0xDA, 0, 0);
                }
            } else if (Memory::Read8(work + 6) == 1) {
                if (em.At(30.0f))
                    game.Call<0x80129788>({work, 0});

                if (em.Ended())
                    game.Call<ActionDone>({work});
            }
        } else if (landing == 2) {
            if (Memory::Read8(work + 6) == 0) {
                if (em.Count() < 10) {
                    em.Count(std::min(em.Count() + ticks, 10));
                    hover(-200.0f * float(5 - em.Count()) / 5.0f);
                }

                if (em.Ended()) {
                    Memory::Write8(work + 6, 1);
                    game.Call<0x80129788>({work, 0});
                    game.Call<Em::SetUp>({work, 0});
                    em.SetMotion(0x3C, 0, 0);
                }
            } else if (Memory::Read8(work + 6) == 1 && em.Ended())
                game.Call<ActionDone>({work});
        } else {
            if (em.Count() < 20) {
                em.Count(std::min(em.Count() + ticks, 20));
                hover(-150.0f * float(20 - em.Count()) / 20.0f);
            }

            if (em.At(30.0f))
                game.Call<0x80129788>({work, 0});

            game.Call<0x8013562C>({work, angles});

            if (Memory::ReadFloat32(work + 0x318) < 0.0f)
                Memory::WriteFloat32(work + 0x318, 0.0f);

            if (em.Ended())
                game.Call<0x80128110>({work});
        }

        break;
    default:
        break;
    }
}

// kind 9 0x8038A81C (r4 the roll 0-23, r5 how it ends): Uragaan's roll.
// State 0 sets up the way (0x80134068: 500, 0, -300 or -500 by roll, -800
// for 23, less 1500 when r5 is 1; rolls 10-22 follow the route table
// 0x805F04A4 through 0x801262DC); state 1 curls up (turning by 2.15 degrees
// times 0.5 or 0.83 times the motion's advance for rolls 2-5, TurnWhile for
// 8-9) and from frame 164 starts rolling (+0x318 23.6, +0x324 4 times the
// motion's rate and its size); state 2 rolls (0x8013562C, the speed at most
// 150 or 95), its magma every 4 ticks from frame 168, curving 0x5B or 0x98
// a tick (2-5), or by 6 or 4 degrees times the speed's share of its limit a
// tick until a three-quarter, half or two-third turn is done (6-9, with holds
// of 15 and 5 ticks), or along the route (10-22); states 3-5 stop. The
// original turned by its amounts and counted its holds once a call, so at 60
// or 120 steps a second the roll curved two or four times as fast: here the
// turns take the step's share and the holds the step's ticks (its moves and
// its tests of the step counter already were).
void Em009RollPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x50);
    GuestCall game{ctx};
    const Em::Enemy em{game, frame.Arg(3)};
    const uint32_t work = em.work, at = frame[0x08];
    const uint8_t roll = uint8_t(frame.Arg(4)), ending = uint8_t(frame.Arg(5));
    const uint32_t route = uint32_t(roll) - 10; // rolls 10-22: 0-12
    const float s = float(GameStep::Scale());
    const int32_t ticks = int32_t(GameStep::Ticks());
    constexpr uint32_t kRouteTable = 0x805F04A4, kRate = 0x80, kAcc = 0x328, kYaw = kAngles + 4;
    float limit = 95.0f;
    bool done = false;

    const auto yaw = [&](int32_t delta) { Memory::Write32(work + kYaw, Memory::Read32(work + kYaw) + uint32_t(delta)); };
    const auto rate = [&] { return Memory::ReadFloat32(work + kRate); };

    // The original's per-call turn, 0.5 + 65536 * 2.1538 * (k * rate) / 360
    // in whole units, over the step's advance (rate * s) instead of the rate.
    const auto curl = [&](float k) {
        const float advance = rate() * s;
        return int32_t(int32_t(0.5f + 65536.0f * (2.1538463f * (k * advance)) / 360.0f) & 0xFFFF);
    };

    // The per-call turn by the speed's share of the limit, k degrees.
    const auto spin = [&](float k) {
        const float share = Memory::ReadFloat32(work + 0x318) / limit;
        return int32_t(int32_t(0.5f + (65536.0f * (k * share) / 360.0f) * s) & 0xFFFF);
    };

    const auto nextNode = [&] {
        const uint8_t node = Memory::Read8(kRouteTable + route * 12 + Memory::Read8(work + 6));
        return node;
    };

    const auto toNode = [&](uint8_t node) {
        game.Call<0x801262DC>({work, uint32_t((Memory::Read8(work + 0x1E1) & 0xF) << 8 | node) & 0xFFFF, work + 0x36C});
        game.Call<0x80134068>({work, 0x10}, {0.0});
        Memory::Write8(work + 6, uint8_t(Memory::Read8(work + 6) + 1));
    };

    const auto magma = [&](uint32_t joint, uint32_t effect, uint32_t spawn, float z) {
        if (game.Call<0x80130180>({work}) == 1) { // em_magma_check
            if (joint)
                game.Call<JointPosition>({work, joint, at});
            else
                game.Call<0x80041EA4>({at, work + 0x188});

            Memory::WriteFloat32(at + 4, Memory::ReadFloat32(work + 0x214));
            game.Call<0x801049B0>({effect, at, work + kAngles, Memory::Read8(work + 0x1E1)}, {1.0}); // eft009_set_pos
        } else {
            game.Call<Em::Vec3Set>({at}, {0.0, 0.0, double(z)});
            game.Call<Em::Spawn>({work, spawn, joint ? joint : 1, at}, {1.0});

            if (!joint)
                game.Call<Em::Spawn>({work, 0x82, 1, at}, {1.0});
        }
    };

    game.Call<0x80043F0C>({at});
    game.Call<0x8012CF84>({work});

    if (route <= 0xC)
        game.Call<0x80131ED8>({work});

    switch (em.State()) {
    case 0: {
        em.State(1);
        Memory::Write8(work + 6, 0);
        em.Count(0);
        game.Call<Em::SetUp>({work, 0});
        em.SetMotion(0x17, 6, 0);
        game.Call<SetMotionSpeed>({work}, {1.3f});
        game.Call<RoarVoice>({work, 0, 0x15, 8});
        game.Call<0x80130D40>({work, uint32_t(-20)});
        const float extra = ending == 1 ? -1500.0f : 0.0f;
        const auto way = [&](float distance) { game.Call<0x80134068>({work, 0x10}, {double(distance)}); };

        if (roll < 10) {
            if (roll >= 8)
                way(0.0f);
            else if (roll >= 6)
                way(500.0f);
            else if (roll >= 2)
                way(-300.0f + extra);
            else
                way(-500.0f + extra);
        } else if (roll == 23)
            way(-800.0f + extra);
        else if (roll > 23)
            way(-500.0f + extra);
        else {
            game.Call<0x80131DE8>({work});
            game.Call<0x80131E58>({work});
            Memory::Write16(work + 0x338, 0);
            toNode(nextNode());
        }

        break;
    }
    case 1:
        if (roll == 6 || roll == 7) {
            if (em.At(30.0f, 1))
                game.Call<SetMotionSpeed>({work}, {0.8f});

            if (em.At(92.0f, 1))
                game.Call<SetMotionSpeed>({work}, {1.3f});
        }

        switch (roll) {
        case 2:
        case 3:
        case 4:
        case 5:
            if (em.At(90.0f, 1)) {
                const int32_t turn = curl(roll < 4 ? 0.5f : 0.8333333f);
                yaw(roll & 1 ? -turn : turn);
            }

            game.Call<TurnStep>({work, 0, 0});
            break;
        case 8:
        case 9:
            game.Call<TurnWhile>({work, roll == 8 ? uint32_t(-0x1555) : 0x1555u}, {90.0, 110.0});
            game.Call<TurnStep>({work, 0, 0});
            break;
        default:
            if (route <= 0xC) {
                game.Call<0x80131DE8>({work});
                game.Call<0x80131E58>({work});
                game.Call<TurnStep>({work, 0, em.At(90.0f, 1) ? 0x200u : 0u});
            } else
                game.Call<TurnStep>({work, 0, em.At(90.0f, 1) ? 0x100u : 0u});

            break;
        }

        if (em.At(120.0f))
            game.Call<RoarVoice>({work, 0, 6, 27});

        if (em.At(164.0f, 1)) {
            em.NextState();

            for (uint32_t i = 0; i < 6; ++i)
                Memory::WriteFloat32(work + 0x310 + 4 * i, 0.0f);

            game.Call<ChangeScale>({work});
            Memory::WriteFloat32(work + 0x318, 23.6f * rate() * game.F1());
            game.Call<ChangeScale>({work});
            Memory::WriteFloat32(work + 0x324, 4.0f * rate() * game.F1());
            game.Call<RoarVoice>({work, 0, 24, 27});
        }

        break;
    case 2: {
        if (route <= 13 || roll <= 5)
            limit = 150.0f;
        else if (roll - 6u <= 3)
            limit = 95.0f;

        game.Call<0x8013562C>({work, work + kAngles});

        if (Memory::ReadFloat32(work + 0x318) > limit)
            Memory::WriteFloat32(work + 0x318, limit);
        else
            game.Call<SetMotionSpeed>({work}, {Memory::ReadFloat32(work + 0x7C4) + 0.01f * rate()});

        if (em.At(168.0f, 1) && StepCounter::Every(4))
            magma(0, 0x8B, 0x44, 150.0f);

        const auto ends = [&] { return em.At(166.0f, 1); };

        switch (roll) {
        case 0:
            game.Call<0x80133CB4>({work, 0x100});
            game.Call<TurnStep>({work, 0, 0});
            break;
        case 2:
        case 3:
        case 4:
        case 5: {
            const int32_t turn = GameStep::Share(roll < 4 ? 0x5B : 0x98);
            yaw(roll & 1 ? -turn : turn);
            done = game.Call<TurnStep>({work, 0, 0}) == 1 && ends();
            break;
        }
        case 6:
        case 7:
            if (Memory::Read8(work + 6) == 0) {
                if (game.Call<TurnStep>({work, 0, 0}) == 1) {
                    Memory::Write8(work + 6, 1);
                    Memory::Write32(work + kAcc, 0);
                }
            } else if (Memory::Read8(work + 6) == 1) {
                em.Count(em.Count() + ticks);
                const int32_t turn = spin(6.0f);
                yaw(roll == 6 ? turn : -turn);
                Memory::Write32(work + kAcc, Memory::Read32(work + kAcc) + uint32_t(turn));

                if (Memory::Read32(work + kAcc) >= 0xC000) {
                    Memory::Write8(work + 6, 2);
                    em.Count(0);
                }
            } else if (Memory::Read8(work + 6) == 2) {
                em.Count(em.Count() + ticks);
                done = em.Count() >= 15 && ends();
            }

            break;
        case 8:
        case 9: {
            const uint8_t sub = Memory::Read8(work + 6);

            if (sub == 0) {
                em.Count(em.Count() + ticks);

                if (em.Count() >= 0) {
                    Memory::Write8(work + 6, 1);
                    em.Count(0);
                    Memory::Write32(work + kAcc, 0);
                }
            } else if (sub == 1 || sub == 2) {
                em.Count(em.Count() + ticks);
                const int32_t turn = spin(sub == 1 ? 4.0f : 6.0f);
                yaw(roll == 8 ? turn : -turn);
                Memory::Write32(work + kAcc, Memory::Read32(work + kAcc) + uint32_t(turn));

                if (Memory::Read32(work + kAcc) >= (sub == 1 ? 0x8000u : 0xAAABu)) {
                    Memory::Write8(work + 6, uint8_t(sub + 1));
                    em.Count(0);

                    if (sub == 1)
                        Memory::Write32(work + kAcc, 0);
                }
            } else if (sub == 3) {
                em.Count(em.Count() + ticks);
                done = em.Count() >= 5 && ends();
            }

            break;
        }
        default:
            if (route <= 0xC) {
                game.Call<0x80131DE8>({work});
                game.Call<0x80131E58>({work});

                if (game.Call<TurnStep>({work, 0, 0x400}) == 1) {
                    if (const uint8_t node = nextNode(); node == 0xFF)
                        done = true;
                    else
                        toNode(node);
                }
            } else
                done = game.Call<TurnStep>({work, 0, 0x80}) == 1 && ends();

            break;
        }

        if (done) {
            if (ending == 0) {
                em.State(3);
                em.SetMotion(0x18, 0, 0);
                game.Call<RoarVoice>({work, 1, 14, 24});
            } else {
                em.State(4);
                em.SetMotion(0x19, 0, 0);
                game.Call<RoarVoice>({work, 0, 23, 24});
            }

            Memory::WriteFloat32(work + 0x318, Memory::ReadFloat32(work + 0x318) * 0.1f);
            Memory::WriteFloat32(work + 0x31C, 0.0f);
            Memory::WriteFloat32(work + 0x320, 0.0f);
            Memory::WriteFloat32(work + 0x324, -2.0f);
        }

        break;
    }
    case 3:
        if (em.At(40.0f, 3, 52.0f) && StepCounter::Every(2))
            magma(0x23, 0x8C, 0x45, 0.0f);

        if (em.At(72.0f, 3, 84.0f) && StepCounter::Every(2))
            magma(0x23, 0x8C, 0x45, 0.0f);

        if (em.At(24.0f))
            game.Call<RoarVoice>({work, 0, 0x12, 0x10});

        game.Call<0x8013562C>({work, work + kAngles});

        if (Memory::ReadFloat32(work + 0x318) < 0.0f)
            Memory::WriteFloat32(work + 0x318, 0.0f);

        if (em.Ended())
            game.Call<ActionDone>({work});

        break;
    case 4:
        if (em.At(108.0f))
            game.Call<0x80129788>({work, 0});

        game.Call<0x8013562C>({work, work + kAngles});

        if (Memory::ReadFloat32(work + 0x318) < 0.0f)
            Memory::WriteFloat32(work + 0x318, 0.0f);

        if (em.Ended()) {
            em.NextState();
            em.SetMotion(0x71, 0, 0);
        }

        break;
    case 5:
        if (em.Ended())
            game.Call<ActionDone>({work});

        break;
    default:
        break;
    }
}

// kind 15 0x8017AD0C (r4 0 / 1): Lagiacrus' storm (motion 0xD0, +0x8A0 less
// 10). Its count rises each tick; a discharge (0x8010A838) at frame 20 and
// in frames 220-400 each step, a spark (0x80306AFC 1) every 5 ticks there;
// glows (0x8010A930) at 40, 80 and 540, bursts (0x80105690 10 at 0, 210, -30
// in joint 3) at 80, 90 and 100, the shell 0x50 (work, 1 or 0, 30) at 220, a
// voice from 230 to 400; ActionDone94 at its end. The original counted
// calls, so at 60 or 120 steps a second its sparks came two or four times as
// often; here every 5 ticks of game time.
void Em015StormPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x60);
    GuestCall game{ctx};
    const Em::Enemy em{game, frame.Arg(3)};
    const uint32_t work = em.work, at = frame[0x08], joint = frame[0x18];
    const uint8_t how = uint8_t(frame.Arg(4));
    const uint32_t ticks = GameStep::Ticks();

    const auto glow = [&](uint32_t kind, double scale) { game.Call<0x8010A930>({work, kind}, {scale}); };

    const auto burst = [&](double scale) {
        game.Call<Em::Vec3Set>({at}, {0.0, 210.0, -30.0});
        game.Call<0x80135994>({work, 3, joint}); // get_joint_wmat_em
        game.Call<0x800514A4>({at, joint});      // mulVecMat

        for (uint32_t i = 0; i < 3; ++i)
            Memory::WriteFloat32(at + 4 * i, Memory::ReadFloat32(at + 4 * i) + Memory::ReadFloat32(joint + 0x10 * i + 0xC));

        game.Call<0x80105690>({work, 10, 3, at}, {scale});
    };

    const auto shell = [&](uint32_t side) { game(Memory::Read32(Memory::Read32(kShellTable) + 0x50), {work, side, 0x1E}); };

    game.Call<0x80050570>({joint});

    switch (em.State()) {
    case 0:
        em.State(1);
        game.Call<Em::SetUp>({work, 2});
        game.Call<Adjust8A0>({work, uint32_t(-10)});
        game.Call<0x8012F568>({work, 0xD0, 0x14, 0, 1});
        Memory::Write8(work + Em::kSubState, 0);
        em.Count(0);
        break;
    case 1:
        em.CountTicks();

        if (em.At(20.0f))
            game.Call<Discharge>({work, 0});

        if (em.At(220.0f, 3, 400.0f)) {
            if (Em::AnyOf(em.Count() - int32_t(ticks) + 1, ticks, 5, 0))
                game.Call<Spark>({work, 1});

            game.Call<Discharge>({work, 5});
        }

        if (how == 0) {
            if (em.At(40.0f))
                glow(3, 1.0);

            if (em.At(80.0f))
                glow(6, 1.0);

            if (em.At(80.0f) || em.At(90.0f) || em.At(100.0f))
                burst(2.0);

            if (em.At(220.0f))
                shell(1);

            if (em.At(540.0f))
                glow(6, 1.0);
        } else if (how == 1) {
            if (em.At(80.0f) || em.At(90.0f) || em.At(100.0f))
                burst(em.At(90.0f, 1) ? 1.5 : 2.0);

            if (em.At(220.0f))
                shell(0);

            if (em.At(540.0f))
                glow(6, double(0.6f));
        }

        if (Memory::Read8(work + Em::kSubState) == 0) {
            if (em.At(230.0f)) {
                Memory::Write8(work + Em::kSubState, 1);
                game.Call<RoarVoice>({work, 0, 10, 3});
            }
        } else if (Memory::Read8(work + Em::kSubState) == 1 && em.At(400.0f)) {
            Memory::Write8(work + Em::kSubState, 2);
            game.Call<0x80129788>({work, 0});
        }

        if (em.Ended())
            game.Call<ActionDone94>({work});

        break;
    default:
        break;
    }
}

// em024 0x80350DD8 (r4 0: with its effects; r5 1: turning toward -2300, 2-5:
// spinning 0x100 / 0x180 a call left / right): Alatreon's charge-up and
// turn. Motion 0x11 (a voice; +0x378 at most 2500 for the spins) with sparks
// at frames 4-46 and from frame 60 a glow every 8 steps and a spark
// (0x80306AFC 10) every 6 ticks; from frame 40 it spins, from 90 it turns
// until done (0x14, then its end; ActionDone) or 0x8034F2AC breaks off
// (1.4); after 0x11 ends, 0x12 does the same with a glow every 10 steps and a
// spark every 8 ticks. The original counted its sparks and spun once a
// call; here by the step's ticks and share.
void Em024ChargeUpPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x70);
    GuestCall game{ctx};
    const Em::Enemy em{game, frame.Arg(3)};
    const uint32_t work = em.work, at = frame[0x10], joint = frame[0x08], mtx = frame[0x20];
    const bool effects = uint8_t(frame.Arg(4)) == 0;
    const uint32_t how = frame.Arg(5);

    const auto inArea = [&] { return (game.Call<0x802AFCE8>() & 0xFF) == Memory::Read8(work + kArea); };

    const auto spawn = [&](float scale, uint32_t kind, uint32_t bone, bool offset, float y = 0.0f, float z = 0.0f) {
        if (offset)
            game.Call<Em::Vec3Set>({at}, {0.0, double(y), double(z)});

        game.Call<Em::Spawn>({work, kind, bone, offset ? at : 0u}, {double(scale)});
    };

    const auto sound = [&](uint32_t bone, uint32_t id) {
        game.Call<JointPosition>({work, bone, joint});
        game.Call<0x800D9F0C>({Memory::Read32(work + 0xB14), id, 2, joint}); // se_req_pos_ps
    };

    // The sound at the offset (`at`) in joint 0x15's frame.
    const auto soundInJoint = [&] {
        game.Call<0x80135994>({work, 0x15, mtx}); // get_joint_wmat_em
        game.Call<0x800514A4>({at, mtx});         // mulVecMat
        game.Call<0x8010148C>({mtx, at});
        game.Call<0x80101470>({mtx, joint});
        game.Call<0x800D9F0C>({Memory::Read32(work + 0xB14), 0x94, 2, joint});
    };

    // The glow and the spark of the charge: every `glowEvery` steps, every
    // `sparkAfter` + 1 ticks.
    const auto charge = [&](uint32_t glowEvery, int32_t sparkAfter) {
        if (StepCounter::Every(glowEvery))
            spawn(3.0f, 0x67, 3, true);

        if (em.Count() > sparkAfter) {
            em.Count(0);
            game.Call<Spark>({work, 10});
        }

        if (inArea()) {
            game.Call<JointPosition>({work, 3, joint});
            game.Call<0x800DE360>({Memory::Read32(work + 0xB14), joint, 0x1C, Memory::Read16(work + 0x1A)}); // shell_se_req
        }
    };

    // r5 2-5: the yaw spun by the step's share of 0x100 / 0x180; true if it spun.
    const auto spin = [&] {
        if (how < 2 || how > 5)
            return false;

        const int32_t turn = GameStep::Share(how < 4 ? 0x100 : 0x180);
        Memory::Write32(work + kAngles + 4, (Memory::Read32(work + kAngles + 4) + uint32_t(how % 2 == 0 ? turn : -turn)) & 0xFFFF);
        return true;
    };

    // Broken off (1.4), or turned: 0x14 to its end. True when either.
    const auto turnOrBreak = [&](uint32_t max) {
        if (game.Call<0x8034F2AC>({work}) == 1) {
            game.Call<0x80129788>({work, 0});
            game.Call<0x80128A78>({work, 1, 4});
            return true;
        }

        if (game.Call<TurnStep>({work, 0, max}) == 1) {
            game.Call<0x80129788>({work, 0});
            em.State(3);
            em.SetMotion(0x14, 2, 0);
            game.Call<RoarVoice>({work, 1, 0x17, 0x10});
            game.Call<RoarVoice0>({work, 0, 0x14});
            return true;
        }

        return false;
    };

    game.Call<0x80050570>({mtx});

    switch (em.State()) {
    case 0:
        em.State(1);
        game.Call<Em::SetUp>({work, 0});
        em.SetMotion(0x11, 4, 0);
        game.Call<RoarVoice>({work, 0, uint8_t(frame.Arg(4)) == 1 ? 0x1Bu : 1u, 0xB});
        game.Call<TurnSetUp>({work, 0}, {how == 1 ? -2300.0 : 500.0});

        if (how - 2 < 4 && 2500.0f < Memory::ReadFloat32(work + 0x378))
            Memory::WriteFloat32(work + 0x378, 2500.0f);

        game.Call<0x80358854>({work, uint32_t(-20)});
        em.Count(0);
        break;
    case 1: {
        em.CountTicks();

        if (effects) {
            if (em.At(4.0f)) {
                spawn(0.6f, 0x65, 0x15, true);

                if (inArea())
                    sound(0x15, 0x8D);
            }

            if (em.At(12.0f))
                spawn(0.5f, 0x65, 0x1B, true);

            if (em.At(20.0f))
                spawn(0.7f, 0x65, 0xB, true);

            if (em.At(40.0f))
                spawn(1.0f, 0x65, 3, true);

            if (em.At(42.0f))
                spawn(1.0f, 0x65, 0x13, true);

            if (em.At(44.0f))
                spawn(1.0f, 0x65, 0x1D, true);

            if (em.At(46.0f))
                spawn(1.0f, 0x65, 0x21, true);

            if (em.At(60.0f, 1))
                charge(8, 5);
        }

        uint32_t max = 0x80;

        if (em.At(40.0f, 1) && spin())
            max = 0;

        if (em.At(90.0f, 1) && turnOrBreak(max))
            return;

        if (em.Ended()) {
            em.NextState();
            em.SetMotion(0x12, 0, 0);
        }

        break;
    }
    case 2:
        if (effects) {
            em.CountTicks();
            charge(10, 7);
        }

        turnOrBreak(spin() ? 0 : 0x80);
        break;
    case 3:
        if (effects) {
            if (em.At(2.0f)) {
                spawn(1.0f, 0xB6, 0x11, false);

                if (inArea())
                    sound(0x11, 0x8F);
            }

            if (em.At(4.0f))
                spawn(0.8f, 0xB6, 0x20, false);

            if (em.At(8.0f)) {
                spawn(0.7f, 0xB6, 0x1D, false);
                spawn(0.8f, 0xB6, 0x2E, false);
            }

            if (em.At(52.0f)) {
                spawn(1.0f, 0xB0, 0x15, false);
                spawn(1.0f, 0xB1, 0x15, false);

                if (inArea())
                    sound(0x15, 0x94);
            }

            if (em.At(70.0f))
                spawn(0.8f, 0x65, 0x15, true, 100.0f, -50.0f);

            if (em.At(126.0f)) {
                spawn(0.8f, 0x65, 0x15, true, 0.0f, 50.0f);

                if (inArea())
                    soundInJoint();
            }

            if (em.At(132.0f))
                spawn(0.8f, 0x65, 0xB, false);

            if (em.At(144.0f))
                spawn(0.7f, 0x65, 0x24, false);

            if (em.At(180.0f)) {
                spawn(0.5f, 0x65, 0x15, true, 0.0f, 100.0f);

                if (inArea())
                    soundInJoint();
            }

            if (em.At(186.0f))
                spawn(0.4f, 0x65, 0x21, false);

            if (em.At(192.0f))
                spawn(0.4f, 0x65, 0x1B, true);

            if (em.At(212.0f))
                spawn(0.4f, 0x65, 0x2E, false);
        }

        if (em.Ended())
            game.Call<ActionDone>({work});

        break;
    default:
        break;
    }
}

// kind 40 0x801BA438 (r4 0 / 1: a long or a short throw): Uragaan's
// bouncing rock (owner +0x330, kind 9). Its offset (+0x34C) from its start
// (+0x334) flies to the first bounce (+0x340) in 24 / 10 ticks and on to the
// end (+0x334 - +0x340 back) in 21 / 33 ticks, its velocity (+0x310) set to
// reach each in time under its gravity (+0x31C: +0x320 -0.3 / -0.15, then -2 /
// -0.9 a tick), tumbling 0x1555 and 0xAAB a tick; then 1.0, or 11.56 with
// +0x329. The original added its velocity and gravity once a call; here by
// the step's length (the end snaps to the target, as the original's).
void Em040RockPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x60);
    GuestCall game{ctx};
    const Em::Enemy em{game, frame.Arg(3)};
    const uint32_t work = em.work, way = frame[0x08], sum = frame[0x18], heard = frame[0x28];
    const bool longThrow = uint8_t(frame.Arg(4)) == 0;
    const uint32_t owner = Memory::Read32(work + 0x330);
    const float s = float(GameStep::Scale());
    constexpr uint32_t kVelocity = 0x310, kOffset = 0x34C, kStart = 0x334, kBounce = 0x340, kGravity = 0x31C, kPitch = 0x1BC;

    const auto f = [&](uint32_t at) { return Memory::ReadFloat32(work + at); };

    // +0x1A0: the offset plus the way from the position to the start.
    const auto place = [&] {
        game.Call<0x80050D04>({way, work + kStart, work + 0x188}); // VectorSubtractWrapper
        game.Call<0x800513DC>({sum, work + kOffset, way});
        game.Call<0x80041EA4>({work + 0x1A0, sum}); // Vec3Copy
    };

    // The velocity that covers `to` - `from` in `ticks` ticks under the gravity.
    const auto aim = [&](uint32_t to, uint32_t from, bool fromOffset, int32_t ticks, uint32_t gravity) {
        em.Count(ticks);
        Memory::Write32(work + 0x320, gravity);
        const float n = float(ticks);

        for (uint32_t i = 0; i < 3; ++i) {
            const float origin = fromOffset ? f(from + 4 * i) + f(kOffset + 4 * i) : f(from + 4 * i);
            float v = (f(to + 4 * i) - origin) / n;

            if (i == 1) {
                const float half = 0.5f * f(0x320); // fmuls, then fsubs: no fused multiply-add
                const float fall = half * n;
                v = v - fall;
            }

            Memory::WriteFloat32(work + kVelocity + 4 * i, v);
        }
    };

    const auto ownerHears = [&] { return (game.Call<0x802AFCE8>() & 0xFF) == Memory::Read8(work + kArea) && owner && Memory::Read8(owner) && Memory::Read8(owner + 3) == 9; };

    const auto sound = [&](uint32_t id, bool shell) {
        game.Call<0x800513DC>({sum, work + 0x188, work + 0x1A0});
        game.Call<0x80041EA4>({heard, sum});

        if (shell)
            game.Call<0x800DE360>({Memory::Read32(owner + 0xB14), heard, id, Memory::Read16(work + 0x1A)}); // shell_se_req
        else
            game.Call<0x800D9F0C>({Memory::Read32(owner + 0xB14), id, 2, heard}); // se_req_pos_ps
    };

    const auto rolling = [&] { return Memory::Read8(work + 0x32F) == 0 ? 0x22u : 0x23u; };

    // A tick's flight by the step's length: the offset by the velocity, the velocity by the gravity.
    const auto fly = [&](int32_t tumble) {
        Memory::Write32(work + kPitch, Memory::Read32(work + kPitch) + uint32_t(GameStep::Share(tumble)));

        for (uint32_t i = 0; i < 3; ++i)
            Memory::WriteFloat32(work + kOffset + 4 * i, f(kOffset + 4 * i) + f(kVelocity + 4 * i) * s);

        place();

        for (uint32_t i = 0; i < 3; ++i)
            Memory::WriteFloat32(work + kVelocity + 4 * i, f(kVelocity + 4 * i) + f(kGravity + 4 * i) * s);

        em.Count(em.Count() - int32_t(GameStep::Ticks()));
        return em.Count() < 0;
    };

    game.Call<0x80133CA0>({work});

    switch (em.State()) {
    case 0:
        em.State(1);
        game.Call<0x801302AC>({work});
        game.Call<0x80130628>({work});
        place();
        game.Call<0x8013545C>({work});
        aim(kBounce, kStart, true, longThrow ? 0x18 : 10, longThrow ? 0xBE99999A : 0xBE19999A);
        game.Call<RoarVoice>({work, 0, 2, 0x202});
        game.Call<0x8012CF84>({work});
        game.Call<0x80131E18>({work});

        if (ownerHears())
            sound(rolling(), true);

        break;
    case 1:
        if (fly(0x1555)) {
            em.NextState();
            game.Call<0x80050D04>({sum, work + kBounce, work + kStart});
            game.Call<0x80041EA4>({work + kOffset, sum});
            place();
            game.Call<0x8013545C>({work});
            aim(kStart, kBounce, false, longThrow ? 0x15 : 0x21, longThrow ? 0xC0000000 : 0xBF666666);
            const float n = float(em.Count());
            const float turns = 65536.0f * 15.0f * n;
            const uint32_t spin = uint32_t(int32_t(0.5f + turns / 360.0f));
            Memory::Write32(work + kPitch, -(spin & 0xFFFF) & 0xFFFF);

            if (ownerHears())
                sound(0x28, false);
        } else if (ownerHears())
            sound(rolling(), true);

        game.Call<0x8012CF84>({work});
        game.Call<0x80131E18>({work});
        break;
    case 2:
        if (fly(0xAAB)) {
            game.Call<0x80041EA4>({work + 0x188, work + kStart});
            game.Call<Em::Vec3Set>({work + kOffset}, {0.0, 0.0, 0.0});
            place();
            game.Call<0x801297A8>({work});

            if (ownerHears())
                sound(0x28, false);

            game.Call<0x80128A78>({work, Memory::Read8(work + 0x329) == 1 ? 0xBu : 1u, Memory::Read8(work + 0x329) == 1 ? 0x38u : 0u});
        } else {
            game.Call<0x8012CF84>({work});
            game.Call<0x80131E18>({work});
        }

        break;
    default:
        break;
    }
}

// Agnaktor's turn rate: 0.5 + 65536 * (deg * the motion's frames) / 360 in
// whole units, over the step's advance (rate * s) where the originals took
// the rate: a call's turn was a tick's.
int32_t Em019TurnUnits(GuestCall& game, uint32_t work, float degrees) {
    game.Call<0x8012F948>({work}); // the motion's rate
    const float advance = float(game.F1() * GameStep::Scale());
    const float turn = degrees * advance;
    const float units = 65536.0f * turn / 360.0f;
    return int32_t(0.5f + units) & 0xFFFF;
}

// kind 19 0x8037C73C (r4 1-12): Agnaktor's turn (motion 0xD3 at 1.25, voices):
// r4 1-10 spin by 0.27, 0.6, 0.23, 0.37 or 0.67 degrees per frame of the
// motion (odd left, even right) from frame 70 while turning (toward 1200);
// r4 11/12 first move its mark a quarter turn aside (1650 times 0x8013570C
// over 2 / rate) and turn 0x400 a step from frame 70; then 0xD4 to its end
// and 0x80128158. 0x8012CF84, 0x80131ED8 and 0x803790E8 each step. The
// original spun a tick's turn each call.
void Em019TurnPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x40);
    GuestCall game{ctx};
    const Em::Enemy em{game, frame.Arg(3)};
    const uint32_t work = em.work, aside = frame[0x10];
    const uint8_t how = uint8_t(frame.Arg(4));
    game.Call<0x8012CF84>({work});
    game.Call<0x80131ED8>({work});
    game.Call<0x803790E8>({work});

    switch (em.State()) {
    case 0:
        em.State(1);
        game.Call<Em::SetUp>({work, 0});
        em.SetMotion(0xD3, 0, 0);
        game.Call<SetMotionSpeed>({work}, {1.25});
        game.Call<0x80130450>({work}, {0.0});

        if (how - 1u < 10u)
            game.Call<TurnSetUp>({work, 0}, {1200.0});
        else if (how == 11 || how == 12) {
            const uint32_t angle = game.Call<0x80050AF4>({work + 0x188, work + 0x36C}); // calcVecAng2
            game.Call<0x8012F948>({work});                                              // the motion's rate
            const float perRate = 2.0f / game.F1();
            game.Call<0x8013570C>({work}, {3.0, double(0.3f), 2000.0});
            const float reach = 1650.0f * game.F1();
            game.Call<Em::Vec3Set>({aside}, {0.0, 0.0, double(reach * perRate)});
            game.Call<0x800510C8>({aside, angle + (how == 11 ? 0x4000u : uint32_t(-0x4000))}); // rotVecY
            game.Call<0x80073FCC>({work + 0x36C, aside});                                      // VecAddInPlace
            game.Call<TurnSetUp>({work, 0}, {200.0});
        } else
            game.Call<TurnSetUp>({work, 0}, {1000.0});

        game.Call<0x80379000>({work, 0xFF});
        game.Call<RoarVoice>({work, 0, 0x14, 8});
        game.Call<RoarVoice>({work, 1, 0x19, 0x18});
        game.Call<0x80136D78>({work});
        break;
    case 1: {
        if (em.At(120.0f))
            game.Call<RoarVoice>({work, 0, 0x10, 0x13});

        if (em.At(30.0f, 2))
            game.Call<0x80136D78>({work});

        static constexpr float kDegrees[5]{0.26666668f, 0.6f, 0.23333333f, 0.36666667f, 0.6666667f};
        uint32_t max = 0x100, turn = 0;

        if (how - 1u < 10u) {
            max = 0;
            const uint32_t units = uint32_t(Em019TurnUnits(game, work, kDegrees[(how - 1) / 2]));
            turn = how % 2 ? units : uint32_t(-int32_t(units));
        } else if (how == 11 || how == 12) {
            max = em.At(70.0f, 1) ? 0x400 : 0;
            turn = 0;
        }

        if (em.At(70.0f, 1))
            Memory::Write32(work + kAngles + 4, Memory::Read32(work + kAngles + 4) + turn);

        if (game.Call<TurnStep>({work, 0, max}) == 1) {
            em.NextState();
            em.SetMotion(0xD4, 4, 0);
        }

        break;
    }
    case 2:
        if (em.At(18.0f))
            game.Call<0x80129788>({work, 0});

        if (em.At(60.0f, 1))
            game.Call<0x80136D78>({work});

        if (em.Ended())
            game.Call<0x80128158>({work});

        break;
    default:
        break;
    }
}

// kind 19 0x8037CD94: Agnaktor's dive into the lava (motion 0xCD at 1.25,
// +0x8A0 less 15, voices and glows), the leap (0xD5, +0x314 50 times the
// rate and its scale) turning 10 degrees per frame of the motion until it
// is in (0x8012FF18 within 700 of the lava, 0xCE, a splash) or 0x80379510;
// glows every 8 steps and sparks every 12. The original turned a tick's
// turn each call.
void Em019DivePort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x40);
    GuestCall game{ctx};
    const Em::Enemy em{game, frame.Arg(3)};
    const uint32_t work = em.work, at = frame[0x10];
    game.Call<0x8012CF84>({work});
    game.Call<0x80131ED8>({work});
    game.Call<0x803790E8>({work});

    const auto glow = [&](uint32_t kind) { game.Call<0x80104918>({work, 0x19, kind, 0}, {1.0}); }; // the joint effect

    const auto spawn = [&](uint32_t kind, uint32_t joint) {
        game.Call<Em::Vec3Set>({at}, {0.0, 0.0, 0.0});
        game.Call<Em::Spawn>({work, kind, joint, at}, {1.0});
    };

    switch (em.State()) {
    case 0:
        em.State(1);
        game.Call<Em::SetUp>({work, 0});
        game.Call<Adjust8A0>({work, uint32_t(-15)});
        em.SetMotion(0xCD, 0, 0);
        game.Call<SetMotionSpeed>({work}, {1.25});
        game.Call<0x80130450>({work}, {0.0});
        game.Call<0x80379000>({work, 0xFF});
        game.Call<0x80136D78>({work});
        break;
    case 1:
        if (em.At(70.0f, 1))
            game.Call<SetMotionSpeed>({work}, {1.0});

        if (em.At(94.0f, 2))
            game.Call<0x80136D78>({work});

        if (em.At(24.0f))
            game.Call<RoarVoice>({work, 0, 0x21, 2});

        if (em.At(74.0f))
            game.Call<RoarVoice>({work, 0, 0x12, 3});

        if (em.At(2.0f))
            glow(0x65);

        if (em.At(12.0f, 3, 62.0f) && StepCounter::Every(8))
            glow(99);

        if (em.At(86.0f))
            glow(100);

        if (em.Ended()) {
            em.NextState();
            game.Call<0x801306A0>({work, Memory::Read32(work + 0x204)});
            em.SetMotion(0xD5, 0, 0);
            game.Call<0x8013545C>({work});
            game.Call<0x801359B4>({work}); // get_em_chg_scale
            const float scale = game.F1();
            game.Call<0x8012F948>({work}); // the motion's rate
            const float speed = 50.0f * game.F1();
            Memory::WriteFloat32(work + 0x314, speed * scale);
        }

        break;
    case 2: {
        const uint32_t turn = uint32_t(Em019TurnUnits(game, work, 10.0f));
        Memory::Write32(work + kAngles + 4, Memory::Read32(work + kAngles + 4) + turn);
        game.Call<0x8013547C>({work}); // EnemyAddVelocity

        if (em.At(2.0f, 3, 62.0f) && StepCounter::Every(12))
            spawn(0x85, 3);

        if (Memory::Read8(work + 0x23C) || Memory::ReadFloat32(work + 0x18C) < 4000.0f + Memory::ReadFloat32(work + 0x20C)) {
            if (game.Call<0x8012FF18>({work}, {700.0}) == 1) {
                em.NextState();
                game.Call<0x80136E58>({work});
                em.SetMotion(0xCE, 0, 0);
                spawn(100, 0x19);
                game.Call<0x80129788>({work, 0});
            }
        } else
            game.Call<0x80379510>({work});

        break;
    }
    case 3:
        if (em.At(30.0f, 3, 60.0f) && StepCounter::Every(8))
            spawn(99, 0x19);

        if (em.Ended())
            game.Call<0x80379510>({work});

        break;
    default:
        break;
    }
}

// kind 18 0x801981B4 (r4 1: the second voice, r5 1: spinning the other way),
// actions 7.23 and 7.25-7.27: Gobul's spin. State 0 sets it up (motion 0xD4
// at the ground, 0x801923BC 8, +0x8A0 less 12); at the motion's end state 1
// starts it (0xD5 on channel 0 and 0xD7 on 1, a voice, a burst at 0, -80, 0);
// state 2 spins. Its count runs up to 180, the yaw wobbling by 224 *
// cos(4 degrees * count) and turning by 0x60 a call, the two motions blended
// by (1 + cos) / 2, its capsule (+0xA08) sounding on a hit from frame 50. A
// hunter it caught (substate 1, for 10 calls) or 170 calls (substate 2) end
// it 30 calls later (+7) with 7.24. The original counted and turned once a
// call, so at 60 or 120 steps a second Gobul spun two or four times as fast
// and as briefly; here the counts take the step's ticks and the turns its
// share.
void Em018SpinPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0xB0);
    GuestCall game{ctx};
    const Em::Enemy em{game, frame.Arg(3)};
    const uint32_t work = em.work, burstAt = frame[0x18], hit = frame[0x08], capsule = frame[0x30];
    const bool secondVoice = uint8_t(frame.Arg(4)) == 1, back = uint8_t(frame.Arg(5)) == 1;
    const int32_t ticks = int32_t(GameStep::Ticks());
    constexpr uint32_t kYaw = kAngles + 4, kSub = 6, kHold = 7, kSpinSound = 0x310;

    const auto turn = [&](int32_t delta) { Memory::Write32(work + kYaw, (Memory::Read32(work + kYaw) + uint32_t(delta)) & 0xFFFF); };

    // A substate's hold (+7) counted toward `last`; false once past it.
    const auto hold = [&](uint32_t last) {
        const uint32_t held = Memory::Read8(work + kHold) + uint32_t(ticks);

        if (held > last)
            return false;

        Memory::Write8(work + kHold, uint8_t(held));
        return true;
    };

    game.Call<0x800FA484>({hit});
    game.Call<0x800FA41C>({capsule});
    game.Call<0x80136D78>({work});

    switch (em.State()) {
    case 0:
        em.State(1);
        game.Call<Em::SetUp>({work, 0});
        em.SetMotion(0xD4, 0, 0);
        game.Call<0x80130450>({work}, {0.0});
        game.Call<0x801923BC>({work, 8});
        game.Call<Adjust8A0>({work, uint32_t(-12)});
        break;
    case 1:
        if (!em.Ended())
            break;

        em.NextState();
        Memory::Write8(work + kSub, 0);
        Memory::Write8(work + kHold, 0);
        game.Call<0x8012F874>({work});
        em.SetMotion(0xD5, 0, 0);
        game.Call<SetChannelMotion>({work, 1, 0xD7, 0, 0});
        game.Call<RoarVoice>({work, 0, secondVoice ? 0x19u : 0xEu, 2});
        em.Count(0);
        game.Call<Em::Vec3Set>({burstAt}, {0.0, -80.0, 0.0});
        game.Call<ChestBurst>({work, 3, burstAt, 0xF}, {1.0});
        game.Call<0x8013545C>({work});
        Memory::WriteFloat32(work + kSpinSound, 0.0f);
        break;
    case 2: {
        if (em.Count() < 180)
            em.Count(std::min(em.Count() + ticks, 180));

        float weight = 1.0f;

        if (em.Count() < 180) {
            int32_t degrees = int16_t(em.Count() << 2);

            if (degrees >= 360)
                degrees = int16_t(degrees % 360);

            game.Call<0x804644D0>({}, {(6.2831855f * float(degrees)) / 360.0f}); // cosf
            const float c = game.F1();
            weight = 0.5f * (1.0f + c);
            turn(GameStep::Share(int32_t(224.0f * c)));
        }

        weight = std::clamp(weight, 0.0f, 1.0f);
        game.Call<0x800E2718>({work + 0x24, 0}, {weight});
        game.Call<0x800E2718>({work + 0x24, 1}, {1.0f - weight});

        if (Memory::Read8(work + kSub) <= 1) {
            if (Memory::ReadFloat32(work + kSpinSound) == 0.0f && em.At(50.0f))
                Memory::WriteFloat32(work + kSpinSound, 1.0f);

            if (Memory::ReadFloat32(work + kSpinSound) > 0.0f && game.Call<0x8029E35C>({work + 0xA08, capsule, hit, 0}) == 1 &&
                (game.Call<0x802AFCE8>() & 0xFF) == Memory::Read8(work + kArea)) // get_now_areano
                game.Call<0x800DE078>({Memory::Read32(work + 0xB14), capsule, 7, Memory::Read16(work + 0x1A)}); // shell_se_req2
        }

        switch (Memory::Read8(work + kSub)) {
        case 0: {
            turn(back ? -GameStep::Share(0x60) : GameStep::Share(0x60));

            if (em.Count() >= 170) {
                Memory::Write8(work + kSub, 2);
                break;
            }

            const uint32_t hunters = game.Call<0x800CFB34>({2}) & 0xFFFF; // get_move_work_max
            uint32_t hunter = game.Call<0x800CFAF4>({2});                 // get_move_work_adrs

            for (uint32_t i = 0; i < hunters; ++i, hunter += 0xB20) {
                if (Memory::Read8(hunter) && game.Call<0x8012D20C>({Memory::Read8(hunter + 8)}) == 0 &&
                    game.Call<0x8012D118>({work, hunter}) != 0 &&     // EnemySensesPlayer
                    game.Call<0x8026FECC>({hunter, 6, 0x29}) != 0 && // Pl_act_ck
                    Memory::Read8(hunter + 0x3B6) == Memory::Read16(work + 0x1A)) {
                    Memory::Write8(work + kSub, 1);
                    Memory::Write8(work + kHold, 0);
                }
            }

            break;
        }
        case 1:
            if (em.Count() >= 170 || !hold(10))
                Memory::Write8(work + kSub, 2);

            break;
        default:
            if (Memory::Read8(work + kSub) == 2) {
                Memory::Write8(work + kSub, 3);
                Memory::Write8(work + kHold, 0);
                game.Call<0x80129788>({work, 0});
                game.Call<0x801923AC>({work, 8});
            }

            if (!hold(30))
                game.Call<0x80128A78>({work, 7, 0x18});

            break;
        }

        break;
    }
    default:
        break;
    }
}

} // namespace

extern "C" void func_8014BBA4(CpuContext* ctx);
extern "C" void MH3_Em004Spit(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em001Spit", func_8014BBA4, Em001SpitPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8014BBA4, MH3_Em004Spit, "Em001Spit");

extern "C" void func_8014C4C8(CpuContext* ctx);
extern "C" void MH3_Em004Spit3(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em001Spit3", func_8014C4C8, Em001Spit3Port);
}
REGISTER_NATIVE_FUNCTION_AS(0x8014C4C8, MH3_Em004Spit3, "Em001Spit3");

extern "C" void func_8017B7AC(CpuContext* ctx);
extern "C" void MH3_Em015Discharge(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em015Discharge", func_8017B7AC, Em015DischargePort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8017B7AC, MH3_Em015Discharge, "Em015Discharge");

extern "C" void func_8017C6F4(CpuContext* ctx);
extern "C" void MH3_Em015Charge(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em015Charge", func_8017C6F4, Em015ChargePort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8017C6F4, MH3_Em015Charge, "Em015Charge");

extern "C" void func_80188548(CpuContext* ctx);
extern "C" void MH3_LudrothSpit(CpuContext* ctx) {
    PortCheck::Call(ctx, "LudrothSpit", func_80188548, LudrothSpitPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80188548, MH3_LudrothSpit, "LudrothSpit");

extern "C" void func_8014DE50(CpuContext* ctx);
extern "C" void MH3_Em001Dive(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em001Dive", func_8014DE50, Em001DivePort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8014DE50, MH3_Em001Dive, "Em001Dive");

extern "C" void func_8014CF5C(CpuContext* ctx);
extern "C" void MH3_Em001FlyCharge(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em001FlyCharge", func_8014CF5C, Em001FlyChargePort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8014CF5C, MH3_Em001FlyCharge, "Em001FlyCharge");

extern "C" void func_8038A81C(CpuContext* ctx);
extern "C" void MH3_Em009Roll(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em009Roll", func_8038A81C, Em009RollPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8038A81C, MH3_Em009Roll, "Em009Roll");

extern "C" void func_801981B4(CpuContext* ctx);
extern "C" void MH3_Em018Spin(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em018Spin", func_801981B4, Em018SpinPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x801981B4, MH3_Em018Spin, "Em018Spin");

extern "C" void func_8017AD0C(CpuContext* ctx);
extern "C" void MH3_Em015Storm(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em015Storm", func_8017AD0C, Em015StormPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8017AD0C, MH3_Em015Storm, "Em015Storm");

extern "C" void func_80350DD8(CpuContext* ctx);
extern "C" void MH3_Em024ChargeUp(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em024ChargeUp", func_80350DD8, Em024ChargeUpPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80350DD8, MH3_Em024ChargeUp, "Em024ChargeUp");

extern "C" void func_801BA438(CpuContext* ctx);
extern "C" void MH3_Em040Rock(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em040Rock", func_801BA438, Em040RockPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x801BA438, MH3_Em040Rock, "Em040Rock");

extern "C" void func_8037C73C(CpuContext* ctx);
extern "C" void MH3_Em019Turn(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em019Turn", func_8037C73C, Em019TurnPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8037C73C, MH3_Em019Turn, "Em019Turn");

extern "C" void func_8037CD94(CpuContext* ctx);
extern "C" void MH3_Em019Dive(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em019Dive", func_8037CD94, Em019DivePort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8037CD94, MH3_Em019Dive, "Em019Dive");
