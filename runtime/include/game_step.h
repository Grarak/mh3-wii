// Variable-length logic steps. MH3's logic was written for a fixed 1/30 s step
// (a "tick"); the host runs each step for the time that really elapsed, given
// here as a scale of that tick.
#pragma once

#include <any>
#include <cstdint>

namespace GameStep {

// Starts a logic step of `scale` ticks: sets NW4R's animation base rate and the
// game's steps-per-second float, and advances the tick clock.
void Begin(double scale);
double Scale();
// Whole ticks of game time that elapse in this step (0 at short steps, more
// than 1 at long ones). Anything the game counts in whole steps (integer
// timers, nw4r::ef emitters) advances by this instead of by one.
uint32_t Ticks();
// Fraction of a tick elapsed past the last whole tick (0 at the stock step).
double Fraction();
// Ticks of game time elapsed since boot, up to and including this step.
double Elapsed();
// This step's share of an integer amount the game applies once per tick: the
// amount times the step length, rounded so that the shares of consecutive
// steps add up to amount * ticks elapsed (exactly `amount` at the stock step).
int32_t Share(int32_t amount);

// Host-side step state, for the step probe (guest RAM is saved separately).
struct State {
    double scale;
    double clock;
    uint32_t ticks;
    double elapsed;
    std::any effects;
};
State Save();
void Restore(const State& state);

} // namespace GameStep

namespace EffectStep {
std::any Save();
void Restore(const std::any& state);
} // namespace EffectStep
