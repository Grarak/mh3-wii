// nw4r::ef (the effect/particle library) under variable-length logic steps.
//
// ef is tick based: every Effect::Calc emitters run their curves and emission
// and every particle runs one tick (curves at its integer age, fields,
// collision, position += velocity), so at a short step particles would move,
// age and spawn too fast.
//
//  - Emitters keep ticking on whole ticks: Effect::Calc runs once per whole
//    tick of game time elapsed this step (GameStep::Ticks), and a step without
//    one runs a particle-only pass with emitter curves and emission gated off.
//    Emission is an event (particles appear on ticks), so it stays on ticks.
//  - Particles advance by the game time of the pass (dt ticks) and carry a
//    fractional age. ParticleManager::Calc (0x800AC35C) runs every pass for
//    every particle, at the integer tick of its age:
//      velocity += fields * dt, position += (field push + velocity) * momentum
//      * dt: the momentum (the factor ParticleAddPosition 0x800AD130 applies)
//      is scaled by dt for the call, and the velocity change the call made is
//      taken dt times, the position corrected for it;
//      the curves (colours, size, scale, rotation, texture transforms) are
//      keyframed values of the age: they are evaluated at the particle's
//      fractional age, from the curve's values at the ticks on either side;
//      the age crosses whole ticks for the tick's events: child emission
//      (a curve track) and retirement at the end of life only happen then.
//    At dt = 1 (the stock 1/30 s step) the call is the original.
//
// Offsets are MH3's nw4r ef (the same layout as Wii Sports' decompiled ef).
#include "abi_bridge.h"
#include "game_step.h"
#include "guest_call.h"
#include "port_check.h"
#include "memory.h"

#include <array>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>

extern "C" void func_800A4C20(CpuContext*); // nw4r::ef::Effect::Calc(bool onlyBillboard)
extern "C" void func_800A8AC0(CpuContext*); // nw4r::ef::Emitter::CalcEmitter()
extern "C" void func_800A8E5C(CpuContext*); // nw4r::ef::Emitter::CalcEmission()
extern "C" void func_800AC35C(CpuContext*); // nw4r::ef::ParticleManager::Calc()

namespace {

// ut::List: head, tail, u16 count, u16 offset of the link {prev, next} in each object.
struct List {
    uint32_t address;
    uint32_t First() const { return Memory::Read32(address); }
    uint32_t Last() const { return Memory::Read32(address + 4); }
    uint32_t Next(uint32_t object) const {
        return Memory::Read32(object + Memory::Read16(address + 0xA) + 4);
    }
};

enum : uint32_t { kLifeActive = 1 };
enum : uint32_t { kEvalWait = 0 };

enum : uint32_t {
    TrackCount = 0x800AD294,     // (ResEmitter) -> u16 particle tracks
    InitTrackCount = 0x800ADED8, // (ResEmitter) -> u16 of them run only at tick 0
    Track = 0x800ADE30,          // (ResEmitter, u16 index) -> track
    CurveU8 = 0x8009DF14,        // AnimCurveExecuteU8(track, target, tick, seed, life)
    CurveF32 = 0x8009F8C0,       // AnimCurveExecuteF32(...)
    CurveRotate = 0x800A035C,    // AnimCurveExecuteRotate(...)
};

// Effect / Emitter / ParticleManager / Particle fields.
enum : uint32_t {
    kEffectEmitters = 0x24,        // ut::List of Emitter
    kLifeStatus = 0x0C,            // u32 (Emitter, Particle)
    kEmitterLastTranslate = 0x108, // VEC3: emitter position after the last Effect::Calc
    kManagerResource = 0x24,       // ResEmitter
    kManagerParticles = 0x38,      // ut::List of Particle
    kManagerLastCalced = 0x54,     // Particle*: Calc resumes after it
    kParticleCurves = 0x20,        // ParticleParameter: curve targets from here
    kParticleColours = 0x30,       // .. RGBA8 colours up to here
    kParticleRotate = 0x40,        // VEC3 radians
    kParticleFloats = 0x88,        // size .. texture translate: f32 up to here
    kParticleCurvesEnd = 0xA0,     // textures, flags
    kParticleVelocity = 0xA0,      // VEC3
    kParticlePosition = 0xAC,      // VEC3
    kParticlePrevPosition = 0xB8,  // VEC3
    kParticleMomentum = 0xC4,      // f32
    kParticleEval = 0xD8,          // u32
    kParticleTick = 0xDC,          // u16 age in ticks
    kParticleSeed = 0xDE,          // u16
    kParticleLife = 0xE0,          // u16
};
// Track header: u8 magic (0xAB/0xAC), u8 target offset, u8 kind, u8, u8 flags.
enum : uint8_t { kTrackU8 = 0, kTrackF32 = 3, kTrackRotate = 6 };
enum : uint8_t { kTrackSkip = 0x08, kTrackEmitterTick = 0x10 };

using Vec3 = std::array<float, 3>;
Vec3 ReadVec(uint32_t a) { return {Memory::ReadFloat32(a), Memory::ReadFloat32(a + 4), Memory::ReadFloat32(a + 8)}; }
void WriteVec(uint32_t a, const Vec3& v) {
    for (int k = 0; k < 3; ++k) Memory::WriteFloat32(a + 4 * k, v[k]);
}

constexpr size_t kCurveWords = (kParticleCurvesEnd - kParticleCurves) / 4;
using Curves = std::array<uint32_t, kCurveWords>;
Curves ReadCurves(uint32_t particle) {
    Curves c;
    for (size_t i = 0; i < kCurveWords; ++i) c[i] = Memory::Read32(particle + kParticleCurves + 4 * i);
    return c;
}
void WriteCurves(uint32_t particle, const Curves& c) {
    for (size_t i = 0; i < kCurveWords; ++i) Memory::Write32(particle + kParticleCurves + 4 * i, c[i]);
}
float F(uint32_t bits) { float f; std::memcpy(&f, &bits, 4); return f; }
uint32_t Bits(float f) { uint32_t b; std::memcpy(&b, &f, 4); return b; }

struct Particle {
    uint16_t seed = 0; // identity: a reused slot or a stock tick invalidates the entry
    double age = 0;    // ticks, fractional
    uint64_t generation = 0; // Effect::Calc pass that last advanced it
};

struct Step {
    std::unordered_map<uint32_t, Particle> particles; // those between ticks
    uint64_t generation = 0;
    double particleStep = 1.0; // ticks particles advance in this Effect::Calc pass
    bool particlesOnly = false;
};
Step step;

// Particles whose age crosses no whole tick in the running ParticleManager::Calc.
std::unordered_set<uint32_t> betweenTicks;

// Runs the particle's curve tracks at `tick`, as ParticleManager::Calc does
// (without the child and texture tracks, the events, and the emitter-age ones).
void RunCurves(GuestCall& game, uint32_t manager, uint32_t particle, uint32_t tick) {
    const uint32_t res = Memory::Read32(manager + kManagerResource);
    const uint32_t count = game(TrackCount, {res}) & 0xFFFF;
    const uint32_t first = tick == 0 ? 0 : game(InitTrackCount, {res}) & 0xFFFF;
    const uint32_t seed = Memory::Read16(particle + kParticleSeed);
    const uint32_t life = Memory::Read16(particle + kParticleLife);
    for (uint32_t i = first; i < count; ++i) {
        const uint32_t track = game(Track, {res, i});
        const uint8_t magic = Memory::Read8(track), target = Memory::Read8(track + 1);
        const uint8_t kind = Memory::Read8(track + 2), flags = Memory::Read8(track + 4);
        if ((flags & (kTrackSkip | kTrackEmitterTick)) != 0 || uint8_t(magic + 0x55) >= 2) continue;
        if (kind == kTrackU8) game(CurveU8, {track, particle + kParticleCurves + target, tick, seed, life});
        else if (kind == kTrackF32) game(CurveF32, {track, particle + kParticleCurves + target, tick, seed, life});
        else if (kind == kTrackRotate) game(CurveRotate, {track, particle + kParticleRotate, tick, seed, life});
    }
}

// Shows the curves at age `at` (a fraction past a whole tick): the keyframed
// values between the ticks either side, as the curves are linear between keys.
void CurvesAt(GuestCall& game, uint32_t manager, uint32_t particle, double at) {
    const uint32_t tick = uint32_t(at);
    const float t = float(at - tick);
    const Curves kept = ReadCurves(particle);
    RunCurves(game, manager, particle, tick);
    const Curves a = ReadCurves(particle);
    RunCurves(game, manager, particle, tick + 1);
    const Curves b = ReadCurves(particle);
    Curves s = kept;
    constexpr size_t colours = (kParticleColours - kParticleCurves) / 4;
    constexpr size_t floats = (kParticleFloats - kParticleCurves) / 4;
    constexpr size_t rotate = (kParticleRotate - kParticleCurves) / 4;
    for (size_t i = 0; i < colours; ++i) {
        uint32_t v = 0;
        for (int shift = 24; shift >= 0; shift -= 8) {
            const float x = float(a[i] >> shift & 0xFF), y = float(b[i] >> shift & 0xFF);
            v |= uint32_t(std::lround(x + (y - x) * t)) << shift;
        }
        s[i] = v;
    }
    for (size_t i = colours; i < floats; ++i) {
        const float x = F(a[i]), y = F(b[i]);
        // A rotation curve wrapping between the two ticks snaps.
        const bool wraps = i >= rotate && i < rotate + 3 && std::fabs(y - x) > 3.1415927f;
        s[i] = !std::isfinite(x) || !std::isfinite(y) || wraps ? a[i] : Bits(x + (y - x) * t);
    }
    WriteCurves(particle, s);
}

} // namespace

namespace EffectStep {
std::any Save() { return step; }
void Restore(const std::any& state) { step = std::any_cast<const Step&>(state); }
} // namespace EffectStep

static void EffectCalcPort(CpuContext* ctx) {
    const uint32_t effect = ctx->gpr[3];
    const bool onlyBillboard = (ctx->gpr[4] & 0xFF) != 0;
    const double scale = GameStep::Scale();
    const uint32_t ticks = GameStep::Ticks();
    if (onlyBillboard) {
        func_800A4C20(ctx);
        return;
    }
    const CpuContext saved = *ctx;
    if (ticks == 0) {
        // Particles only. Emitters keep their last whole tick's positions, from
        // which emission derives the emitter's velocity.
        const List emitters{effect + kEffectEmitters};
        std::vector<std::pair<uint32_t, std::array<uint32_t, 6>>> kept;
        for (uint32_t e = emitters.First(); e; e = emitters.Next(e)) {
            std::array<uint32_t, 6> words;
            for (size_t i = 0; i < 6; ++i) words[i] = Memory::Read32(e + kEmitterLastTranslate + i * 4);
            kept.emplace_back(e, words);
        }
        step.particleStep = scale;
        step.particlesOnly = true;
        ++step.generation;
        func_800A4C20(ctx);
        step.particlesOnly = false;
        for (const auto& [e, words] : kept)
            for (size_t i = 0; i < 6; ++i) Memory::Write32(e + kEmitterLastTranslate + i * 4, words[i]);
    } else {
        step.particleStep = scale / ticks;
        for (uint32_t i = 0; i < ticks; ++i) {
            *ctx = saved;
            ++step.generation;
            func_800A4C20(ctx);
        }
    }
    step.particleStep = 1.0;
    *ctx = saved;
}
extern "C" void func_800A4C20(CpuContext* ctx);
extern "C" void MH3_EffectCalc(CpuContext* ctx) { PortCheck::Call(ctx, "EffectCalc", func_800A4C20, EffectCalcPort); }
REGISTER_NATIVE_FUNCTION_AS(0x800A4C20, MH3_EffectCalc, "EffectCalc");

static void EmitterCalcEmitterPort(CpuContext* ctx) {
    if (!step.particlesOnly) func_800A8AC0(ctx);
}
extern "C" void func_800A8AC0(CpuContext* ctx);
extern "C" void MH3_EmitterCalcEmitter(CpuContext* ctx) { PortCheck::Call(ctx, "EmitterCalcEmitter", func_800A8AC0, EmitterCalcEmitterPort); }
REGISTER_NATIVE_FUNCTION_AS(0x800A8AC0, MH3_EmitterCalcEmitter, "EmitterCalcEmitter");

static void EmitterCalcEmissionPort(CpuContext* ctx) {
    if (!step.particlesOnly) func_800A8E5C(ctx);
}
extern "C" void func_800A8E5C(CpuContext* ctx);
extern "C" void MH3_EmitterCalcEmission(CpuContext* ctx) { PortCheck::Call(ctx, "EmitterCalcEmission", func_800A8E5C, EmitterCalcEmissionPort); }
REGISTER_NATIVE_FUNCTION_AS(0x800A8E5C, MH3_EmitterCalcEmission, "EmitterCalcEmission");

static void ParticleManagerCalcPort(CpuContext* ctx) {
    // Whole ticks with every particle on a whole tick (the stock 1/30 s step)
    // are the original.
    if (step.particleStep == 1.0 && step.particles.empty()) {
        func_800AC35C(ctx);
        return;
    }
    const CpuContext saved = *ctx;
    const uint32_t manager = ctx->gpr[3];
    const List list{manager + kManagerParticles};
    const uint32_t lastCalced = Memory::Read32(manager + kManagerLastCalced);

    struct Work {
        uint32_t address;
        double age, dt;
        float momentum;
        Vec3 velocity, position;
    };
    std::vector<Work> work;
    for (uint32_t p = lastCalced ? list.Next(lastCalced) : list.First(); p; p = list.Next(p)) {
        if (Memory::Read32(p + kLifeStatus) != kLifeActive || Memory::Read32(p + kParticleEval) != kEvalWait)
            continue;
        const uint16_t tick = Memory::Read16(p + kParticleTick);
        double age = tick, dt = step.particleStep;
        if (auto it = step.particles.find(p); it != step.particles.end()) {
            const Particle& e = it->second;
            if (e.seed == Memory::Read16(p + kParticleSeed) && uint32_t(e.age) == tick) {
                age = e.age;
                // Calc again in the same pass (calc remain: a particle emitted
                // with ticks to catch up) is a whole tick.
                if (e.generation == step.generation) dt = 1.0;
            }
        }
        Work w{p, age, dt, Memory::ReadFloat32(p + kParticleMomentum), ReadVec(p + kParticleVelocity),
               ReadVec(p + kParticlePosition)};
        if (dt != 1.0) Memory::WriteFloat32(p + kParticleMomentum, float(w.momentum * dt));
        if (std::floor(age + dt) == std::floor(age)) betweenTicks.insert(p);
        work.push_back(w);
    }
    func_800AC35C(ctx);
    betweenTicks.clear();
    *ctx = saved;

    GuestFrame frame(ctx, 0x20);
    GuestCall game{ctx};
    for (const Work& w : work) {
        const uint32_t p = w.address;
        if (Memory::Read32(p + kLifeStatus) != kLifeActive) { // retired
            step.particles.erase(p);
            continue;
        }
        if (w.dt != 1.0) {
            Memory::WriteFloat32(p + kParticleMomentum, w.momentum);
            const Vec3 after = ReadVec(p + kParticleVelocity);
            Vec3 velocity, position = ReadVec(p + kParticlePosition), previous;
            for (int k = 0; k < 3; ++k) {
                velocity[k] = float(w.velocity[k] + (after[k] - w.velocity[k]) * w.dt);
                position[k] = float(position[k] - (after[k] - velocity[k]) * w.momentum * w.dt);
                // The previous position trails by a tick's movement, as on the Wii.
                previous[k] = float(position[k] - (position[k] - w.position[k]) / w.dt);
            }
            WriteVec(p + kParticleVelocity, velocity);
            WriteVec(p + kParticlePosition, position);
            WriteVec(p + kParticlePrevPosition, previous);
        }
        const double age = w.age + w.dt;
        Memory::Write16(p + kParticleTick, uint16_t(age));
        if (age == std::floor(age)) {
            step.particles.erase(p);
            continue;
        }
        step.particles[p] = {Memory::Read16(p + kParticleSeed), age, step.generation};
        // After the tick from age k the Wii shows the curves at k: the curves
        // run one tick behind the age, here at age - 1.
        if (age > 1.0) CurvesAt(game, manager, p, age - 1.0);
    }
}
extern "C" void func_800AC35C(CpuContext* ctx);
extern "C" void MH3_ParticleManagerCalc(CpuContext* ctx) { PortCheck::Call(ctx, "ParticleManagerCalc", func_800AC35C, ParticleManagerCalcPort); }
REGISTER_NATIVE_FUNCTION_AS(0x800AC35C, MH3_ParticleManagerCalc, "ParticleManagerCalc");


// Child emission is a curve event: only when the particle's age crosses a tick.
extern "C" void func_800A1D0C(CpuContext* ctx);
static void AnimCurveExecuteChildPort(CpuContext* ctx) {
    if (!betweenTicks.count(ctx->gpr[4])) func_800A1D0C(ctx);
}
extern "C" void MH3_AnimCurveExecuteChild(CpuContext* ctx) { PortCheck::Call(ctx, "AnimCurveExecuteChild", func_800A1D0C, AnimCurveExecuteChildPort); }
REGISTER_NATIVE_FUNCTION_AS(0x800A1D0C, MH3_AnimCurveExecuteChild, "AnimCurveExecuteChild");
