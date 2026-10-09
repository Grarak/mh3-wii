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
//   0x8025DC40, 0x8025DA9C  (plw, alt) another console's player standing or
//               resting; freed after 300 steps
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
    kFlag1 = 1,          // u8
    kFlag6 = 6,          // u8
    kFlag7 = 7,          // u8
    kFlag20 = 20,        // u8
    kFlag24 = 24,        // u8
    kPosition = 0x3C,    // VEC3
    kPositionCopy = 0x48, // VEC3 (0x80041EA4 copies the position there)
    kLastY = 0x60,       // f32 (copied back to the position's y)
    kSpeed = 0x6C,       // f32
    kFlag614 = 0x266,    // u8
    kCondition = 0x378,  // s16: stamina (stamina.cpp)
    kInputs = 6,         // u8: inputs counted in the escape stage (0x80257EEC)
    kStages = 7,         // u8: escape stages left (0x80257EEC)
    kVisible = 0x77,     // u8
    kWord1008 = 0x3F0,   // s16
    kChargeSteps = 0x310, // s16: steps charged (0x8028138C)
    kChargeLevel = 0x312, // u8: the level they reach (0x802812C0)
    kMotionRate = 0x354, // f32: the motion's speed
    kLean = 0xAC,        // u32: a turn (0x10000 a circle), as s16
    kCommandBits = 0xE0, // u32[]: command bits (0x8026A6A8 tests one)
    kCommands = 0xF0,    // u32[]: the step's command bits (0x8026A708 tests one)
    kPadHeld = 0xC8,     // u16
    kPadPressed = 0xCC,  // u16
    kFlag15 = 15,        // u8
    kWord16 = 16,        // u16
    kGroundKind = 22,    // u8 (GetGroundHit's)
    kClimbs = 0x5C7,     // u8
    kWord1020 = 0x3FC,   // s16
    kWord1024 = 0x400,   // s16
    kFlag789 = 0x315,    // u8
    kFlag1468 = 0x5BC,   // u8
    kFlag1469 = 0x5BD,   // u8
    kFlag1470 = 0x5BE,   // u8
    kFlag1640 = 0x668,   // u8
    kSteps1012 = 0x3F4,  // s16: steps, to 200 (0x80258230)
    kSignal = 0x656,     // u8 (set by others while +0x657 is 1)
    kWatching = 0x657,   // u8
    kStaminaMax = 0x37A, // s16: the stamina gauge's length (?): a rest ends adding it; the idle is tired at 150 or less
    kItem774 = 0x306,    // u16
    kWord1606 = 0x646,   // s16
    kWord1608 = 0x648,   // s16
    kState918 = 0x396,   // s16 (0x80277BA8)
    kState1408 = 0x580,  // s16 (0x80277CAC)
};

enum : uint32_t {
    NextAction = 0x80275B28,  // (plw, u8, u16 action, u16): +14 = 1, then 0x80275754 sets it
    ActionFlags = 0x80275B68, // (plw, u16 flag9 | 0x8000, a, b): flag 9 and act flags 1, 2, 4
    EndAction = 0x8027621C,   // (plw, u8 flag9, s32 blend, s32 flags): back to the field stance
    MotionAfterSet = 0x80277CBC, // (plw)
    CanGoOn = 0x802D7A08,     // () -> 1
    WaitStart = 0x802D7AB4,   // ()
    CameraFollow = 0x802BC700, // ()
    RotVecY = 0x800510C8,     // rotVecY (VEC3*, u32 angle)
    VecAdd = 0x80073FCC,      // (inout VEC3*, add VEC3*)
    ButtonBlocked = 0x802A0368, // (plw) -> 0 when a held button may count
    ClearActFlags = 0x8026FF54, // (plw, flags): clears flag bits (as SetActFlags)
    Nearby = 0x8026F96C,      // (plw, 0) -> u8
    InEventDemo = 0x8027BCAC, // PlInEventDemo (1) -> 1
    CopyVec = 0x80041EA4,     // (dst, src)
    FrameCheck = 0x8026A378,  // Pl_frame_check (plw, u16 channel, f1 frame, f2) -> 1 once past it
    StatusMessage = 0x802672D4, // PlShowStatusMessage (plw, 1, id, 0)
    StaminaChange = 0x8027695C, // PlStaminaChange (plw, s16 delta), per tick (stamina.cpp)
    AddHealth = 0x802762F0,   // PlAddHealth (plw, s16 amount) (player_health.cpp)
    Watch = 0x802DE5DC,       // (plw, plw + 1432)
    SetCondition = 0x802768CC, // PlSetConditionState (plw, s16 delta): stamina (stamina.cpp)
    SetEffect013 = 0x801072B4, // eft013_set (plw, u8)
    Cue = 0x80245D54,         // (plw, u16 id, u8 table 0-3): 0x802779D8 with the table's lists
    SkillCheck = 0x80271374,  // Pl_Skill_ck (plw, skill) -> 1
    SetState1408 = 0x80277CAC, // (plw, s16): +0x580
    SetState918 = 0x80277BA8, // (plw, s16): +0x396
    EscapeStages = 0x80794510, // u8[]: the inputs each escape stage takes, by stages left
    ChargeLevel = 0x802812C0, // (plw) -> u8: the level +0x310 reaches (thresholds 0x805C7478)
    ChargeTime = 0x80281210, // (plw, s16 steps) -> s16: scaled by skills 191, 192
    ChargeEffect = 0x802811B0, // (plw)
    GroundHit = 0x802916A0,   // GetGroundHit (VEC3*, s32, u8 kind) -> f1 height
    ClimateActive = 0x80276864, // PlClimateActive (plw, 0) -> 1 in heat or cold without protection
    InWater = 0x80278878,     // Pl_suimen_ck (plw) -> 1 at the water's surface
    AnyInput = 0x8026F8EC,    // (plw) -> 1 when the local player moves the stick or presses a button
    EndActionReset = 0x8027629C, // (plw, u8 flag9, s32 blend, s32 flags): +0x442 = 0, +0x30A = 255, then as EndAction
};
constexpr uint32_t kCharFlag246 = 0xF6; // MHchar u8 (0x8026A350)

// Bit n of the u32 array at `words` (0x8026A6A8 and 0x8026A708 on the PLW's).
bool Bit(uint32_t words, uint32_t n) { return (Memory::Read32(words + n / 32 * 4) >> (n % 32)) & 1; }

struct Player : Plw::Player {
    void Flags(uint32_t mode, uint32_t a, uint32_t b) const { game.Call<ActionFlags>({plw, mode, a, b}); }
    void End(uint32_t flag9, int32_t blend) const { game.Call<EndAction>({plw, flag9, uint32_t(blend), 0}); }
    void Next(uint32_t a, uint32_t action, uint32_t b) const { game.Call<NextAction>({plw, a, action, b}); }
};

// 0x802580D4: effect 13 (10) every 50 steps.
void Effect13Every50(const Player& p) {
    if (p.Every(50)) p.game.Call<SetEffect013>({p.plw, 10});
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
        if (game.Call<MasterCheck>({p.plw}) != 0 && p.CountDown()) p.End(1, 4);
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
        if (p.CountDown()) p.End(0, 12);
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
        if (p.CountDown()) p.End(0, 4);
        else if (game.Call<CanGoOn>() == 1) p.Next(0, 34, 0);
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
        if (p.Master()) game.Call<CameraFollow>();
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
            if ((Memory::Read16(pad + (alternative ? 4 : 0)) & mask) == 0) break;
            if (game.Call<ButtonBlocked>({plw}) != 0 && i != 6) break;
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
        if (speed < -250.0f) Memory::WriteFloat32(plw + kSpeed, -250.0f);
        else Memory::WriteFloat32(plw + kPosition + 8, Memory::ReadFloat32(plw + kPosition + 8) + 10.0f * s);
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
            Memory::WriteFloat32(plw + kPosition + k,
                                 Memory::ReadFloat32(plw + kPosition + k) + Memory::ReadFloat32(step + k));
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
    const auto s16 = [](uint32_t a) { return int32_t(int16_t(Memory::Read16(a))); };
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
        if (!p.Master() || p.Timer() > 0) break;
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
        if (p.Ended()) p.End(0, 4);
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
        if (game.Call<MasterCheck>({plw}) == 0) break;
        p.CountDownToZero();
        // The original counts this by 1 a step (it was left counting steps).
        const int32_t steps = int16_t(Memory::Read16(plw + kSteps1012)) + int32_t(GameStep::Ticks());
        Memory::Write16(plw + kSteps1012, uint16_t(steps));
        if (int16_t(steps) >= 200) p.Next(6, 30, 12);
        else if ((game.Call<Nearby>({plw, 0}) & 0xFF) >= 1) game.Call<0x8026A40C>({plw});
        else if (p.Timer() <= 0) p.Next(6, 28, 1);
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
        if (!p.Master()) break;
        if ((game.Call<Nearby>({plw, 0}) & 0xFF) >= 1 && game.Call<0x8026A6A8>({plw, 37}) == 1) {
            game.Call<0x8026A450>({plw});
            game.Call<0x80252B50>({plw, 0xC16D, 16020, 43, 44});
        } else if (p.Timer() <= 0) {
            if (int16_t(Memory::Read16(plw + kCondition)) > 0) p.Next(1, 8, 0);
            else p.Next(0, 150, 0);
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
        if (game.Call<0x8045169C>() == 1) Memory::Write8(plw + kSignal, 0);
        p.Timer(0);
        break;
    case 1: {
        if (game.Call<MasterCheck>({plw}) == 0) break;
        p.CountUp();
        const uint32_t signal = Memory::Read8(plw + kSignal);
        if (p.Timer() <= 600 && signal == 0) break;
        if (signal == 1) p.Next(0, alt ? 160 : 73, 0);
        else game.Call<EndAction>({plw, Memory::Read8(plw + kFlag9), 2, 0});
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
            if (b == 0) Memory::Write16(plw + 1048, 900);
            else Memory::Write16(plw + 1054, 450);
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
        if (p.CountDown()) p.NextState();
        counting = 1;
        break;
    case 2:
        counting = 0;
        break;
    }
    if (p.Master()) game.Call<0x80259030>({plw, 0, uint32_t(int32_t(int16_t(b))), counting});
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
                if ((game.Call<Rand>({1}) & 0x7F) != 0) continue;
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
        if (standing) p.Set(5, 4, 0);
        else p.Set(122, 6, 0);
        p.Timer(150);
        break;
    case 1:
        if (!p.CountDown()) break;
        p.NextState();
        p.Set(standing ? 6 : 124, 4, 0);
        break;
    case 2:
        if (!p.Ended()) break;
        game.Call<SetCondition>({plw, uint32_t(int32_t(int16_t(Memory::Read16(plw + kStaminaMax))))});
        if (standing) p.End(0, 6);
        else p.End(3, 4);
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
            if (game.Call<InEventDemo>({1}) == 0) game.Call<0x80272E94>({plw, Memory::Read16(plw + kItem774), uint32_t(-1)});
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
        if (p.Ended()) game.Call<EndAction>({plw, Memory::Read8(plw + kFlag9), 2, 0});
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
        constexpr struct { float frame; uint32_t id; } kCues[] = {{48, 8}, {68, 9}, {168, 9}, {190, 8}};
        for (const auto& cue : kCues)
            if (game.Call<FrameCheck>({plw, 0}, {cue.frame, 0.0}) == 1) game.Call<Cue>({plw, cue.id, 3});
        if (!p.Master() || p.Timer() > 0) break;
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
        if (!p.Ended()) break;
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
        if (p.Ended()) game.Call<EndActionReset>({plw, 0, 4, 0});
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
            if (game.Call<SkillCheck>({plw, 9}) == 1) needed = int8_t(needed * 2);
            if (int32_t(Memory::Read8(plw + kInputs)) >= needed) nextStage();
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
    for (const uint32_t flag : {kFlag1468, kFlag1469, kFlag1640, kFlag1470}) Memory::Write8(plw + flag, 0);
    p.game.Call<ClearActFlags>({plw, 2048});
}
void RemoteStart(const Player& p) {
    Memory::Write8(p.plw + kVisible, 1);
    p.Timer(300);
    Memory::Write8(p.plw + kFlag24, 0);
}
// The steps count only for players that aren't this console's.
void RemoteWait(const Player& p) {
    if (p.game.Call<MasterCheck>({p.plw}) != 0) return;
    p.Timer(p.Timer() - int32_t(GameStep::Ticks()));
    if (p.Timer() < 0) p.NextState();
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
    case 1: RemoteWait(p); break;
    case 2: RemoteFree(p); break;
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
    case 2: RemoteWait(p); break;
    case 3: RemoteFree(p); break;
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
        if (game.Call<MasterCheck>({plw}) == 0) break;
        if (game.Call<0x80277E10>({plw, 0}, {85.0, -60.0}) != 0) {
            Memory::Write32(plw + kAngle, Memory::Read32(plw + kAngle2) & 0xFFFF);
            p.Next(6, arg ? 68 : 48, 0);
            break;
        }
        const uint32_t step = frame[8];
        MoveTurned(p, step, 5.0f);
        for (uint32_t k = 0; k < 12; k += 4)
            Memory::WriteFloat32(plw + kPosition + k,
                                 Memory::ReadFloat32(plw + kPosition + k) + Memory::ReadFloat32(step + k));
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
    Memory::Write16(plw + kChargeSteps,
                    uint16_t(int16_t(Memory::Read16(plw + kChargeSteps)) + int32_t(GameStep::Ticks())));
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
            // Command 17 or 13 (the button let go) ends the charge (+6).
            if (Bit(plw + kCommands, 17) || Bit(plw + kCommands, 13)) Memory::Write8(plw + kFlag6, 1);
            if (Memory::Read8(plw + kFlag6) == 0) {
                Memory::WriteFloat32(plw + kMotionRate, 0.5f);
                ChargeStep(p);
                if (p.CountDown()) Memory::Write8(plw + kFlag6, 1);
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
        if (game.Call<MasterCheck>({plw}) == 0) break;
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
        if (Memory::Read8(plw + kFlag24) == 0) game.Call<0x8027714C>({plw, 0x805BEFE4, 0});
        else game.Call<0x80277204>({plw, 0});
        break;
    case 1:
        // The original counts on past 0 while 0x8027A3A4 says no.
        if (p.Timer() > 0 || game.Call<0x8027A3A4>({plw}) == 0) {
            if (p.CountDown()) Memory::Write32(plw + kLean, 0);
        }
        if (p.Master() && game.Call<0x8026FEA8>({plw}) == 1 && Memory::Read8(plw + kFlag24) == 1)
            game.Call<0x8027A5E0>({plw, 1050, 0});
        if (Memory::Read8(plw + kFlag24) == 1) break;
        if (game.Call<ClimateActive>({plw, 0}) == 1) p.SetX(121, 4, 0);
        else if (int16_t(Memory::Read16(plw + kStaminaMax)) <= 150) p.SetX(360, 4, 0);
        else if (game.Call<InWater>({plw}) == 1) p.SetX(118, 4, 0);
        else p.SetX(100, 4, 0);
        break;
    }
}

// 0x80253CA0: motion 105, the lean held within 60 degrees, draining
// stamina (4 a step) for 25 steps; when it ends action 8 near something,
// else 9 on command bit 37 (or for another console's player), else 7.
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
        if (lean <= -10922) Memory::Write32(plw + kLean, 0x10000 - 10922);
        else if (lean >= 10923) Memory::Write32(plw + kLean, 10923);
        break;
    }
    case 1:
        if (p.Timer() > 0) {
            p.CountDownToZero();
            game.Call<StaminaChange>({plw, uint32_t(-4)}); // per tick (stamina.cpp)
        }
        if (!p.Ended()) break;
        if (game.Call<MasterCheck>({plw}) == 0) p.Next(1, 9, 128);
        else if ((game.Call<Nearby>({plw, 0}) & 0xFF) >= 1) p.Next(1, 8, 0);
        else if (Bit(plw + kCommandBits, 37)) p.Next(1, 9, 128);
        else p.Next(1, 7, 128);
        break;
    }
}

} // namespace

extern "C" void func_802535B8(CpuContext* ctx);
extern "C" void MH3_FieldActionMotion8(CpuContext* ctx) { PortCheck::Call(ctx, "FieldActionMotion8", func_802535B8, Motion8); }
REGISTER_NATIVE_FUNCTION_AS(0x802535B8, MH3_FieldActionMotion8, "FieldActionMotion8");

extern "C" void func_80257E08(CpuContext* ctx);
extern "C" void MH3_FieldActionMotion26(CpuContext* ctx) { PortCheck::Call(ctx, "FieldActionMotion26", func_80257E08, Motion26); }
REGISTER_NATIVE_FUNCTION_AS(0x80257E08, MH3_FieldActionMotion26, "FieldActionMotion26");

extern "C" void func_8025198C(CpuContext* ctx);
extern "C" void MH3_FieldActionWait120(CpuContext* ctx) { PortCheck::Call(ctx, "FieldActionWait120", func_8025198C, Wait120); }
REGISTER_NATIVE_FUNCTION_AS(0x8025198C, MH3_FieldActionWait120, "FieldActionWait120");

extern "C" void func_8025D5F8(CpuContext* ctx);
extern "C" void MH3_FieldActionSlide221(CpuContext* ctx) { PortCheck::Call(ctx, "FieldActionSlide221", func_8025D5F8, Slide221); }
REGISTER_NATIVE_FUNCTION_AS(0x8025D5F8, MH3_FieldActionSlide221, "FieldActionSlide221");

extern "C" void func_8026AB00(CpuContext* ctx);
extern "C" void MH3_PlButtonHoldUpdate(CpuContext* ctx) { PortCheck::Call(ctx, "PlButtonHoldUpdate", func_8026AB00, ButtonHold); }
REGISTER_NATIVE_FUNCTION_AS(0x8026AB00, MH3_PlButtonHoldUpdate, "PlButtonHoldUpdate");

extern "C" void func_80259C9C(CpuContext* ctx);
extern "C" void MH3_FieldActionSink206(CpuContext* ctx) { PortCheck::Call(ctx, "FieldActionSink206", func_80259C9C, Sink206); }
REGISTER_NATIVE_FUNCTION_AS(0x80259C9C, MH3_FieldActionSink206, "FieldActionSink206");

extern "C" void func_8025B3BC(CpuContext* ctx);
extern "C" void MH3_FieldActionWait300(CpuContext* ctx) { PortCheck::Call(ctx, "FieldActionWait300", func_8025B3BC, Wait300); }
REGISTER_NATIVE_FUNCTION_AS(0x8025B3BC, MH3_FieldActionWait300, "FieldActionWait300");

extern "C" void func_802595A0(CpuContext* ctx);
extern "C" void MH3_FieldActionWalk52(CpuContext* ctx) { PortCheck::Call(ctx, "FieldActionWalk52", func_802595A0, Walk52); }
REGISTER_NATIVE_FUNCTION_AS(0x802595A0, MH3_FieldActionWalk52, "FieldActionWalk52");

extern "C" void func_8025BC60(CpuContext* ctx);
extern "C" void MH3_FieldActionTableMotion(CpuContext* ctx) { PortCheck::Call(ctx, "FieldActionTableMotion", func_8025BC60, TableMotionThenGo); }
REGISTER_NATIVE_FUNCTION_AS(0x8025BC60, MH3_FieldActionTableMotion, "FieldActionTableMotion");

extern "C" void func_80250D40(CpuContext* ctx);
extern "C" void MH3_FieldActionMotion318(CpuContext* ctx) { PortCheck::Call(ctx, "FieldActionMotion318", func_80250D40, Motion318); }
REGISTER_NATIVE_FUNCTION_AS(0x80250D40, MH3_FieldActionMotion318, "FieldActionMotion318");

extern "C" void func_80258230(CpuContext* ctx);
extern "C" void MH3_FieldActionMotion39(CpuContext* ctx) { PortCheck::Call(ctx, "FieldActionMotion39", func_80258230, Motion39); }
REGISTER_NATIVE_FUNCTION_AS(0x80258230, MH3_FieldActionMotion39, "FieldActionMotion39");

extern "C" void func_802547A0(CpuContext* ctx);
extern "C" void MH3_FieldActionMotion120(CpuContext* ctx) { PortCheck::Call(ctx, "FieldActionMotion120", func_802547A0, Motion120); }
REGISTER_NATIVE_FUNCTION_AS(0x802547A0, MH3_FieldActionMotion120, "FieldActionMotion120");

extern "C" void func_802497CC(CpuContext* ctx);
extern "C" void MH3_FieldActionAwaitSignal(CpuContext* ctx) { PortCheck::Call(ctx, "FieldActionAwaitSignal", func_802497CC, AwaitSignal); }
REGISTER_NATIVE_FUNCTION_AS(0x802497CC, MH3_FieldActionAwaitSignal, "FieldActionAwaitSignal");

extern "C" void func_80259374(CpuContext* ctx);
extern "C" void MH3_FieldActionMotion51(CpuContext* ctx) { PortCheck::Call(ctx, "FieldActionMotion51", func_80259374, Motion51); }
REGISTER_NATIVE_FUNCTION_AS(0x80259374, MH3_FieldActionMotion51, "FieldActionMotion51");

extern "C" void func_80250630(CpuContext* ctx);
extern "C" void MH3_FieldActionSleep(CpuContext* ctx) { PortCheck::Call(ctx, "FieldActionSleep", func_80250630, Sleep); }
REGISTER_NATIVE_FUNCTION_AS(0x80250630, MH3_FieldActionSleep, "FieldActionSleep");

extern "C" void func_8024A7C0(CpuContext* ctx);
extern "C" void MH3_FieldActionRest(CpuContext* ctx) { PortCheck::Call(ctx, "FieldActionRest", func_8024A7C0, Rest); }
REGISTER_NATIVE_FUNCTION_AS(0x8024A7C0, MH3_FieldActionRest, "FieldActionRest");

extern "C" void func_8024B8CC(CpuContext* ctx);
extern "C" void MH3_FieldActionMotion8Or364(CpuContext* ctx) { PortCheck::Call(ctx, "FieldActionMotion8Or364", func_8024B8CC, Motion8Or364); }
REGISTER_NATIVE_FUNCTION_AS(0x8024B8CC, MH3_FieldActionMotion8Or364, "FieldActionMotion8Or364");

extern "C" void func_8025C100(CpuContext* ctx);
extern "C" void MH3_FieldActionMotion507Cues(CpuContext* ctx) { PortCheck::Call(ctx, "FieldActionMotion507Cues", func_8025C100, Motion507Cues); }
REGISTER_NATIVE_FUNCTION_AS(0x8025C100, MH3_FieldActionMotion507Cues, "FieldActionMotion507Cues");

extern "C" void func_80258394(CpuContext* ctx);
extern "C" void MH3_FieldActionStruggle(CpuContext* ctx) { PortCheck::Call(ctx, "FieldActionStruggle", func_80258394, Struggle); }
REGISTER_NATIVE_FUNCTION_AS(0x80258394, MH3_FieldActionStruggle, "FieldActionStruggle");

extern "C" void func_802580D4(CpuContext* ctx);
extern "C" void MH3_PlEffect13Every50(CpuContext* ctx) { PortCheck::Call(ctx, "PlEffect13Every50", func_802580D4, Effect13); }
REGISTER_NATIVE_FUNCTION_AS(0x802580D4, MH3_PlEffect13Every50, "PlEffect13Every50");

extern "C" void func_80257EEC(CpuContext* ctx);
extern "C" void MH3_FieldActionEscape(CpuContext* ctx) { PortCheck::Call(ctx, "FieldActionEscape", func_80257EEC, Escape); }
REGISTER_NATIVE_FUNCTION_AS(0x80257EEC, MH3_FieldActionEscape, "FieldActionEscape");

extern "C" void func_8025DC40(CpuContext* ctx);
extern "C" void MH3_FieldActionRemoteStand(CpuContext* ctx) { PortCheck::Call(ctx, "FieldActionRemoteStand", func_8025DC40, RemoteStand); }
REGISTER_NATIVE_FUNCTION_AS(0x8025DC40, MH3_FieldActionRemoteStand, "FieldActionRemoteStand");

extern "C" void func_8025DA9C(CpuContext* ctx);
extern "C" void MH3_FieldActionRemoteRest(CpuContext* ctx) { PortCheck::Call(ctx, "FieldActionRemoteRest", func_8025DA9C, RemoteRest); }
REGISTER_NATIVE_FUNCTION_AS(0x8025DA9C, MH3_FieldActionRemoteRest, "FieldActionRemoteRest");

extern "C" void func_802596E8(CpuContext* ctx);
extern "C" void MH3_FieldActionWalk53(CpuContext* ctx) { PortCheck::Call(ctx, "FieldActionWalk53", func_802596E8, Walk53); }
REGISTER_NATIVE_FUNCTION_AS(0x802596E8, MH3_FieldActionWalk53, "FieldActionWalk53");

extern "C" void func_8028138C(CpuContext* ctx);
extern "C" void MH3_PlChargeStep(CpuContext* ctx) { PortCheck::Call(ctx, "PlChargeStep", func_8028138C, Charge); }
REGISTER_NATIVE_FUNCTION_AS(0x8028138C, MH3_PlChargeStep, "PlChargeStep");

extern "C" void func_80281DBC(CpuContext* ctx);
extern "C" void MH3_FieldActionCharge1133(CpuContext* ctx) { PortCheck::Call(ctx, "FieldActionCharge1133", func_80281DBC, ChargeHold<0, 1133, 36, 0>); }
REGISTER_NATIVE_FUNCTION_AS(0x80281DBC, MH3_FieldActionCharge1133, "FieldActionCharge1133");

extern "C" void func_80282770(CpuContext* ctx);
extern "C" void MH3_FieldActionCharge1170(CpuContext* ctx) { PortCheck::Call(ctx, "FieldActionCharge1170", func_80282770, ChargeHold<3, 1170, 65, 128>); }
REGISTER_NATIVE_FUNCTION_AS(0x80282770, MH3_FieldActionCharge1170, "FieldActionCharge1170");

extern "C" void func_80247704(CpuContext* ctx);
extern "C" void MH3_FieldActionClimbUp(CpuContext* ctx) { PortCheck::Call(ctx, "FieldActionClimbUp", func_80247704, ClimbUp); }
REGISTER_NATIVE_FUNCTION_AS(0x80247704, MH3_FieldActionClimbUp, "FieldActionClimbUp");

extern "C" void func_80249944(CpuContext* ctx);
extern "C" void MH3_FieldActionStand(CpuContext* ctx) { PortCheck::Call(ctx, "FieldActionStand", func_80249944, Stand); }
REGISTER_NATIVE_FUNCTION_AS(0x80249944, MH3_FieldActionStand, "FieldActionStand");

extern "C" void func_80253CA0(CpuContext* ctx);
extern "C" void MH3_FieldActionMotion105(CpuContext* ctx) { PortCheck::Call(ctx, "FieldActionMotion105", func_80253CA0, Motion105); }
REGISTER_NATIVE_FUNCTION_AS(0x80253CA0, MH3_FieldActionMotion105, "FieldActionMotion105");
