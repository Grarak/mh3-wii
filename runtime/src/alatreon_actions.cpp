// Alatreon (em024), RMHP08. C reconstructions (decompiled with Ghidra) of two
// of its actions, which the enemy update calls once per step.
//
// The intro action (0x803559F0) runs while the Sacred Pinnacle's event demo
// plays (quest 19001): the
// monster is posed from keyframe tables by the demo clock (DemoPose,
// demo_pose.cpp) through states 1-11, then in state 12 it breathes on the
// hunter.
//
// Two things in state 12 were written for one call a tick:
//
//  - while its motion is in [56, 138] it spawns a breath effect (kind 1 on
//    joint 0x15) every 8 calls, counting the calls at +0x20: at 120 steps a
//    second that was four times as many effects. It counts ticks, and the
//    effect comes on the step whose ticks include a multiple of 8;
//  - on demo steps 834, 844 and 854 it fires a shell (shell_set slot 0x8C)
//    at three spots. The demo clock holds between ticks, so the original
//    fired each shell on every step of that tick. It fires on the step whose
//    ticks reach the demo step (Em::DemoAt).
//
// The meteor breath (0x80353EBC), one of its attacks in the fight, has the
// same breath effect every 8 calls in [56, 138] (3.5 times as many at
// 120 fps on the Pi), then drops three shells at random spots of a table of
// six around it on frames 164, 224 and 284, never the same spot twice.
//
// At the stock 1/30 s step these are the originals
// (MH3_PORT_CHECK=AlatreonIntro,AlatreonMeteors MH3_VARIABLE_STEP=0, quest
// 19001).
#include "abi_bridge.h"
#include "enemy_work.h"
#include "game_step.h"
#include "guest_call.h"
#include "memory.h"
#include "port_check.h"

#include <cstdint>

namespace {

// Enemy work fields.
enum : uint32_t {
    kAngles = 0x1BC, // s32[3]: the effects' orientation
    kPositionY = 0x18C,
    kArea = 0x1E1, // u8
};

enum : uint32_t {
    Acted = 0x80131DE8,         // (work): marks the enemy as updated this step
    AfterFrameCheck = 0x8012F990, // em_after_frame_check (work, mode; f1, f2): over the advance just made
    SetMotionEx = 0x8012F568,   // (work, motion, blend, flags, 1)
    DemoStart = 0x801461D0,     // (work, 0)
    DemoEnter = 0x80146200,     // (work)
    SetScale = 0x80130628,      // (work; f1)
    JointPosition = 0x8013599C, // get_joint_wpos_em (work, joint, VEC3* out)
    Effect009 = 0x801049B0,     // eft009_set_pos (kind, VEC3* at, angles*, u8 area; f1 scale)
    Rumble = 0x80056AB8,        // (work, 0x16, 10)
    Roar = 0x801293A0,          // (work, 0, 0x16, 5)
    ToVec3 = 0x8005001C,        // vec_to_mh_vec3 (VEC3* out, Vec* in)
    RotateY = 0x800510C8,       // rotVecY (VEC3*, angle)
    Vec3Add = 0x800513DC,       // (out, a, b)
    Vec3Copy = 0x80041EA4,      // (to, from)
    ActionDone = 0x80127FAC,    // (work): back to the AI
};

enum : uint32_t {
    kSpot1 = 0x06,     // u8: the first shell's spot
    kSpot2 = 0x07,     // u8: the second's
    kRandom = 0x1EC,   // u16: the enemy's random word
    kYaw = 0x1C0,      // s32
    kPosition = 0x188, // VEC3
};

constexpr uint32_t kSpots = 0x805EC570; // Vec[6]: the shells' spots around the monster

constexpr uint32_t kShellTable = 0x80797020; // shell_set_func_ptr: the shell functions' table
constexpr uint32_t kShellSlot = 0x8C;        // (work, 0, VEC3* at, 60; f1 scale)

// The keyframe tables of the demo poses.
constexpr uint32_t kApproachKeys = 0x805ECEB8;
constexpr uint32_t kLandKeys = 0x805ECFA8;
constexpr uint32_t kWalkKeys = 0x805ED210;
constexpr uint32_t kTurnKeys = 0x805ED420;
constexpr uint32_t kRiseKeys = 0x805ED510;

void AlatreonIntroPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x40);
    GuestCall game{ctx};
    const Em::Enemy em{game, frame.Arg(3)};
    const uint32_t work = em.work;
    const uint32_t shellAt = frame[0x14], at = frame[0x08];
    game.Call<Acted>({work});

    const auto demo = [&] { return Em::DemoClock(game); };
    const auto reached = [&](uint32_t steps) { return game.Call<Em::DemoReached>({steps}) == 1; };
    const auto pose = [&](uint32_t keys) { game.Call<Em::DemoPose>({work, uint32_t(demo()), keys, 0}); };
    const auto poseAxes = [&](uint32_t keys, uint32_t axes) { game.Call<Em::DemoPoseAxes>({work, uint32_t(demo()), keys, 0, axes, 0}); };

    const auto setVec = [&](uint32_t vec, float x, float y, float z) {
        Memory::WriteFloat32(vec, x);
        Memory::WriteFloat32(vec + 4, y);
        Memory::WriteFloat32(vec + 8, z);
    };

    // An eft009 effect at a joint, 5 above the monster's height.
    const auto jointEffect = [&](float frameAt, uint32_t joint, uint32_t kind, float scale) {
        if (game.Call<AfterFrameCheck>({work, 0}, {frameAt, 0.0f}) != 1)
            return;

        game.Call<JointPosition>({work, joint, at});
        Memory::WriteFloat32(at + 4, 5.0f + Memory::ReadFloat32(work + kPositionY));
        game.Call<Effect009>({kind, at, work + kAngles, Memory::Read8(work + kArea)}, {scale});
    };

    const auto shell = [&](float x, float z) {
        setVec(shellAt, x, 0.0f, z);
        const uint32_t fn = Memory::Read32(Memory::Read32(kShellTable) + kShellSlot);
        game(fn, {work, 0, shellAt, 60}, {1.0f});
    };

    switch (em.State()) {
    case 0:
        em.NextState();
        game.Call<Em::SetUp>({work, 0});
        em.SetMotion(1, 0, 0);
        game.Call<DemoStart>({work, 0});
        break;
    case 1:
        if (reached(0x122)) {
            em.NextState();
            game.Call<SetScale>({work}, {0.0f});
            game.Call<DemoEnter>({work});
            em.SetMotion(2, 0, 0);
            game.Call<Em::SetPosition>({work}, {-13940.56f, 4260.0f, -75.42f});
            poseAxes(kApproachKeys, 1);
            game.Call<Em::SetAngles>({work}, {0.0f, 90.0f, 0.0f});
        }

        break;
    case 2:
        if (!reached(0x202))
            poseAxes(kApproachKeys, 1);
        else
            poseAxes(kLandKeys, 5);

        if (reached(0x208)) {
            em.NextState();
            game.Call<SetMotionEx>({work, 0x24, 0x10, 0, 1});
        }

        break;
    case 3:
        jointEffect(188.0f, 0xE, 0x85, 4.5f);
        jointEffect(212.0f, 3, 0x93, 1.3f);
        jointEffect(240.0f, 3, 0x88, 2.0f);

        if (!reached(0x2FA))
            poseAxes(kLandKeys, 5);
        else
            pose(kWalkKeys);

        if (reached(0x2FE)) {
            em.NextState();
            em.SetMotion(0x25, 0, 0);
        }

        break;
    case 4:
        pose(kWalkKeys);

        if (reached(900)) {
            em.NextState();
            game.Call<SetMotionEx>({work, 0x2A, 0x10, 0, 1});
        }

        break;
    case 5:
        pose(kWalkKeys);

        if (reached(0x3BA)) {
            em.NextState();
            pose(kTurnKeys);
        }

        break;
    case 6:
        pose(kTurnKeys);

        if (reached(0x3D6)) {
            em.NextState();
            em.SetMotion(0x2A, 0, 0x14);
            game.Call<Em::SetAngles>({work}, {0.0f, 146.6816f, 0.0f});
        }

        break;
    case 7:
        pose(kTurnKeys);

        if (reached(0x3F8)) {
            em.NextState();
            game.Call<Em::SetUp>({work, 0});
            em.SetMotion(0x2B, 0, 0);
        }

        break;
    case 8:
    case 9:
    case 10:
    case 11: {
        static constexpr uint32_t kEnd[] = {0x480, 0x506, 0x58C, 0x666};
        static constexpr uint32_t kMotion[][2] = {{0xC, 0xC}, {0xC, 6}, {1, 8}};
        const uint32_t state = em.State();
        poseAxes(kRiseKeys, 5);

        if (!reached(kEnd[state - 8]))
            break;

        em.NextState();

        if (state < 11) {
            game.Call<SetMotionEx>({work, kMotion[state - 8][0], kMotion[state - 8][1], 0, 1});
            break;
        }

        em.SetMotion(0x20, 0, 0);
        game.Call<Em::SetPosition>({work}, {-84.7694f, 250.0058f, -299.5185f});
        game.Call<Em::SetAngles>({work}, {0.0f, 146.6816f, 0.0f});
        em.Count(0);
        break;
    }
    case 12: {
        if (em.At(50.0f))
            game.Call<Rumble>({work, 0x16, 10});

        setVec(at, 0.0f, -30.0f, 100.0f);

        if (em.At(50.0f))
            em.Spawn(1.0f, 0, 0x15, at);

        // The breath: an effect every 8 ticks while the motion is in [56, 138].
        if (em.At(56.0f, 3, 138.0f)) {
            if (em.CountEvery(8))
                em.Spawn(1.0f, 1, 0x15, at);

            em.CountTicks();
        }

        // Shells on demo steps 834, 844 and 854 (the original: steps * 2 ==
        // 0x684, 0x698, 0x6AC).
        const int32_t steps = demo();

        if (Em::DemoAt(steps, 0x342))
            shell(1640.0f, -460.0f);

        if (Em::DemoAt(steps, 0x34C))
            shell(700.0f, -3740.0f);

        if (Em::DemoAt(steps, 0x356))
            shell(3230.0f, -4220.0f);

        if (reached(0x7C8)) {
            em.NextState();
            em.Count(0);
            game.Call<SetMotionEx>({work, 1, 0x10, 0, 1});
        }

        break;
    }
    }
}

void AlatreonMeteorsPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x60);
    GuestCall game{ctx};
    const Em::Enemy em{game, frame.Arg(3)};
    const uint32_t work = em.work;
    const uint32_t at = frame[0x08], spot = frame[0x14], sum = frame[0x20], shellAt = frame[0x2C];

    // A shell at spot i of the table, turned to the monster's yaw.
    const auto shell = [&](uint32_t i) {
        game.Call<ToVec3>({spot, kSpots + 12 * i});
        game.Call<RotateY>({spot, Memory::Read32(work + kYaw)});
        game.Call<Vec3Add>({sum, work + kPosition, spot});
        game.Call<Vec3Copy>({shellAt, sum});
        const uint32_t fn = Memory::Read32(Memory::Read32(kShellTable) + kShellSlot);
        game(fn, {work, 1, shellAt, 60}, {1.0f});
    };

    if (em.State() == 0) {
        em.State(1);
        Memory::Write8(work + kSpot1, 0);
        Memory::Write8(work + kSpot2, 0);
        game.Call<Em::SetUp>({work, 0});
        em.SetMotion(0x20, 4, 0);
        em.Count(0);
        return;
    }

    if (em.State() != 1)
        return;

    if (em.At(50.0f)) {
        game.Call<Roar>({work, 0, 0x16, 5});
        game.Call<Rumble>({work, 0x16, 10});
    }

    Memory::WriteFloat32(at, 0.0f);
    Memory::WriteFloat32(at + 4, -30.0f);
    Memory::WriteFloat32(at + 8, 100.0f);

    if (em.At(50.0f))
        em.Spawn(1.0f, 0, 0x15, at);

    // The breath: an effect every 8 ticks while the motion is in [56, 138].
    if (em.At(56.0f, 3, 138.0f)) {
        if (em.CountEvery(8))
            em.Spawn(1.0f, 1, 0x15, at);

        em.CountTicks();
    }

    const uint32_t random = Memory::Read16(work + kRandom);

    if (em.At(164.0f)) {
        const uint32_t first = random % 6;
        Memory::Write8(work + kSpot1, uint8_t(first));
        shell(first);
    }

    if (em.At(224.0f)) {
        uint32_t second = (random >> 4) % 5;

        if (Memory::Read8(work + kSpot1) <= second)
            ++second;

        Memory::Write8(work + kSpot2, uint8_t(second));
        shell(second);
    }

    if (em.At(284.0f)) {
        // A third spot, past the two taken (in ascending order).
        uint32_t third = (random >> 8) & 3;
        const uint32_t first = Memory::Read8(work + kSpot1), second = Memory::Read8(work + kSpot2);
        const uint32_t low = first < second ? first : second, high = first < second ? second : first;

        if (low <= third)
            ++third;

        if (high <= third)
            ++third;

        shell(third);
    }

    if (em.Ended())
        game.Call<ActionDone>({work});
}

} // namespace

extern "C" void func_803559F0(CpuContext* ctx);
extern "C" void MH3_AlatreonIntro(CpuContext* ctx) {
    PortCheck::Call(ctx, "AlatreonIntro", func_803559F0, AlatreonIntroPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x803559F0, MH3_AlatreonIntro, "AlatreonIntro");

extern "C" void func_80353EBC(CpuContext* ctx);
extern "C" void MH3_AlatreonMeteors(CpuContext* ctx) {
    PortCheck::Call(ctx, "AlatreonMeteors", func_80353EBC, AlatreonMeteorsPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80353EBC, MH3_AlatreonMeteors, "AlatreonMeteors");
