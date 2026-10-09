// The enemy work (_ENEMY_WORK), RMHP08: what the enemy action ports share.
// An action is a state machine over the byte at +5, run once per logic step
// by the enemy update; most wait on motion frames (em_frame_check) or on the
// end of a motion, and many keep a count of their own at +0x20.
//
// The counts were written for one call a tick. The helpers here take the
// step's whole ticks instead (GameStep::Ticks(): 0 on a step shorter than a
// tick, more than 1 on a long one), so that a count, and a test of it, keeps
// the Wii's rate at any step length. At the stock 1/30 s step (one tick a
// step) each is the original's +1 and test.
#pragma once

#include "game_step.h"
#include "guest_call.h"
#include "memory.h"

#include <cstdint>

namespace Em {

enum : uint32_t {
    kState = 0x05,         // u8: the action's state
    kSubState = 0x06,      // u8
    kCount = 0x20,         // s32: the action's count (calls, here ticks)
    kPosition = 0x188,     // VEC3
    kPrevPosition = 0x194, // VEC3
};

enum : uint32_t {
    SetUp = 0x801304DC,        // (work, kind)
    SetMotion = 0x8012F61C,    // (work, motion, flags, blend)
    FrameCheck = 0x8012F980,   // em_frame_check (work, mode; f1 frame, f2 end) -> 1
    MotionDone = 0x8012F9A0,   // (work) -> 1 when the motion has ended (0x800E21FC)
    DemoSteps = 0x80146048,    // () -> s16, the event demo clock (EventDemoGetSteps)
    DemoReached = 0x8014606C,  // (n) -> 1 from demo step n on
    SetPosition = 0x801460BC,  // (work; f1-f3): the position and the previous position
    SetAngles = 0x80146170,    // (work; f1-f3 degrees)
    DemoPose = 0x8014620C,     // (work, steps, position keys, angle keys): demo_pose.cpp
    DemoPoseAxes = 0x80146308, // (work, steps, position keys, angle keys, axes, axes)
    Spawn = 0x8030456C,        // (work, kind, joint, VEC3* offset or 0; f1 scale): an enemy effect
    Vec3Set = 0x8005000C,      // setVector3 (VEC3*; f1-f3)
};

// x mod n in [0, n), as the originals' `& (n - 1)` reads a negative count.
inline int32_t Mod(int32_t x, int32_t n) {
    const int32_t r = x % n;
    return r < 0 ? r + n : r;
}

// Whether one of the `ticks` values from `first` on is k mod n.
inline bool AnyOf(int32_t first, uint32_t ticks, int32_t n, int32_t k) {
    if (ticks == 0)
        return false;

    return Mod(k - first, n) < int32_t(ticks);
}

struct Enemy {
    GuestCall& game;
    uint32_t work;

    uint32_t State() const { return Memory::Read8(work + kState); }
    void State(uint8_t state) const { Memory::Write8(work + kState, state); }
    void NextState() const { State(uint8_t(State() + 1)); }
    int32_t Count() const { return int32_t(Memory::Read32(work + kCount)); }
    void Count(int32_t count) const { Memory::Write32(work + kCount, uint32_t(count)); }

    // The originals add 1 to the count each call: here the step's ticks.
    void CountTicks() const { Count(Count() + int32_t(GameStep::Ticks())); }

    // The originals' `count % n == k` (or `(count & (n - 1)) == k`) before
    // their +1 of the count: true on the step whose ticks include such a count.
    bool CountEvery(int32_t n, int32_t k = 0) const { return AnyOf(Count(), GameStep::Ticks(), n, k); }

    // em_frame_check: mode 0 (the step's motion advance crosses `frame`), or
    // the level modes (1: reached, 3: in [frame, end], ...).
    bool At(float frame, uint32_t mode = 0, float end = 0.0f) const { return game.Call<FrameCheck>({work, mode}, {frame, end}) == 1; }

    bool Ended() const { return game.Call<MotionDone>({work}) == 1; }
    void SetMotion(uint32_t motion, uint32_t flags, uint32_t blend) const { game.Call<Em::SetMotion>({work, motion, flags, blend}); }
    void Spawn(float scale, uint32_t kind, uint32_t joint, uint32_t offset = 0) const { game.Call<Em::Spawn>({work, kind, joint, offset}, {scale}); }
};

// 0x80056AB8 (work, kind, length), which the translator inlines: the
// rumble request of a monster (0x8066D178).
inline void Rumble(uint32_t work, uint32_t kind, uint32_t length) {
    constexpr uint32_t kRequest = 0x8066D178;
    Memory::Write8(kRequest + 0x5C, 1);
    Memory::Write8(kRequest + 0x5D, 0);
    Memory::Write32(kRequest + 0x58, work);
    Memory::Write32(kRequest + 0x54, length);
    Memory::Write8(kRequest + 0x60, uint8_t(kind));
    Memory::Write8(kRequest + 0x62, 0);
}

// The event demo clock (a tick site: whole ticks).
inline int32_t DemoClock(GuestCall& game) {
    return int16_t(game.Call<DemoSteps>());
}

// The originals' `steps % n == k` on the demo clock, which holds between ticks
// (so the test passed on every step of such a tick): true on the step whose
// ticks moved the clock onto such a value.
inline bool DemoEvery(int32_t steps, int32_t n, int32_t k = 0) {
    const uint32_t ticks = GameStep::Ticks();
    return AnyOf(steps - int32_t(ticks) + 1, ticks, n, k);
}

// The originals' `steps == n` on the demo clock: true on the step whose ticks
// moved the clock onto n (or past it, on a step of several ticks).
inline bool DemoAt(int32_t steps, int32_t n) {
    return DemoEvery(steps - n, 0x10000, 0);
}

} // namespace Em
