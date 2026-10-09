// The player work (_PLW), RMHP08: what the village's and the field's player
// action ports share. An action is a state machine over the byte at +5,
// stepped once per logic step, most waiting on the step countdown at +0x28
// or on the end of a motion (the MHchar at +0x13C plays them).
#pragma once

#include "game_step.h"
#include "guest_call.h"
#include "memory.h"

#include <algorithm>

namespace Plw {

enum : uint32_t {
    kState = 5,     // u8: the action's state
    kFlag9 = 9,     // u8
    kFlag14 = 14,   // u8 (set before the next action)
    kSteps = 0x20,  // s32: the player's step counter, 1 a tick (player_timers.cpp)
    kTimer = 0x28,  // s32: the action's step countdown (or count)
    kAngle2 = 0x58, // u32
    kAngle = 0xA8,  // u32: facing
    kPlayer = 0xB6, // u16: the player's index (low byte)
    kChar = 0x13C,  // MHchar*
};

constexpr uint32_t kMotionHold = 0xF5;   // MHchar u8: 0x8026A334 / 0x8026A340
constexpr uint32_t kMotionNumber = 0x54; // MHchar u16 (Get_motion_no 0x8026A36C)

enum : uint32_t {
    SetMotion = 0x8026A288,   // Pl_chr_set (plw, u16 motion, s32 blend, s32 flags): restarts it
    SetMotionX = 0x8026A2AC,  // Pl_chr_setX (plw, u16 motion, s32 blend, s32 flags): unless playing
    MotionEnded = 0x8026A3A0, // (plw) -> 1 when the motion has ended (0x800E21FC)
    MasterCheck = 0x8026FE38, // Pl_master_ck (plw) -> 1 for the local player
    SetActFlags = 0x8026FF24, // (plw, flags): sets flag bits (+0x360 for bit 31, else +0x35C)
    Rand = 0x800CEF20,        // ran_suu (u32 generator) -> u16
};

struct Player {
    GuestCall& game;
    uint32_t plw;

    uint32_t State() const { return Memory::Read8(plw + kState); }
    void NextState() const { Memory::Write8(plw + kState, uint8_t(State() + 1)); }
    int32_t Timer() const { return int32_t(Memory::Read32(plw + kTimer)); }
    void Timer(int32_t steps) const { Memory::Write32(plw + kTimer, uint32_t(steps)); }
    // The originals take 1 a step and fire once the countdown is out (<= 0):
    // here the ticks of the step.
    bool CountDown() const {
        Timer(Timer() - int32_t(GameStep::Ticks()));
        return Timer() <= 0;
    }

    // The originals take 1 a step while it is above 0.
    void CountDownToZero() const {
        if (Timer() > 0)
            Timer(std::max(Timer() - int32_t(GameStep::Ticks()), 0));
    }

    // The originals add 1 a step.
    void CountUp() const { Timer(Timer() + int32_t(GameStep::Ticks())); }
    // The originals' `steps % n == 0` on the step counter, which runs a tick
    // at a time: true on the step whose ticks passed a multiple of n.
    bool Every(int32_t n) const {
        int32_t rest = int32_t(Memory::Read32(plw + kSteps)) % n;

        if (rest < 0)
            rest += n;

        return rest < int32_t(GameStep::Ticks());
    }

    uint32_t Index() const { return Memory::Read16(plw + kPlayer) & 0xFF; }
    uint32_t Char() const { return Memory::Read32(plw + kChar); }
    uint32_t Motion() const { return Memory::Read16(Char() + kMotionNumber); }
    void HoldMotion(uint8_t hold) const { Memory::Write8(Char() + kMotionHold, hold); }
    bool Master() const { return game.Call<MasterCheck>({plw}) == 1; }
    bool Ended() const { return game.Call<MotionEnded>({plw}) == 1; }
    void Set(uint32_t motion, int32_t blend, int32_t flags) const { game.Call<SetMotion>({plw, motion, uint32_t(blend), uint32_t(flags)}); }
    void SetX(uint32_t motion, int32_t blend, int32_t flags) const { game.Call<SetMotionX>({plw, motion, uint32_t(blend), uint32_t(flags)}); }
    void Flag9(uint8_t value) const { Memory::Write8(plw + kFlag9, value); } // 0x8020A650, 0x80275B60
};

} // namespace Plw
