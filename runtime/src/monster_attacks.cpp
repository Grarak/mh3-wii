// Monster attacks with counts per call, RMHP08. C reconstructions (decompiled
// with Ghidra) of six enemy actions that count their calls at +0x20 while a
// motion is before or past a frame, which the enemy update calls once per
// step:
//
//   0x8014BBA4, 0x8014C4C8  em004 (Gigginox) spits: a sound (0x801039C4 1)
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
//
// At 120 steps a second the sounds and sparks came four times as often and
// the blend took a quarter of the time. The counts take the step's whole
// ticks; the sound and the spark come on the step whose ticks include a
// multiple of 8 or 3 (Em::Enemy::CountEvery), and the blend weight is the
// ticks plus the fraction of a tick past the last one, so it grows on every
// step at the Wii's rate. Their periodic tests of the global step counter
// (`system_w+0xC & 3`, hooked in the originals) go through StepCounter::Every.
//
// At the stock 1/30 s step these are the originals (MH3_PORT_CHECK=Em004Spit,
// ... MH3_VARIABLE_STEP=0; the Ludroth spit in the Royal Ludroth arena 9003,
// the others through the enemy fuzz's A: entries).
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
    Em004Prepare = 0x80154CD8,   // (work)
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
    Em004ShellAim = 0x8015461C,  // (params*, 0, u16 angle, u16 angle2)
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

// em004's spits: the motion's speed from a table by its frame (kinds 3 and
// 1), the hiss (sound 1) every 8 ticks until frame 56.
void Em004Hiss(const Em::Enemy& em, GuestCall& game) {
    if (em.At(2.0f))
        game.Call<Sound>({em.work, 3});

    if (!em.At(56.0f, 1)) {
        if (em.CountEvery(8))
            game.Call<Sound>({em.work, 1});

        em.CountTicks();
    }
}

void Em004SpeedFromKeys(GuestCall& game, uint32_t work, uint32_t keys) {
    game.Call<KeyData>({keys}, {Memory::ReadFloat32(work + kMotionFrame)});
    game.Call<SetMotionSpeed>({work}); // f1: the key's speed
}

// em004 0x8014BBA4 (r4: the spit's direction 0-2, or 3 the slow one).
void Em004SpitPort(CpuContext* ctx) {
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
            game.Call<Em004Prepare>({work});

        em.SetMotion(0x33, 6, 0);
        return;
    }

    if (em.State() != 1)
        return;

    if (kind == 3)
        Em004SpeedFromKeys(game, work, 0x805A2838);

    Em004Hiss(em, game);

    if (kind == 3) {
        if (em.At(78.0f)) {
            game.Call<Em004ShellAim>({params, 0, 0, 0});
            Throw(game, work, params, 1);
            game.Call<SoundAt>({work, 4, 0, 0});
        }
    } else {
        const uint32_t angle = kind == 1 ? 0xF8E4 : kind == 2 ? 0x71C : 0;

        if (em.At(78.0f)) {
            game.Call<Em004ShellAim>({params, 0, angle, 0});
            Throw(game, work, params, 0);
            SpitSound(game, work, params, 0);
            SpitSound(game, work, params, 2);
        }
    }

    if (em.Ended())
        game.Call<ActionDone>({work});
}

// em004 0x8014C4C8 (r4: 1 the slow one): three spits at frames 78, 120 and
// 164.
void Em004Spit3Port(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x30);
    GuestCall game{ctx};
    const Em::Enemy em{game, frame.Arg(3)};
    const uint32_t work = em.work, params = frame[0x08];
    const uint8_t kind = uint8_t(frame.Arg(4));
    game.Call<ShellParams>({params});

    if (em.State() == 0) {
        em.State(1);
        game.Call<Em::SetUp>({work, 0});
        game.Call<Em004Prepare>({work});
        em.SetMotion(0xD0, 4, 0);
        return;
    }

    if (em.State() != 1)
        return;

    if (kind == 1)
        Em004SpeedFromKeys(game, work, 0x805A2860);

    Em004Hiss(em, game);

    const bool first = em.At(78.0f);

    if (first)
        game.Call<Em004ShellAim>({params, 0, 0x38E, 0});

    const bool second = em.At(120.0f);

    if (second)
        game.Call<Em004ShellAim>({params, 0, 0x38E, 0xF334});

    uint32_t spit = first || second ? 1 : 0;

    if (em.At(164.0f)) {
        game.Call<Em004ShellAim>({params, 0, 0x321, 0x105B});
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
        game.Call<Em004Prepare>({work});
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

} // namespace

extern "C" void func_8014BBA4(CpuContext* ctx);
extern "C" void MH3_Em004Spit(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em004Spit", func_8014BBA4, Em004SpitPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8014BBA4, MH3_Em004Spit, "Em004Spit");

extern "C" void func_8014C4C8(CpuContext* ctx);
extern "C" void MH3_Em004Spit3(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em004Spit3", func_8014C4C8, Em004Spit3Port);
}
REGISTER_NATIVE_FUNCTION_AS(0x8014C4C8, MH3_Em004Spit3, "Em004Spit3");

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
