#include "game_step.h"
#include "memory.h"

#ifdef MH3_DEVELOPMENT
#include "dev_options.h"

#include <cstdio>
#endif

#include <cmath>

namespace GameStep {
namespace {

constexpr uint32_t kAnimationBaseRate = 0x80793628; // f32 nw4r::g3d::FrameCtrl::smBaseUpdateRate
constexpr uint32_t kFrameRate = 0x8065B4D0;         // f32 steps per second (60 / vcnt at boot)

double scale = 1.0;
double clock = 0.0; // fraction of a tick carried to the next step
uint32_t ticks = 1;
double elapsed = 0.0;
bool variable = false;

#ifdef MH3_DEVELOPMENT
// MH3_TRACE=rand: how far the game's random streams moved per tick of game
// time. Rand (0x800CEF20, also inlined into its callers) steps one of four
// u16 seeds (0x8065AA78) to seed * 176 % 65363; each step's start finds how
// many times each moved since the last by stepping the old seed to the new.
// Every 300 ticks an `MH3 rand` line: the ticks and steps, each stream's
// advances per tick, and the seeds that jumped (set, not stepped).
void TraceRand(double ranTicks) {
    static const TraceFlag on("rand");

    if (!on)
        return;

    constexpr uint32_t kSeeds = 0x8065AA78;
    static uint16_t last[4];
    static bool primed;
    static uint64_t advances[4], jumps[4], steps;
    static double tickSum;
    uint16_t now[4];

    for (uint32_t i = 0; i < 4; ++i)
        now[i] = Memory::Read16(kSeeds + 2 * i);

    if (primed) {
        for (uint32_t i = 0; i < 4; ++i) {
            uint32_t seed = last[i], count = 0;

            while (seed != now[i] && count < 65363) {
                seed = (seed ? seed : 1) * 176 % 65363;
                ++count;
            }

            if (seed == now[i])
                advances[i] += count;
            else
                ++jumps[i];
        }

        tickSum += ranTicks;
        ++steps;

        if (tickSum >= 300.0) {
            std::fprintf(stderr, "MH3 rand: %.1f ticks, %llu steps; per tick %.3f %.3f %.3f %.3f; jumps %llu %llu %llu %llu\n", tickSum,
                         (unsigned long long)steps, advances[0] / tickSum, advances[1] / tickSum, advances[2] / tickSum, advances[3] / tickSum,
                         (unsigned long long)jumps[0], (unsigned long long)jumps[1], (unsigned long long)jumps[2], (unsigned long long)jumps[3]);
            tickSum = 0;
            steps = 0;

            for (uint32_t i = 0; i < 4; ++i)
                advances[i] = jumps[i] = 0;
        }
    }

    for (uint32_t i = 0; i < 4; ++i)
        last[i] = now[i];

    primed = true;
}
#endif

} // namespace

// NW4R advances every g3d animation by rate * FrameCtrl::smBaseUpdateRate per
// step. The game converts seconds to frame counts with the fps float (every
// use multiplies by it), and its counters then advance once per frame: at
// variable step they advance by whole ticks, so the float stays at the tick
// rate, 30.
void SetVariable(bool enabled) {
    variable = enabled;
}

bool Variable() {
    return variable;
}

void Begin(double s) {
#ifdef MH3_DEVELOPMENT
    TraceRand(scale);
#endif
    scale = s;
    Memory::WriteFloat32(kAnimationBaseRate, float(s));
    Memory::WriteFloat32(kFrameRate, float(variable ? 30.0 : 30.0 / s));
    elapsed += s;
    clock += s;
    const double whole = std::floor(clock);
    ticks = uint32_t(whole);
    clock -= whole;
}

double Scale() {
    return scale;
}

uint32_t Ticks() {
    return ticks;
}

double Elapsed() {
    return elapsed;
}

double Fraction() {
    return clock;
}

int32_t Share(int32_t amount) {
    return int32_t(std::lround(amount * elapsed) - std::lround(amount * (elapsed - scale)));
}

State Save() {
    return {scale, clock, ticks, elapsed, EffectStep::Save()};
}

void Restore(const State& state) {
    scale = state.scale;
    clock = state.clock;
    ticks = state.ticks;
    elapsed = state.elapsed;
    EffectStep::Restore(state.effects);
}

} // namespace GameStep
