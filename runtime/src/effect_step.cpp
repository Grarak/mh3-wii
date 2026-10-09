// nw4r::ef (the effect/particle library) under variable-length logic steps.
//
// ef is tick based: every Effect::Calc emitters run their curves and emission
// and every particle runs one tick (curves at its integer age, fields,
// collision, position += velocity), so at a short step particles would move,
// age and spawn too fast. Instead of rewriting the library for fractional
// ticks, its own per-tick code keeps running on whole ticks and is driven by
// the step clock:
//
//  - Effect::Calc runs once per whole tick of game time that elapsed this
//    step (GameStep::Ticks). A step without one runs a particle-only pass:
//    emitter curves and emission are gated off, so emitters keep ticking on
//    whole ticks of game time.
//  - Each particle carries its own clock. ParticleManager::Calc only runs a
//    particle's real tick when that clock crosses a whole tick; the tick's
//    before and after states are kept, and between ticks the particle is
//    shown at its place in that tick (position, velocity, size, scale,
//    rotation, texture transform, colours), which at a 1/30 s step is exactly
//    the stock state after each tick. Curve events (child emission, texture
//    changes) still fire once per whole tick.
//
// Offsets are MH3's nw4r ef (the same layout as Wii Sports' decompiled ef).
#include "abi_bridge.h"
#include "game_step.h"
#include "port_check.h"
#include "memory.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <utility>
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
enum : uint32_t { kEvalWait = 0, kEvalDone = 1 };

// Effect / Emitter / ParticleManager / Particle fields.
enum : uint32_t {
    kEffectEmitters = 0x24,        // ut::List of Emitter
    kLifeStatus = 0x0C,            // u32 (Emitter, Particle)
    kEmitterLastTranslate = 0x108, // VEC3: emitter position after the last Effect::Calc
    kEmitterPrevTranslate = 0x114, // VEC3: the one before (velocity inheritance)
    kManagerParticles = 0x38,      // ut::List of Particle
    kManagerLastCalced = 0x54,     // Particle*: Calc resumes after it
    kParticleState = 0x20,         // ParticleParameter .. momentum
    kParticleEval = 0xD8,          // u32
    kParticleTick = 0xDC,          // u16 age in ticks
    kParticleSeed = 0xDE,          // u16
};

// ParticleParameter (0x20..0xC8) as words: colours, size, scale, rotate,
// texture scale/rotate/translate, textures and flags, velocity, position,
// previous position, momentum.
constexpr size_t kStateWords = (0xC8 - kParticleState) / 4;
using State = std::array<uint32_t, kStateWords>;
constexpr size_t Word(uint32_t offset) { return (offset - kParticleState) / 4; }

State ReadState(uint32_t particle) {
    State s;
    for (size_t i = 0; i < kStateWords; ++i) s[i] = Memory::Read32(particle + kParticleState + i * 4);
    return s;
}
void WriteState(uint32_t particle, const State& s) {
    for (size_t i = 0; i < kStateWords; ++i) Memory::Write32(particle + kParticleState + i * 4, s[i]);
}

float F(uint32_t bits) { float f; std::memcpy(&f, &bits, 4); return f; }
uint32_t Bits(float f) { uint32_t b; std::memcpy(&b, &f, 4); return b; }

// The particle a fraction t into its tick from `before` to `after`.
State Between(const State& before, const State& after, float t) {
    State s = after;
    const auto lerp = [&](size_t i, float wrap) {
        const float a = F(before[i]), b = F(after[i]);
        // Angles and texture offsets that wrapped during the tick snap.
        if (std::isfinite(a) && std::isfinite(b) && !(wrap > 0 && std::fabs(b - a) > wrap))
            s[i] = Bits(a + (b - a) * t);
    };
    for (size_t i = Word(0x20); i < Word(0x30); ++i) { // colours, RGBA8
        uint32_t v = 0;
        for (int shift = 24; shift >= 0; shift -= 8) {
            const float a = float(before[i] >> shift & 0xFF), b = float(after[i] >> shift & 0xFF);
            v |= uint32_t(std::lround(a + (b - a) * t)) << shift;
        }
        s[i] = v;
    }
    for (size_t i = Word(0x30); i < Word(0x40); ++i) lerp(i, 0);                // size, scale
    for (size_t i = Word(0x40); i < Word(0x70); ++i) lerp(i, 3.1415927f);       // rotate, texture scale/rotate
    for (size_t i = Word(0x70); i < Word(0x88); ++i) lerp(i, 0.5f);             // texture translate
    for (size_t i = Word(0xA0); i < Word(0xB8); ++i) lerp(i, 0);                // velocity, position
    // Previous position trails by the whole tick's movement, as after a tick.
    for (size_t k = 0; k < 3; ++k) {
        const size_t pos = Word(0xAC) + k, prev = Word(0xB8) + k;
        s[prev] = Bits(F(s[pos]) - (F(after[pos]) - F(before[pos])));
    }
    return s;
}

struct Particle {
    uint16_t seed = 0, tick = 0; // identity: a reused slot or a stock tick invalidates the entry
    float t = 0;                 // clock: fraction of the current tick elapsed
    uint64_t generation = 0;     // Effect::Calc pass that last ran its tick
    State before{}, after{};
};

struct Step {
    std::unordered_map<uint32_t, Particle> particles;
    uint64_t generation = 0;
    double particleStep = 1.0; // ticks particles advance in this Effect::Calc pass
    bool particlesOnly = false;
};
Step step;

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
    // Whole ticks with no particle between ticks (the stock 1/30 s step) are
    // the original. Once particles are tracked, a whole tick still goes through
    // their clocks: their RAM holds a between-ticks state, not their last tick.
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
        Particle* particle;
        double remaining; // ticks still to advance in this pass
        bool known;       // the entry holds this particle's last tick
        bool catchUp;     // an extra tick (calc remain) within the same pass
        State pre;
    };
    std::vector<Work> work;
    for (uint32_t p = lastCalced ? list.Next(lastCalced) : list.First(); p; p = list.Next(p)) {
        if (Memory::Read32(p + kLifeStatus) != kLifeActive || Memory::Read32(p + kParticleEval) != kEvalWait)
            continue;
        Memory::Write32(p + kParticleEval, kEvalDone);
        Particle& entry = step.particles[p];
        const bool known = entry.seed == Memory::Read16(p + kParticleSeed) &&
                           entry.tick == Memory::Read16(p + kParticleTick) && entry.tick != 0;
        const bool catchUp = known && entry.generation == step.generation;
        work.push_back({p, &entry, catchUp ? 0.0 : step.particleStep, known, catchUp, {}});
    }

    // Advances a particle's clock; true when its next real tick is due now.
    const auto due = [](Work& w) {
        if (!w.known || w.catchUp) return true;
        Particle& e = *w.particle;
        const double r = std::min(w.remaining, 1.0 - e.t);
        e.t = float(e.t + r);
        w.remaining -= r;
        return e.t >= 1.0f && w.remaining > 0;
    };

    bool ran = false;
    std::vector<size_t> pending(work.size());
    for (size_t i = 0; i < work.size(); ++i) pending[i] = i;
    for (int round = 0; round < 16 && !pending.empty(); ++round) {
        std::vector<size_t> ticking;
        for (size_t i : pending) {
            Work& w = work[i];
            if (due(w)) {
                ticking.push_back(i);
            } else {
                WriteState(w.address, Between(w.particle->before, w.particle->after, w.particle->t));
            }
        }
        if (ticking.empty()) break;
        for (size_t i : ticking) {
            Work& w = work[i];
            if (w.known) WriteState(w.address, w.particle->after);
            w.pre = ReadState(w.address);
            Memory::Write32(w.address + kParticleEval, kEvalWait);
        }
        Memory::Write32(manager + kManagerLastCalced, lastCalced);
        *ctx = saved;
        func_800AC35C(ctx);
        ran = true;
        pending.clear();
        for (size_t i : ticking) {
            Work& w = work[i];
            if (Memory::Read32(w.address + kLifeStatus) != kLifeActive) { // retired this tick
                step.particles.erase(w.address);
                continue;
            }
            Particle& e = *w.particle;
            e.before = w.pre;
            e.after = ReadState(w.address);
            e.seed = Memory::Read16(w.address + kParticleSeed);
            e.tick = Memory::Read16(w.address + kParticleTick);
            e.generation = step.generation;
            e.t = w.catchUp ? 1.0f : 0.0f; // a catch-up tick is shown whole
            w.known = true;
            w.catchUp = false;
            pending.push_back(i);
        }
    }
    if (!ran) Memory::Write32(manager + kManagerLastCalced, list.Last());
    *ctx = saved;
}
extern "C" void func_800AC35C(CpuContext* ctx);
extern "C" void MH3_ParticleManagerCalc(CpuContext* ctx) { PortCheck::Call(ctx, "ParticleManagerCalc", func_800AC35C, ParticleManagerCalcPort); }
REGISTER_NATIVE_FUNCTION_AS(0x800AC35C, MH3_ParticleManagerCalc, "ParticleManagerCalc");

