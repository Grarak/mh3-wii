#include "game_step.h"
#include "memory.h"

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
