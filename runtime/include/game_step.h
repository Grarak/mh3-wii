// Variable-length logic steps. MH3's logic was written for a fixed 1/30 s step
// (a "tick"); the host runs each step for the time that really elapsed, given
// here as a scale of that tick.
#pragma once

#include <any>
#include <cstdint>

namespace GameStep {

// Variable-length steps (the frame pacer's mode): the fps float stays at the
// tick rate instead of following the step length.
void SetVariable(bool enabled);
bool Variable();

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

struct CpuContext;

namespace EffectStep {
std::any Save();
void Restore(const std::any& state);

// Effect::Calc on the guest worker (guest_worker.h). The task pass's calls are
// collected (Defer before it, Collected after it) and run on the worker while
// the game thread traverses the scene and draws the opaque buckets (Launch);
// Join waits for them, or runs them here when they were never launched. Every
// other use of the effect system joins first, so effects belong to one thread
// at a time.
void Defer(bool enabled);
void Collected();
void Launch(const CpuContext& game);
void Join(CpuContext* cpu);

// The overlapped draw (the default, docs/pipelined-draw.md): the
// game thread launches a step's calls at its handoff and owns the list, joining
// them in the next step's Defer; the draw thread's Join only waits for the
// calculation and puts the effect view, and its Launch does nothing.
void SetOverlap(bool enabled);
#ifdef MH3_DEVELOPMENT
// MH3_TRACE=particles: child emissions an Effect::Calc found queued.
void NoteQueuedChildren(uint32_t count);
#endif
} // namespace EffectStep
