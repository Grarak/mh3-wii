// Field player actions, RMHP08. The hunting fields' player (PLW, see
// player_work.h) runs one action at a time, a state machine stepped once per
// logic step. Ported here (each takes the PLW in r3):
//   0x802535B8  motion 8; the local player's action ends after 15 steps
//   0x80257E08  (plw, long) motion 26 for 40 steps (80 when long), then ends
//   0x8025198C  motion 1 for 120 steps, or until 0x802D7A08 says to go on
//               (action 34)
//   0x8025D5F8  motion 221, moving forward 10 a step; after 90 steps action 9
//   0x8026AB00  PlButtonHoldUpdate (plw, pad): per button of 8, the steps it
//               has been held, up to 100
//   0x80259C9C  motion 206 facing back, sinking (speed +0x6C falls by 4 a
//               step to -250) while moving 10 a step along z; action 6 after
//               75 steps (it counts 2 a step)
//   0x8025B3BC  motion 259; the local player's goes on (0x803AB11C) after 300
//               steps
//   0x802595A0  (plw, s16 arg) motion 52 moving forward 2 a step, 20 steps
//               counted down, 0x80259030 each step
//   0x8025BC60  (plw, index, alt) a motion from one of two tables (0x805C5BC0,
//               0x805C5BE4); after 60 steps the action ends (near something or
//               in an event demo) or 0x8027D740 goes on
//   0x80250D40  motion 318, then (frame 46) 345; after 60 steps the local
//               player gets status message 25; the action ends with the motion
//   0x80258230  motion 39: after 200 steps action (6, 30); with something near
//               0x8026A40C, else after 10 steps action (6, 28)
//   0x802547A0  motion 120 draining stamina (1 a step, stamina.cpp): with
//               something near 0x80252B50 may take over; after 20 steps action
//               8 (or 150 when +0x378 is out)
//   0x802497CC  (plw, alt) motion 1 (100 when alt); the local player waits up
//               to 600 steps for +1622, then action 73/160 on 1, else it ends
//   0x80259374  (plw, a, b) motion 51, 4 steps counted down; 0x80259030 for
//               the local player each step
//   0x80250630  sleep: motions 327/328 (turning over at random, 1 in 128 a
//               step) healing 2 a step; the local player's wakes (action 86)
//               after 120 steps
//   0x8024A7C0  (plw, kind) rest: stamina +4 a step; motion 5/122 for 150
//               steps, then 6/124 until it ends (and +0x37A more stamina)
//   0x8024B8CC  (plw, kind) motion 8 (14 steps, then 9), 364 or 309 until it
//               ends
//   0x8025C100  (plw, alt) motion 507 (527 when alt) with cues at frames 48,
//               68, 168, 190; after 60 steps as 0x8025BC60
//   0x80258394  motion 225, then 210 for 240 steps (480 with skill 6), a step
//               less for each input (one per 3 steps), then 207 until it ends
//   0x802580D4  effect 13 every 50 steps of the player's step counter
//   0x80257EEC  (plw, alt) held: 5 stages to escape, each a count of inputs
//               (0x80794510) within 300 steps or lost when they run out
//   0x802596E8  (plw, s16 arg) motion 53/59 moving forward 5 a step until
//               0x80277E10 finds the way, then action 48/68
//   0x8028138C  a step of charging (+0x310 counts it)
//   0x80281DBC, 0x80282770  charge holds (motion 1133 / 1170): charging from
//               frame 34 while held, up to 90 steps; the attack from frame 66
//   0x80247704  climbing up a ledge (motion 17); after 30 steps it may end
//   0x80249944  standing: idle motion 100, 118, 121 or 360
//   0x80253CA0  motion 105 draining stamina (4 a step) for 25 steps
//   0x80246BE0  motion 1, after 30 steps 66 (63), then 67 (65); the tired
//               idle 303
//   0x802516C0  (plw, alt) using an item: motion 314 (365) for 60 steps,
//               then action 170 (173) while the item lasts
//   0x80251470  (plw, fresh, alt) the item's repeat: motion 314 (365), after
//               30 steps command 76 goes on to 171 (174); 60 steps, command 7
//               ends it
//   0x802886AC  (plw, next, alt) weapon motion 1001 (1050), for the steps
//               0x805CB9B0 gives +0x26C, then the action table's next
//   0x8025AA9C  (plw, mode) motion 241 waiting on 0x8012D4FC (up to 300
//               steps) or 0x8012D4CC, then action 7
//   0x8024FF6C  (plw, kind) motion 7 then 8 (or 8, or 118); after 20 steps
//               commands 7 and 1 end it
//   0x8025DC40, 0x8025DA9C  (plw, alt) another console's player standing or
//               resting; freed after 300 steps
//   0x802821DC  (plw, kind) a charge in motion 1150 from frame 48, then action
//               48
//   0x8025B15C  (plw, kind) motion 210 (209), now and then 223; the local
//               player's leaves after 300 steps
//   0x8028563C, 0x802869D4  a spin after a charge (1103 / 1162): the charge
//               level + 2 motion loops, steered, then 1104 / 1163
//   0x802598C4, 0x8025CECC  falls (motion 56), landing by the steps fallen
//   0x802501B8  waiting in motion 1, then placed beside the spot 0x803BAA14
//               finds (action 84)
//   0x80253038  a run (motion 119) draining stamina; after 40 steps action 8
//   0x80254A28  a jump (motion 15): a ledge catches it, or it lands
//   0x80254F18  (plw, kind) climbing over or dropping off (motion 11, 12, 13)
//   0x802813D0  (plw, kind) a charge in motion 1101, then action 31 (32, 33)
//   0x80256A58  (plw, kind) knocked flying (205 / 208), landing, getting up
//   0x80258AC8  (plw, kind) grabbed by a monster (271 / 219)
//   0x8026A57C  VelocityStep: the jumps' velocity and position, per step
//
// The originals count by 1 per step and move by a fixed amount per step (and
// test the player's step counter, which runs a tick at a time, for `% n == 0`). Here
// the counts are the whole ticks of game time in the step (GameStep::Ticks())
// and the movement is scaled by the step length, so the actions take the
// same time and cover the same ground at any step length; at the stock 1/30 s
// step (one tick) these are the originals.
#include "player_work.h"
#include "port_check.h"

#include <algorithm>

namespace {

using namespace Plw;

// PLW fields of the field's actions.
enum : uint32_t {
    kFlag1 = 1,           // u8
    kFlag6 = 6,           // u8
    kFlag7 = 7,           // u8
    kFlag20 = 20,         // u8
    kFlag24 = 24,         // u8
    kPosition = 0x3C,     // VEC3
    kPositionCopy = 0x48, // VEC3 (0x80041EA4 copies the position there)
    kLastY = 0x60,        // f32 (copied back to the position's y)
    kSpeed = 0x6C,        // f32
    kFlag614 = 0x266,     // u8
    kCondition = 0x378,   // s16: stamina (stamina.cpp)
    kInputs = 6,          // u8: inputs counted in the escape stage (0x80257EEC)
    kStages = 7,          // u8: escape stages left (0x80257EEC)
    kVisible = 0x77,      // u8
    kWord1008 = 0x3F0,    // s16
    kChargeSteps = 0x310, // s16: steps charged (0x8028138C)
    kChargeLevel = 0x312, // u8: the level they reach (0x802812C0)
    kMotionRate = 0x354,  // f32: the motion's speed
    kLean = 0xAC,         // u32: a turn (0x10000 a circle), as s16
    kCommandBits = 0xE0,  // u32[]: command bits (0x8026A6A8 tests one)
    kCommands = 0xF0,     // u32[]: the step's command bits (0x8026A708 tests one)
    kPadHeld = 0xC8,      // u16
    kPadPressed = 0xCC,   // u16
    kFlag15 = 15,         // u8
    kWord16 = 16,         // u16
    kGroundKind = 22,     // u8 (GetGroundHit's)
    kClimbs = 0x5C7,      // u8
    kWord1020 = 0x3FC,    // s16
    kWord1024 = 0x400,    // s16
    kFlag789 = 0x315,     // u8
    kFlag1468 = 0x5BC,    // u8
    kFlag1469 = 0x5BD,    // u8
    kFlag1470 = 0x5BE,    // u8
    kFlag1640 = 0x668,    // u8
    kSteps1012 = 0x3F4,   // s16: steps, to 200 (0x80258230)
    kSignal = 0x656,      // u8 (set by others while +0x657 is 1)
    kWatching = 0x657,    // u8
    kStaminaMax = 0x37A,  // s16: the stamina gauge's length (?): a rest ends adding it; the idle is tired at 150 or less
    kItem774 = 0x306,     // u16
    kWord1606 = 0x646,    // s16
    kWord1608 = 0x648,    // s16
    kState918 = 0x396,    // s16 (0x80277BA8)
    kState1408 = 0x580,   // s16 (0x80277CAC)
};

enum : uint32_t {
    NextAction = 0x80275B28,        // (plw, u8, u16 action, u16): +14 = 1, then 0x80275754 sets it
    ActionFlags = 0x80275B68,       // (plw, u16 flag9 | 0x8000, a, b): flag 9 and act flags 1, 2, 4
    EndAction = 0x8027621C,         // (plw, u8 flag9, s32 blend, s32 flags): back to the field stance
    MotionAfterSet = 0x80277CBC,    // (plw)
    CanGoOn = 0x802D7A08,           // () -> 1
    WaitStart = 0x802D7AB4,         // ()
    CameraFollow = 0x802BC700,      // ()
    RotVecY = 0x800510C8,           // rotVecY (VEC3*, u32 angle)
    VecAdd = 0x80073FCC,            // (inout VEC3*, add VEC3*)
    ButtonBlocked = 0x802A0368,     // (plw) -> 0 when a held button may count
    ClearActFlags = 0x8026FF54,     // (plw, flags): clears flag bits (as SetActFlags)
    Nearby = 0x8026F96C,            // (plw, 0) -> u8
    InEventDemo = 0x8027BCAC,       // PlInEventDemo (1) -> 1
    CopyVec = 0x80041EA4,           // (dst, src)
    FrameCheck = 0x8026A378,        // Pl_frame_check (plw, u16 channel, f1 frame, f2) -> 1 once past it
    StatusMessage = 0x802672D4,     // PlShowStatusMessage (plw, 1, id, 0)
    StaminaChange = 0x8027695C,     // PlStaminaChange (plw, s16 delta), per tick (stamina.cpp)
    AddHealth = 0x802762F0,         // PlAddHealth (plw, s16 amount) (player_health.cpp)
    Watch = 0x802DE5DC,             // (plw, plw + 1432)
    SetCondition = 0x802768CC,      // PlSetConditionState (plw, s16 delta): stamina (stamina.cpp)
    SetEffect013 = 0x801072B4,      // eft013_set (plw, u8)
    Cue = 0x80245D54,               // (plw, u16 id, u8 table 0-3): 0x802779D8 with the table's lists
    SkillCheck = 0x80271374,        // Pl_Skill_ck (plw, skill) -> 1
    SetState1408 = 0x80277CAC,      // (plw, s16): +0x580
    SetState918 = 0x80277BA8,       // (plw, s16): +0x396
    EscapeStages = 0x80794510,      // u8[]: the inputs each escape stage takes, by stages left
    ChargeLevel = 0x802812C0,       // (plw) -> u8: the level +0x310 reaches (thresholds 0x805C7478)
    ChargeTime = 0x80281210,        // (plw, s16 steps) -> s16: scaled by skills 191, 192
    ChargeEffect = 0x802811B0,      // (plw)
    GroundHit = 0x802916A0,         // GetGroundHit (VEC3*, s32, u8 kind) -> f1 height
    ClimateActive = 0x80276864,     // PlClimateActive (plw, 0) -> 1 in heat or cold without protection
    InWater = 0x80278878,           // Pl_suimen_ck (plw) -> 1 at the water's surface
    AnyInput = 0x8026F8EC,          // (plw) -> 1 when the local player moves the stick or presses a button
    Vec3Ctor = 0x80043F0C,          // (VEC3*): nothing
    CatSkillCheck = 0x80272B14,     // Pl_cat_skill_ck (plw, skill) -> 1
    PlayModeCheck = 0x800CF27C,     // PlayMode_ck () -> u8
    MotionFrame = 0x8026A3B0,       // (plw) -> f1: the motion's frame
    SetMotionAttr = 0x8026A138,     // Pl_chr_set_attr (plw, u16 motion, s32 blend, s32 frame, u32)
    RotVecXYZ = 0x80051224,         // rotVecXYZ (VEC3*, u32 angles[3])
    GroundHit2 = 0x802916C8,        // GetGroundHit2 (VEC3*, s32, u8 kind, u8* hit) -> f1 height
    ClearVelocity = 0x8026A5D4,     // (plw): +0x78..+0x8C = 0
    ClearAcceleration = 0x8026A5F4, // (plw): +0x84..+0x8C = 0
    PlVelocityStep = 0x8026A57C,    // (plw): VelocityStep below
    JointPos = 0x8026A3EC,          // pl_get_joint_wpos (plw, joint, VEC3* out)
    EnemyJointPos = 0x8013599C,     // get_joint_wpos_em (em, joint, VEC3* out)
    VecSub = 0x80050D04,            // (out, a, b): a - b
    VecAngles = 0x80050D7C,         // calcVecAngXY (VEC3*, u32* x, u32* y)
    VecAngle2 = 0x80050AF4,         // calcVecAng2 (VEC3* from, VEC3* to) -> u32 y angle
    GetMoveWorkAdrs = 0x800CFAF4,   // get_move_work_adrs (type) -> the works (2840 bytes each for 3)
    GetMoveWorkMax = 0x800CFB34,    // get_move_work_max (type) -> u16
    GetNowAreaNo = 0x802AFCE8,      // get_now_areano () -> u8
    SetAction = 0x80275754,         // (plw, u8, u16 action, u16)
    EndActionReset = 0x8027629C,    // (plw, u8 flag9, s32 blend, s32 flags): +0x442 = 0, +0x30A = 255, then as EndAction
};
constexpr uint32_t kCharFlag246 = 0xF6; // MHchar u8 (0x8026A350)

// Bit n of the u32 array at `words` (0x8026A6A8 and 0x8026A708 on the PLW's).
bool Bit(uint32_t words, uint32_t n) {
    return (Memory::Read32(words + n / 32 * 4) >> (n % 32)) & 1;
}

struct Player : Plw::Player {
    void Flags(uint32_t mode, uint32_t a, uint32_t b) const { game.Call<ActionFlags>({plw, mode, a, b}); }
    void End(uint32_t flag9, int32_t blend) const { game.Call<EndAction>({plw, flag9, uint32_t(blend), 0}); }
    void Next(uint32_t a, uint32_t action, uint32_t b) const { game.Call<NextAction>({plw, a, action, b}); }
};

// 0x802580D4: effect 13 (10) every 50 steps.
void Effect13Every50(const Player& p) {
    if (p.Every(50))
        p.game.Call<SetEffect013>({p.plw, 10});
}

// 0x802535B8.
void Motion8(CpuContext* ctx) {
    GuestFrame frame(ctx, 16);
    GuestCall game{ctx};
    const Player p{{game, frame.Arg(3)}};
    switch (p.State()) {
    case 0:
        p.NextState();
        p.Timer(15);
        p.Flags(1, 0, 0);
        p.Set(8, 10, 0);
        break;
    case 1:
        if (game.Call<MasterCheck>({p.plw}) != 0 && p.CountDown())
            p.End(1, 4);
        break;
    }
}

// 0x80257E08 (plw, long).
void Motion26(CpuContext* ctx) {
    GuestFrame frame(ctx, 16);
    GuestCall game{ctx};
    const Player p{{game, frame.Arg(3)}};
    switch (p.State()) {
    case 0:
        p.NextState();
        Memory::Write8(p.plw + kFlag24, 0);
        p.Set(26, 6, 0);
        game.Call<MotionAfterSet>({p.plw});
        p.Flags(0, 0, 1);
        p.Timer(frame.Arg(4) ? 80 : 40);
        break;
    case 1:
        if (p.CountDown())
            p.End(0, 12);
        break;
    }
}

// 0x8025198C.
void Wait120(CpuContext* ctx) {
    GuestFrame frame(ctx, 16);
    GuestCall game{ctx};
    const Player p{{game, frame.Arg(3)}};
    Memory::Write16(p.plw + kState1408, 2);
    switch (p.State()) {
    case 0:
        p.NextState();
        Memory::Write8(p.plw + kFlag24, 0);
        p.Flags(0, 0, 0);
        p.SetX(1, 4, 0);
        p.Timer(120);
        game.Call<WaitStart>();
        break;
    case 1:
        if (p.CountDown())
            p.End(0, 4);
        else if (game.Call<CanGoOn>() == 1)
            p.Next(0, 34, 0);
        break;
    }
}

// 0x8025D5F8.
void Slide221(CpuContext* ctx) {
    GuestFrame frame(ctx, 32);
    GuestCall game{ctx};
    const Player p{{game, frame.Arg(3)}};
    const uint32_t plw = p.plw;
    Memory::Write16(plw + kState1408, 10);
    Memory::Write16(plw + kState918, 10);
    Memory::Write8(plw + kFlag1, 1);
    switch (p.State()) {
    case 0:
        p.NextState();
        Memory::Write8(plw + kFlag6, 0);
        Memory::Write8(plw + kFlag7, 0);
        p.Set(221, 0, 0);
        p.Timer(0);
        p.Flags(0, 0, 0);
        if (p.Master())
            game.Call<CameraFollow>();
        Memory::WriteFloat32(plw + kSpeed, 40.0f);
        break;
    case 1: {
        p.CountUp();
        if (p.Timer() > 90) {
            p.Next(9, 1, 2);
            break;
        }
        // Forward (facing +0x58) by 10 a step: here 10 times the step length.
        const uint32_t move = frame[8];
        Memory::WriteFloat32(move, 0.0f);
        Memory::WriteFloat32(move + 4, 0.0f);
        Memory::WriteFloat32(move + 8, float(10.0 * GameStep::Scale()));
        game.Call<RotVecY>({move, Memory::Read32(plw + kAngle2)});
        game.Call<VecAdd>({plw + kPosition, move});
        break;
    }
    }
}

// 0x8026AB00 PlButtonHoldUpdate (plw, pad). Per button i of 8 (masks at
// 0x805C6750): state (pad +96 + i) 0 waits for a press (button 1 reads the
// alternative buttons at +4 when pad +112 is 1), counting it when
// 0x802A0368(plw) allows (button 6 always); state 1 counts the steps it is
// held (pad +104 + i, s8, up to 100) until it is let go.
void ButtonHold(CpuContext* ctx) {
    GuestFrame frame(ctx, 48);
    GuestCall game{ctx};
    const uint32_t plw = frame.Arg(3), pad = frame.Arg(4);
    constexpr uint32_t kMasks = 0x805C6750;
    for (uint32_t i = 0; i < 8; ++i) {
        const uint32_t state = pad + 96 + i, held = pad + 104 + i;
        const uint32_t mask = Memory::Read16(kMasks + 2 * i);
        switch (Memory::Read8(state)) {
        case 0: {
            Memory::Write8(held, 0);
            const bool alternative = Memory::Read8(pad + 112) == 1 && i == 1;
            if ((Memory::Read16(pad + (alternative ? 4 : 0)) & mask) == 0)
                break;
            if (game.Call<ButtonBlocked>({plw}) != 0 && i != 6)
                break;
            Memory::Write8(state, uint8_t(Memory::Read8(state) + 1));
            Memory::Write8(held, 1);
            break;
        }
        case 1:
            if (Memory::Read16(pad) & mask) {
                const int32_t steps = int8_t(Memory::Read8(held)) + int32_t(GameStep::Ticks());
                Memory::Write8(held, uint8_t(int8_t(steps) < 100 ? steps : 100));
            } else {
                Memory::Write8(state, 0);
            }
            break;
        }
    }
}

// Moves the PLW by `step` (VEC3 in the guest frame, set by the caller) turned
// by its facing (+0x58), times the step length: the originals move it by the
// turned VEC3 once per step.
void MoveTurned(const Player& p, uint32_t step, float forward) {
    Memory::WriteFloat32(step, 0.0f);
    Memory::WriteFloat32(step + 4, 0.0f);
    Memory::WriteFloat32(step + 8, float(forward * GameStep::Scale()));
    p.game.Call<RotVecY>({step, Memory::Read32(p.plw + kAngle2)});
}

// 0x80259C9C.
void Sink206(CpuContext* ctx) {
    GuestFrame frame(ctx, 32);
    GuestCall game{ctx};
    const Player p{{game, frame.Arg(3)}};
    const uint32_t plw = p.plw;
    p.CountUp();
    switch (p.State()) {
    case 0:
        p.NextState();
        p.Flags(0, 0, 1);
        p.Set(206, 0, 0);
        game.Call<MotionAfterSet>({plw});
        game.Call<0x8027D554>({plw});
        game.Call<0x80257ED4>({plw});
        game.Call<0x80245E04>({plw, 0});
        p.Timer(0);
        Memory::Write32(plw + kAngle2, 0x8000);
        Memory::Write32(plw + kAngle, 0x8000);
        Memory::Write8(plw + kFlag24, 0);
        break;
    case 1: {
        p.CountUp(); // (a second count: the timer runs 2 a step here)
        if (p.Timer() >= 150) {
            p.Next(6, 62, 0x2000);
            break;
        }
        const float s = float(GameStep::Scale());
        const float speed = Memory::ReadFloat32(plw + kSpeed) - 4.0f * s;
        Memory::WriteFloat32(plw + kSpeed, speed);
        if (speed < -250.0f)
            Memory::WriteFloat32(plw + kSpeed, -250.0f);
        else
            Memory::WriteFloat32(plw + kPosition + 8, Memory::ReadFloat32(plw + kPosition + 8) + 10.0f * s);
        game.Call<CopyVec>({plw + kPositionCopy, plw + kPosition});
        break;
    }
    }
}

// 0x8025B3BC.
void Wait300(CpuContext* ctx) {
    GuestFrame frame(ctx, 16);
    GuestCall game{ctx};
    const Player p{{game, frame.Arg(3)}};
    const uint32_t plw = p.plw;
    Memory::Write16(plw + kState918, 2);
    game.Call<0x80245E04>({plw, 0});
    Memory::Write16(plw + kState1408, 10);
    game.Call<0x8027A1E0>({plw});
    const uint32_t state = p.State();
    if (state == 0) {
        p.NextState();
        game.Call<0x8027AC70>({plw});
        p.Set(259, 0, 0);
        game.Call<0x80275524>({plw, 3});
        game.Call<MotionAfterSet>({plw});
        game.Call<0x8025B138>({plw});
        p.Flags(3, 0, 1);
        game.Call<ClearActFlags>({plw, 4});
        game.Call<ClearActFlags>({plw, 16});
        Memory::Write8(plw + kFlag24, 0);
        p.Timer(0);
        if (p.Master()) {
            game.Call<0x802BC6C0>({1});
            game.Call<0x803AB160>({Memory::Read8(plw + 8)});
        }
    } else if (state <= 2 && p.Master() && p.State() == 1) {
        p.CountUp();
        if (p.Timer() >= 300) {
            p.NextState();
            game.Call<0x803AB11C>({plw});
        }
    }
}

// 0x802595A0 (plw, s16 arg).
void Walk52(CpuContext* ctx) {
    GuestFrame frame(ctx, 32);
    GuestCall game{ctx};
    const Player p{{game, frame.Arg(3)}};
    const uint32_t plw = p.plw;
    const uint32_t arg = uint32_t(int32_t(int16_t(frame.Arg(4))));
    switch (p.State()) {
    case 0:
        p.NextState();
        p.Timer(20);
        Memory::Write8(plw + kFlag614, 30);
        p.Set(52, 4, 0);
        game.Call<0x8027AC70>({plw});
        game.Call<MotionAfterSet>({plw});
        p.Flags(0, 0, 1);
        game.Call<0x8027D554>({plw});
        game.Call<SetActFlags>({plw, 64});
        game.Call<0x8027714C>({plw, 0x805C571C, 0});
        break;
    case 1: {
        const uint32_t step = frame[8];
        MoveTurned(p, step, 2.0f);
        for (uint32_t k = 0; k < 12; k += 4)
            Memory::WriteFloat32(plw + kPosition + k, Memory::ReadFloat32(plw + kPosition + k) + Memory::ReadFloat32(step + k));
        p.CountDownToZero();
        game.Call<0x80259030>({plw, 1, arg, 0});
        break;
    }
    }
}

// 0x8025BC60 (plw, index, alt).
void TableMotionThenGo(CpuContext* ctx) {
    GuestFrame frame(ctx, 32);
    GuestCall game{ctx};
    const Player p{{game, frame.Arg(3)}};
    const uint32_t plw = p.plw, index = frame.Arg(4), alt = frame.Arg(5);
    const auto s16 = [](uint32_t a) {
        return int32_t(int16_t(Memory::Read16(a)));
    };
    switch (p.State()) {
    case 0: {
        p.NextState();
        p.Flags(alt ? 3 : 0, 0, 0);
        const uint32_t entry = (alt ? 0x805C5BE4u : 0x805C5BC0u) + 6 * index; // {motion, blend, flags}
        p.Set(uint32_t(s16(entry)) & 0xFFFF, s16(entry + 2), s16(entry + 4));
        p.Timer(60);
        break;
    }
    case 1:
        p.CountDownToZero();
        if (!p.Master() || p.Timer() > 0)
            break;
        if ((game.Call<Nearby>({plw, 0}) & 0xFF) != 0 || game.Call<InEventDemo>({1}) == 1)
            game.Call<EndAction>({plw, Memory::Read8(plw + kFlag9), 4, 0});
        else
            game.Call<0x8027D740>({plw, uint32_t(int32_t(int16_t(alt)))});
        break;
    }
}

// 0x80250D40.
void Motion318(CpuContext* ctx) {
    GuestFrame frame(ctx, 16);
    GuestCall game{ctx};
    const Player p{{game, frame.Arg(3)}};
    const uint32_t plw = p.plw;
    Memory::Write8(p.Char() + kCharFlag246, 2);
    Memory::Write16(plw + kState1408, 2);
    // The original: - 1 while not 0.
    if (const int32_t t = p.Timer(); t != 0)
        p.Timer(t > 0 ? std::max(t - int32_t(GameStep::Ticks()), 0) : t - int32_t(GameStep::Ticks()));
    switch (p.State()) {
    case 0:
        p.NextState();
        Memory::Write8(plw + kFlag6, 0);
        p.Flags(0, 0, 0);
        p.Set(318, -4, 0);
        p.Timer(60);
        break;
    case 1:
        if (game.Call<FrameCheck>({plw, 1}, {46.0, 0.0}) == 1) {
            p.NextState();
            p.Set(345, 0, 0);
        }
        break;
    case 2:
        if (p.Master() && p.Timer() == 0 && Memory::Read8(plw + kFlag6) == 0) {
            Memory::Write8(plw + kFlag6, 1);
            game.Call<StatusMessage>({plw, 1, 25, 0});
        }
        if (p.Ended())
            p.End(0, 4);
        break;
    }
}

// 0x80258230.
void Motion39(CpuContext* ctx) {
    GuestFrame frame(ctx, 32);
    GuestCall game{ctx};
    const Player p{{game, frame.Arg(3)}};
    const uint32_t plw = p.plw;
    switch (p.State()) {
    case 0:
        p.NextState();
        p.Flags(0, 0, 1);
        game.Call<SetActFlags>({plw, 384});
        p.Set(39, 0, 0);
        game.Call<MotionAfterSet>({plw});
        Memory::Write32(plw + kAngle, Memory::Read32(plw + kAngle2));
        Memory::Write8(plw + kFlag24, 0);
        game.Call<0x8027714C>({plw, 0x805C56D8, 0});
        p.Timer(10);
        game.Call<0x8027AC70>({plw});
        break;
    case 1: {
        Effect13Every50(p);
        if (game.Call<MasterCheck>({plw}) == 0)
            break;
        p.CountDownToZero();
        // The original counts this by 1 a step (it was left counting steps).
        const int32_t steps = int16_t(Memory::Read16(plw + kSteps1012)) + int32_t(GameStep::Ticks());
        Memory::Write16(plw + kSteps1012, uint16_t(steps));
        if (int16_t(steps) >= 200)
            p.Next(6, 30, 12);
        else if ((game.Call<Nearby>({plw, 0}) & 0xFF) >= 1)
            game.Call<0x8026A40C>({plw});
        else if (p.Timer() <= 0)
            p.Next(6, 28, 1);
        break;
    }
    }
}

// 0x802547A0.
void Motion120(CpuContext* ctx) {
    GuestFrame frame(ctx, 16);
    GuestCall game{ctx};
    const Player p{{game, frame.Arg(3)}};
    const uint32_t plw = p.plw;
    switch (p.State()) {
    case 0:
        p.NextState();
        p.Flags(3, 0, 0);
        game.Call<SetActFlags>({plw, 192});
        p.SetX(120, 4, 0);
        game.Call<0x802785C8>({plw, 1});
        Memory::Write8(plw + kFlag614, 10);
        game.Call<0x8027714C>({plw, 0x805BF5D4, 0});
        p.Timer(20);
        Memory::Write16(plw + kWord1606, 0);
        Memory::Write16(plw + kWord1608, 0);
        break;
    case 1:
        game.Call<StaminaChange>({plw, uint32_t(-1)});
        p.CountDownToZero();
        if (!p.Master())
            break;
        if ((game.Call<Nearby>({plw, 0}) & 0xFF) >= 1 && game.Call<0x8026A6A8>({plw, 37}) == 1) {
            game.Call<0x8026A450>({plw});
            game.Call<0x80252B50>({plw, 0xC16D, 16020, 43, 44});
        } else if (p.Timer() <= 0) {
            if (int16_t(Memory::Read16(plw + kCondition)) > 0)
                p.Next(1, 8, 0);
            else
                p.Next(0, 150, 0);
        }
        break;
    }
}

// 0x802497CC (plw, alt).
void AwaitSignal(CpuContext* ctx) {
    GuestFrame frame(ctx, 16);
    GuestCall game{ctx};
    const Player p{{game, frame.Arg(3)}};
    const uint32_t plw = p.plw, alt = frame.Arg(4);
    Memory::Write8(plw + kWatching, 1);
    game.Call<Watch>({plw, plw + 1432});
    game.Call<0x8027A1E0>({plw});
    switch (p.State()) {
    case 0:
        p.NextState();
        p.Flags(alt ? 3 : 0, 0, 0);
        p.SetX(alt ? 100 : 1, 6, 0);
        game.Call<MotionAfterSet>({plw});
        if (game.Call<0x8045169C>() == 1)
            Memory::Write8(plw + kSignal, 0);
        p.Timer(0);
        break;
    case 1: {
        if (game.Call<MasterCheck>({plw}) == 0)
            break;
        p.CountUp();
        const uint32_t signal = Memory::Read8(plw + kSignal);
        if (p.Timer() <= 600 && signal == 0)
            break;
        if (signal == 1)
            p.Next(0, alt ? 160 : 73, 0);
        else
            game.Call<EndAction>({plw, Memory::Read8(plw + kFlag9), 2, 0});
        break;
    }
    }
}

// 0x80259374 (plw, a, b).
void Motion51(CpuContext* ctx) {
    GuestFrame frame(ctx, 32);
    GuestCall game{ctx};
    const Player p{{game, frame.Arg(3)}};
    const uint32_t plw = p.plw, a = frame.Arg(4), b = frame.Arg(5);
    uint32_t counting = frame.Arg(31); // (the original passes its caller's r31 in the other states)
    switch (p.State()) {
    case 0:
        p.NextState();
        Memory::Write8(plw + kFlag24, 0);
        game.Call<0x8027AC70>({plw});
        p.SetX(51, 4, 0);
        if (a == 0) {
            Memory::Write16(plw + 1014, 0);
            Memory::Write16(plw + 1008, 0);
            game.Call<0x80257ED4>({plw});
            game.Call<0x80245E04>({plw, 0});
            if (b == 0)
                Memory::Write16(plw + 1048, 900);
            else
                Memory::Write16(plw + 1054, 450);
            Memory::Write16(plw + 1114, 0);
            Memory::Write16(plw + 1116, 0);
        } else {
            game.Call<0x80245E04>({plw, 1});
        }
        p.Timer(4);
        p.Flags(0, 0, 1);
        game.Call<0x8027D554>({plw});
        game.Call<0x8027714C>({plw, 0x805C571C, 0});
        [[fallthrough]];
    case 1:
        if (p.CountDown())
            p.NextState();
        counting = 1;
        break;
    case 2:
        counting = 0;
        break;
    }
    if (p.Master())
        game.Call<0x80259030>({plw, 0, uint32_t(int32_t(int16_t(b))), counting});
}

// 0x80250630: sleep.
void Sleep(CpuContext* ctx) {
    GuestFrame frame(ctx, 32);
    GuestCall game{ctx};
    const Player p{{game, frame.Arg(3)}};
    const uint32_t plw = p.plw;
    Memory::Write16(plw + kState1408, 2);
    Memory::Write16(plw + kState918, 2);
    switch (p.State()) {
    case 0:
        p.NextState();
        Memory::Write8(plw + kFlag6, 0);
        p.Flags(0, 0, 0);
        p.Timer(120);
        p.Set(327, 0, 0);
        game.Call<0x80114C84>({plw, 0});
        break;
    case 1:
        if (p.CountDown() && p.Master()) {
            p.Next(0, 86, 0);
            break;
        }
        if (p.Motion() == 327) {
            // The original rolls once a step: here once a tick.
            for (uint32_t tick = 0, ticks = GameStep::Ticks(); tick < ticks; ++tick) {
                if ((game.Call<Rand>({1}) & 0x7F) != 0)
                    continue;
                p.Set(328, 4, 0);
                break;
            }
        } else if (p.Motion() == 328 && p.Ended()) {
            p.Set(327, 4, 0);
        }
        // The original heals 2 a step: here its share of this step.
        game.Call<AddHealth>({plw, uint32_t(GameStep::Share(2))});
        break;
    }
}

// 0x8024A7C0 (plw, kind).
void Rest(CpuContext* ctx) {
    GuestFrame frame(ctx, 16);
    GuestCall game{ctx};
    const Player p{{game, frame.Arg(3)}};
    const uint32_t plw = p.plw, kind = frame.Arg(4);
    const bool standing = kind == 0 || kind == 2;
    // The original: + 4 a step. Here its share of this step.
    game.Call<SetCondition>({plw, uint32_t(GameStep::Share(4))});
    switch (p.State()) {
    case 0:
        p.NextState();
        Memory::Write8(plw + kFlag24, 0);
        p.Flags(standing ? 0 : 3, 0, 0);
        if (standing)
            p.Set(5, 4, 0);
        else
            p.Set(122, 6, 0);
        p.Timer(150);
        break;
    case 1:
        if (!p.CountDown())
            break;
        p.NextState();
        p.Set(standing ? 6 : 124, 4, 0);
        break;
    case 2:
        if (!p.Ended())
            break;
        game.Call<SetCondition>({plw, uint32_t(int32_t(int16_t(Memory::Read16(plw + kStaminaMax))))});
        if (standing)
            p.End(0, 6);
        else
            p.End(3, 4);
        break;
    }
}

// 0x8024B8CC (plw, kind).
void Motion8Or364(CpuContext* ctx) {
    GuestFrame frame(ctx, 16);
    GuestCall game{ctx};
    const Player p{{game, frame.Arg(3)}};
    const uint32_t plw = p.plw, kind = frame.Arg(4);
    switch (p.State()) {
    case 0:
        p.NextState();
        if (kind == 0) {
            p.Flags(0, 0, 0);
            p.Set(8, 6, 0);
        } else if (kind == 1) {
            p.Flags(3, 0, 0);
            p.Set(364, 0, 30);
        } else if (kind == 2) {
            p.Flags(1, 0, 0);
            p.Set(309, 0, 20);
        }
        p.Timer(0);
        if (p.Master()) {
            game.Call<0x8024D1A8>({plw});
            if (game.Call<InEventDemo>({1}) == 0)
                game.Call<0x80272E94>({plw, Memory::Read16(plw + kItem774), uint32_t(-1)});
        }
        break;
    case 1:
        if (kind == 1 || kind == 2) {
            p.NextState();
        } else if (kind == 0) {
            p.CountUp();
            if (p.Timer() >= 14) {
                p.NextState();
                p.Set(9, 0, 0);
            }
        }
        break;
    case 2:
        if (p.Ended())
            game.Call<EndAction>({plw, Memory::Read8(plw + kFlag9), 2, 0});
        break;
    }
}

// 0x8025C100 (plw, alt): motion 507 (527 when alt) for 60 steps with cues at
// frames 48, 68, 168 and 190, then as 0x8025BC60.
void Motion507Cues(CpuContext* ctx) {
    GuestFrame frame(ctx, 16);
    GuestCall game{ctx};
    const Player p{{game, frame.Arg(3)}};
    const uint32_t plw = p.plw, alt = frame.Arg(4);
    switch (p.State()) {
    case 0:
        p.NextState();
        p.Flags(alt ? 3 : 0, 0, 0);
        p.Set(alt ? 527 : 507, 4, 0);
        p.Timer(60);
        break;
    case 1: {
        p.CountDownToZero();
        constexpr struct {
            float frame;
            uint32_t id;
        } kCues[] = {{48, 8}, {68, 9}, {168, 9}, {190, 8}};
        for (const auto& cue : kCues)
            if (game.Call<FrameCheck>({plw, 0}, {cue.frame, 0.0}) == 1)
                game.Call<Cue>({plw, cue.id, 3});
        if (!p.Master() || p.Timer() > 0)
            break;
        if ((game.Call<Nearby>({plw, 0}) & 0xFF) != 0 || game.Call<InEventDemo>({1}) == 1)
            p.End(Memory::Read8(plw + kFlag9), 4);
        else
            game.Call<0x8027D740>({plw, uint32_t(int32_t(int16_t(alt)))});
        break;
    }
    }
}

// 0x80258394: motion 225, then 210 held for 240 steps (480 with skill 6),
// shortened by a step for each input (at most every 3 steps); then 207 and
// the action ends with it.
void Struggle(CpuContext* ctx) {
    GuestFrame frame(ctx, 32);
    GuestCall game{ctx};
    const Player p{{game, frame.Arg(3)}};
    const uint32_t plw = p.plw;
    switch (p.State()) {
    case 0:
        p.NextState();
        Memory::Write8(plw + kFlag7, 0);
        p.Flags(0, 0, 1);
        Memory::Write8(plw + kFlag24, 0);
        Memory::Write16(plw + kWord1008, 0);
        Memory::Write16(plw + kSteps1012, 0);
        game.Call<0x80257ED4>({plw});
        game.Call<0x80245E04>({plw, 0});
        p.Set(225, 4, 0);
        Memory::Write8(plw + kFlag24, 0);
        game.Call<0x8027AC70>({plw});
        break;
    case 1:
        Effect13Every50(p);
        if (!p.Ended())
            break;
        p.NextState();
        p.Timer(game.Call<SkillCheck>({plw, 6}) == 1 ? 480 : 240);
        p.Set(210, 0, 154);
        break;
    case 2: {
        // The input cooldown (+7) and the countdown run by the tick.
        const uint32_t cooldown = Memory::Read8(plw + kFlag7);
        Memory::Write8(plw + kFlag7, uint8_t(cooldown - std::min(cooldown, GameStep::Ticks())));
        if (game.Call<AnyInput>({plw}) == 1 && Memory::Read8(plw + kFlag7) == 0) {
            Memory::Write8(plw + kFlag7, 3);
            p.Timer(p.Timer() - 1);
        }
        if (p.CountDown()) {
            p.NextState();
            p.Set(207, 0, 0);
        } else {
            Effect13Every50(p);
        }
        break;
    }
    case 3:
        if (p.Ended())
            game.Call<EndActionReset>({plw, 0, 4, 0});
        break;
    }
}

// 0x802580D4.
void Effect13(CpuContext* ctx) {
    GuestFrame frame(ctx, 16);
    GuestCall game{ctx};
    Effect13Every50(Player{{game, frame.Arg(3)}});
}

// 0x80257EEC (plw, alt): held (motion 26, or 262 when alt) until 5 stages
// are escaped: each takes a count of inputs (0x80794510, twice with skill 9)
// within 300 steps, or is lost when the 300 steps run out.
void Escape(CpuContext* ctx) {
    GuestFrame frame(ctx, 16);
    GuestCall game{ctx};
    const Player p{{game, frame.Arg(3)}};
    const uint32_t plw = p.plw, alt = frame.Arg(4);
    const auto nextStage = [&] {
        Memory::Write8(plw + kInputs, 0);
        Memory::Write8(plw + kStages, uint8_t(Memory::Read8(plw + kStages) - 1));
        p.Timer(300);
    };
    switch (p.State()) {
    case 0:
        p.NextState();
        Memory::Write8(plw + kFlag24, 0);
        game.Call<0x80277CB4>({plw, 0});
        p.Flags(alt ? 3 : 0, 0, 1);
        p.Set(alt ? 262 : 26, 4, 0);
        game.Call<MotionAfterSet>({plw});
        game.Call<0x8027AC70>({plw});
        p.Timer(300);
        Memory::Write8(plw + kInputs, 0);
        Memory::Write8(plw + kStages, 5);
        Memory::Write16(plw + kWord1024, 0);
        Memory::Write16(plw + kWord1020, 0);
        game.Call<0x80245E04>({plw, 0});
        Memory::Write32(plw + kAngle, Memory::Read32(plw + kAngle2) & 0xFFFF);
        game.Call<0x8010C5B8>({plw});
        break;
    case 1:
        if (p.CountDown()) {
            nextStage();
        } else if (game.Call<AnyInput>({plw}) == 1) {
            // The original counts the steps with input: here their ticks.
            const uint32_t inputs = Memory::Read8(plw + kInputs) + GameStep::Ticks();
            Memory::Write8(plw + kInputs, uint8_t(std::min(inputs, 255u)));
            int8_t needed = int8_t(Memory::Read8(EscapeStages + Memory::Read8(plw + kStages)));
            if (game.Call<SkillCheck>({plw, 9}) == 1)
                needed = int8_t(needed * 2);
            if (int32_t(Memory::Read8(plw + kInputs)) >= needed)
                nextStage();
        }
        if (Memory::Read8(plw + kStages) == 0) {
            game.Call<0x80257ED4>({plw});
            Memory::Write8(plw + kFlag789, 20);
            game.Call<EndActionReset>({plw, Memory::Read8(plw + kFlag9), 6, 0});
        }
        break;
    }
}

// 0x8025DC40 and 0x8025DA9C: another player's (stand-in) actions; each step
// clears its targeting state. After 300 steps the PLW is freed.
void RemoteStep(const Player& p) {
    const uint32_t plw = p.plw;
    p.game.Call<SetState1408>({plw, 10});
    p.game.Call<SetState918>({plw, 10});
    p.game.Call<0x8027A1E0>({plw});
    p.game.Call<0x8027AC70>({plw});
    for (const uint32_t flag : {kFlag1468, kFlag1469, kFlag1640, kFlag1470})
        Memory::Write8(plw + flag, 0);
    p.game.Call<ClearActFlags>({plw, 2048});
}
void RemoteStart(const Player& p) {
    Memory::Write8(p.plw + kVisible, 1);
    p.Timer(300);
    Memory::Write8(p.plw + kFlag24, 0);
}
// The steps count only for players that aren't this console's.
void RemoteWait(const Player& p) {
    if (p.game.Call<MasterCheck>({p.plw}) != 0)
        return;
    p.Timer(p.Timer() - int32_t(GameStep::Ticks()));
    if (p.Timer() < 0)
        p.NextState();
}
void RemoteFree(const Player& p) {
    Memory::Write8(p.plw + 0, 0);
    Memory::Write8(p.plw + kFlag1, 0);
}

// 0x8025DC40 (plw, alt): motion 210 standing on the ground (259 when alt).
void RemoteStand(CpuContext* ctx) {
    GuestFrame frame(ctx, 32);
    GuestCall game{ctx};
    const Player p{{game, frame.Arg(3)}};
    const uint32_t plw = p.plw, alt = frame.Arg(4);
    RemoteStep(p);
    switch (p.State()) {
    case 0:
        p.NextState();
        RemoteStart(p);
        if (alt == 0) {
            p.Flags(0, 0, 0);
            p.Set(210, 4, 0);
            Memory::Write32(plw + kPosition + 4, Memory::Read32(plw + kLastY));
        } else {
            p.Flags(3, 0, 0);
            p.Set(259, 4, 0);
        }
        break;
    case 1:
        RemoteWait(p);
        break;
    case 2:
        RemoteFree(p);
        break;
    }
}

// 0x8025DA9C (plw, alt): motion 21 on the ground, then 5; or 122 when alt.
void RemoteRest(CpuContext* ctx) {
    GuestFrame frame(ctx, 32);
    GuestCall game{ctx};
    const Player p{{game, frame.Arg(3)}};
    const uint32_t plw = p.plw, alt = frame.Arg(4);
    RemoteStep(p);
    switch (p.State()) {
    case 0:
        p.NextState();
        RemoteStart(p);
        if (alt == 0) {
            p.Flags(0, 0, 0);
            p.Set(21, 4, 0);
            Memory::Write32(plw + kPosition + 4, Memory::Read32(plw + kLastY));
        } else {
            Memory::Write8(plw + kState, 2);
            p.Flags(3, 0, 0);
            p.Set(122, 0, 86);
        }
        break;
    case 1:
        if (p.Ended()) {
            p.NextState();
            p.Set(5, 0, 146);
        }
        break;
    case 2:
        RemoteWait(p);
        break;
    case 3:
        RemoteFree(p);
        break;
    }
}

// 0x802596E8 (plw, s16 arg): motion 53 (59 when +0x14) moving forward 5 a
// step, 20 steps counted down, 0x80259030 each step, until 0x80277E10 finds
// the way (85, -60): then action (6, 48), or (6, 68) with arg. Only the
// local player's moves.
void Walk53(CpuContext* ctx) {
    GuestFrame frame(ctx, 32);
    GuestCall game{ctx};
    const Player p{{game, frame.Arg(3)}};
    const uint32_t plw = p.plw;
    const uint32_t arg = uint32_t(int32_t(int16_t(frame.Arg(4))));
    switch (p.State()) {
    case 0:
        p.NextState();
        p.SetX(Memory::Read8(plw + kFlag20) == 0 ? 53 : 59, 4, 0);
        game.Call<0x8027AC70>({plw});
        p.Timer(20);
        Memory::Write8(plw + kFlag614, 30);
        game.Call<MotionAfterSet>({plw});
        p.Flags(0, 0, 1);
        game.Call<0x8027D554>({plw});
        game.Call<SetActFlags>({plw, 384});
        game.Call<0x8027714C>({plw, 0x805C571C, 0});
        break;
    case 1: {
        if (game.Call<MasterCheck>({plw}) == 0)
            break;
        if (game.Call<0x80277E10>({plw, 0}, {85.0, -60.0}) != 0) {
            Memory::Write32(plw + kAngle, Memory::Read32(plw + kAngle2) & 0xFFFF);
            p.Next(6, arg ? 68 : 48, 0);
            break;
        }
        const uint32_t step = frame[8];
        MoveTurned(p, step, 5.0f);
        for (uint32_t k = 0; k < 12; k += 4)
            Memory::WriteFloat32(plw + kPosition + k, Memory::ReadFloat32(plw + kPosition + k) + Memory::ReadFloat32(step + k));
        p.CountDownToZero();
        game.Call<0x80259030>({plw, 1, arg, 0});
        break;
    }
    }
}

// 0x8028138C: a step of charging. The original counts 1 a step: here the
// step's ticks, so a charge takes the same time at any step length.
void ChargeStep(const Player& p) {
    const uint32_t plw = p.plw;
    Memory::Write16(plw + kChargeSteps, uint16_t(int16_t(Memory::Read16(plw + kChargeSteps)) + int32_t(GameStep::Ticks())));
    Memory::Write8(plw + kChargeLevel, uint8_t(p.game.Call<ChargeLevel>({plw})));
    p.game.Call<ChargeEffect>({plw});
}
void Charge(CpuContext* ctx) {
    GuestFrame frame(ctx, 16);
    GuestCall game{ctx};
    ChargeStep(Player{{game, frame.Arg(3)}});
}

// 0x80281DBC and 0x80282770: a charge (motion 1133 / 1170). From frame 34
// the motion runs at half speed charging until the button is let go, up to
// 90 steps (ChargeTime); from frame 66, once let go or out of time, the
// local player's goes on to the attack (action 36 / 65).
template <uint32_t kMode, uint32_t kMotion, uint32_t kAction, uint32_t kActionB>
void ChargeHold(CpuContext* ctx) {
    GuestFrame frame(ctx, 16);
    GuestCall game{ctx};
    const Player p{{game, frame.Arg(3)}};
    const uint32_t plw = p.plw;
    game.Call<0x80277CB4>({plw, 2});
    switch (p.State()) {
    case 0:
        p.NextState();
        p.Flags(kMode, 1, 0);
        p.Set(kMotion, 4, 0);
        Memory::Write8(plw + kChargeLevel, 0);
        Memory::Write16(plw + kChargeSteps, 0);
        p.Timer(int16_t(game.Call<ChargeTime>({plw, 90})));
        break;
    case 1:
        if (game.Call<FrameCheck>({plw, 1}, {34.0, 0.0}) == 1) {
            if (Memory::Read8(plw + kFlag7) == 0) {
                Memory::Write8(plw + kFlag7, 1);
                game.Call<0x8011ADBC>({plw, 0});
            }
            // Command 17 clear (the button let go) or 13 ends the charge (+6).
            if (!Bit(plw + kCommands, 17) || Bit(plw + kCommands, 13))
                Memory::Write8(plw + kFlag6, 1);
            if (Memory::Read8(plw + kFlag6) == 0) {
                Memory::WriteFloat32(plw + kMotionRate, 0.5f);
                ChargeStep(p);
                if (p.CountDown())
                    Memory::Write8(plw + kFlag6, 1);
            } else {
                Memory::WriteFloat32(plw + kMotionRate, 1.0f);
            }
        }
        if (game.Call<FrameCheck>({plw, 1}, {66.0, 0.0}) == 1 && Memory::Read8(plw + kFlag6) == 1 && p.Master()) {
            Memory::Write8(plw + kChargeLevel, uint8_t(game.Call<ChargeLevel>({plw})));
            Memory::WriteFloat32(plw + kMotionRate, 1.0f);
            p.Next(4, kAction, kActionB);
        }
        break;
    }
}

// 0x80247704: climbing up a ledge (motion 17): the player moves up 210 and
// 55 ahead onto the ground there; after 30 steps the local player's may walk
// off (action 6), or goes on at once (action 7) on 0x80277144, command bit 6
// or pad bit 0x2000.
void ClimbUp(CpuContext* ctx) {
    GuestFrame frame(ctx, 64);
    GuestCall game{ctx};
    const Player p{{game, frame.Arg(3)}};
    const uint32_t plw = p.plw;
    game.Call<SetState1408>({plw, 2});
    switch (p.State()) {
    case 0: {
        p.NextState();
        p.Flags(0, 0, 0);
        Memory::Write32(plw + kAngle, Memory::Read32(plw + kAngle2));
        p.Set(17, 0, 0);
        game.Call<MotionAfterSet>({plw});
        p.Timer(30);
        const uint32_t up = frame[32], top = frame[20];
        Memory::WriteFloat32(up + 0, 0.0f);
        Memory::WriteFloat32(up + 4, 210.0f);
        Memory::WriteFloat32(up + 8, 55.0f);
        game.Call<RotVecY>({up, Memory::Read32(plw + kAngle2)});
        for (uint32_t k = 0; k < 12; k += 4)
            Memory::WriteFloat32(top + k, Memory::ReadFloat32(plw + kPosition + k) + Memory::ReadFloat32(up + k));
        game.Call<GroundHit>({top, uint32_t(-6), Memory::Read8(plw + kGroundKind)});
        Memory::WriteFloat32(plw + kPosition + 4, float(game.F1()));
        Memory::WriteFloat32(plw + kPosition + 0, Memory::ReadFloat32(plw + kPosition + 0) + Memory::ReadFloat32(up + 0));
        Memory::WriteFloat32(plw + kPosition + 8, Memory::ReadFloat32(plw + kPosition + 8) + Memory::ReadFloat32(up + 8));
        game.Call<0x80245E04>({plw, 0});
        game.Call<0x800DC548>({plw + kPosition});
        if (Memory::Read8(plw + kFlag15) == 2 && Memory::Read16(plw + kWord16) == 1)
            Memory::Write8(plw + kClimbs, uint8_t(Memory::Read8(plw + kClimbs) + 1));
        break;
    }
    case 1:
        p.CountDownToZero();
        if (game.Call<MasterCheck>({plw}) == 0)
            break;
        if (game.Call<0x80277144>({plw}) == 1 || Bit(plw + kCommandBits, 6) || (Memory::Read16(plw + kPadHeld) & 0x2000))
            p.Next(0, 7, 0);
        else if ((Memory::Read16(plw + kPadPressed) & 0x1000) && p.Timer() == 0)
            p.Next(2, 6, 0);
        break;
    }
}

// 0x80249944: standing in the field: idle motion 100 (118 at the water's
// surface, 121 in heat or cold, 360 when +0x37A is 150 or less); +24 set
// holds it. The lean (+0xAC) is cleared 30 steps in.
void Stand(CpuContext* ctx) {
    GuestFrame frame(ctx, 16);
    GuestCall game{ctx};
    const Player p{{game, frame.Arg(3)}};
    const uint32_t plw = p.plw;
    switch (p.State()) {
    case 0:
        p.NextState();
        p.Flags(0x8003, 0, 0);
        p.Timer(30);
        if (Memory::Read8(plw + kFlag24) == 0)
            game.Call<0x8027714C>({plw, 0x805BEFE4, 0});
        else
            game.Call<0x80277204>({plw, 0});
        break;
    case 1:
        // The original counts on past 0 while 0x8027A3A4 says no.
        if (p.Timer() > 0 || game.Call<0x8027A3A4>({plw}) == 0) {
            if (p.CountDown())
                Memory::Write32(plw + kLean, 0);
        }
        if (p.Master() && game.Call<0x8026FEA8>({plw}) == 1 && Memory::Read8(plw + kFlag24) == 1)
            game.Call<0x8027A5E0>({plw, 1050, 0});
        if (Memory::Read8(plw + kFlag24) == 1)
            break;
        if (game.Call<ClimateActive>({plw, 0}) == 1)
            p.SetX(121, 4, 0);
        else if (int16_t(Memory::Read16(plw + kStaminaMax)) <= 150)
            p.SetX(360, 4, 0);
        else if (game.Call<InWater>({plw}) == 1)
            p.SetX(118, 4, 0);
        else
            p.SetX(100, 4, 0);
        break;
    }
}

// 0x80253CA0: motion 105, the lean held within 60 degrees, draining
// stamina (4 a step) for 25 steps; when it ends action 8 when 0x8026F96C
// finds none, else 9 on command bit 37 (or for another console's player),
// else 7.
void Motion105(CpuContext* ctx) {
    GuestFrame frame(ctx, 16);
    GuestCall game{ctx};
    const Player p{{game, frame.Arg(3)}};
    const uint32_t plw = p.plw;
    switch (p.State()) {
    case 0: {
        p.NextState();
        p.Flags(3, 0, 0);
        game.Call<SetActFlags>({plw, 192});
        p.Set(105, 4, 0);
        game.Call<MotionAfterSet>({plw});
        game.Call<0x8027714C>({plw, 0x805BF220, 0});
        game.Call<0x802785C8>({plw, 1});
        p.Timer(25);
        // Within 60 degrees (10923): the original's bounds, -10922 .. 10923.
        const int32_t lean = int16_t(Memory::Read32(plw + kLean));
        if (lean <= -10922)
            Memory::Write32(plw + kLean, 0x10000 - 10922);
        else if (lean >= 10923)
            Memory::Write32(plw + kLean, 10923);
        break;
    }
    case 1:
        if (p.Timer() > 0) {
            p.CountDownToZero();
            game.Call<StaminaChange>({plw, uint32_t(-4)}); // per tick (stamina.cpp)
        }
        if (!p.Ended())
            break;
        if (game.Call<MasterCheck>({plw}) == 0)
            p.Next(1, 9, 128);
        else if ((game.Call<Nearby>({plw, 0}) & 0xFF) < 1)
            p.Next(1, 8, 0);
        else if (Bit(plw + kCommandBits, 37))
            p.Next(1, 9, 128);
        else
            p.Next(1, 7, 128);
        break;
    }
}

// 0x80246BE0: motion 1 (0x8027714C with 0x805BE408, or 0x80277204 when +24
// is set), and 30 steps in motion 66 (63 when +20 is set). As each ends the
// motions go on: 21 to 5, 63 to 65, 66 to 67; otherwise the tired idle 303
// when +0x37A is 150 or less (not with +24 set).
void Motion63Or66(CpuContext* ctx) {
    GuestFrame frame(ctx, 16);
    GuestCall game{ctx};
    const Player p{{game, frame.Arg(3)}};
    const uint32_t plw = p.plw;
    switch (p.State()) {
    case 0:
        p.NextState();
        p.Flags(0x8000, 0, 0);
        if (Memory::Read8(plw + kFlag24) == 0)
            game.Call<0x8027714C>({plw, 0x805BE408, 0});
        else
            game.Call<0x80277204>({plw, 0});
        p.Timer(0);
        [[fallthrough]];
    case 1:
        p.CountUp();
        if (p.Timer() > 30 && p.Motion() == 1)
            p.Set(Memory::Read8(plw + kFlag20) ? 63 : 66, 6, 0);
        if (p.Master() && game.Call<0x8026FEA8>({plw}) == 1 && Memory::Read8(plw + kFlag24) == 1)
            game.Call<0x8027A5E0>({plw, 1001, 0});
        switch (p.Motion()) {
        case 21:
            if (p.Ended())
                p.Set(5, 4, 144);
            break;
        case 63:
            if (p.Ended())
                p.Set(65, 2, 0);
            break;
        case 66:
            if (p.Ended())
                p.Set(67, 2, 0);
            break;
        default:
            if (Memory::Read8(plw + kFlag24) != 1 && int16_t(Memory::Read16(plw + kStaminaMax)) <= 150)
                p.SetX(303, 4, 0);
            break;
        }
        break;
    }
}

// 0x802516C0 (plw, alt): using the item at +774: motion 314 (365 when alt),
// 60 steps, then on to action 170 (173) while 0x80273218 still has the item,
// or the end. Items 380 and 583 tell the object at *0x80797020 (its call at
// +120, with 3 or 5), 384 goes to 0x802ADEF0.
void UseItem(CpuContext* ctx) {
    GuestFrame frame(ctx, 16);
    GuestCall game{ctx};
    const Player p{{game, frame.Arg(3)}};
    const uint32_t plw = p.plw, alt = frame.Arg(4);
    game.Call<SetState1408>({plw, 2});
    switch (p.State()) {
    case 0: {
        p.NextState();
        if (alt == 0) {
            p.Flags(0, 0, 0);
            p.Set(314, 0, 66);
        } else {
            p.Flags(3, 0, 0);
            p.Set(365, 0, 66);
        }
        game.Call<MotionAfterSet>({plw});
        p.Timer(60);
        if (p.Master() && int32_t(game.Call<0x80273218>({plw, Memory::Read16(plw + kItem774)})) <= 0)
            break;
        if (game.Call<InEventDemo>({1}) == 0 && Memory::Read16(plw + kItem774) != 384)
            game.Call<0x80272E94>({plw, Memory::Read16(plw + kItem774), uint32_t(-1)});
        game.Call<0x802BE5CC>({plw, 0});
        const uint32_t item = Memory::Read16(plw + kItem774);
        if (item == 380 || item == 583) {
            const uint32_t obj = Memory::Read32(0x80797020);
            game(Memory::Read32(obj + 120), {plw, item == 380 ? 3u : 5u, obj});
        } else if (item == 384) {
            game.Call<0x802ADEF0>({plw});
        }
        break;
    }
    case 1: {
        // The original fires only as the countdown reaches 0 (not below).
        const int32_t before = p.Timer();
        if (!p.CountDown() || before <= 0)
            break;
        if (int32_t(game.Call<0x80273218>({plw, Memory::Read16(plw + kItem774)})) < 1 || Memory::Read16(plw + kItem774) == 384)
            p.End(Memory::Read8(plw + kFlag9), 6);
        else
            p.Next(0, alt ? 173 : 170, 44);
        break;
    }
    }
}

// 0x80251470 (plw, fresh, alt): holding the item for another use: motion 314
// (365 when alt), fresh with sound 13 or carried on from the last use (blend
// -4, +0x5E8/+0x583 cleared, +0x38 the facing). After 30 steps (+6) the local
// player's command 76 goes on to action 171 (174) while the item lasts; after
// 60 (+0x28) command 7 ends it, as an event demo at once.
void UseItemRepeat(CpuContext* ctx) {
    GuestFrame frame(ctx, 32);
    GuestCall game{ctx};
    const Player p{{game, frame.Arg(3)}};
    const uint32_t plw = p.plw, fresh = frame.Arg(4), alt = frame.Arg(5);
    const uint32_t motion = alt == 0 ? 314 : 365;
    game.Call<SetState1408>({plw, 2});
    switch (p.State()) {
    case 0:
        p.NextState();
        Memory::Write8(plw + kFlag24, 0);
        p.Flags(alt == 0 ? 0 : 3, 0, 0);
        if (fresh == 0) {
            p.Set(motion, -4, 0);
            Memory::Write16(plw + 0x5E8, 0);
            Memory::Write8(plw + 0x583, 0);
            Memory::Write16(plw + 0x38, uint16_t(Memory::Read32(plw + kAngle)));
        } else {
            if (p.Master())
                game.Call<0x800DDD1C>({13, plw + kPosition}); // map_se_req
            p.Set(motion, 0, 66);
        }
        game.Call<MotionAfterSet>({plw});
        p.Timer(60);
        Memory::Write8(plw + kFlag6, 30);
        break;
    case 1: {
        p.CountDownToZero();
        const uint32_t hold = Memory::Read8(plw + kFlag6);
        Memory::Write8(plw + kFlag6, uint8_t(hold - std::min(hold, GameStep::Ticks())));
        if (!p.Master())
            break;
        if (game.Call<InEventDemo>({1}) == 1) {
            p.End(Memory::Read8(plw + kFlag9), 6);
            break;
        }
        if (Memory::Read8(plw + kFlag6) == 0) {
            game.Call<0x8027A5E0>({plw, motion, 2});
            if (game.Call<0x8026A6A8>({plw, 76}) == 1 && int32_t(game.Call<0x80273218>({plw, Memory::Read16(plw + kItem774)})) >= 1) {
                p.Next(0, alt == 0 ? 171 : 174, 108);
                break;
            }
        }
        if (p.Timer() == 0 && game.Call<0x8026A6A8>({plw, 7}) == 1)
            p.End(Memory::Read8(plw + kFlag9), 6);
        break;
    }
    }
}

// 0x802886AC (plw, next, alt): weapon motion 1001 (1050 when alt, with
// 0x80279F20's 1030/1031 or 1080/1081) for as many steps as 0x805CB9B0 gives
// the weapon kind at +0x26C (40, 41, other), 0x8031D2F8 on for its first 5
// steps (+6). Then the local player goes on to entry `next` of the u16
// actions at 0x80794654 (0x8079465C).
void Weapon1001(CpuContext* ctx) {
    GuestFrame frame(ctx, 32);
    GuestCall game{ctx};
    const Player p{{game, frame.Arg(3)}};
    const uint32_t plw = p.plw, next = frame.Arg(4), alt = frame.Arg(5);
    switch (p.State()) {
    case 0: {
        p.NextState();
        Memory::Write8(plw + 0x36A, 6);
        if (alt == 0) {
            p.Flags(0, 1, 0);
            p.Set(1001, 4, 0);
            game.Call<0x80279F20>({plw, 1030, 1031});
        } else {
            p.Flags(3, 1, 0);
            p.Set(1050, 4, 0);
            game.Call<0x80279F20>({plw, 1080, 1081});
        }
        game.Call<0x8031D2F8>({plw, 1});
        const uint32_t kind = Memory::Read8(plw + 0x26C);
        p.Timer(int32_t(Memory::Read32(0x805CB9B0 + 4 * (kind == 40 ? 1 : kind == 41 ? 2 : 0))));
        Memory::Write8(plw + kFlag6, 5);
        Memory::Write32(plw + kAngle2, Memory::Read32(plw + kAngle));
        Memory::Write32(plw + 0x54, Memory::Read32(plw + 0xAC));
        break;
    }
    case 1: {
        if (const uint32_t n = Memory::Read8(plw + kFlag6)) {
            Memory::Write8(plw + kFlag6, uint8_t(n - std::min(n, GameStep::Ticks())));
            if (Memory::Read8(plw + kFlag6) == 0)
                game.Call<0x8031D2F8>({plw, 0});
        }
        if (!game.Call<MasterCheck>({plw}))
            break;
        if (p.CountDown()) {
            const uint32_t action = Memory::Read16((alt == 0 ? 0x80794654 : 0x8079465C) + 2 * next);
            game.Call<SetAction>({plw, 4, action, alt == 0 ? 5u : 133u});
        } else {
            game.Call<0x8027A5E0>({plw, alt == 0 ? 1001u : 1050u, 0});
        }
        break;
    }
    }
}

// 0x8025AA9C (plw, mode): motion 241. The local player's mode 0 waits up to
// 300 steps for 0x8012D4FC to find one: its kind (+3: 1, 2, 4, 7) picks 15,
// 17, 14 or 22 (else 15), plus 0-3 at random, for 0x8025A844, then action 7
// (2); running out, or the finder's flag, is action 7 (3). Mode 1 (with
// 0x8027714C's 0x805C5880) steps 0x8025A860 until +0x3A4 or 0x8012D4CC clears.
void Motion241(CpuContext* ctx) {
    GuestFrame frame(ctx, 32);
    GuestCall game{ctx};
    const Player p{{game, frame.Arg(3)}};
    const uint32_t plw = p.plw, mode = frame.Arg(4), found = frame[8];
    game.Call<SetState918>({plw, 2});
    game.Call<SetState1408>({plw, 2});
    switch (p.State()) {
    case 0:
        p.NextState();
        p.SetX(241, 2, 0);
        game.Call<MotionAfterSet>({plw});
        p.Flags(0, 0, 1);
        game.Call<0x8027D554>({plw});
        Memory::Write8(plw + kFlag24, 0);
        if (mode == 0)
            p.Timer(300);
        Memory::Write8(plw + kFlag6, 30);
        Memory::Write8(plw + kFlag7, 3);
        if (mode == 1)
            game.Call<0x8027714C>({plw, 0x805C5880, 0});
        break;
    case 1: {
        if (!p.Master())
            break;
        const uint32_t n = Memory::Read8(plw + kFlag6);
        Memory::Write8(plw + kFlag6, uint8_t(n - std::min(n, GameStep::Ticks())));
        if (mode == 0) {
            if (const uint32_t hit = game.Call<0x8012D4FC>({0, plw, found})) {
                const uint32_t kind = Memory::Read8(hit + 3);
                const uint32_t base = kind == 2 ? 17 : kind == 4 ? 14 : kind == 7 ? 22 : 15;
                game.Call<0x8025A844>({plw, (base + (game.Call<Rand>({1}) & 3)) & 0xFF});
                p.Next(7, 2, 0);
                break;
            }
            // The original fires once the countdown was out before the step.
            p.Timer(p.Timer() - int32_t(GameStep::Ticks()));
            if (p.Timer() < 0 || Memory::Read8(found) == 1)
                p.Next(7, 3, 0);
        } else if (mode == 1) {
            game.Call<0x8025A860>({plw});
            if (Memory::Read8(plw + 0x3A4) == 0 || game.Call<0x8012D4CC>({1, plw, found}) == 0)
                p.Next(7, 3, 0);
        }
        break;
    }
    }
}

// 0x8024FF6C (plw, kind): motion 7 into 8 (kind 0), or 8 (1) or 118 (2) at
// once, each held with 0x8027A5E0. The local player's commands 7 and 1 (or an
// event demo) end it once 20 steps are out.
void Motion7Or8(CpuContext* ctx) {
    GuestFrame frame(ctx, 16);
    GuestCall game{ctx};
    const Player p{{game, frame.Arg(3)}};
    const uint32_t plw = p.plw, kind = frame.Arg(4);
    switch (p.State()) {
    case 0:
        p.NextState();
        p.Flags(1, 0, 0);
        Memory::Write8(plw + 0x5E6, 1);
        Memory::Write8(plw + 0x5E5, 4);
        Memory::Write8(plw + kFlag24, 0);
        p.Timer(20);
        if (kind == 0) {
            p.Set(7, 2, 0);
        } else if (kind == 1) {
            p.NextState();
            p.Set(8, 2, 0);
        } else if (kind == 2) {
            p.NextState();
            p.Set(118, 2, 0);
            p.Flags(3, 0, 0);
        }
        break;
    case 1:
        p.CountDownToZero();
        if (p.Ended()) {
            p.NextState();
            p.Set(8, 6, 0);
            game.Call<0x8027A5E0>({plw, 8, 1});
        } else {
            game.Call<0x8027A5E0>({plw, 7, 1});
        }
        break;
    case 2:
        if (!game.Call<MasterCheck>({plw}))
            break;
        game.Call<0x8027A5E0>({plw, kind == 2 ? 118u : 8u, 1});
        if (p.Timer() > 0) {
            p.CountDownToZero();
            break;
        }
        if (game.Call<0x8026A6A8>({plw, 7}) == 1 || game.Call<0x8026A6A8>({plw, 1}) == 1 || game.Call<InEventDemo>({1}) == 1) {
            game.Call<0x8027A1E0>({plw});
            p.End(kind == 2 ? 3 : 1, 4);
        }
        break;
    }
}

// 0x802821DC (plw, kind): a charge (motion 1150, from 1164, 1162 or 1167
// first). From frame 48 the local player's motion runs at half speed
// charging (ChargeTime of 90 steps) until command 5 clears (the button let
// go) or 13, then goes on to the attack (action 48).
void Charge1150(CpuContext* ctx) {
    GuestFrame frame(ctx, 16);
    GuestCall game{ctx};
    const Player p{{game, frame.Arg(3)}};
    const uint32_t plw = p.plw, kind = frame.Arg(4);
    game.Call<0x80277CB4>({plw, 2});
    switch (p.State()) {
    case 0:
        p.NextState();
        p.Flags(3, 1, 0);
        Memory::Write8(plw + kFlag24, 1);
        Memory::Write8(plw + kChargeLevel, 0);
        Memory::Write16(plw + kChargeSteps, 0);
        p.Timer(int16_t(game.Call<ChargeTime>({plw, 90})));
        if (kind == 0) {
            p.NextState();
            p.Set(1150, 4, 0);
        } else if (kind == 1) {
            p.Set(1164, 4, 0);
        } else if (kind == 2) {
            p.Set(1162, 4, 0);
        } else if (kind == 3) {
            game.Call<0x8027E21C>({plw, 1});
            p.Set(1167, 4, 0);
        }
        break;
    case 1:
        if (p.Ended()) {
            p.NextState();
            p.Set(1150, 2, 48);
        }
        break;
    case 2:
        if (!game.Call<MasterCheck>({plw}))
            break;
        if (game.Call<FrameCheck>({plw, 1}, {48.0, 0.0}) != 1)
            break;
        if (Memory::Read8(plw + kFlag7) == 0) {
            Memory::Write8(plw + kFlag7, 1);
            game.Call<0x8011ADBC>({plw, 0});
        }
        if (!Bit(plw + kCommands, 5) || Bit(plw + kCommands, 13))
            Memory::Write8(plw + kFlag6, 1);
        if (Memory::Read8(plw + kFlag6) == 0) {
            Memory::WriteFloat32(plw + kMotionRate, 0.5f);
            ChargeStep(p);
            if (p.CountDown())
                Memory::Write8(plw + kFlag6, 1);
        }
        if (Memory::Read8(plw + kFlag6) == 1) {
            Memory::WriteFloat32(plw + kMotionRate, 1.0f);
            Memory::Write8(plw + kChargeLevel, uint8_t(game.Call<ChargeLevel>({plw})));
            p.Next(4, 48, 388);
        }
        break;
    }
}

// 0x8025B15C (plw, kind): motion 210 (from frame 170 for kind 1; 209 from
// 120 for kind 2, MHchar +0xF6 = 2), 1 in 64 steps past frame 164 turning
// into 223 and back to 210 at 164. The local player's leaves (state 2,
// 0x803AB11C) after 300 steps.
void Motion210(CpuContext* ctx) {
    GuestFrame frame(ctx, 16);
    GuestCall game{ctx};
    const Player p{{game, frame.Arg(3)}};
    const uint32_t plw = p.plw, kind = frame.Arg(4);
    game.Call<SetState918>({plw, 2});
    game.Call<SetState1408>({plw, 10});
    game.Call<0x8027A1E0>({plw});
    game.Call<0x80245E04>({plw, 0});
    Memory::Write8(plw + kFlag1468, 0);
    Memory::Write8(plw + kFlag1470, 0);
    Memory::Write8(plw + kFlag1469, 0);
    Memory::Write8(plw + kFlag1640, 0);
    game.Call<ClearActFlags>({plw, 2048});
    if (kind == 2)
        Memory::Write8(p.Char() + kCharFlag246, 2);
    const uint32_t state = p.State();
    if (state == 0) {
        p.NextState();
        game.Call<0x8027AC70>({plw});
        p.Timer(0);
        if (kind == 0)
            p.Set(210, 0, 0);
        else if (kind == 1)
            p.Set(210, 0, 170);
        else if (kind == 2)
            p.Set(209, 0, 120);
        game.Call<0x80275524>({plw, 3});
        game.Call<MotionAfterSet>({plw});
        game.Call<0x8025B138>({plw});
        p.Flags(0, 0, 1);
        game.Call<ClearActFlags>({plw, 4});
        game.Call<ClearActFlags>({plw, 16});
        Memory::Write8(plw + kFlag24, 0);
        if (p.Master()) {
            game.Call<0x802BC6C0>();
            game.Call<0x803AB160>({Memory::Read8(plw + 8)});
        }
        return;
    }
    if (state > 2)
        return;
    const uint32_t motion = p.Motion();
    if (motion == 210) {
        if (game.Call<FrameCheck>({plw, 0}, {164.0, 0.0}) == 1 && (game.Call<Rand>({1}) & 63) == 0)
            p.Set(223, 0, 0);
    } else if (motion == 223) {
        if (p.Ended())
            p.Set(210, 0, 164);
    }
    if (p.Master() && p.State() == 1) {
        p.CountUp();
        if (p.Timer() >= 300) {
            p.NextState();
            game.Call<0x803AB11C>({plw});
        }
    }
}

// 0x8028563C and 0x802869D4: a spin after a charge (motion 1103 / 1162):
// the charge level + 2 loops (up to 5), cues at frames 26 and 52 of each,
// the local player steering it (0x8026FB84 2048 / 1024: 192 a step either
// way). The last loop's frame 44 goes on to the finish (motion 1104 / 1163),
// which ends the action; each loop from the second takes 0x8027714C's list.
// The loops (+0x28, +6) count motion loops, not steps.
struct SpinParams {
    uint32_t motion, finish, mode, flag9;
    uint32_t cue0, cue26, cue52, cueFinish; // 0x80284268's
    uint32_t list0, list;                   // 0x8027714C's
    double rate;                            // 0x80284654's while finishing
};
template <const SpinParams& k>
void Spin(CpuContext* ctx) {
    GuestFrame frame(ctx, 16);
    GuestCall game{ctx};
    const Player p{{game, frame.Arg(3)}};
    const uint32_t plw = p.plw;
    switch (p.State()) {
    case 0:
        p.NextState();
        p.Set(k.motion, 6, 0);
        Memory::Write8(plw + kFlag6, uint8_t(std::min<uint32_t>((Memory::Read8(plw + kChargeLevel) + 2) & 0xFF, 5)));
        Memory::Write8(plw + kFlag7, 0);
        p.Timer(0);
        Memory::Write32(plw + kAngle, Memory::Read32(plw + kAngle2) & 0xFFFF);
        game.Call<0x80284268>({plw, k.cue0, 0});
        p.Flags(k.mode, 1, 0);
        game.Call<0x80277CB4>({plw, 60});
        game.Call<0x8027714C>({plw, k.list0, 0});
        break;
    case 1:
        game.Call<0x80277CB4>({plw, 2});
        if (p.Master()) {
            // The original: 192 a step. Here its share of this step.
            if (game.Call<0x8026FB84>({plw, 2048}) == 1)
                Memory::Write32(plw + kAngle, Memory::Read32(plw + kAngle) + uint32_t(GameStep::Share(192)));
            else if (game.Call<0x8026FB84>({plw, 1024}) == 1)
                Memory::Write32(plw + kAngle, Memory::Read32(plw + kAngle) - uint32_t(GameStep::Share(192)));
        }
        if (game.Call<FrameCheck>({plw, 0}, {26.0, 0.0}) == 1)
            game.Call<0x80284268>({plw, k.cue26, 0});
        if (game.Call<FrameCheck>({plw, 0}, {52.0, 0.0}) == 1)
            game.Call<0x80284268>({plw, k.cue52, 0});
        if (game.Call<FrameCheck>({plw, 0}, {44.0, 0.0}) != 1)
            break;
        p.Timer(p.Timer() + 1);
        Memory::Write8(plw + kFlag6, uint8_t(Memory::Read8(plw + kFlag6) - 1));
        if (Memory::Read8(plw + kFlag6) == 0) {
            p.NextState();
            p.Set(k.finish, 0, 0);
            Memory::Write16(plw + 182, uint16_t(Memory::Read16(plw + 182) + 3));
            game.Call<0x80284268>({plw, k.cueFinish, 0});
            game.Call<0x8027714C>({plw, 0, 0});
        } else if (p.Timer() >= 2) {
            game.Call<0x8027714C>({plw, k.list, 0});
        }
        break;
    case 2:
        game.Call<0x80277CB4>({plw, 2});
        if (p.Ended())
            p.End(k.flag9, 6);
        else
            game.Call<0x80284654>({plw}, {k.rate});
        break;
    }
}
constexpr SpinParams kSpin1103{1103, 1104, 0, 0, 10, 11, 12, 13, 0x805CA2DC, 0x805CA308, 38.0};
constexpr SpinParams kSpin1162{1162, 1163, 3, 3, 34, 31, 32, 33, 0x805CA414, 0x805CA440, -1.0};

// The falls (motion 56) of 0x802598C4 and 0x8025CECC. The velocity (+0x78,
// y +0x7C, forward +0x80) takes the acceleration (+0x84..+0x8C) and moves the
// player turned by the facing (+0x58); kind 1 starts at 8 forward and -10 up
// (falling at -8/11), others at 20 forward and 18 up, the forward slowing to
// 0 over 80 steps. The originals take the acceleration and the move once per
// step: here times the step length.
constexpr uint32_t kVelocity = 0x78, kAcceleration = 0x84;

void FallStart(const Player& p, uint32_t kind) {
    const uint32_t plw = p.plw;
    if (kind == 1) {
        p.Set(56, 8, 32);
        Memory::Write8(plw + 0x585, 0);
        Memory::WriteFloat32(plw + kVelocity + 0, 0.0f);
        Memory::WriteFloat32(plw + kVelocity + 8, 8.0f);
        Memory::WriteFloat32(plw + kVelocity + 4, -10.0f);
        Memory::WriteFloat32(plw + kAcceleration + 0, 0.0f);
        Memory::WriteFloat32(plw + kAcceleration + 8, 0.0f);
        Memory::WriteFloat32(plw + kAcceleration + 4, Memory::ReadFloat32(0x8079FF60 - 15228)); // -8/11
    } else {
        p.Set(56, 0, 0);
        Memory::WriteFloat32(plw + kVelocity + 0, 0.0f);
        Memory::WriteFloat32(plw + kVelocity + 8, 20.0f);
        Memory::WriteFloat32(plw + kVelocity + 4, 18.0f);
        Memory::WriteFloat32(plw + kAcceleration + 0, 0.0f);
        Memory::WriteFloat32(plw + kAcceleration + 8, -(20.0f / 80.0f));
        p.game.Call<0x8026A608>({plw, 19});
    }
}

// The velocity takes the acceleration (forward and sideways not below 0) and
// moves the player turned by the facing, times the step length. True when
// the player is airborne (0x80245ED0) and falling.
bool FallMove(const Player& p, uint32_t move) {
    const uint32_t plw = p.plw;
    const float s = float(GameStep::Scale());
    for (uint32_t k = 0; k < 12; k += 4)
        Memory::WriteFloat32(plw + kVelocity + k, Memory::ReadFloat32(plw + kVelocity + k) + Memory::ReadFloat32(plw + kAcceleration + k) * s);
    if (Memory::ReadFloat32(plw + kVelocity + 0) < 0.0f)
        Memory::WriteFloat32(plw + kVelocity + 0, 0.0f);
    if (Memory::ReadFloat32(plw + kVelocity + 8) < 0.0f)
        Memory::WriteFloat32(plw + kVelocity + 8, 0.0f);
    for (uint32_t k = 0; k < 12; k += 4)
        Memory::WriteFloat32(move + k, Memory::ReadFloat32(plw + kVelocity + k) * s);
    p.game.Call<RotVecY>({move, Memory::Read32(plw + kAngle2)});
    for (uint32_t k = 0; k < 12; k += 4)
        Memory::WriteFloat32(plw + kPosition + k, Memory::ReadFloat32(plw + kPosition + k) + Memory::ReadFloat32(move + k));
    return p.game.Call<0x80245ED0>({plw}) != 1 && Memory::ReadFloat32(plw + kVelocity + 4) < 0.0f;
}

// One step of a fall; true when it lands at the ground height (+0x60). The
// step count (+0x28) takes 1 more a step while falling.
bool FallStep(const Player& p, uint32_t move) {
    const uint32_t plw = p.plw;
    if (!FallMove(p, move))
        return false;
    p.CountUp();
    if (!(Memory::ReadFloat32(plw + kPosition + 4) < Memory::ReadFloat32(plw + kLastY)))
        return false;
    Memory::WriteFloat32(plw + kPosition + 4, Memory::ReadFloat32(plw + kLastY));
    Memory::Write8(plw + 27, 1);
    Memory::Write8(plw + kFlag9, 0);
    Memory::Write8(plw + 0x585, 0);
    return true;
}

// 0x802598C4 (plw, kind, alt): a fall; landing after 50 steps is action 2
// (2), after 30 action 50 (70 when alt), else 51 (71).
void Fall(CpuContext* ctx) {
    GuestFrame frame(ctx, 48);
    GuestCall game{ctx};
    const Player p{{game, frame.Arg(3)}};
    const uint32_t plw = p.plw, kind = frame.Arg(4), alt = frame.Arg(5);
    game.Call<Vec3Ctor>({frame[8]});
    p.CountUp();
    switch (p.State()) {
    case 0:
        p.NextState();
        p.Flags(2, 0, 1);
        game.Call<0x8027D554>({plw});
        game.Call<0x8027AC70>({plw});
        FallStart(p, kind);
        game.Call<MotionAfterSet>({plw});
        p.Timer(0);
        Memory::Write8(plw + kFlag7, 0);
        break;
    case 1:
        if (!FallStep(p, frame[8]))
            break;
        if (p.Timer() >= 50) {
            game.Call<0x80245E04>({plw, 0});
            p.Next(2, 2, 0);
        } else {
            p.Next(6, p.Timer() >= 30 ? (alt == 0 ? 50 : 70) : (alt == 0 ? 51 : 71), 0);
        }
        break;
    }
}

// 0x8025CECC (plw, kind): a fall (0x8025C834 each step); landing after 50
// steps (60 on 0x8027BE38) without skill 36 is action 2 (2), after 30 (40)
// action 7 (12), else 8.
void Fall2(CpuContext* ctx) {
    GuestFrame frame(ctx, 32);
    GuestCall game{ctx};
    const Player p{{game, frame.Arg(3)}};
    const uint32_t plw = p.plw, kind = frame.Arg(4);
    game.Call<Vec3Ctor>({frame[8]});
    game.Call<0x8025C834>({plw});
    p.CountUp();
    switch (p.State()) {
    case 0:
        p.NextState();
        p.Flags(2, 0, 0);
        FallStart(p, kind);
        game.Call<MotionAfterSet>({plw});
        p.Timer(0);
        Memory::Write8(plw + kFlag7, 0);
        game.Call<0x8027AC70>({plw});
        break;
    case 1: {
        if (!FallStep(p, frame[8]))
            break;
        int32_t hard = 50, soft = 30;
        if (game.Call<CatSkillCheck>({plw, 36}) != 1 && game.Call<0x8027BE38>({plw}) == 1)
            hard = 60, soft = 40;
        if (p.Timer() >= hard && game.Call<CatSkillCheck>({plw, 36}) == 0) {
            Memory::Write8(plw + 0x5C4, Memory::Read8(plw + 0x5C4) & 0xF0);
            game.Call<0x8010D6EC>({plw});
            p.Next(2, 2, 0);
        } else {
            p.Next(12, p.Timer() >= soft ? 7 : 8, 2);
        }
        break;
    }
    }
}

// 0x802501B8: waiting in motion 1 (then 66 into 67, or 63 into 65 with +20)
// for 30 steps (none in play mode 2). Then the local player's, unless an
// event demo or +0x5BD (255 to start, cleared outside play mode 2) ends it:
// with +0x5BF 0, 0x803BAA14 finds the spot at the player's position; of
// kind 8 the player faces its way, gets the point beside it (120 times the
// player's entry of 0x805C5280, turned) as +0x90 and goes on to action 84;
// none ends it. +0x5BF 1: 0x8028F3CC, then the end.
void AwaitPlace(CpuContext* ctx) {
    GuestFrame frame(ctx, 64);
    GuestCall game{ctx};
    const Player p{{game, frame.Arg(3)}};
    const uint32_t plw = p.plw, spot = frame[40], side = frame[28];
    game.Call<Vec3Ctor>({spot});
    game.Call<Vec3Ctor>({side});
    p.Timer(std::max(p.Timer() - int32_t(GameStep::Ticks()), 0));
    Memory::Write8(p.Char() + kCharFlag246, 2);
    game.Call<SetState1408>({plw, 2});
    game.Call<SetState918>({plw, 2});
    switch (p.State()) {
    case 0:
        p.NextState();
        p.Flags(0, 0, 0);
        p.Set(1, 4, 0);
        if ((game.Call<PlayModeCheck>() & 0xFF) == 2) {
            Memory::Write8(plw + kFlag1469, 0);
            p.Timer(0);
        } else {
            Memory::Write8(plw + kFlag1469, 255);
            p.Timer(30);
        }
        Memory::Write8(plw + 0x5BF, 0);
        Memory::Write16(plw + 0x5C2, 30);
        Memory::Write8(plw + 0x5C0, 0);
        break;
    case 1: {
        switch (p.Motion()) {
        case 1:
            if (p.Timer() <= 0)
                p.Set(Memory::Read8(plw + kFlag20) == 0 ? 66 : 63, 6, 0);
            break;
        case 63:
            if (p.Ended())
                p.Set(65, 2, 0);
            break;
        case 66:
            if (p.Ended())
                p.Set(67, 2, 0);
            break;
        }
        if (!p.Master() || p.Timer() > 0)
            break;
        if (game.Call<InEventDemo>({1}) == 1 || ((game.Call<PlayModeCheck>() & 0xFF) != 2 && Memory::Read8(plw + kFlag1469) == 0)) {
            p.End(0, 4);
            break;
        }
        if ((game.Call<PlayModeCheck>() & 0xFF) != 2 && Memory::Read8(plw + kFlag1469) != 1)
            break;
        const uint32_t mode = Memory::Read8(plw + 0x5BF);
        if (mode == 0) {
            const uint32_t kind = frame[8], angle = frame[20];
            const uint32_t found = game.Call<0x803BAA14>({Memory::Read8(plw + 8), Memory::Read8(plw + kGroundKind), plw + kPosition, kind, spot, frame[16], frame[12]});
            if ((found & 0xFFFF) == 0xFFFF) {
                p.End(0, 4);
                Memory::Write8(plw + kFlag1469, 0);
            } else if (int16_t(Memory::Read16(kind)) == 8) {
                Memory::Write32(plw + kAngle, Memory::Read32(angle));
                Memory::WriteFloat32(side + 0, 120.0f * Memory::ReadFloat32(0x805C5280 + 4 * Memory::Read8(plw + 8)));
                Memory::WriteFloat32(side + 4, 0.0f);
                Memory::WriteFloat32(side + 8, 0.0f);
                game.Call<RotVecY>({side, Memory::Read32(angle)});
                for (uint32_t k = 0; k < 12; k += 4)
                    Memory::WriteFloat32(plw + 0x90 + k, Memory::ReadFloat32(spot + k) + Memory::ReadFloat32(side + k));
                game.Call<0x8027AE8C>({plw, 20});
                p.Next(0, 84, 0);
                Memory::Write8(plw + kFlag1469, 0);
            }
        } else if (mode == 1) {
            game.Call<0x8028F3CC>();
            p.End(0, 4);
            Memory::Write8(plw + kFlag1469, 0);
        }
        break;
    }
    }
}

// 0x80253038: a run: motion 119 (from half the frame of 115; skills 197 and
// 198 1.1 / 0.9 times as fast) draining stamina (1 a step, stamina.cpp), or
// 1053 on +24. The local player's runs out of stamina into action 36, or 27
// on 0x80131AFC; with something near and command 37 0x80252B50 may take over
// once 15 steps (+7) are out; after 40 steps action 8 (7 with 2 or more near).
// 2 a step forward along the angles at +0x54: here times the step length.
void Run119(CpuContext* ctx) {
    GuestFrame frame(ctx, 48);
    GuestCall game{ctx};
    const Player p{{game, frame.Arg(3)}};
    const uint32_t plw = p.plw, move = frame[8];
    game.Call<Vec3Ctor>({move});
    switch (p.State()) {
    case 0:
        p.NextState();
        p.Flags(3, 0, 0);
        game.Call<SetActFlags>({plw, 192});
        if (Memory::Read8(plw + kFlag24) == 1) {
            p.SetX(1053, 4, 0);
            Memory::Write8(plw + kFlag6, 0);
        } else {
            if (p.Motion() == 115) {
                game.Call<MotionFrame>({plw});
                const int32_t from = int32_t(float(game.F1()) * 0.5f);
                game.Call<SetMotionAttr>({plw, 119, 4, uint32_t(from), 1});
            } else {
                p.SetX(119, 4, 0);
            }
            game.Call<0x802785C8>({plw, 1});
            Memory::Write8(plw + kFlag6, 1);
            if (game.Call<SkillCheck>({plw, 197}) == 1)
                Memory::WriteFloat32(plw + kMotionRate, Memory::ReadFloat32(plw + kMotionRate) * 1.1f);
            else if (game.Call<SkillCheck>({plw, 198}) == 1)
                Memory::WriteFloat32(plw + kMotionRate, Memory::ReadFloat32(plw + kMotionRate) * 0.9f);
        }
        Memory::Write8(plw + kFlag614, 10);
        if (Memory::Read8(plw + kFlag24) == 0)
            game.Call<0x8027714C>({plw, 0x805BF5D4, 0});
        else
            game.Call<0x80277204>({plw, 0});
        p.Timer(40);
        Memory::Write8(plw + kFlag7, 15);
        Memory::Write16(plw + kWord1606, 0);
        Memory::Write16(plw + kWord1608, 0);
        return;
    case 1: {
        if (Memory::Read8(plw + kFlag6) != 0)
            game.Call<StaminaChange>({plw, uint32_t(-1)});
        const uint32_t wait = Memory::Read8(plw + kFlag7);
        Memory::Write8(plw + kFlag7, uint8_t(wait - std::min(wait, GameStep::Ticks())));
        p.CountDownToZero();
        if (p.Master()) {
            if (Memory::Read8(plw + kFlag6) != 0) {
                if (game.Call<0x802762B8>({plw, 150}) == 0) {
                    p.Next(1, 36, 1);
                    return;
                }
                if (game.Call<0x80131AFC>({Memory::Read8(plw + 8)}) == 1) {
                    p.Next(1, 27, 1);
                    return;
                }
            }
            if ((game.Call<Nearby>({plw, 0}) & 0xFF) >= 1 && game.Call<0x8026A6A8>({plw, 37}) == 1) {
                if (Memory::Read8(plw + kFlag7) == 0) {
                    game.Call<0x8026A450>({plw});
                    game.Call<0x80252B50>({plw, 0xC16D, 16020, 43, 44});
                }
            } else if (p.Timer() <= 0) {
                p.Next(1, (game.Call<Nearby>({plw, 0}) & 0xFF) < 2 ? 8 : 7, 0);
                return;
            } else if (Memory::Read8(plw + kFlag7) == 0 && (game.Call<Nearby>({plw, 0}) & 0xFF) >= 1) {
                game.Call<0x8026A450>({plw});
            }
        }
        Memory::WriteFloat32(move + 0, 0.0f);
        Memory::WriteFloat32(move + 4, 0.0f);
        Memory::WriteFloat32(move + 8, float(2.0 * GameStep::Scale()));
        game.Call<RotVecXYZ>({move, plw + 0x54});
        for (uint32_t k = 0; k < 12; k += 4)
            Memory::WriteFloat32(plw + kPosition + k, Memory::ReadFloat32(plw + kPosition + k) + Memory::ReadFloat32(move + k));
        break;
    }
    }
}

// 0x80254A28 (plw, kind): a jump (motion 15): kind 0 20 forward and 18 up,
// kind 1 8 forward and -20 up (falling at -8/11), as the falls. Falling, a
// ledge 0x80277CF8 finds within 180/30 catches the local player's (action 6,
// on 0x802762D4 with +0x5C7 20 or less); on the ground (GetGroundHit2) it
// lands: out of health 0x80278C48; after 30 steps without skill 36 action 2
// (2); the local player's with command 0 and something near rolls on
// (motion 16, then action 14), else action 2 (3).
void Jump15(CpuContext* ctx) {
    GuestFrame frame(ctx, 48);
    GuestCall game{ctx};
    const Player p{{game, frame.Arg(3)}};
    const uint32_t plw = p.plw, kind = frame.Arg(4), move = frame[16], height = frame[12], hit = frame[8];
    game.Call<Vec3Ctor>({move});
    p.CountUp();
    switch (p.State()) {
    case 0:
        p.NextState();
        p.Flags(2, 0, 0);
        game.Call<0x8027A1E0>({plw});
        if (kind == 0) {
            p.Set(15, 0, 0);
            Memory::WriteFloat32(plw + kVelocity + 0, 0.0f);
            Memory::WriteFloat32(plw + kVelocity + 4, 18.0f);
            Memory::WriteFloat32(plw + kVelocity + 8, 20.0f);
            Memory::WriteFloat32(plw + kAcceleration + 0, 0.0f);
            Memory::WriteFloat32(plw + kAcceleration + 8, 0.0f);
            game.Call<0x8026A608>({plw, 19});
        } else if (kind == 1) {
            Memory::Write8(plw + 0x585, 0);
            p.Set(15, 8, 32);
            Memory::WriteFloat32(plw + kVelocity + 0, 0.0f);
            Memory::WriteFloat32(plw + kVelocity + 4, -20.0f);
            Memory::WriteFloat32(plw + kVelocity + 8, 8.0f);
            Memory::WriteFloat32(plw + kAcceleration + 0, 0.0f);
            Memory::WriteFloat32(plw + kAcceleration + 4, Memory::ReadFloat32(0x8079FF60 - 15228)); // -8/11
            Memory::WriteFloat32(plw + kAcceleration + 8, 0.0f);
        }
        game.Call<MotionAfterSet>({plw});
        p.Timer(0);
        Memory::Write8(plw + kFlag7, 0);
        break;
    case 1: {
        if (!FallMove(p, move))
            break;
        if (game.Call<0x80277CF8>({plw, height}, {180.0, 30.0}) != 0 && game.Call<0x802762D4>({plw, 1}) == 1 && Memory::Read8(plw + kClimbs) <= 20) {
            Memory::WriteFloat32(plw + kPosition + 4, Memory::ReadFloat32(height));
            Memory::WriteFloat32(plw + kLastY, Memory::ReadFloat32(height));
            Memory::Write8(plw + 27, 1);
            Memory::Write32(plw + kAngle, Memory::Read32(plw + kAngle2));
            p.Next(0, 6, 0);
            break;
        }
        game.Call<GroundHit2>({plw + kPosition, uint32_t(-6), Memory::Read8(plw + kGroundKind), hit});
        const float ground = float(game.F1());
        Memory::WriteFloat32(height, ground);
        if (!(ground >= Memory::ReadFloat32(plw + kPosition + 4)) || Memory::Read8(hit) == 0)
            break;
        game.Call<0x80275B60>({plw, 0});
        Memory::WriteFloat32(plw + kPosition + 4, ground);
        Memory::Write8(plw + 0x585, 0);
        if (int16_t(Memory::Read16(plw + 0x370)) <= 0 && p.Master()) {
            game.Call<0x80278C48>({plw});
            break;
        }
        if (p.Timer() >= 30 && game.Call<CatSkillCheck>({plw, 36}) == 0) {
            p.Next(2, 2, 0);
            break;
        }
        if (p.Master() && game.Call<0x8027AC7C>({plw}) == 0 && Memory::Read8(plw + kFlag24) == 0 && game.Call<0x8026A6A8>({plw, 0}) == 1 && (game.Call<Nearby>({plw, 0}) & 0xFF) >= 1) {
            p.NextState();
            p.Set(16, 0, 0);
        } else {
            p.Next(2, 3, 0);
        }
        break;
    }
    case 2:
        if (p.Ended())
            p.Next(1, 14, 0);
        break;
    }
}

// 0x8026A57C (plw): the velocity (+0x78) takes the acceleration (+0x84) and
// moves the position (+0x3C), once per step in the original: here times the
// step length (the jumps 0x8024F674, 0x80254F18, 0x80256A58, 0x80256F04,
// 0x80258AC8).
void VelocityStep(CpuContext* ctx) {
    const uint32_t plw = ctx->gpr[3];
    const float s = float(GameStep::Scale());
    for (uint32_t k = 0; k < 12; k += 4) {
        const float v = Memory::ReadFloat32(plw + kVelocity + k) + Memory::ReadFloat32(plw + kAcceleration + k) * s;
        Memory::WriteFloat32(plw + kVelocity + k, v);
        Memory::WriteFloat32(plw + kPosition + k, Memory::ReadFloat32(plw + kPosition + k) + v * s);
    }
}

// 0x80254F18 (plw, kind): climbing over (motion 11, kind 0) into a jump off
// (motion 12, 30 up, 24 with +24), or dropping off at once (kind 1; kind 2
// from 195 down and 30 back, -18 up): falling (VelocityStep), a ledge
// 0x80277CF8 finds catches the player (action 6, not kind 2); on the ground
// (GetGroundHit2) out of health 0x80278C48, after 30 steps without skill 36
// action 2 (2), else motion 13 to the end.
void ClimbOver(CpuContext* ctx) {
    GuestFrame frame(ctx, 48);
    GuestCall game{ctx};
    const Player p{{game, frame.Arg(3)}};
    const uint32_t plw = p.plw, kind = frame.Arg(4), move = frame[16], height = frame[12], hit = frame[8];
    game.Call<Vec3Ctor>({move});
    p.CountUp();
    switch (p.State()) {
    case 0:
        game.Call<ClearVelocity>({plw});
        if (kind != 1 && kind != 2) {
            p.NextState();
            p.Set(11, 2, 0);
            p.Flags(0, 0, 0);
        } else {
            if (kind == 2) {
                Memory::WriteFloat32(plw + kPosition + 4, Memory::ReadFloat32(plw + kPosition + 4) - 195.0f);
                Memory::WriteFloat32(move + 0, 0.0f);
                Memory::WriteFloat32(move + 4, 0.0f);
                Memory::WriteFloat32(move + 8, -30.0f);
                game.Call<RotVecY>({move, Memory::Read32(plw + kAngle2)});
                Memory::WriteFloat32(plw + kPosition + 0, Memory::ReadFloat32(plw + kPosition + 0) + Memory::ReadFloat32(move + 0));
                Memory::WriteFloat32(plw + kPosition + 8, Memory::ReadFloat32(plw + kPosition + 8) + Memory::ReadFloat32(move + 8));
            }
            Memory::Write8(plw + 0x585, 0);
            Memory::Write8(plw + kState, 2);
            p.Flags(2, 0, 0);
            game.Call<ClearAcceleration>({plw});
            Memory::WriteFloat32(plw + kVelocity + 4, -18.0f);
            Memory::WriteFloat32(plw + kAcceleration + 4, Memory::ReadFloat32(0x8079FF60 - 15228)); // -8/11
            p.Set(12, kind == 1 ? 4 : 0, 22);
        }
        game.Call<0x8027A1E0>({plw});
        game.Call<MotionAfterSet>({plw});
        p.Timer(0);
        break;
    case 1: {
        if (!p.Ended())
            break;
        p.NextState();
        game.Call<ClearAcceleration>({plw});
        const float speed = 15.0f * (Memory::Read8(plw + kFlag24) == 1 ? 0.8f : 1.0f);
        Memory::WriteFloat32(plw + kVelocity + 4, 2.0f * speed);
        game.Call<0x8026A608>({plw, 20});
        p.Set(12, 2, 0);
        game.Call<0x80275B60>({plw, 2});
        game.Call<MotionAfterSet>({plw});
        p.Timer(0);
        break;
    }
    case 2: {
        game.Call<PlVelocityStep>({plw});
        if (game.Call<0x80245ED0>({plw}) == 1 || !(Memory::ReadFloat32(plw + kVelocity + 4) < 0.0f))
            break;
        if (kind != 2 && game.Call<0x80277CF8>({plw, height}, {180.0, 30.0}) != 0 && game.Call<0x802762D4>({plw, 1}) == 1) {
            Memory::Write32(plw + kAngle, Memory::Read32(plw + kAngle2));
            Memory::WriteFloat32(plw + kPosition + 4, Memory::ReadFloat32(height));
            Memory::WriteFloat32(plw + kLastY, Memory::ReadFloat32(height));
            Memory::Write8(plw + 27, 1);
            p.Next(0, 6, 0);
            break;
        }
        game.Call<GroundHit2>({plw + kPosition, uint32_t(-6), Memory::Read8(plw + kGroundKind), hit});
        const float ground = float(game.F1());
        Memory::WriteFloat32(height, ground);
        if (!(ground >= Memory::ReadFloat32(plw + kPosition + 4)) || Memory::Read8(hit) == 0)
            break;
        Memory::WriteFloat32(plw + kPosition + 4, ground);
        game.Call<0x80275B60>({plw, 0});
        if (int16_t(Memory::Read16(plw + 0x370)) <= 0 && p.Master()) {
            game.Call<0x80278C48>({plw});
        } else if (p.Timer() >= 30 && game.Call<CatSkillCheck>({plw, 36}) == 0) {
            p.Next(2, 2, 0);
        } else {
            p.NextState();
            p.Set(13, 0, 0);
        }
        break;
    }
    case 3:
        if (p.Ended())
            p.End(0, 4);
        break;
    }
}

// 0x802813D0 (plw, kind): a charge in motion 1101, entered by kind: 0 at
// once, 1 from 1131, 2 from 1120, 3 (1118) and 4 at once, 5 (1124) and 6
// (1126 into 1101) turned an eighth either way. From frame 34 (0 for kind
// 3, 2 for kind 5) the motion runs at half speed charging (ChargeTime of 90
// steps) until command 5 clears (the button let go) or 13; from frame 58
// (24, 26) the local player's goes on to the attack: action 31 (32, 33).
void Charge1101(CpuContext* ctx) {
    GuestFrame frame(ctx, 32);
    GuestCall game{ctx};
    const Player p{{game, frame.Arg(3)}};
    const uint32_t plw = p.plw, kind = frame.Arg(4);
    game.Call<0x80277CB4>({plw, 2});
    switch (p.State()) {
    case 0:
        p.NextState();
        p.Flags(0, 1, 0);
        Memory::Write8(plw + kFlag24, 1);
        game.Call<0x8027AC70>({plw});
        Memory::Write8(plw + kChargeLevel, 0);
        Memory::Write16(plw + kChargeSteps, 0);
        p.Timer(int16_t(game.Call<ChargeTime>({plw, 90})));
        switch (kind) {
        case 0:
            p.NextState();
            p.Set(1101, 4, 0);
            game.Call<MotionAfterSet>({plw});
            break;
        case 1:
            p.Set(1131, 2, 0);
            game.Call<0x8027E21C>({plw, 1});
            break;
        case 2:
            p.Set(1120, 4, 0);
            break;
        case 3:
            p.NextState();
            p.Set(1118, 4, 0);
            break;
        case 4:
            p.NextState();
            p.Set(1101, 4, 0);
            game.Call<MotionAfterSet>({plw});
            game.Call<0x8027E21C>({plw, 1});
            break;
        case 5:
        case 6: {
            if (kind == 5)
                p.NextState();
            const uint32_t angle = Memory::Read32(plw + kAngle2) + (kind == 5 ? 8192 : uint32_t(-8192));
            Memory::Write32(plw + kAngle2, angle);
            Memory::Write32(plw + kAngle, angle & 0xFFFF);
            p.Set(kind == 5 ? 1124 : 1126, 0, 0);
            break;
        }
        }
        break;
    case 1: {
        // Into the charge's motion from the entry's.
        const int32_t from = kind == 1 ? 56 : kind == 2 ? 50 : kind == 6 ? 52 : -1;
        if (from < 0) {
            p.NextState();
        } else if (p.Ended()) {
            p.NextState();
            p.Set(1101, kind == 2 ? 2 : 0, from);
        }
        break;
    }
    case 2: {
        if (!game.Call<MasterCheck>({plw}))
            break;
        const double start = kind == 3 ? 0.0 : kind == 5 ? 2.0 : 34.0;
        const double end = kind == 3 ? 24.0 : kind == 5 ? 26.0 : 58.0;
        if (game.Call<FrameCheck>({plw, 1}, {start, 0.0}) == 1) {
            if (Memory::Read8(plw + kFlag7) == 0) {
                Memory::Write8(plw + kFlag7, 1);
                game.Call<0x8011ADBC>({plw, 0});
            }
            if (!Bit(plw + kCommands, 5) || Bit(plw + kCommands, 13))
                Memory::Write8(plw + kFlag6, 1);
            if (Memory::Read8(plw + kFlag6) == 0) {
                Memory::WriteFloat32(plw + kMotionRate, 0.5f);
                ChargeStep(p);
                if (p.CountDown())
                    Memory::Write8(plw + kFlag6, 1);
            } else {
                Memory::WriteFloat32(plw + kMotionRate, 1.0f);
            }
        }
        if (game.Call<FrameCheck>({plw, 1}, {end, 0.0}) == 1 && Memory::Read8(plw + kFlag6) == 1) {
            Memory::Write8(plw + kChargeLevel, uint8_t(game.Call<ChargeLevel>({plw})));
            Memory::WriteFloat32(plw + kMotionRate, 1.0f);
            if (kind == 3)
                p.Next(4, 32, 4);
            else if (kind == 5)
                p.Next(4, 33, 4);
            else
                p.Next(4, 31, 260);
        }
        break;
    }
    }
}

// 0x80256A58 (plw, kind): knocked flying (motion and frame from 0x805C56B8
// by kind: 205 or 208, into 214 / 220). Thrown from joint 14's height back
// 22.34 (kind 0, falling at -1/3), 19.04 (2, -0.36) or forward 23 (1 and
// 3, -0.25), turned by the facing (VelocityStep). For 16 steps it is kept
// 63 above the ground (0x80246B18); then it lands: on ground kind 1 63 above
// it into the kind's motion at 0x80794508, getting up (0x805C56C8, then
// 0x8025687C; out of health 0x80278BBC) at its end (frame 58 for kind 2);
// on kind 2 at the ground into action 151 (or 0x80278C48 out of health).
void KnockedFlying(CpuContext* ctx) {
    GuestFrame frame(ctx, 48);
    GuestCall game{ctx};
    const Player p{{game, frame.Arg(3)}};
    const uint32_t plw = p.plw, kind = frame.Arg(4), v = frame[12], ground = frame[8];
    game.Call<Vec3Ctor>({v});
    game.Call<SetState918>({plw, 2});
    switch (p.State()) {
    case 0: {
        p.NextState();
        p.Flags(2, 0, 1);
        game.Call<0x8027D554>({plw});
        Memory::Write8(plw + 0x585, 0);
        game.Call<JointPos>({plw, 14, v});
        if (Memory::ReadFloat32(plw + kPosition + 4) < Memory::ReadFloat32(v + 4))
            Memory::WriteFloat32(plw + kPosition + 4, Memory::ReadFloat32(v + 4));
        game.Call<ClearAcceleration>({plw});
        Memory::WriteFloat32(v + 0, 0.0f);
        Memory::WriteFloat32(v + 4, 0.0f);
        p.Set(Memory::Read16(0x805C56B8 + 4 * kind), 0, Memory::Read16(0x805C56B8 + 4 * kind + 2));
        float speed = 0, fall = 0;
        if (kind == 0)
            speed = Memory::ReadFloat32(0x8079FF60 - 15176), fall = Memory::ReadFloat32(0x8079FF60 - 15172);
        else if (kind == 2)
            speed = Memory::ReadFloat32(0x8079FF60 - 15168), fall = Memory::ReadFloat32(0x8079FF60 - 15164);
        else if (kind == 1 || kind == 3)
            speed = 23.0f, fall = -0.25f;
        if (kind <= 3) {
            Memory::WriteFloat32(v + 8, speed);
            Memory::WriteFloat32(plw + kAcceleration + 4, fall);
        }
        game.Call<RotVecY>({v, Memory::Read32(plw + kAngle2)});
        Memory::WriteFloat32(plw + kVelocity + 0, Memory::ReadFloat32(v + 0));
        Memory::WriteFloat32(plw + kVelocity + 4, 0.0f);
        Memory::WriteFloat32(plw + kVelocity + 8, Memory::ReadFloat32(v + 8));
        p.Timer(16);
        break;
    }
    case 1: {
        if (p.Ended()) {
            if (p.Motion() == 205)
                p.Set(214, 0, 0);
            if (p.Motion() == 208)
                p.Set(220, 0, 0);
        }
        game.Call<PlVelocityStep>({plw});
        if (game.Call<0x80245ED0>({plw}) == 1)
            break;
        game.Call<0x8025FF70>({plw, 0});
        const uint32_t on = game.Call<0x80246B18>({plw, ground});
        const float y = Memory::ReadFloat32(plw + kPosition + 4), floor = Memory::ReadFloat32(ground);
        if (p.Timer() > 0) {
            p.CountDownToZero();
            if (y <= floor + 63.0f && on != 0)
                Memory::WriteFloat32(plw + kPosition + 4, floor + 63.0f);
            break;
        }
        if (!(Memory::ReadFloat32(plw + kVelocity + 4) <= 0.0f))
            break;
        if (on == 1 && y <= floor + 63.0f) {
            p.NextState();
            Memory::WriteFloat32(plw + kPosition + 4, floor);
            Memory::Write8(plw + 27, 1);
            p.Set(Memory::Read16(0x80794508 + 2 * kind), 0, 0);
            game.Call<0x80275B60>({plw, 4});
        } else if (on == 2 && y <= floor) {
            Memory::WriteFloat32(plw + kPosition + 4, floor);
            Memory::Write8(plw + 27, 1);
            Memory::Write8(plw + kFlag9, 3);
            if (p.Master() && int16_t(Memory::Read16(plw + 0x370)) <= 0)
                game.Call<0x80278C48>({plw});
            else
                p.Next(0, 151, 12);
        }
        break;
    }
    case 2: {
        const bool done = kind == 2 ? game.Call<FrameCheck>({plw, 1}, {58.0, 0.0}) == 1 : p.Ended();
        if (!done)
            break;
        if (int16_t(Memory::Read16(plw + 0x370)) > 0) {
            p.NextState();
            p.Set(Memory::Read16(0x805C56C8 + 4 * kind), Memory::Read16(0x805C56C8 + 4 * kind + 2), 0);
            game.Call<0x8025687C>({plw, 0});
        } else if (p.Master()) {
            game.Call<0x80278BBC>({plw, kind == 0 ? 2u : 3u, 0});
        } else {
            Memory::Write8(plw + kState, 4);
            Memory::Write8(p.Char() + kCharFlag246, 2);
        }
        break;
    }
    case 3:
        if (p.Ended()) {
            game.Call<EndActionReset>({plw, 0, 6, 0});
            game.Call<0x80277BB0>({plw, 6});
        }
        break;
    case 4:
        Memory::Write8(p.Char() + kCharFlag246, 2);
        break;
    }
}

// 0x80258AC8 (plw, kind): grabbed by a monster: kind 0 (motion 271, the
// monster of kind 18 whose +26 is +0x3B6, to its joint 15 over 60 steps,
// 65 steps, then 273 to the end), kind 1 (motion 219 into 220, the monster
// of kind 4, to its +392 over 50 steps, at least 15 up; landing 63 above
// the ground: 215, turned round 216 to the end, or 0x8025A844 and action
// 7). With no such monster in the area thrown 14 back and 20 up (falling
// at -8/11). The flight is VelocityStep's.
void Grabbed(CpuContext* ctx) {
    GuestFrame frame(ctx, 80);
    GuestCall game{ctx};
    const Player p{{game, frame.Arg(3)}};
    const uint32_t plw = p.plw, kind = frame.Arg(4), target = frame[32], v = frame[20], d = frame[8];
    game.Call<Vec3Ctor>({target});
    game.Call<Vec3Ctor>({v});
    switch (p.State()) {
    case 0: {
        p.NextState();
        game.Call<SetState918>({plw, 10});
        if (kind == 0) {
            p.Set(271, 0, 0);
            p.Flags(3, 0, 1);
            p.Timer(65);
        } else {
            p.Set(219, 0, 0);
            p.Flags(2, 0, 1);
            p.Timer(90);
        }
        game.Call<0x8027D554>({plw});
        game.Call<JointPos>({plw, 14, v});
        if (Memory::ReadFloat32(plw + kPosition + 4) < Memory::ReadFloat32(v + 4))
            Memory::WriteFloat32(plw + kPosition + 4, Memory::ReadFloat32(v + 4));
        Memory::Write8(plw + 0x585, 0);
        game.Call<ClearVelocity>({plw});
        game.Call<0x8027AC70>({plw});
        bool found = false;
        uint32_t em = game.Call<GetMoveWorkAdrs>({3});
        const uint32_t count = game.Call<GetMoveWorkMax>({3}) & 0xFFFF;
        for (uint32_t i = 0; i < count && !found; ++i, em += 2840) {
            if (Memory::Read8(em + 0) == 0 || Memory::Read8(em + 1) == 0)
                continue;
            if (Memory::Read8(em + 481) != (game.Call<GetNowAreaNo>() & 0xFF))
                continue;
            if ((Memory::Read16(em + 26) & 0xFF) != Memory::Read8(plw + 0x3B6))
                continue;
            const uint32_t emKind = Memory::Read8(em + 3);
            if (kind == 0 && emKind == 18) {
                found = true;
                game.Call<EnemyJointPos>({em, 15, target});
                game.Call<VecSub>({d, target, plw + kPosition});
                game.Call<CopyVec>({v, d});
                game.Call<VecAngles>({v, plw + 0x54, plw + kAngle2});
                Memory::Write32(plw + 0xAC, Memory::Read32(plw + 0x54));
                Memory::Write32(plw + kAngle, Memory::Read32(plw + kAngle2));
            } else if (kind == 1 && emKind == 4) {
                found = true;
                game.Call<CopyVec>({target, em + 392});
                const uint32_t angle = game.Call<VecAngle2>({plw + kPosition, target});
                Memory::Write32(plw + kAngle2, angle);
                Memory::Write32(plw + kAngle, angle);
            }
        }
        if (!found) {
            Memory::WriteFloat32(v + 0, 0.0f);
            Memory::WriteFloat32(v + 4, 0.0f);
            Memory::WriteFloat32(v + 8, -7.0f);
            game.Call<RotVecY>({v, Memory::Read32(plw + kAngle2)});
            Memory::WriteFloat32(plw + kVelocity + 0, 2.0f * Memory::ReadFloat32(v + 0));
            Memory::WriteFloat32(plw + kVelocity + 8, 2.0f * Memory::ReadFloat32(v + 8));
            Memory::WriteFloat32(plw + kVelocity + 4, 20.0f);
            Memory::WriteFloat32(plw + kAcceleration + 4, Memory::ReadFloat32(0x8079FF60 - 15228)); // -8/11
            break;
        }
        // To the target over 60 (50) steps.
        const float steps = kind == 0 ? 60.0f : 50.0f;
        for (uint32_t k = 0; k < 12; k += 4)
            Memory::WriteFloat32(v + k, (Memory::ReadFloat32(target + k) - Memory::ReadFloat32(plw + kPosition + k)) / steps);
        Memory::WriteFloat32(plw + kVelocity + 0, Memory::ReadFloat32(v + 0));
        Memory::WriteFloat32(plw + kVelocity + 8, Memory::ReadFloat32(v + 8));
        if (kind == 0) {
            Memory::WriteFloat32(plw + kVelocity + 4, Memory::ReadFloat32(v + 4));
        } else {
            if (Memory::ReadFloat32(v + 4) <= 15.0f)
                Memory::WriteFloat32(v + 4, 15.0f);
            Memory::WriteFloat32(plw + kVelocity + 4, Memory::ReadFloat32(v + 4));
            game.Call<0x8026A608>({plw, 50});
        }
        break;
    }
    case 1:
        if (p.Ended() && p.Motion() == 219)
            p.Set(220, 0, 0);
        game.Call<PlVelocityStep>({plw});
        if (game.Call<0x80245ED0>({plw}) == 1)
            break;
        if (kind == 0) {
            if (!p.CountDown())
                break;
            Memory::Write8(plw + kState, 3);
            p.Set(273, 4, 0);
            break;
        }
        if (!(Memory::ReadFloat32(plw + kVelocity + 4) <= 0.0f) || !(Memory::ReadFloat32(plw + kPosition + 4) <= Memory::ReadFloat32(plw + kLastY) + 63.0f))
            break;
        p.NextState();
        game.Call<SetState918>({plw, 10});
        p.Set(215, 0, 0);
        Memory::WriteFloat32(plw + kPosition + 4, Memory::ReadFloat32(plw + kLastY));
        Memory::Write8(plw + 27, 1);
        Memory::Write8(plw + kFlag9, 4);
        if (kind == 1) {
            game.Call<0x8025A844>({plw, 0});
            p.Next(7, 1, 2);
        }
        break;
    case 2:
        game.Call<SetState918>({plw, 10});
        if (!p.Ended())
            break;
        p.NextState();
        Memory::Write32(plw + kAngle2, (Memory::Read32(plw + kAngle2) + 0x8000) & 0xFFFF);
        Memory::Write32(plw + kAngle, Memory::Read32(plw + kAngle2));
        p.Set(216, 0, 0);
        break;
    case 3:
        if (!p.Ended())
            break;
        game.Call<EndActionReset>({plw, kind == 0 ? 3u : 0u, 8, 0});
        game.Call<SetState918>({plw, 10});
        break;
    }
}

} // namespace

extern "C" void func_802535B8(CpuContext* ctx);
extern "C" void MH3_FieldActionMotion8(CpuContext* ctx) {
    PortCheck::Call(ctx, "FieldActionMotion8", func_802535B8, Motion8);
}
REGISTER_NATIVE_FUNCTION_AS(0x802535B8, MH3_FieldActionMotion8, "FieldActionMotion8");

extern "C" void func_80257E08(CpuContext* ctx);
extern "C" void MH3_FieldActionMotion26(CpuContext* ctx) {
    PortCheck::Call(ctx, "FieldActionMotion26", func_80257E08, Motion26);
}
REGISTER_NATIVE_FUNCTION_AS(0x80257E08, MH3_FieldActionMotion26, "FieldActionMotion26");

extern "C" void func_8025198C(CpuContext* ctx);
extern "C" void MH3_FieldActionWait120(CpuContext* ctx) {
    PortCheck::Call(ctx, "FieldActionWait120", func_8025198C, Wait120);
}
REGISTER_NATIVE_FUNCTION_AS(0x8025198C, MH3_FieldActionWait120, "FieldActionWait120");

extern "C" void func_8025D5F8(CpuContext* ctx);
extern "C" void MH3_FieldActionSlide221(CpuContext* ctx) {
    PortCheck::Call(ctx, "FieldActionSlide221", func_8025D5F8, Slide221);
}
REGISTER_NATIVE_FUNCTION_AS(0x8025D5F8, MH3_FieldActionSlide221, "FieldActionSlide221");

extern "C" void func_8026AB00(CpuContext* ctx);
extern "C" void MH3_PlButtonHoldUpdate(CpuContext* ctx) {
    PortCheck::Call(ctx, "PlButtonHoldUpdate", func_8026AB00, ButtonHold);
}
REGISTER_NATIVE_FUNCTION_AS(0x8026AB00, MH3_PlButtonHoldUpdate, "PlButtonHoldUpdate");

extern "C" void func_80259C9C(CpuContext* ctx);
extern "C" void MH3_FieldActionSink206(CpuContext* ctx) {
    PortCheck::Call(ctx, "FieldActionSink206", func_80259C9C, Sink206);
}
REGISTER_NATIVE_FUNCTION_AS(0x80259C9C, MH3_FieldActionSink206, "FieldActionSink206");

extern "C" void func_8025B3BC(CpuContext* ctx);
extern "C" void MH3_FieldActionWait300(CpuContext* ctx) {
    PortCheck::Call(ctx, "FieldActionWait300", func_8025B3BC, Wait300);
}
REGISTER_NATIVE_FUNCTION_AS(0x8025B3BC, MH3_FieldActionWait300, "FieldActionWait300");

extern "C" void func_802595A0(CpuContext* ctx);
extern "C" void MH3_FieldActionWalk52(CpuContext* ctx) {
    PortCheck::Call(ctx, "FieldActionWalk52", func_802595A0, Walk52);
}
REGISTER_NATIVE_FUNCTION_AS(0x802595A0, MH3_FieldActionWalk52, "FieldActionWalk52");

extern "C" void func_8025BC60(CpuContext* ctx);
extern "C" void MH3_FieldActionTableMotion(CpuContext* ctx) {
    PortCheck::Call(ctx, "FieldActionTableMotion", func_8025BC60, TableMotionThenGo);
}
REGISTER_NATIVE_FUNCTION_AS(0x8025BC60, MH3_FieldActionTableMotion, "FieldActionTableMotion");

extern "C" void func_80250D40(CpuContext* ctx);
extern "C" void MH3_FieldActionMotion318(CpuContext* ctx) {
    PortCheck::Call(ctx, "FieldActionMotion318", func_80250D40, Motion318);
}
REGISTER_NATIVE_FUNCTION_AS(0x80250D40, MH3_FieldActionMotion318, "FieldActionMotion318");

extern "C" void func_80258230(CpuContext* ctx);
extern "C" void MH3_FieldActionMotion39(CpuContext* ctx) {
    PortCheck::Call(ctx, "FieldActionMotion39", func_80258230, Motion39);
}
REGISTER_NATIVE_FUNCTION_AS(0x80258230, MH3_FieldActionMotion39, "FieldActionMotion39");

extern "C" void func_802547A0(CpuContext* ctx);
extern "C" void MH3_FieldActionMotion120(CpuContext* ctx) {
    PortCheck::Call(ctx, "FieldActionMotion120", func_802547A0, Motion120);
}
REGISTER_NATIVE_FUNCTION_AS(0x802547A0, MH3_FieldActionMotion120, "FieldActionMotion120");

extern "C" void func_802497CC(CpuContext* ctx);
extern "C" void MH3_FieldActionAwaitSignal(CpuContext* ctx) {
    PortCheck::Call(ctx, "FieldActionAwaitSignal", func_802497CC, AwaitSignal);
}
REGISTER_NATIVE_FUNCTION_AS(0x802497CC, MH3_FieldActionAwaitSignal, "FieldActionAwaitSignal");

extern "C" void func_80259374(CpuContext* ctx);
extern "C" void MH3_FieldActionMotion51(CpuContext* ctx) {
    PortCheck::Call(ctx, "FieldActionMotion51", func_80259374, Motion51);
}
REGISTER_NATIVE_FUNCTION_AS(0x80259374, MH3_FieldActionMotion51, "FieldActionMotion51");

extern "C" void func_80250630(CpuContext* ctx);
extern "C" void MH3_FieldActionSleep(CpuContext* ctx) {
    PortCheck::Call(ctx, "FieldActionSleep", func_80250630, Sleep);
}
REGISTER_NATIVE_FUNCTION_AS(0x80250630, MH3_FieldActionSleep, "FieldActionSleep");

extern "C" void func_8024A7C0(CpuContext* ctx);
extern "C" void MH3_FieldActionRest(CpuContext* ctx) {
    PortCheck::Call(ctx, "FieldActionRest", func_8024A7C0, Rest);
}
REGISTER_NATIVE_FUNCTION_AS(0x8024A7C0, MH3_FieldActionRest, "FieldActionRest");

extern "C" void func_8024B8CC(CpuContext* ctx);
extern "C" void MH3_FieldActionMotion8Or364(CpuContext* ctx) {
    PortCheck::Call(ctx, "FieldActionMotion8Or364", func_8024B8CC, Motion8Or364);
}
REGISTER_NATIVE_FUNCTION_AS(0x8024B8CC, MH3_FieldActionMotion8Or364, "FieldActionMotion8Or364");

extern "C" void func_8025C100(CpuContext* ctx);
extern "C" void MH3_FieldActionMotion507Cues(CpuContext* ctx) {
    PortCheck::Call(ctx, "FieldActionMotion507Cues", func_8025C100, Motion507Cues);
}
REGISTER_NATIVE_FUNCTION_AS(0x8025C100, MH3_FieldActionMotion507Cues, "FieldActionMotion507Cues");

extern "C" void func_80258394(CpuContext* ctx);
extern "C" void MH3_FieldActionStruggle(CpuContext* ctx) {
    PortCheck::Call(ctx, "FieldActionStruggle", func_80258394, Struggle);
}
REGISTER_NATIVE_FUNCTION_AS(0x80258394, MH3_FieldActionStruggle, "FieldActionStruggle");

extern "C" void func_802580D4(CpuContext* ctx);
extern "C" void MH3_PlEffect13Every50(CpuContext* ctx) {
    PortCheck::Call(ctx, "PlEffect13Every50", func_802580D4, Effect13);
}
REGISTER_NATIVE_FUNCTION_AS(0x802580D4, MH3_PlEffect13Every50, "PlEffect13Every50");

extern "C" void func_80257EEC(CpuContext* ctx);
extern "C" void MH3_FieldActionEscape(CpuContext* ctx) {
    PortCheck::Call(ctx, "FieldActionEscape", func_80257EEC, Escape);
}
REGISTER_NATIVE_FUNCTION_AS(0x80257EEC, MH3_FieldActionEscape, "FieldActionEscape");

extern "C" void func_8025DC40(CpuContext* ctx);
extern "C" void MH3_FieldActionRemoteStand(CpuContext* ctx) {
    PortCheck::Call(ctx, "FieldActionRemoteStand", func_8025DC40, RemoteStand);
}
REGISTER_NATIVE_FUNCTION_AS(0x8025DC40, MH3_FieldActionRemoteStand, "FieldActionRemoteStand");

extern "C" void func_8025DA9C(CpuContext* ctx);
extern "C" void MH3_FieldActionRemoteRest(CpuContext* ctx) {
    PortCheck::Call(ctx, "FieldActionRemoteRest", func_8025DA9C, RemoteRest);
}
REGISTER_NATIVE_FUNCTION_AS(0x8025DA9C, MH3_FieldActionRemoteRest, "FieldActionRemoteRest");

extern "C" void func_802596E8(CpuContext* ctx);
extern "C" void MH3_FieldActionWalk53(CpuContext* ctx) {
    PortCheck::Call(ctx, "FieldActionWalk53", func_802596E8, Walk53);
}
REGISTER_NATIVE_FUNCTION_AS(0x802596E8, MH3_FieldActionWalk53, "FieldActionWalk53");

extern "C" void func_8028138C(CpuContext* ctx);
extern "C" void MH3_PlChargeStep(CpuContext* ctx) {
    PortCheck::Call(ctx, "PlChargeStep", func_8028138C, Charge);
}
REGISTER_NATIVE_FUNCTION_AS(0x8028138C, MH3_PlChargeStep, "PlChargeStep");

extern "C" void func_80281DBC(CpuContext* ctx);
extern "C" void MH3_FieldActionCharge1133(CpuContext* ctx) {
    PortCheck::Call(ctx, "FieldActionCharge1133", func_80281DBC, ChargeHold<0, 1133, 36, 0>);
}
REGISTER_NATIVE_FUNCTION_AS(0x80281DBC, MH3_FieldActionCharge1133, "FieldActionCharge1133");

extern "C" void func_80282770(CpuContext* ctx);
extern "C" void MH3_FieldActionCharge1170(CpuContext* ctx) {
    PortCheck::Call(ctx, "FieldActionCharge1170", func_80282770, ChargeHold<3, 1170, 65, 128>);
}
REGISTER_NATIVE_FUNCTION_AS(0x80282770, MH3_FieldActionCharge1170, "FieldActionCharge1170");

extern "C" void func_80247704(CpuContext* ctx);
extern "C" void MH3_FieldActionClimbUp(CpuContext* ctx) {
    PortCheck::Call(ctx, "FieldActionClimbUp", func_80247704, ClimbUp);
}
REGISTER_NATIVE_FUNCTION_AS(0x80247704, MH3_FieldActionClimbUp, "FieldActionClimbUp");

extern "C" void func_80249944(CpuContext* ctx);
extern "C" void MH3_FieldActionStand(CpuContext* ctx) {
    PortCheck::Call(ctx, "FieldActionStand", func_80249944, Stand);
}
REGISTER_NATIVE_FUNCTION_AS(0x80249944, MH3_FieldActionStand, "FieldActionStand");

extern "C" void func_80253CA0(CpuContext* ctx);
extern "C" void MH3_FieldActionMotion105(CpuContext* ctx) {
    PortCheck::Call(ctx, "FieldActionMotion105", func_80253CA0, Motion105);
}
REGISTER_NATIVE_FUNCTION_AS(0x80253CA0, MH3_FieldActionMotion105, "FieldActionMotion105");

extern "C" void func_80246BE0(CpuContext* ctx);
extern "C" void MH3_FieldActionMotion63Or66(CpuContext* ctx) {
    PortCheck::Call(ctx, "FieldActionMotion63Or66", func_80246BE0, Motion63Or66);
}
REGISTER_NATIVE_FUNCTION_AS(0x80246BE0, MH3_FieldActionMotion63Or66, "FieldActionMotion63Or66");

extern "C" void func_802516C0(CpuContext* ctx);
extern "C" void MH3_FieldActionUseItem(CpuContext* ctx) {
    PortCheck::Call(ctx, "FieldActionUseItem", func_802516C0, UseItem);
}
REGISTER_NATIVE_FUNCTION_AS(0x802516C0, MH3_FieldActionUseItem, "FieldActionUseItem");

extern "C" void func_80251470(CpuContext* ctx);
extern "C" void MH3_FieldActionUseItemRepeat(CpuContext* ctx) {
    PortCheck::Call(ctx, "FieldActionUseItemRepeat", func_80251470, UseItemRepeat);
}
REGISTER_NATIVE_FUNCTION_AS(0x80251470, MH3_FieldActionUseItemRepeat, "FieldActionUseItemRepeat");

extern "C" void func_802886AC(CpuContext* ctx);
extern "C" void MH3_FieldActionWeapon1001(CpuContext* ctx) {
    PortCheck::Call(ctx, "FieldActionWeapon1001", func_802886AC, Weapon1001);
}
REGISTER_NATIVE_FUNCTION_AS(0x802886AC, MH3_FieldActionWeapon1001, "FieldActionWeapon1001");

extern "C" void func_8025AA9C(CpuContext* ctx);
extern "C" void MH3_FieldActionMotion241(CpuContext* ctx) {
    PortCheck::Call(ctx, "FieldActionMotion241", func_8025AA9C, Motion241);
}
REGISTER_NATIVE_FUNCTION_AS(0x8025AA9C, MH3_FieldActionMotion241, "FieldActionMotion241");

extern "C" void func_8024FF6C(CpuContext* ctx);
extern "C" void MH3_FieldActionMotion7Or8(CpuContext* ctx) {
    PortCheck::Call(ctx, "FieldActionMotion7Or8", func_8024FF6C, Motion7Or8);
}
REGISTER_NATIVE_FUNCTION_AS(0x8024FF6C, MH3_FieldActionMotion7Or8, "FieldActionMotion7Or8");

extern "C" void func_802821DC(CpuContext* ctx);
extern "C" void MH3_FieldActionCharge1150(CpuContext* ctx) {
    PortCheck::Call(ctx, "FieldActionCharge1150", func_802821DC, Charge1150);
}
REGISTER_NATIVE_FUNCTION_AS(0x802821DC, MH3_FieldActionCharge1150, "FieldActionCharge1150");

extern "C" void func_8025B15C(CpuContext* ctx);
extern "C" void MH3_FieldActionMotion210(CpuContext* ctx) {
    PortCheck::Call(ctx, "FieldActionMotion210", func_8025B15C, Motion210);
}
REGISTER_NATIVE_FUNCTION_AS(0x8025B15C, MH3_FieldActionMotion210, "FieldActionMotion210");

extern "C" void func_8028563C(CpuContext* ctx);
extern "C" void MH3_FieldActionSpin1103(CpuContext* ctx) {
    PortCheck::Call(ctx, "FieldActionSpin1103", func_8028563C, Spin<kSpin1103>);
}
REGISTER_NATIVE_FUNCTION_AS(0x8028563C, MH3_FieldActionSpin1103, "FieldActionSpin1103");

extern "C" void func_802869D4(CpuContext* ctx);
extern "C" void MH3_FieldActionSpin1162(CpuContext* ctx) {
    PortCheck::Call(ctx, "FieldActionSpin1162", func_802869D4, Spin<kSpin1162>);
}
REGISTER_NATIVE_FUNCTION_AS(0x802869D4, MH3_FieldActionSpin1162, "FieldActionSpin1162");

extern "C" void func_802598C4(CpuContext* ctx);
extern "C" void MH3_FieldActionFall(CpuContext* ctx) {
    PortCheck::Call(ctx, "FieldActionFall", func_802598C4, Fall);
}
REGISTER_NATIVE_FUNCTION_AS(0x802598C4, MH3_FieldActionFall, "FieldActionFall");

extern "C" void func_8025CECC(CpuContext* ctx);
extern "C" void MH3_FieldActionFall2(CpuContext* ctx) {
    PortCheck::Call(ctx, "FieldActionFall2", func_8025CECC, Fall2);
}
REGISTER_NATIVE_FUNCTION_AS(0x8025CECC, MH3_FieldActionFall2, "FieldActionFall2");

extern "C" void func_802501B8(CpuContext* ctx);
extern "C" void MH3_FieldActionAwaitPlace(CpuContext* ctx) {
    PortCheck::Call(ctx, "FieldActionAwaitPlace", func_802501B8, AwaitPlace);
}
REGISTER_NATIVE_FUNCTION_AS(0x802501B8, MH3_FieldActionAwaitPlace, "FieldActionAwaitPlace");

extern "C" void func_80253038(CpuContext* ctx);
extern "C" void MH3_FieldActionRun119(CpuContext* ctx) {
    PortCheck::Call(ctx, "FieldActionRun119", func_80253038, Run119);
}
REGISTER_NATIVE_FUNCTION_AS(0x80253038, MH3_FieldActionRun119, "FieldActionRun119");

extern "C" void func_80254A28(CpuContext* ctx);
extern "C" void MH3_FieldActionJump15(CpuContext* ctx) {
    PortCheck::Call(ctx, "FieldActionJump15", func_80254A28, Jump15);
}
REGISTER_NATIVE_FUNCTION_AS(0x80254A28, MH3_FieldActionJump15, "FieldActionJump15");

extern "C" void func_80254F18(CpuContext* ctx);
extern "C" void MH3_FieldActionClimbOver(CpuContext* ctx) {
    PortCheck::Call(ctx, "FieldActionClimbOver", func_80254F18, ClimbOver);
}
REGISTER_NATIVE_FUNCTION_AS(0x80254F18, MH3_FieldActionClimbOver, "FieldActionClimbOver");

extern "C" void func_802813D0(CpuContext* ctx);
extern "C" void MH3_FieldActionCharge1101(CpuContext* ctx) {
    PortCheck::Call(ctx, "FieldActionCharge1101", func_802813D0, Charge1101);
}
REGISTER_NATIVE_FUNCTION_AS(0x802813D0, MH3_FieldActionCharge1101, "FieldActionCharge1101");

extern "C" void func_80256A58(CpuContext* ctx);
extern "C" void MH3_FieldActionKnockedFlying(CpuContext* ctx) {
    PortCheck::Call(ctx, "FieldActionKnockedFlying", func_80256A58, KnockedFlying);
}
REGISTER_NATIVE_FUNCTION_AS(0x80256A58, MH3_FieldActionKnockedFlying, "FieldActionKnockedFlying");

extern "C" void func_80258AC8(CpuContext* ctx);
extern "C" void MH3_FieldActionGrabbed(CpuContext* ctx) {
    PortCheck::Call(ctx, "FieldActionGrabbed", func_80258AC8, Grabbed);
}
REGISTER_NATIVE_FUNCTION_AS(0x80258AC8, MH3_FieldActionGrabbed, "FieldActionGrabbed");

extern "C" void func_8026A57C(CpuContext* ctx);
extern "C" void MH3_VelocityStep(CpuContext* ctx) {
    PortCheck::Call(ctx, "VelocityStep", func_8026A57C, VelocityStep);
}
REGISTER_NATIVE_FUNCTION_AS(0x8026A57C, MH3_VelocityStep, "VelocityStep");
