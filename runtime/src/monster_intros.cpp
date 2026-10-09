// Monster intros, RMHP08. C reconstructions (decompiled with Ghidra) of ten
// enemy intro actions: the actions that place and animate a monster in its
// first-encounter event demo, which the enemy update calls once per step while
// the demo plays (each is slot 3's action of its kind, em number = the game's
// monster list index):
//
//   0x8038C188  kind 9 (Uragaan)      0x801C8228  kind 4 (Gigginox)
//   0x8032F5B4  em026 (Giggi)       0x802FE5B4  em023 (Epioth)
//   0x8018B47C  em016/017/021 (the Ludroth kinds' shared intro)
//   0x80162564  kind 8 (Barroth)       0x801BF050  kind 6 (Diablos)
//   0x80150830  kinds 1/2 (Rathian, Rathalos)    0x8017E56C  em015 (Lagiacrus)
//   0x801D42F0  kind 5 (Barioth)
//
// Each is a state machine (+5) that moves on when the event demo clock
// (EventDemoGetSteps, a tick site: whole ticks) reaches a step (DemoReached
// takes half steps) or when a motion ends, and spawns effects on motion
// frames. Things in them were written for one call a tick:
//
//  - the kind 9, kind 4 and em026 intros pose the monster from keyframe tables
//    (DemoPose / DemoPoseAxes, demo_pose.cpp) by a count of their own at
//    +0x20 that went up by one a call: at 120 steps a second the monster ran
//    its cutscene path four times as fast, then jumped to the next state's
//    spot. The count takes the step's whole ticks (DemoPose adds the fraction
//    of a tick, so the pose moves on every step);
//  - effects spawned while a motion is in a range on demo steps that are
//    multiples of 4, 8 or 3 (`steps & 3`, `& 7`, `% 3`) and on given demo
//    steps (`== 0x8F`): the clock holds between ticks, so the test passed on
//    every step of such a tick, four times as many effects at 120 fps. They
//    pass on the step whose ticks reach such a step (Em::DemoEvery, DemoAt);
//  - the steam of the kind 6, kinds 1/2, em015 and kind 5 roars every 8 calls
//    in a range of the motion (+0x20): every 8 ticks
//    (Em::Enemy::CountEvery).
//
// At the stock 1/30 s step these are the originals (MH3_PORT_CHECK=Em009Intro,
// ... MH3_VARIABLE_STEP=0; the demos need a first encounter, so the enemy
// fuzz's A: entries check them, see port_check.h).
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

#include <cstdint>

namespace {

// Enemy work fields besides enemy_work.h's.
enum : uint32_t {
    kInDemo = 0x001,    // u8: 1 while the monster plays in the event demo (0x80146200, inlined)
    kSubState = 0x006,  // u8
    kAngleX = 0x1BC,    // s32 angle units
    kAngleY = 0x1C0,    // the yaw
    kAngleZ = 0x1C4,
    kArea = 0x1E1,      // u8: the area the monster is in
    kRoamMask = 0x1E4,  // u8: 0x3F after kind 8's intro set-up
    kGroundY = 0x20C,   // f32: the ground height under the monster
    kActed = 0x30C,     // u8: 1, the monster's action ran this step (0x80131DE8, inlined)
    kActed2 = 0x30E,    // u8: 1 every call of kind 4's intro (0x80131E00, inlined)
    kHold785 = 0x785,   // u8: 1 every call of em023's intro and kind 4's posed states (0x80133CA0, inlined)
    kHold79C = 0x79C,   // u16: 1 from kind 6's intro (0x80136D78, inlined)
};

enum : uint32_t {
    SetMotionEx = 0x8012F568,   // (work, motion, blend, flags, 1)
    SetMotionSpeed = 0x8012F92C, // (work; f1): the motion's speed (+0x7C4) and its MHchar rate
    SetScale = 0x80130628,      // (work; f1)
    DemoStart = 0x801461D0,     // (work, 0)
    JointPosition = 0x8013599C, // get_joint_wpos_em (work, joint, VEC3* out)
    ChangeScale = 0x801359B4,   // get_em_chg_scale (work) -> f1
    JointEffect = 0x80104918,   // (work, kind, joint, 0; f1 scale): an effect on a joint
    GroundEffect = 0x8010D314,  // (VEC3* at, u8 area, kind, angle; f1 scale): dust on the ground
    SpotEffect = 0x801066F8,    // (work, VEC3* at, 0; f1 scale)
    Vec3Copy = 0x80041EA4,      // (to, from)
    ShellParams = 0x801497EC,   // (params*): the shell parameters' constructor
    ShellAim = 0x8015FD80,      // (params*, direction, 4, VEC3* offset, 0, angle)
    Em008Signal = 0x8015D8C4,   // (work): kind 8's intro, demo step 0x280 / 2
};

constexpr uint32_t kShellTable = 0x80797020; // shell_set_func_ptr: the shell functions' table
constexpr uint32_t kShellThrow = 0x3C;       // (work, params*, 4)

// What every intro does, over its work and the original's local VEC3 at sp+8.
struct Intro {
    GuestCall game;
    Em::Enemy em;
    uint32_t work;
    uint32_t at;

    Intro(CpuContext* ctx, const GuestFrame& frame) : game{ctx}, em{game, frame.Arg(3)}, work(frame.Arg(3)), at(frame[0x08]) {}

    uint32_t State() const { return em.State(); }

    void NextState() const { em.NextState(); }

    int32_t Demo() { return Em::DemoClock(game); }

    // DemoReached: from demo step half_steps / 2 on.
    bool Reached(uint32_t halfSteps) { return game.Call<Em::DemoReached>({halfSteps}) == 1; }

    // `steps & (n - 1)` / `steps % n` == k on the demo clock, once per tick.
    bool DemoEvery(int32_t n, int32_t k = 0) { return Em::DemoEvery(Demo(), n, k); }

    bool At(float frame, uint32_t mode = 0, float end = 0.0f) const { return em.At(frame, mode, end); }

    bool Ended() const { return em.Ended(); }

    void Acted() const { Memory::Write8(work + kActed, 1); }

    void SetUp(uint32_t kind) { game.Call<Em::SetUp>({work, kind}); }

    void Motion(uint32_t motion, uint32_t flags, uint32_t blend) const { em.SetMotion(motion, flags, blend); }

    void MotionEx(uint32_t motion, uint32_t blend, uint32_t flags, uint32_t last) { game.Call<SetMotionEx>({work, motion, blend, flags, last}); }

    void MotionSpeed(float speed) { game.Call<SetMotionSpeed>({work}, {speed}); }

    void Scale(float scale) { game.Call<SetScale>({work}, {scale}); }

    void StartDemo() { game.Call<DemoStart>({work, 0}); }

    void EnterDemo() const { Memory::Write8(work + kInDemo, 1); }

    void Place(float x, float y, float z) { game.Call<Em::SetPosition>({work}, {x, y, z}); }

    void Face(float x, float y, float z) { game.Call<Em::SetAngles>({work}, {x, y, z}); }

    // The keyframe poses at `steps` (the demo clock or the intro's count).
    void Pose(int32_t steps, uint32_t positionKeys, uint32_t angleKeys) { game.Call<Em::DemoPose>({work, uint32_t(int32_t(int16_t(steps))), positionKeys, angleKeys}); }

    void PoseAxes(int32_t steps, uint32_t positionKeys, uint32_t angleKeys, uint32_t positionAxes, uint32_t angleAxes) {
        game.Call<Em::DemoPoseAxes>({work, uint32_t(int32_t(int16_t(steps))), positionKeys, angleKeys, positionAxes, angleAxes});
    }

    // setVector3 (0x8005000C, inlined) on the local VEC3.
    void Offset(float x, float y, float z) const {
        Memory::WriteFloat32(at, x);
        Memory::WriteFloat32(at + 4, y);
        Memory::WriteFloat32(at + 8, z);
    }

    // An enemy effect (0x8030456C) on a joint, at the local VEC3's offset or none.
    void Spawn(float scale, uint32_t kind, uint32_t joint, bool offset = true) const { em.Spawn(scale, kind, joint, offset ? at : 0); }

    void JointFx(float scale, uint32_t kind, uint32_t joint) { game.Call<JointEffect>({work, kind, joint, 0}, {scale}); }

    // The local VEC3 at a joint, 5 above the ground.
    void AtJoint(uint32_t joint) {
        game.Call<JointPosition>({work, joint, at});
        Memory::WriteFloat32(at + 4, 5.0f + Memory::ReadFloat32(work + kGroundY));
    }

    void Dust(float scale, uint32_t kind, uint32_t angle) { game.Call<GroundEffect>({at, Memory::Read8(work + kArea), kind, angle}, {scale}); }

    void Rumble(uint32_t kind, uint32_t length) const { Em::Rumble(work, kind, length); }
};

// kind 9 (0x8038C188): runs in along a path (states 2-6, posed by its count),
// kicking up dust (kind 0x44 every 4 demo steps while its motion is past
// frame 168, 0x45 every 3 in its frame ranges), then stands at five spots.
void Em009IntroPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x20);
    Intro in(ctx, frame);
    const uint32_t work = in.work;
    constexpr uint32_t kRunKeys = 0x805F0D30, kTurnKeys = 0x805F0DE0, kTurnAngleKeys = 0x805F0F50, kLeapKeys = 0x805F0F78;
    in.em.CountTicks();
    in.Acted();

    // The run's dust: every 4 demo steps while the motion is in range.
    const auto runDust = [&](float from, uint32_t mode, float to) {
        if (in.At(from, mode, to) && in.DemoEvery(4)) {
            in.Offset(0.0f, 0.0f, 150.0f);
            in.Spawn(1.0f, 0x44, 1);
        }
    };

    // The landing's sprays: on demo steps that are k mod 3 while the motion is in range.
    const auto spray = [&](float from, float to, int32_t k, uint32_t joint) {
        if (in.At(from, 3, to) && in.DemoEvery(3, k))
            in.Spawn(1.0f, 0x45, joint, false);
    };

    const auto stomp = [&](float at) {
        if (in.At(at)) {
            in.AtJoint(3);
            in.Dust(1.0f, 4, 0);
        }
    };

    const auto stand = [&](uint32_t halfSteps, uint32_t blend, float x, float z, bool face) {
        if (!in.Reached(halfSteps))
            return;

        in.NextState();
        in.Motion(0x12, 0, blend);
        in.Place(x, 45.0f, z);

        if (face)
            in.Face(0.0f, 86.25f, 0.0f);
    };

    switch (in.State()) {
    case 0:
        in.NextState();
        in.SetUp(0);
        in.Motion(1, 0, 0);
        in.StartDemo();
        in.em.Count(0);
        break;
    case 1:
        if (in.Reached(0x8C)) {
            in.NextState();
            in.EnterDemo();
            in.Scale(0.0f);
            in.Motion(0x17, 0, 0xA4);
            in.Pose(in.em.Count(), kRunKeys, 0);
            in.Face(0.0f, -129.75f, 0.0f);
        }

        break;
    case 2:
        runDust(168.0f, 1, 0.0f);
        in.Pose(in.em.Count(), kRunKeys, 0);

        if (in.Reached(0xF2)) {
            in.NextState();
            in.em.Count(0);
            in.Face(0.0f, -192.03f, 0.0f);
            in.PoseAxes(0, kTurnKeys, kTurnAngleKeys, 7, 2);
        }

        break;
    case 3: {
        runDust(168.0f, 1, 0.0f);
        const int32_t steps = in.Demo();

        if (Em::DemoAt(steps, 0x8F))
            in.JointFx(1.0f, 1, 6);

        // Dust at its feet on demo steps 0xC1 and 0xE5.
        if (Em::DemoAt(steps, 0xC1) || Em::DemoAt(steps, 0xE5)) {
            in.game.Call<Vec3Copy>({in.at, work + Em::kPosition});
            Memory::WriteFloat32(in.at + 4, 5.0f + Memory::ReadFloat32(work + kGroundY));
            in.Dust(1.2f, 1, 0);
        }

        in.PoseAxes(in.em.Count(), kTurnKeys, kTurnAngleKeys, 7, 2);

        if (in.Reached(0x1DE)) {
            in.NextState();
            in.MotionSpeed(1.25f);
        }

        break;
    }
    case 4:
        runDust(168.0f, 1, 0.0f);
        in.PoseAxes(in.em.Count(), kTurnKeys, kTurnAngleKeys, 7, 2);

        if (in.Reached(0x26C)) {
            in.NextState();
            in.em.Count(0);
            in.Motion(0x17, 0, 0xD6);
            in.Place(12.93f, 45.0f, -224.16f);
            in.PoseAxes(0, kLeapKeys, 0, 5, 0);
            in.Face(0.0f, 125.25f, 0.0f);
        }

        break;
    case 5:
        runDust(214.0f, 3, 222.0f);
        in.PoseAxes(in.em.Count(), kLeapKeys, 0, 5, 0);

        if (in.Reached(0x280)) {
            in.NextState();
            in.Motion(0x18, 10, 10);
        }

        break;
    case 6:
        stomp(40.0f);
        spray(40.0f, 52.0f, 0, 0x23);
        spray(48.0f, 60.0f, 1, 0x1C);
        spray(72.0f, 84.0f, 0, 0x23);
        in.PoseAxes(in.em.Count(), kLeapKeys, 0, 5, 0);

        if (in.Reached(0x2BE)) {
            in.NextState();
            in.Motion(0x18, 0, 0x48);
            in.Place(-300.4792f, 35.0f, -718.8161f);
            in.Face(0.0f, 126.25f, 0.0f);
        }

        break;
    case 7:
        spray(72.0f, 84.0f, 0, 0x23);
        stomp(72.0f);

        if (in.Reached(0x34C)) {
            in.NextState();
            in.Motion(2, 6, 0);
            in.Face(0.0f, 126.25f, 0.0f);
        }

        break;
    case 8:
        if (in.Reached(0x428)) {
            in.NextState();
            in.Motion(0x12, 0, 0);
            in.Place(-1083.3802f, 35.0f, -828.7873f);
            in.Face(0.0f, 86.25f, 0.0f);
        }

        break;
    case 9:
        stand(0x4B6, 0x6E, -1483.3368f, -866.4448f, true);
        break;
    case 10:
        stand(0x4F2, 0xA0, -1479.4661f, -854.7481f, false);
        break;
    case 11:
        stand(0x526, 0xA6, -1484.9506f, -855.1076f, true);
        break;
    case 12:
        stand(0x5CE, 0x164, -1442.4564f, -793.2897f, true);
        break;
    case 13:
        if (in.Reached(0x624)) {
            in.NextState();
            in.Motion(2, 6, 0);
            in.Place(-1343.641f, 45.0f, -845.8662f);
            in.Face(0.0f, 86.25f, 0.0f);
        }

        break;
    }
}

// kind 4 (0x801C8228): bursts out of the sand (states 5-7, posed by its
// count), then digs and charges; sand (kind 0x80) every 4 demo steps while
// its motion is in [20, 68] (state 10) and [40, 88] (state 12).
void Em004IntroPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x20);
    Intro in(ctx, frame);
    const uint32_t work = in.work;
    constexpr uint32_t kRiseKeys = 0x805B5418, kLeapKeys = 0x805B5520, kLeapAngleKeys = 0x805B55D0;
    in.Acted();
    Memory::Write8(work + kActed2, 1);

    // The original adds 1 to the count after the switch and poses by the count
    // before it: here the count the step's ticks reach, less that 1.
    const int32_t posed = in.em.Count() + int32_t(GameStep::Ticks()) - 1;

    const auto sand = [&](float from, float to) {
        if (in.At(from, 3, to) && in.DemoEvery(4)) {
            in.Offset(0.0f, 0.0f, 0.0f);
            in.Spawn(1.0f, 0x80, 0x35);
        }
    };

    switch (in.State()) {
    case 0:
        in.NextState();
        in.SetUp(0);
        in.Motion(1, 0, 0);
        in.StartDemo();
        break;
    case 1:
        if (in.Reached(0x48)) {
            in.NextState();
            in.EnterDemo();
            in.Scale(0.0f);
            in.Motion(0xCC, 0, 0x46);
            in.Place(1007.335f, 1522.6617f, -2913.5522f);
            in.Face(0.0f, -7.5f, 0.0f);
        }

        break;
    case 2:
        if (in.Reached(0xD6)) {
            in.NextState();
            in.StartDemo();
        }

        break;
    case 3:
        if (in.Reached(0x134)) {
            in.NextState();
            in.Motion(8, 0, 0x60);
            in.SetUp(0);
            in.EnterDemo();
            in.Place(427.3593f, 7.9003f, -465.4141f);
            in.Face(0.0f, -134.75f, 0.0f);
        }

        break;
    case 4:
        if (in.Reached(0x178)) {
            in.NextState();
            in.Motion(0xDE, 0, 0x5E);
            in.Scale(0.0f);
            in.Place(-588.8713f, 867.8231f, -2503.702f);
            in.Face(-6.0f, 181.3f, 0.0f);
        }

        break;
    case 5:
        if (in.Reached(0x2D2)) {
            in.NextState();
            in.Motion(0xD3, 0, 0);
            in.em.Count(0);
            in.Place(-255.67f, 807.8f, -2485.78f);
            in.PoseAxes(0, kRiseKeys, 0, 5, 0);
            in.Face(-90.0f, -15.8f, 0.0f);
            Memory::Write8(work + kHold785, 1);
        }

        break;
    case 6:
        in.PoseAxes(posed, kRiseKeys, 0, 5, 0);
        Memory::Write8(work + kHold785, 1);

        if (in.Reached(0x31E)) {
            in.NextState();
            in.Motion(0xD4, 0, 0);
        }

        break;
    case 7:
        in.PoseAxes(posed, kLeapKeys, kLeapAngleKeys, 7, 1);

        if (in.Reached(0x340)) {
            in.NextState();
            in.Motion(0xD5, 0, 8);
            in.SetUp(0);
            in.Place(-57.0105f, 1.4f, -1597.74f);
        }

        break;
    case 8:
        if (in.Reached(0x38E)) {
            in.NextState();
            in.Motion(1, 6, 0);
        }

        break;
    case 9:
        if (in.Reached(0x3AA)) {
            in.NextState();
            in.Motion(0x1A, 0, 0);
            in.Place(-605.2861f, 1.4f, -975.6735f);
        }

        break;
    case 10:
        sand(20.0f, 68.0f);

        if (in.Reached(0x430)) {
            in.NextState();
            in.Motion(0x1A, 0, 0x72);
            in.Place(-447.6424f, 1.4f, -1413.7517f);
            in.Face(0.0f, 9.75f, 0.0f);
        }

        break;
    case 11:
        if (in.Ended()) {
            in.NextState();
            in.Motion(0x21, 0, 0);
        }

        break;
    case 12:
        if (in.At(2.0f))
            in.Spawn(1.0f, 0x81, 0x35, false);

        sand(40.0f, 88.0f);

        if (in.Reached(0x52C)) {
            in.NextState();
            in.Motion(8, 0, 0x1E);
            in.Place(-421.4869f, 1.4f, -1261.5358f);
            in.Face(0.0f, 9.75f, 0.0f);
        }

        break;
    case 13:
        if (in.Reached(0x77A)) {
            in.NextState();
            in.Motion(1, 0, 0xBD);
            in.Place(-605.7778f, 1.4f, -802.101f);
            in.Face(0.0f, 165.0f, 0.0f);
        }

        break;
    }

    in.em.CountTicks();
}

// em026 (0x8032F5B4): drops in (state 3, posed by its count: one table for
// its first 40 steps, then another).
void Em026IntroPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x20);
    Intro in(ctx, frame);
    constexpr uint32_t kDropKeys = 0x805E08F8, kLandKeys = 0x805E09C8;
    in.em.CountTicks();
    const int32_t count = in.em.Count();

    switch (in.State()) {
    case 0:
        in.em.State(1);
        in.SetUp(0);
        in.Motion(1, 0, 0);
        in.StartDemo();
        break;
    case 1:
        if (in.Reached(0x61E)) {
            in.NextState();
            in.em.Count(0);
        }

        break;
    case 2:
        if (in.Reached(0x632)) {
            in.NextState();
            in.EnterDemo();
            in.Scale(0.0f);
            in.Motion(4, 0, 0);
            in.Place(-554.49f, 60.39f, -1710.73f);
            in.PoseAxes(in.em.Count(), kDropKeys, 0, 5, 0);
            in.Face(0.0f, 69.38f, 0.0f);
        }

        break;
    case 3:
        if (count * 2 < 0x50)
            in.PoseAxes(count, kDropKeys, 0, 5, 0);
        else
            in.Pose(count, kLandKeys, 0);

        if (in.At(14.0f)) {
            in.Offset(0.0f, 0.0f, 20.0f);
            in.Spawn(1.0f, 199, 7);
        }

        if (in.Reached(0x710)) {
            in.NextState();
            in.EnterDemo();
            in.Motion(9, 0, 0x1A);
            in.Place(-459.8198f, 14.8009f, -1862.3315f);
            in.Face(0.0f, -69.375f, 0.0f);
        }

        break;
    }
}

// em023 (0x802FE5B4): swims past twice (splash kind 0xCC), then struggles in
// states 3-9 (spray 0x5E every 4 demo steps while its motion is in [76, 94]).
void Em023IntroPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x20);
    Intro in(ctx, frame);
    Memory::Write8(in.work + kHold785, 1);

    const auto splash = [&](float at) {
        if (in.At(at)) {
            in.Offset(0.0f, 0.0f, 20.0f);
            in.Spawn(0.7f, 0xCC, 0x13);
        }
    };

    const uint32_t state = in.State();

    switch (state) {
    case 0:
        in.NextState();
        in.SetUp(2);
        in.Motion(9, 0, 0x3C);
        in.Place(1321.0f, -556.0f, -3320.3f);
        in.Face(4.0f, -90.0f, 0.0f);
        break;
    case 1:
        splash(152.0f);

        if (in.Reached(0x17E)) {
            in.NextState();
            in.Motion(9, 0, 0);
            in.Place(835.7f, -1060.0f, -2950.0f);
            in.Face(6.0f, -90.0f, 0.0f);
        }

        break;
    case 2:
        splash(80.0f);

        if (in.Reached(0x30A)) {
            in.NextState();
            in.Motion(0xF, 0, 0);
        }

        break;
    case 3:
    case 4:
    case 5:
    case 6:
    case 7:
    case 8:
    case 9: {
        static constexpr uint32_t kEnd[] = {0x36A, 0x38E, 0x3B2, 0x3D6, 0x3FA, 0x41E, 0x448};

        if (in.At(76.0f, 3, 94.0f) && in.DemoEvery(4)) {
            in.Offset(0.0f, -50.0f, 0.0f);
            in.Spawn(1.6f, 0x5E, 3);
        }

        in.Pose(in.Demo(), 0x805D9BE0, 0x805DA5B0);

        if (!in.Reached(kEnd[state - 3]))
            break;

        in.NextState();

        if (state < 9)
            in.Motion(0xF, 0x10, 0x4C);
        else
            in.Motion(0x70, 0, 0x16C);

        break;
    }
    case 10:
        if (in.At(404.0f)) {
            in.Offset(0.0f, -30.0f, 60.0f);
            in.Spawn(0.7f, 3, 0x14);
        }

        in.Pose(in.Demo(), 0x805DAFC0, 0x805DB0D0);

        if (in.Reached(0x4B6)) {
            in.NextState();
            in.Motion(0x70, 0, 0);
            in.Face(169.4f, 251.15f, 190.8f);
        }

        break;
    case 11:
        in.PoseAxes(in.Demo(), 0x805DB270, 0x805DB2D0, 7, 2);

        if (in.Reached(0x58A)) {
            in.NextState();
            in.StartDemo();
        }

        break;
    }
}

// em016/017/021 (0x8018B47C): surfaces and wades out (splashes 0xCC, wake
// 0xCD every 4 demo steps while its motion is in [8, 40] or [48, 112]), then
// shakes off on land.
void LudrothIntroPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x20);
    Intro in(ctx, frame);
    const uint32_t work = in.work;
    constexpr uint32_t kSwimKeys = 0x805AC578, kWadeKeys = 0x805AC6F0, kShoreKeys = 0x805AC9B0;
    in.Acted();

    const auto wake = [&](float from, float to, float scale) {
        if (in.At(from, 3, to) && in.DemoEvery(4)) {
            in.Offset(0.0f, 0.0f, -100.0f);
            in.Spawn(scale, 0xCD, 0x26);
        }
    };

    const auto splash = [&](float at, float z, float scale) {
        if (in.At(at)) {
            in.Offset(0.0f, 0.0f, z);
            in.Spawn(scale, 0xCC, 0x16);
        }
    };

    switch (in.State()) {
    case 0:
        in.NextState();
        in.SetUp(2);
        in.Motion(0x28, 0, 0);
        in.StartDemo();
        in.Place(7946.29f, -637.43f, 2108.5f);
        in.Face(0.0f, -104.25f, 0.0f);
        break;
    case 1:
        if (in.Reached(400)) {
            in.NextState();
            in.Motion(0x37, 0, 0);
            in.EnterDemo();
        }

        break;
    case 2:
        splash(30.0f, 50.0f, 0.7f);

        if (in.Reached(0x1DE)) {
            in.NextState();
            in.MotionEx(0x2C, 0x18, 0, 1);
            in.Place(7296.27f, -637.43f, 1943.41f);
        }

        break;
    case 3:
        wake(8.0f, 40.0f, 1.0f);

        if (in.Reached(0x1FA)) {
            in.NextState();
            in.Motion(0x2C, 0, 0x1E);
            in.Place(7113.8f, -587.43f, 3348.87f);
            in.Face(0.0f, -104.25f, 0.0f);
            in.PoseAxes(in.Demo(), kSwimKeys, 0, 5, 0);
        }

        break;
    case 4:
        wake(48.0f, 112.0f, 0.8f);
        splash(182.0f, 80.0f, 0.7f);
        Memory::WriteFloat32(work + Em::kPosition + 4, -587.43f);
        in.PoseAxes(in.Demo(), kSwimKeys, 0, 5, 0);

        if (in.Reached(0x2B2)) {
            in.NextState();
            in.MotionEx(0x29, 0xE, 0, 1);
        }

        break;
    case 5:
        wake(8.0f, 40.0f, 1.0f);
        wake(48.0f, 112.0f, 0.8f);
        splash(182.0f, 80.0f, 0.7f);
        in.Pose(in.Demo(), kWadeKeys, 0);

        if (in.Reached(0x382)) {
            in.NextState();
            in.Scale(0.0f);
            in.MotionEx(0x3C, 0xE, 0x6C, 1);
            in.Place(7085.349f, -457.9968f, 3055.059f);
        }

        break;
    case 6:
        splash(108.0f, 100.0f, 1.0f);
        in.Pose(in.Demo(), kWadeKeys, 0);

        if (in.Reached(0x3B0)) {
            in.NextState();
            in.Motion(0x40, 0, 0);
            in.MotionSpeed(0.6f);
            in.Place(1295.11f, 333.97f, -473.6f);
            in.Face(0.0f, -91.71f, 0.0f);
            in.PoseAxes(in.Demo(), kShoreKeys, 0, 3, 0);
            in.JointFx(0.8f, 3, 0x11);
        }

        break;
    case 7:
        if (in.At(4.0f)) {
            in.Offset(0.0f, -75.0f, 0.0f);
            in.Spawn(0.6f, 0x32, 3);
        }

        in.PoseAxes(in.Demo(), kShoreKeys, 0, 3, 0);

        if (in.Reached(0x420)) {
            in.NextState();
            in.SetUp(0);
            in.Motion(0x3D, 0, 0);
            in.Place(530.997f, 398.066f, -1397.1093f);
            in.Face(0.0f, -165.75f, 0.0f);
        }

        break;
    case 8:
        if (in.At(2.0f))
            in.JointFx(0.4f, 0x15, 0x24);

        if (in.Reached(0x456)) {
            in.NextState();
            in.Motion(1, 0, 0);
            in.Place(404.177f, 47.9447f, -1965.6384f);
        }

        break;
    case 9:
        if (in.Reached(0x4A6)) {
            in.NextState();
            in.MotionEx(0x1A, 10, 0, 1);
        }

        break;
    case 10:
        if (in.Reached(0x582)) {
            in.NextState();
            in.MotionEx(1, 0x1E, 0, 1);
        }

        break;
    }
}

// kind 8 (0x80162564): runs on, throws two shells on each even pass of its
// motion in state 10 (frames 102 and 106, a direction and an angle each),
// then roars (breath kind 1 every 8 demo steps while its motion is in
// [164, 264]).
void Em008IntroPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x40);
    Intro in(ctx, frame);
    const uint32_t work = in.work;
    const uint32_t params = frame[0x18]; // the shell parameters (0x20 bytes)
    in.game.Call<ShellParams>({params});

    const auto place = [&](float z) { in.Place(-574.857f, -26.6507f, z); };

    const auto resetYaw = [&] { Memory::Write32(work + kAngleY, 0); };

    // One shell: a direction (0, 0x10, 0x20, 0x30) and an angle.
    const auto shell = [&](bool raised, uint32_t direction, uint32_t angle) {
        if (raised)
            in.Offset(0.0f, 75.0f, 75.0f);
        else
            in.Offset(0.0f, 0.0f, 0.0f);

        in.game.Call<ShellAim>({params, direction, 4, in.at, 0, angle});
        const uint32_t fn = Memory::Read32(Memory::Read32(kShellTable) + kShellThrow);
        in.game(fn, {work, params, 4});
    };

    // The two shells of each even pass (+6) of the motion, at frames 102 and 106.
    struct Throw {
        bool raised;
        uint32_t direction, angle;
    };

    static constexpr Throw kThrows[8][2] = {
        {{true, 0, 0x2000}, {true, 0x10, 0x6000}},
        {{false, 0x10, 0x1000}, {false, 0x30, 0xE000}},
        {{false, 0, 0x4500}, {false, 0x30, 0x3500}},
        {{false, 0x10, 0xF000}, {false, 0x20, 0xA000}},
        {{true, 0x20, 0x8000}, {true, 0x30, 0xC500}},
        {{false, 0x30, 0}, {false, 0, 0xC500}},
        {{false, 0x10, 0x9000}, {false, 0x20, 0x5000}},
        {{false, 0, 0xA500}, {false, 0x30, 0x2000}},
    };

    switch (in.State()) {
    case 0:
        in.NextState();
        in.em.Count(0);
        in.SetUp(0);
        in.Motion(1, 0, 0);
        Memory::Write32(work + kAngleX, 0);
        Memory::Write32(work + kAngleY, 0);
        Memory::Write32(work + kAngleZ, 0);
        place(-2456.36f);
        in.StartDemo();
        Memory::Write8(work + kRoamMask, 0x3F);
        break;
    case 1:
        if (in.Reached(0x4A)) {
            in.NextState();
            in.EnterDemo();
            in.Motion(10, 0, 0);
            resetYaw();
            place(-2456.36f);
        }

        break;
    case 2:
        if (in.Ended()) {
            in.NextState();
            in.Motion(0xB, 4, 0);
        }

        break;
    case 3:
        if (in.Reached(0xDC)) {
            in.NextState();
            in.Motion(0xB, 0, 0x26);
            resetYaw();
            place(412.3778f);
        }

        break;
    case 4:
        if (in.Ended()) {
            in.NextState();
            in.MotionEx(1, 0xC, 0, 1);
            resetYaw();
            place(477.3114f);
        }

        break;
    case 5:
        if (in.Reached(0x1A4)) {
            in.NextState();
            Memory::Write8(work + kSubState, 0);
            in.Motion(0x24, 0, 0);
            resetYaw();
            place(515.1197f);
        }

        break;
    case 6:
        if (Memory::Read8(work + kSubState) == 0 && in.Reached(0x280)) {
            Memory::Write8(work + kSubState, 1);
            in.game.Call<Em008Signal>({work});
        }

        if (in.Reached(0x2E8)) {
            in.NextState();
            in.MotionEx(0x29, 10, 0, 1);
        }

        break;
    case 7:
        if (in.Reached(0x312)) {
            in.NextState();
            in.MotionEx(0x70, 0, 0x42, 0);
        }

        break;
    case 8:
        if (in.Reached(0x382)) {
            in.NextState();
            in.Motion(0x70, 0, 0xB8);
        }

        break;
    case 9:
        if (in.Reached(0x38E)) {
            in.NextState();
            Memory::Write8(work + kSubState, 0);
            in.MotionEx(0x19, 6, 0, 1);
            in.em.Count(0xF);
        }

        break;
    case 10: {
        if (in.At(100.0f)) {
            in.Offset(0.0f, 75.0f, 75.0f);
            in.Spawn(1.0f, 9, 0x14);
        }

        if (in.At(100.0f)) {
            in.Offset(0.0f, 0.0f, 0.0f);
            in.Spawn(1.0f, 9, 0x11);
        }

        if (in.At(100.0f)) {
            in.Offset(0.0f, 0.0f, 0.0f);
            in.Spawn(1.0f, 9, 3);
        }

        const uint32_t pass = Memory::Read8(work + kSubState);

        if (pass % 2 == 0 && pass <= 14) {
            const Throw* pair = kThrows[pass / 2];

            if (in.At(102.0f))
                shell(pair[0].raised, pair[0].direction, pair[0].angle);

            if (in.At(106.0f))
                shell(pair[1].raised, pair[1].direction, pair[1].angle);
        }

        if (in.Ended())
            Memory::Write8(work + kSubState, uint8_t(Memory::Read8(work + kSubState) + 1));

        if (in.Reached(0x46C)) {
            in.NextState();
            in.Motion(0x2A, 0, 0);
        }

        break;
    }
    case 11:
        if (in.Ended()) {
            in.NextState();
            in.Motion(0xF, 6, 0);
        }

        break;
    case 12:
        if (in.At(160.0f))
            in.Rumble(0x14, 10);

        if (in.At(160.0f)) {
            in.Offset(0.0f, -50.0f, 100.0f);
            in.Spawn(1.0f, 0, 0x13);
        }

        if (in.At(164.0f, 3, 264.0f) && in.DemoEvery(8)) {
            in.Offset(0.0f, -50.0f, 100.0f);
            in.Spawn(1.0f, 1, 0x13);
        }

        if (in.Ended()) {
            in.NextState();
            in.Motion(2, 6, 0);
        }

        break;
    }
}

// kind 6 (0x801BF050): rises out of the mud three times (mud kinds 0x69/0x6B
// at frames 32/38, 0x1A at 60/72/84), shakes off (0x28/0x20 every 3 demo
// steps while its motion is in [60, 100]), then roars: steam (kind 1) every
// 8 ticks while its motion is in [116, 286], counted at +0x20.
void Em006IntroPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x20);
    Intro in(ctx, frame);
    const uint32_t work = in.work;
    in.Acted();

    const auto hold = [&] { Memory::Write16(work + kHold79C, 1); };

    const auto drip = [&] {
        if (in.At(2.0f))
            in.JointFx(1.0f, 3, 0x27);
    };

    const auto mudSplash = [&] {
        if (in.At(32.0f) || in.At(38.0f)) {
            in.Offset(0.0f, 0.0f, 0.0f);
            in.Spawn(1.0f, 0x69, 9);
            in.Spawn(1.0f, 0x6B, 0x10);
        }
    };

    const auto mudDrops = [&] {
        if (in.At(60.0f) || in.At(72.0f) || in.At(84.0f)) {
            in.Offset(0.0f, 0.0f, 0.0f);
            in.Spawn(1.0f, 0x1A, 0x28);
        }
    };

    const auto holdBefore10 = [&] {
        if (in.At(10.0f, 2))
            hold();
    };

    // The roar's steam: every 8 ticks while the motion is in [116, 286].
    const auto steam = [&] {
        if (in.At(116.0f, 3, 286.0f)) {
            if (in.em.CountEvery(8))
                in.Spawn(1.0f, 1, 0x18);

            in.em.CountTicks();
        }
    };

    switch (in.State()) {
    case 0:
        in.NextState();
        in.SetUp(0);
        in.Motion(1, 0, 0);
        in.StartDemo();
        break;
    case 1:
        if (in.Reached(0x2EE)) {
            in.NextState();
            in.EnterDemo();
            in.Motion(0xD8, 0, 0);
            in.Place(2288.7334f, -202.2364f, -3699.9988f);
            in.Face(0.0f, -15.0f, 0.0f);
            hold();
        }

        break;
    case 2:
        drip();
        mudSplash();
        mudDrops();
        holdBefore10();

        if (in.Reached(0x302)) {
            in.NextState();
            in.StartDemo();
            in.Place(1967.0365f, -1405.2365f, -3238.8828f);
        }

        break;
    case 3:
        drip();
        mudSplash();
        mudDrops();

        if (in.Reached(0x30C)) {
            in.NextState();
            in.EnterDemo();
            in.Motion(0xD8, 0, 0);
            in.Place(2134.7334f, -202.2364f, -3878.9988f);
            hold();
        }

        break;
    case 4:
        drip();
        mudDrops();
        mudSplash();

        if (in.At(60.0f, 3, 100.0f) && in.DemoEvery(3)) {
            in.JointFx(1.0f, 0x28, 0x2B);
            in.JointFx(1.0f, 0x20, 0x2B);
        }

        if (in.At(120.0f)) {
            in.AtJoint(0x21);
            in.game.Call<ChangeScale>({work});
            in.Dust(0.8f * in.game.F1(), 3, Memory::Read32(work + kAngleY));
        }

        if (in.At(146.0f)) {
            in.AtJoint(3);
            in.game.Call<SpotEffect>({work, in.at, 0}, {1.0f});
        }

        holdBefore10();

        if (in.Reached(0x3DA)) {
            in.NextState();
            in.Place(1072.77f, 103.0f, 179.3f);
            in.Face(0.0f, -15.0f, 0.0f);
        }

        break;
    case 5:
        if (in.Reached(0x408)) {
            in.NextState();
            in.Motion(2, 0, 0);
        }

        break;
    case 6:
        if (in.Reached(0x496)) {
            in.NextState();
            in.Motion(0xD3, 0x14, 0xC);
            in.em.Count(0);
        }

        break;
    case 7:
        if (in.At(110.0f))
            in.Rumble(0x19, 0x78);

        in.Offset(0.0f, 20.0f, 50.0f);

        if (in.At(110.0f))
            in.Spawn(1.0f, 0, 0x18);

        steam();

        if (in.Reached(0x56A)) {
            in.NextState();
            in.Motion(0xD3, 0, 0x78);
            in.em.Count(0);
        }

        break;
    case 8:
        in.Offset(0.0f, 20.0f, 50.0f);
        steam();

        if (in.Reached(0x5FE)) {
            in.NextState();
            in.Motion(2, 0, 0);
        }

        break;
    }
}

// kinds 1/2 (0x80150830): crawls down a wall and across the ceiling (posed by the
// demo clock), drops and roars: steam (kind 1) every 8 ticks while its motion
// is in [86, 204], counted at +0x20.
void Em001IntroPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x20);
    Intro in(ctx, frame);
    constexpr uint32_t kWallKeys = 0x805A389C, kCeilingKeys = 0x805A3A40, kCeilingAngleKeys = 0x805A4020;
    constexpr uint32_t kDropKeys = 0x805A42A0, kHangKeys = 0x805A43D0, kHangAngleKeys = 0x805A4590;
    constexpr uint32_t kFloorKeys = 0x805A4650, kFloorAngleKeys = 0x805A4780;

    const auto wall = [&] { in.PoseAxes(in.Demo(), kWallKeys, 0, 6, 0); };

    const auto ceiling = [&] { in.Pose(in.Demo(), kCeilingKeys, kCeilingAngleKeys); };

    const auto hang = [&] { in.PoseAxes(in.Demo(), kHangKeys, kHangAngleKeys, 7, 2); };

    const auto floor = [&] { in.PoseAxes(in.Demo(), kFloorKeys, kFloorAngleKeys, 5, 2); };

    // The next state and motion when the motion ends.
    const auto afterMotion = [&](uint32_t motion, uint32_t blend) {
        if (!in.Ended())
            return false;

        in.NextState();
        in.Motion(motion, 0, blend);
        return true;
    };

    switch (in.State()) {
    case 0:
        in.NextState();
        in.game.Call<0x801302AC>({in.work}); // its f1 goes on to SetScale
        in.game.Call<SetScale>({in.work});
        in.Motion(0x1A, 0, 0);
        in.Face(0.0f, 180.0f, 0.0f);
        in.StartDemo();
        break;
    case 1:
        if (in.Reached(0x15A)) {
            in.NextState();
            in.Motion(0x1A, 0, 0);
            in.Place(510.33f, -2053.39f, 3316.39f);
            wall();
            in.EnterDemo();
        }

        break;
    case 2:
        wall();
        afterMotion(0x1A, 0);
        break;
    case 3:
        wall();

        if (in.At(78.0f)) {
            in.NextState();
            in.MotionEx(0x3D, 0x1E, 0, 1);
        }

        break;
    case 4:
        wall();

        if (in.Reached(0x280)) {
            in.NextState();
            in.Motion(0x1F, 0, 0x16);
            ceiling();
        }

        break;
    case 5:
        ceiling();
        afterMotion(0x20, 0);
        break;
    case 6:
        ceiling();
        afterMotion(0x1F, 0);
        break;
    case 7:
        ceiling();

        if (in.At(74.0f)) {
            in.NextState();
            in.Motion(0x3D, 10, 0);
        }

        break;
    case 8:
        ceiling();

        if (in.Reached(0x3EE)) {
            in.NextState();
            in.Motion(0xDB, 0, 0x28);
            in.Pose(in.Demo(), kDropKeys, 0);
            in.Face(0.0f, 0.0f, 0.0f);
        }

        break;
    case 9:
        in.Pose(in.Demo(), kDropKeys, 0);

        if (in.Reached(0x434)) {
            in.NextState();
            in.Motion(0x1B, 0, 4);
            hang();
        }

        break;
    case 10:
        hang();

        if (in.Reached(0x468)) {
            in.NextState();
            in.Motion(0xCE, 0, 2);
        }

        break;
    case 11:
        hang();
        afterMotion(0xCF, 2);
        break;
    case 12:
        hang();

        if (in.Reached(0x506)) {
            in.NextState();
            in.MotionEx(0xCF, 0x14, 0x22, 1);
            in.Place(-1503.96f, 994.4f, -2969.05f);
            floor();
        }

        break;
    case 13:
    case 14:
        floor();
        afterMotion(0x1B, 0);
        break;
    case 15:
        floor();

        if (afterMotion(0x1C, 0))
            in.em.Count(0);

        break;
    case 16:
        if (in.At(86.0f))
            in.Rumble(0x1B, 10);

        in.Offset(0.0f, -20.0f, 80.0f);

        if (in.At(86.0f))
            in.Spawn(1.0f, 0, 0x1A);

        // The roar's steam: every 8 ticks while the motion is in [86, 204].
        if (in.At(86.0f, 3, 204.0f)) {
            if (in.em.CountEvery(8))
                in.Spawn(1.0f, 1, 0x1A);

            in.em.CountTicks();
        }

        floor();
        break;
    }
}

// em015 (0x8017E56C): rises from the water, turns, and roars: steam (kind 1)
// every 8 ticks while its motion is in [186, 226], counted at +0x20.
void Em015IntroPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x20);
    Intro in(ctx, frame);
    const uint32_t work = in.work;
    const auto spot = [&] { in.Place(1278.0f, 0.0f, 890.0f); };

    switch (in.State()) {
    case 0:
        in.NextState();
        in.SetUp(0);
        in.Motion(0x14, 0, 0);
        spot();
        in.Face(0.0f, 196.0f, 0.0f);
        break;
    case 1:
        if (in.Reached(0x78)) {
            in.NextState();
            in.Motion(4, 0, 0);
        }

        break;
    case 2:
        if (in.At(240.0f)) {
            in.Offset(0.0f, 180.0f, 60.0f);
            in.game.Call<0x8010757C>({work, in.at, 0x19, 10}, {2.5}); // an effect at the offset
        }

        if (in.Reached(0x296)) {
            in.NextState();
            in.Motion(5, 6, 0);
            in.MotionSpeed(0.44f);
            spot();
        }

        break;
    case 3:
        if (in.Reached(0x306)) {
            in.NextState();
            in.Motion(7, 0, 10);
            in.MotionSpeed(0.52f);
            spot();
            in.Face(0.0f, 178.0f, 0.0f);
        }

        break;
    case 4:
        if (in.Reached(0x34A)) {
            in.NextState();
            in.Motion(7, 2, 0x2E);
            spot();
            in.Face(0.0f, 178.0f, 0.0f);
        }

        break;
    case 5:
        if (in.Reached(0x458)) {
            in.NextState();
            in.Motion(0x18, 0, 0);
            spot();
        }

        break;
    case 6:
        in.game.Call<0x80133EA0>({work, 0x4000}, {0.0, 40.0}); // turns while the motion is in [0, 40]

        if (in.At(70.0f)) {
            in.NextState();
            in.Motion(0x18, 0xC, 0x54);
            spot();
            in.Face(0.0f, 268.0f, 0.0f);
        }

        break;
    case 7:
        if (in.Reached(0x4BC)) {
            in.NextState();
            Memory::Write8(work + kSubState, 0);
            in.Motion(0x14, 0, 0);
            spot();
            in.em.Count(0);
        }

        break;
    case 8:
        if (Memory::Read8(work + kSubState) != 0)
            break;

        if (in.At(180.0f)) {
            in.Rumble(0x1A, 10);
            in.Offset(0.0f, -50.0f, 100.0f);
            in.Spawn(1.0f, 0, 0x19);
        }

        // The roar's steam: every 8 ticks while the motion is in [186, 226].
        if (in.At(186.0f, 3, 226.0f)) {
            if (in.em.CountEvery(8)) {
                in.Offset(0.0f, -50.0f, 100.0f);
                in.Spawn(1.0f, 1, 0x19);
            }

            in.em.CountTicks();
        }

        if (in.Ended())
            Memory::Write8(work + kSubState, uint8_t(Memory::Read8(work + kSubState) + 1));

        break;
    }
}

// kind 5 (0x801D42F0): falls in from above (posed by the demo clock), lands,
// walks on and roars: steam (kind 1) every 8 ticks while its motion is in
// [142, 264], counted at +0x20.
void Em005IntroPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x20);
    Intro in(ctx, frame);
    const uint32_t work = in.work;
    constexpr uint32_t kFallKeys = 0x805B69B8, kFallAngleKeys = 0x805B6B68, kWalkKeys = 0x805B6BA8;

    // 0x80130450 (work; f1), inlined: the height above the ground.
    const auto height = [&](float h) {
        Memory::WriteFloat32(work + 0x1AC, h);
        Memory::Write8(work + 0x1DF, 1);
    };

    const auto heightAboveGround = [&] { height(Memory::ReadFloat32(work + Em::kPosition + 4) - Memory::ReadFloat32(work + kGroundY)); };

    const auto lean = [&] {
        in.Face(32.3f, 40.3f, 21.6f);
        Memory::Write8(work + kHold785, 1);
    };

    const auto fall = [&] {
        in.PoseAxes(in.Demo(), kFallKeys, kFallAngleKeys, 7, 2);
        heightAboveGround();
    };

    const auto walk = [&] {
        in.Face(0.0f, -168.7f, 0.0f);
        in.Pose(in.Demo(), kWalkKeys, 0);
        heightAboveGround();
    };

    switch (in.State()) {
    case 0:
        in.NextState();
        in.em.Count(0);
        in.game.Call<0x801302AC>({work}); // its f1 goes on to SetScale
        in.game.Call<SetScale>({work});
        in.Motion(1, 0, 0);
        in.StartDemo();
        break;
    case 1:
        if (in.Reached(0x10E)) {
            in.NextState();
            in.EnterDemo();
            in.Motion(1, 0, 0);
            in.Face(32.3f, 40.3f, 21.6f);
            in.Place(-2494.9f, 7556.9f, -13608.3f);
        }

        break;
    case 2:
        lean();

        if (in.Reached(0x1B4)) {
            in.NextState();
            in.Motion(2, 10, 0);
        }

        break;
    case 3:
        lean();

        if (in.At(180.0f, 1)) {
            in.NextState();
            in.Motion(8, 8, 0);
            in.MotionSpeed(1.7f);
        }

        break;
    case 4:
        lean();

        if (in.At(26.0f)) {
            in.Offset(0.0f, 0.0f, 200.0f);
            in.game.Call<0x80104A34>({work, 1, 5, 0, in.at}, {0.7}); // an effect at the offset
        }

        if (in.Reached(0x294)) {
            in.NextState();
            in.SetUp(0);
            in.Motion(0xDA, 0, 0x56);
            in.MotionSpeed(0.5f);
            in.Face(0.0f, 13.52f, 0.0f);
            fall();
        }

        break;
    case 5:
        fall();

        if (in.Reached(0x2AE)) {
            in.NextState();
            in.Motion(0xDB, 0, 0);
        }

        break;
    case 6:
        fall();

        // The landing's dust at its feet.
        if (in.At(44.0f)) {
            in.game.Call<JointPosition>({work, 3, in.at});
            Memory::WriteFloat32(in.at + 4, 5.0f + Memory::ReadFloat32(work + Em::kPosition + 4));
            in.Dust(1.3f, 5, 0);
        }

        if (in.Ended()) {
            in.NextState();
            in.Motion(2, 4, 0);
            in.MotionSpeed(0.8f);
        }

        break;
    case 7:
        fall();

        if (in.At(72.0f, 1)) {
            in.NextState();
            in.MotionSpeed(1.0f);
        }

        break;
    case 8:
        fall();

        if (in.Reached(0x3FC)) {
            in.NextState();
            in.Motion(2, 0, 0x110);
            in.Face(0.0f, 191.4f, 0.0f);
            in.Place(1804.0f, 0.0f, -4165.6f);
            height(0.0f);
        }

        break;
    case 9:
        if (in.At(286.0f, 1)) {
            in.NextState();
            in.Motion(0xDF, 10, 0);
        }

        break;
    case 10:
        if (in.Reached(0x42A)) {
            walk();
        } else if (!(Memory::ReadFloat32(work + 0x24 + 0x44) > 0.0f)) {
            // Not cross-fading (0x8012F9AC, inlined): walks by its motion's speed table.
            in.game.Call<0x801356A8>({work, 0x805B5A98});
            Memory::WriteFloat32(work + 0x318, in.game.F1());
            in.game.Call<0x80135558>({work, work + kAngleX}); // EnemyAddTurnedVelocity
        }

        if (in.Reached(0x454)) {
            in.NextState();
            in.Motion(0xDF, 0, 0);
            in.MotionSpeed(0.35f);
        }

        break;
    case 11:
        walk();

        if (in.At(17.0f, 1)) {
            in.NextState();
            in.MotionSpeed(1.0f);
        }

        break;
    case 12:
        walk();

        if (in.Reached(0x49C)) {
            in.NextState();
            in.Face(0.0f, 193.0f, 0.0f);
            in.Place(1985.8f, 0.0f, -6341.3f);
        }

        break;
    case 13:
        if (in.Reached(0x5AA)) {
            in.NextState();
            in.Motion(0x12, 0, 0);
            in.MotionSpeed(0.39f);
        }

        break;
    case 14:
        if (in.Reached(0x6E4)) {
            in.NextState();
            in.Motion(5, 0, 0);
            in.Face(0.0f, 193.0f, 0.0f);
            in.Place(1683.3f, 0.0f, -6181.6f);
            in.em.Count(0);
        }

        break;
    case 15:
        if (in.At(124.0f))
            in.Rumble(0x1D, 10);

        if (in.At(124.0f)) {
            in.Offset(0.0f, 0.0f, 200.0f);
            in.Spawn(1.0f, 0, 0x1B);
        }

        // The roar's steam: every 8 ticks while the motion is in [142, 264].
        if (in.At(142.0f, 3, 264.0f)) {
            if (in.em.CountEvery(8)) {
                in.Offset(0.0f, 0.0f, 200.0f);
                in.Spawn(1.0f, 1, 0x1B);
            }

            in.em.CountTicks();
        }

        if (in.Ended()) {
            in.NextState();
            in.Motion(2, 4, 0);
            in.MotionSpeed(0.65f);
        }

        break;
    }
}

// kind 12 0x80173314: Great Jaggi's first-encounter intro (Moga Woods).
// Placed at -600, 0, 1370 facing -30.4 degrees, it waits (motion 1) to demo
// step 0x14C, then follows the keys 0x805A91C0 (DemoPose from 0x14C, its
// height from the ground, 200 / 120 behind its keyed place) through motions
// 10 and 0xB, then 9: from step 0x1F1 it eases channel 9 in over its count
// (blend 1 - count * 0.5 / (40 / rate), frame count * rate), and at 0x21B,
// 0x2B3 and 0x3E7 it is placed for the next shots. The original counted the
// ease in calls; here in ticks.
void Em012IntroPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x50);
    Intro in(ctx, frame);
    const uint32_t work = in.work;
    constexpr uint32_t kKeys = 0x805A91C0;
    const float shot = float(in.Demo() * 2);

    const auto rate = [&] {
        in.game.Call<0x8012F948>({work});
        return in.game.F1();
    };

    // The keyed pose, the height above the ground and the place `back` behind it.
    const auto follow = [&](float back) {
        in.game.Call<0x801321B8>({work});
        in.Pose(int16_t(in.Demo() - 0x14C), kKeys, 0);
        in.game.Call<0x80130450>({work}, {double(Memory::ReadFloat32(work + 0x18C) - Memory::ReadFloat32(work + 0x20C))});
        in.game.Call<Em::Vec3Set>({in.at}, {0.0, 0.0, double(back)});
        in.game.Call<0x800510C8>({in.at, Memory::Read32(work + kAngleY)}); // rotVecY
        const float x = Memory::ReadFloat32(work + 0x188) + Memory::ReadFloat32(in.at);
        const float y = Memory::ReadFloat32(work + 0x18C) + Memory::ReadFloat32(in.at + 4);
        const float z = Memory::ReadFloat32(work + 0x190) + Memory::ReadFloat32(in.at + 8);
        in.Place(x, y, z);
    };

    switch (in.State()) {
    case 0:
        in.NextState();
        in.em.Count(0);
        in.SetUp(0);
        in.Motion(1, 0, 0);
        Memory::Write32(work + 0x1BC, 0);
        Memory::Write32(work + kAngleY, 0xFFFFE2AB);
        Memory::Write32(work + 0x1C4, 0);
        in.Place(-600.0f, 0.0f, 1370.0f);
        in.StartDemo();
        in.game.Call<0x80136D78>({work});
        break;
    case 1:
        in.game.Call<0x80136D78>({work});

        if (in.Reached(0x298)) {
            in.NextState();
            in.game.Call<0x80146200>({work});
            in.Motion(10, 0, 0x1A);
            in.MotionSpeed(0.8f);
            Memory::Write32(work + kAngleY, 0xFFFFE2AB);
            follow(-200.0f);
        }

        break;
    case 2:
        in.game.Call<0x80136D78>({work});
        follow(shot == 666.0f ? -200.0f : -120.0f);

        if (in.Reached(0x2DC)) {
            in.NextState();
            in.Motion(10, 2, 0x1A);
            in.MotionSpeed(1.0f);
        }

        break;
    case 3:
        if (shot < 772.0f)
            in.game.Call<0x80136D78>({work});

        follow(-120.0f);

        if (in.Reached(0x386)) {
            in.NextState();
            in.MotionEx(0xB, 2, 0, 0);
            in.MotionSpeed(0.8f);
        }

        break;
    case 4:
        in.game.Call<0x80136D78>({work});
        follow(-120.0f);

        if (in.Reached(0x3C0)) {
            in.NextState();
            in.MotionEx(9, 0x18, 0x6A, 1);
            in.MotionSpeed(0.5f);
            in.game.Call<0x80130450>({work}, {0.0});
        }

        break;
    case 5:
        in.game.Call<0x80136D78>({work});

        if (in.Reached(0x3E2)) {
            in.NextState();
            in.game.Call<0x8012F7BC>({work, 9, 0}, {1.0});
            in.MotionSpeed(0.5f);
            in.em.Count(0);
        }

        break;
    case 6: {
        in.game.Call<0x80136D78>({work});
        in.em.CountTicks();
        const float perTick = 0.5f / (40.0f / rate());
        const float blend = 1.0f - float(in.em.Count()) * perTick;
        const int32_t channelFrame = int32_t(float(in.em.Count()) * rate());
        in.game.Call<0x8012F7BC>({work, 9, uint32_t(channelFrame)}, {double(blend)});

        if (in.Reached(0x436)) {
            in.NextState();
            in.MotionEx(9, 0, 0, 0);
            in.MotionSpeed(0.6f);
            Memory::Write32(work + kAngleY, 0xFFFFE111);
            in.Place(544.8445f, 17.4916f, -2514.2f);
        }

        break;
    }
    case 7:
        if (in.Reached(0x566)) {
            in.NextState();
            in.MotionEx(0xC9, 0, 0x12, 0);
            in.MotionSpeed(0.8f);
            Memory::Write32(work + kAngleY, 0x800);
            in.Place(-314.772f, 17.4916f, -54.9744f);
        }

        break;
    case 8:
        if (in.Reached(0x7CE)) {
            in.NextState();
            in.MotionEx(9, 0, 8, 0);
            in.MotionSpeed(0.65f);
            Memory::Write32(work + kAngleY, 0xFFFFD3BC);
            in.Place(-314.772f, 17.4916f, -54.9744f);
        }

        break;
    default:
        break;
    }
}

} // namespace

extern "C" void func_8017E56C(CpuContext* ctx);
extern "C" void MH3_Em015Intro(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em015Intro", func_8017E56C, Em015IntroPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8017E56C, MH3_Em015Intro, "Em015Intro");

extern "C" void func_80173314(CpuContext* ctx);
extern "C" void MH3_Em012Intro(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em012Intro", func_80173314, Em012IntroPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80173314, MH3_Em012Intro, "Em012Intro");

extern "C" void func_801D42F0(CpuContext* ctx);
extern "C" void MH3_Em007Intro(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em005Intro", func_801D42F0, Em005IntroPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x801D42F0, MH3_Em007Intro, "Em005Intro");

extern "C" void func_80150830(CpuContext* ctx);
extern "C" void MH3_Em004Intro(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em001Intro", func_80150830, Em001IntroPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80150830, MH3_Em004Intro, "Em001Intro");

extern "C" void func_8038C188(CpuContext* ctx);
extern "C" void MH3_Em011Intro(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em009Intro", func_8038C188, Em009IntroPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8038C188, MH3_Em011Intro, "Em009Intro");

extern "C" void func_801C8228(CpuContext* ctx);
extern "C" void MH3_Em006Intro(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em004Intro", func_801C8228, Em004IntroPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x801C8228, MH3_Em006Intro, "Em004Intro");

extern "C" void func_8032F5B4(CpuContext* ctx);
extern "C" void MH3_Em026Intro(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em026Intro", func_8032F5B4, Em026IntroPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8032F5B4, MH3_Em026Intro, "Em026Intro");

extern "C" void func_802FE5B4(CpuContext* ctx);
extern "C" void MH3_Em023Intro(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em023Intro", func_802FE5B4, Em023IntroPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x802FE5B4, MH3_Em023Intro, "Em023Intro");

extern "C" void func_8018B47C(CpuContext* ctx);
extern "C" void MH3_LudrothIntro(CpuContext* ctx) {
    PortCheck::Call(ctx, "LudrothIntro", func_8018B47C, LudrothIntroPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8018B47C, MH3_LudrothIntro, "LudrothIntro");

extern "C" void func_80162564(CpuContext* ctx);
extern "C" void MH3_Em010Intro(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em008Intro", func_80162564, Em008IntroPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80162564, MH3_Em010Intro, "Em008Intro");

extern "C" void func_801BF050(CpuContext* ctx);
extern "C" void MH3_Em008Intro(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em006Intro", func_801BF050, Em006IntroPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x801BF050, MH3_Em008Intro, "Em006Intro");
