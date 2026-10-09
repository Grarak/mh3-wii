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
//      keyframed values of the age: where a value runs linearly through its
//      tick it is evaluated at the particle's fractional age, from the
//      curve's values at the ticks on either side; a value that jumps at the
//      tick (a random pick per tick, a key's corner) and the texture offsets
//      (a flipbook's frames) hold until the next tick, as on the Wii
//      (blending them made fire churn at every pass);
//      the age crosses whole ticks for the tick's events: child emission
//      (a curve track) and retirement at the end of life only happen then.
//    At dt = 1 (the stock 1/30 s step) the call is the original.
//
// Offsets are MH3's nw4r ef (the same layout as Wii Sports' decompiled ef).
// Effects, particles and their resources live in main RAM: accessed flat.
#include "abi_bridge.h"
#include "game_step.h"
#include "guest_call.h"
#include "port_check.h"
#include "memory.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <array>
#include <cmath>
#include <cstring>
#include <vector>

extern "C" void func_800A4C20(CpuContext*);  // nw4r::ef::Effect::Calc(bool onlyBillboard)
extern "C" void func_800A8AC0(CpuContext*);  // nw4r::ef::Emitter::CalcEmitter()
extern "C" void func_800A8E5C(CpuContext*);  // nw4r::ef::Emitter::CalcEmission()
extern "C" void func_800AC35C(CpuContext*);  // nw4r::ef::ParticleManager::Calc()
void ParticleManagerCalcNative(CpuContext*); // its port (particle_manager_calc.cpp)
void EffectCalcNative(CpuContext*);          // Effect::Calc's port (effect_calc.cpp)
// Native ports (ef_anim_curve.cpp, nw4r_accessors.cpp), called directly.
extern "C" void MH3_ResEmitterNumPtclTrack(CpuContext*);
extern "C" void MH3_ResEmitterGetPtclTrack(CpuContext*);
extern "C" void MH3_ResEmitterNumPtclInitTrack(CpuContext*);
extern "C" void MH3_AnimCurveExecuteU8(CpuContext*);
extern "C" void MH3_AnimCurveExecuteF32(CpuContext*);
extern "C" void MH3_AnimCurveExecuteRotate(CpuContext*);

#ifdef MH3_DEVELOPMENT
void MH3FrameSplitCall(unsigned kind); // game_frame.cpp
#endif
namespace {

// ut::List: head, tail, u16 count, u16 offset of the link {prev, next} in each object.
struct List {
    uint32_t address;
    uint32_t First() const { return MemoryInline::FlatRead32(address); }
    uint32_t Last() const { return MemoryInline::FlatRead32(address + 4); }
    uint32_t Next(uint32_t object) const { return MemoryInline::FlatRead32(object + MemoryInline::FlatRead16(address + 0xA) + 4); }
};

enum : uint32_t {
    kLifeActive = 1
};
enum : uint32_t {
    kEvalWait = 0
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
    kParticleTexTranslate = 0x70,  // VEC2[3]
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
enum : uint8_t {
    kTrackU8 = 0,
    kTrackF32 = 3,
    kTrackRotate = 6
};
enum : uint8_t {
    kTrackSkip = 0x08,
    kTrackEmitterTick = 0x10
};

using Vec3 = std::array<float, 3>;
Vec3 ReadVec(uint32_t a) {
    return {MemoryInline::FlatReadFloat32(a), MemoryInline::FlatReadFloat32(a + 4), MemoryInline::FlatReadFloat32(a + 8)};
}
void WriteVec(uint32_t a, const Vec3& v) {
    for (int k = 0; k < 3; ++k)
        MemoryInline::FlatWriteFloat32(a + 4 * k, v[k]);
}

constexpr size_t kCurveWords = (kParticleCurvesEnd - kParticleCurves) / 4;
using Curves = std::array<uint32_t, kCurveWords>;
void ReadCurves(uint32_t particle, Curves& c) {
    for (size_t i = 0; i < kCurveWords; ++i)
        c[i] = MemoryInline::FlatRead32(particle + kParticleCurves + 4 * i);
}
Curves ReadCurves(uint32_t particle) {
    Curves c;
    ReadCurves(particle, c);
    return c;
}
void WriteCurves(uint32_t particle, const Curves& c) {
    for (size_t i = 0; i < kCurveWords; ++i)
        MemoryInline::FlatWrite32(particle + kParticleCurves + 4 * i, c[i]);
}
float F(uint32_t bits) {
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}
uint32_t Bits(float f) {
    uint32_t b;
    std::memcpy(&b, &f, 4);
    return b;
}

struct Particle {
    uint16_t seed = 0;       // identity: a reused slot or a stock tick invalidates the entry
    double age = 0;          // ticks, fractional
    uint64_t generation = 0; // Effect::Calc pass that last advanced it
};

// Guest address -> V, open addressing with linear probing: a multiplicative
// hash (no division, which the Cortex-A9 does in software) and the entries
// inline, where std::unordered_map chased a node per lookup.
template <class V>
class AddressMap {
  public:
    AddressMap() { Rehash(64); }
    bool Empty() const { return count_ == 0; }
    V* Find(uint32_t key) {
        for (uint32_t i = Home(key);; i = (i + 1) & mask_) {
            if (slots_[i].key == key)
                return &slots_[i].value;
            if (slots_[i].key == 0)
                return nullptr;
        }
    }
    V& operator[](uint32_t key) {
        if (V* v = Find(key))
            return *v;
        if (2 * (count_ + 1) > slots_.size())
            Rehash(uint32_t(slots_.size()) * 2);
        uint32_t i = Home(key);
        while (slots_[i].key != 0)
            i = (i + 1) & mask_;
        ++count_;
        slots_[i] = {key, V{}};
        return slots_[i].value;
    }
    void Erase(uint32_t key) {
        uint32_t i = Home(key);
        while (slots_[i].key != key) {
            if (slots_[i].key == 0)
                return;
            i = (i + 1) & mask_;
        }
        // Backward-shift deletion: pull later entries of the run into the hole.
        for (uint32_t j = (i + 1) & mask_; slots_[j].key != 0; j = (j + 1) & mask_) {
            const uint32_t home = Home(slots_[j].key);
            if (((j - home) & mask_) >= ((j - i) & mask_)) {
                slots_[i] = slots_[j];
                i = j;
            }
        }
        slots_[i].key = 0;
        --count_;
    }

  private:
    struct Slot {
        uint32_t key; // 0: empty
        V value;
    };
    uint32_t Home(uint32_t key) const { return ((key >> 2) * 0x9E3779B1u) >> shift_; }
    void Rehash(uint32_t size) {
        std::vector<Slot> old = std::move(slots_);
        slots_.assign(size, Slot{0, V{}});
        mask_ = size - 1;
        shift_ = 32 - uint32_t(__builtin_ctz(size));
        count_ = 0;
        for (const Slot& s : old)
            if (s.key != 0)
                (*this)[s.key] = s.value;
    }
    std::vector<Slot> slots_;
    uint32_t mask_ = 0, shift_ = 0, count_ = 0;
};

struct Step {
    AddressMap<Particle> particles; // those between ticks
    uint64_t generation = 0;
    double particleStep = 1.0; // ticks particles advance in this Effect::Calc pass
    bool particlesOnly = false;
};
Step step;

// Particles whose age crosses no whole tick in the running ParticleManager::Calc.
AddressMap<bool> betweenTicks;

// Runs the particle's curve tracks at `tick`, as ParticleManager::Calc does
// (without the child and texture tracks, the events, and the emitter-age ones).
void RunCurves(GuestCall& game, uint32_t manager, uint32_t particle, uint32_t tick) {
    // The track accessors and curve executors are native ports reading only
    // their argument registers: call them without the indirect dispatch.
    auto native = [&](void (*fn)(CpuContext*), std::initializer_list<uint32_t> ints) {
        uint32_t r = 3;
        for (uint32_t v : ints)
            game.cpu->gpr[r++] = v;
        fn(game.cpu);
        return game.cpu->gpr[3];
    };
    const uint32_t res = MemoryInline::FlatRead32(manager + kManagerResource);
    const uint32_t count = native(MH3_ResEmitterNumPtclTrack, {res}) & 0xFFFF;
    const uint32_t first = tick == 0 ? 0 : native(MH3_ResEmitterNumPtclInitTrack, {res}) & 0xFFFF;
    const uint32_t seed = MemoryInline::FlatRead16(particle + kParticleSeed);
    const uint32_t life = MemoryInline::FlatRead16(particle + kParticleLife);
    for (uint32_t i = first; i < count; ++i) {
        const uint32_t track = native(MH3_ResEmitterGetPtclTrack, {res, i});
        const uint8_t magic = MemoryInline::FlatRead8(track), target = MemoryInline::FlatRead8(track + 1);
        const uint8_t kind = MemoryInline::FlatRead8(track + 2), flags = MemoryInline::FlatRead8(track + 4);
        if ((flags & (kTrackSkip | kTrackEmitterTick)) != 0 || uint8_t(magic + 0x55) >= 2)
            continue;
        if (kind == kTrackU8)
            native(MH3_AnimCurveExecuteU8, {track, particle + kParticleCurves + target, tick, seed, life});
        else if (kind == kTrackF32)
            native(MH3_AnimCurveExecuteF32, {track, particle + kParticleCurves + target, tick, seed, life});
        else if (kind == kTrackRotate)
            native(MH3_AnimCurveExecuteRotate, {track, particle + kParticleRotate, tick, seed, life});
    }
}

// Which bytes of the curve block (kParticleCurves..kParticleCurvesEnd) the
// particle's curve tracks write - those RunCurves runs past the initial ones.
// Every executor writes one element (a byte for u8, a word for f32 and
// rotation) per set bit of the track's channel mask, from its target; no
// other state decides it. `inside` is false when a track writes past the block.
struct CurveFootprint {
    std::array<uint32_t, 4> bits{}; // one per byte of the 128-byte block
    bool inside = true;
    bool Has(size_t byte) const { return bits[byte / 32] >> (byte % 32) & 1; }
};
CurveFootprint FootprintOf(GuestCall& game, uint32_t manager) {
    auto native = [&](void (*fn)(CpuContext*), std::initializer_list<uint32_t> ints) {
        uint32_t r = 3;
        for (uint32_t v : ints)
            game.cpu->gpr[r++] = v;
        fn(game.cpu);
        return game.cpu->gpr[3];
    };
    constexpr uint32_t kChannelCount = 0x80592428, kChannelBound = 0x80592528; // u8[256] tables
    CurveFootprint f;
    const uint32_t res = MemoryInline::FlatRead32(manager + kManagerResource);
    const uint32_t count = native(MH3_ResEmitterNumPtclTrack, {res}) & 0xFFFF;
    const uint32_t first = native(MH3_ResEmitterNumPtclInitTrack, {res}) & 0xFFFF;
    for (uint32_t i = first; i < count; ++i) {
        const uint32_t track = native(MH3_ResEmitterGetPtclTrack, {res, i});
        const uint8_t magic = MemoryInline::FlatRead8(track), target = MemoryInline::FlatRead8(track + 1);
        const uint8_t kind = MemoryInline::FlatRead8(track + 2), mask = MemoryInline::FlatRead8(track + 3);
        const uint8_t flags = MemoryInline::FlatRead8(track + 4);
        if ((flags & (kTrackSkip | kTrackEmitterTick)) != 0 || uint8_t(magic + 0x55) >= 2)
            continue;
        uint32_t base, width;
        if (kind == kTrackU8)
            base = target, width = 1;
        else if (kind == kTrackF32)
            base = target, width = 4;
        else if (kind == kTrackRotate)
            base = kParticleRotate - kParticleCurves, width = 4;
        else
            continue;
        if (MemoryInline::FlatRead8(kChannelCount + mask) == 0)
            continue;
        const uint32_t bound = MemoryInline::FlatRead8(kChannelBound + mask);
        uint32_t at = base;
        for (uint32_t bit = 1; bit <= bound; bit = (bit << 1) & 0xFFFF, at += width) {
            if ((mask & bit) == 0)
                continue;
            if (at + width > kParticleCurvesEnd - kParticleCurves) {
                f.inside = false;
                return f;
            }
            for (uint32_t b = at; b < at + width; ++b)
                f.bits[b / 32] |= 1u << (b % 32);
        }
    }
    return f;
}

// The curves shown at a fractional tick: a value that changes by the same
// amount into its tick as out of it (a keyframed ramp) is blended between the
// ticks either side, `a` (the tick's) and `b` (the next); one that jumps at
// the tick holds the tick's value, as the Wii shows it for the whole tick.
// `before` is the previous tick's. Bytes outside `owned` keep `kept`.
Curves BlendCurves(const Curves& kept, const Curves& before, const Curves& a, const Curves& b, float t, const CurveFootprint* owned) {
    // Into and out of the tick: the same step (within a tenth, or the
    // rounding of a u8) is a ramp.
    auto ramp = [](double in, double out, double slack) {
        return std::fabs(out - in) <= 0.1 * std::max(std::fabs(in), std::fabs(out)) + slack;
    };
    Curves s = kept;
    constexpr size_t colours = (kParticleColours - kParticleCurves) / 4;
    constexpr size_t floats = (kParticleFloats - kParticleCurves) / 4;
    constexpr size_t rotate = (kParticleRotate - kParticleCurves) / 4;
    constexpr size_t translate = (kParticleTexTranslate - kParticleCurves) / 4;
    for (size_t i = 0; i < colours; ++i) {
        uint32_t v = 0;
        for (int shift = 24; shift >= 0; shift -= 8) {
            // Guest bytes are big-endian: the top byte is the word's first.
            if (owned && !owned->Has(i * 4 + (24 - shift) / 8)) {
                v |= kept[i] & 0xFFu << shift;
                continue;
            }
            const float w = float(before[i] >> shift & 0xFF);
            const float x = float(a[i] >> shift & 0xFF), y = float(b[i] >> shift & 0xFF);
            const float shown = ramp(x - w, y - x, 1.0) ? x + (y - x) * t : x;
            v |= uint32_t(shown + 0.5f) << shift; // shown >= 0: lround without the libm call
        }
        s[i] = v;
    }
    for (size_t i = colours; i < floats; ++i) {
        if (owned && !owned->Has(i * 4))
            continue;
        const float w = F(before[i]), x = F(a[i]), y = F(b[i]);
        // A rotation curve wrapping between the two ticks snaps. Texture
        // offsets step through a flipbook's frames even when evenly: never
        // blended.
        const bool wraps = i >= rotate && i < rotate + 3 && std::fabs(y - x) > 3.1415927f;
        const bool blend = i < translate && std::isfinite(w) && std::isfinite(x) && std::isfinite(y) && !wraps && ramp(double(x) - w, double(y) - x, 1e-6);
        s[i] = blend ? Bits(x + (y - x) * t) : a[i];
    }
    return s;
}

// Shows the curves at age `at` (a fraction past a whole tick) from three
// fresh evaluations.
void CurvesAtFull(GuestCall& game, uint32_t manager, uint32_t particle, double at) {
    const uint32_t tick = uint32_t(at);
    const Curves kept = ReadCurves(particle);
    Curves before = kept;
    if (tick > 0) {
        RunCurves(game, manager, particle, tick - 1);
        before = ReadCurves(particle);
        WriteCurves(particle, kept);
    }
    RunCurves(game, manager, particle, tick);
    const Curves a = ReadCurves(particle);
    RunCurves(game, manager, particle, tick + 1);
    const Curves b = ReadCurves(particle);
    WriteCurves(particle, BlendCurves(kept, before, a, b, float(at - tick), nullptr));
}

// A particle's recent curve evaluations. The values a track writes depend
// only on the track, the tick, the particle's seed and its life, so they are
// reused while those match: consecutive passes need overlapping ticks.
struct CurveCache {
    uint32_t res = 0;
    uint16_t seed = 0, life = 0;
    CurveFootprint owned;
    std::array<uint32_t, 3> ticks{~0u, ~0u, ~0u};
    std::array<Curves, 3> values;
};
AddressMap<CurveCache> curveCache;

// CurvesAtFull with the evaluations reused: ParticleManager::Calc just left
// the curves at `keptTick` (the tick the particle's age started the pass in)
// in the block, and the cache holds earlier passes' ticks. Only the bytes the
// tracks own are taken from evaluations; the rest keep the block's.
void CurvesAt(GuestCall& game, uint32_t manager, uint32_t particle, double at, uint32_t keptTick) {
    const uint32_t tick = uint32_t(at);
    // The first tick runs the initial tracks too: rare, and done in full.
    if (tick == 0)
        return CurvesAtFull(game, manager, particle, at);
    const uint32_t res = MemoryInline::FlatRead32(manager + kManagerResource);
    const uint16_t seed = MemoryInline::FlatRead16(particle + kParticleSeed), life = MemoryInline::FlatRead16(particle + kParticleLife);
    CurveCache& cache = curveCache[particle];
    if (cache.res != res || cache.seed != seed || cache.life != life) {
        cache.res = res;
        cache.seed = seed;
        cache.life = life;
        cache.owned = FootprintOf(game, manager);
        cache.ticks = {~0u, ~0u, ~0u};
    }
    if (!cache.owned.inside)
        return CurvesAtFull(game, manager, particle, at);

    // The three ticks' curves live in the cache's slots (128 bytes each: no
    // copies in and out). A tick already cached keeps its slot; the others
    // take the slots left over.
    const Curves kept = ReadCurves(particle);
    std::array<int, 3> slotOf{-1, -1, -1};
    bool used[3] = {};
    for (uint32_t k = 0; k < 3; ++k)
        for (int s = 0; s < 3; ++s)
            if (!used[s] && cache.ticks[s] == tick - 1 + k) {
                slotOf[k] = s, used[s] = true;
                break;
            }
    for (uint32_t k = 0; k < 3; ++k) {
        const uint32_t want = tick - 1 + k;
        int s = slotOf[k];
        if (s < 0) {
            s = int(std::find(used, used + 3, false) - used);
            slotOf[k] = s, used[s] = true;
            cache.ticks[s] = want;
            if (want != keptTick) {
                RunCurves(game, manager, particle, want);
                ReadCurves(particle, cache.values[s]);
                continue;
            }
        }
        if (want == keptTick)
            cache.values[s] = kept;
    }
    const std::array<const Curves*, 3> got{&cache.values[slotOf[0]], &cache.values[slotOf[1]], &cache.values[slotOf[2]]};
    WriteCurves(particle, BlendCurves(kept, *got[0], *got[1], *got[2], float(at - tick), &cache.owned));
}

} // namespace

namespace EffectStep {
std::any Save() {
    return step;
}
void Restore(const std::any& state) {
    step = std::any_cast<const Step&>(state);
}
} // namespace EffectStep

static void EffectCalcPort(CpuContext* ctx) {
    const uint32_t effect = ctx->gpr[3];
    const bool onlyBillboard = (ctx->gpr[4] & 0xFF) != 0;
    const double scale = GameStep::Scale();
    const uint32_t ticks = GameStep::Ticks();
    if (onlyBillboard) {
        EffectCalcNative(ctx);
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
            for (size_t i = 0; i < 6; ++i)
                words[i] = MemoryInline::FlatRead32(e + kEmitterLastTranslate + i * 4);
            kept.emplace_back(e, words);
        }
        step.particleStep = scale;
        step.particlesOnly = true;
        ++step.generation;
        EffectCalcNative(ctx);
        step.particlesOnly = false;
        for (const auto& [e, words] : kept)
            for (size_t i = 0; i < 6; ++i)
                MemoryInline::FlatWrite32(e + kEmitterLastTranslate + i * 4, words[i]);
    } else {
        step.particleStep = scale / ticks;
        for (uint32_t i = 0; i < ticks; ++i) {
            *ctx = saved;
            ++step.generation;
            EffectCalcNative(ctx);
        }
    }
    step.particleStep = 1.0;
    *ctx = saved;
}
extern "C" void func_800A4C20(CpuContext* ctx);
extern "C" void MH3_EffectCalc(CpuContext* ctx) {
#ifdef MH3_DEVELOPMENT
    MH3FrameSplitCall(2);
#endif
    PortCheck::Call(ctx, "EffectCalc", func_800A4C20, EffectCalcPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x800A4C20, MH3_EffectCalc, "EffectCalc");

static void EmitterCalcEmitterPort(CpuContext* ctx) {
    if (!step.particlesOnly)
        func_800A8AC0(ctx);
}
extern "C" void func_800A8AC0(CpuContext* ctx);
extern "C" void MH3_EmitterCalcEmitter(CpuContext* ctx) {
    PortCheck::Call(ctx, "EmitterCalcEmitter", func_800A8AC0, EmitterCalcEmitterPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x800A8AC0, MH3_EmitterCalcEmitter, "EmitterCalcEmitter");

static void EmitterCalcEmissionPort(CpuContext* ctx) {
    if (!step.particlesOnly)
        func_800A8E5C(ctx);
}
extern "C" void func_800A8E5C(CpuContext* ctx);
extern "C" void MH3_EmitterCalcEmission(CpuContext* ctx) {
    PortCheck::Call(ctx, "EmitterCalcEmission", func_800A8E5C, EmitterCalcEmissionPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x800A8E5C, MH3_EmitterCalcEmission, "EmitterCalcEmission");

static void ParticleManagerCalcPort(CpuContext* ctx) {
    // Whole ticks with every particle on a whole tick (the stock 1/30 s step)
    // are the original.
    if (step.particleStep == 1.0 && step.particles.Empty()) {
        ParticleManagerCalcNative(ctx);
        return;
    }
    // One frame for the whole port: Calc runs inside it, and leaving it
    // restores the caller's registers.
    GuestFrame frame(ctx, 0x20);
    const uint32_t manager = frame.Arg(3);
    const List list{manager + kManagerParticles};
    const uint32_t lastCalced = MemoryInline::FlatRead32(manager + kManagerLastCalced);

    struct Work {
        uint32_t address;
        double age, dt;
        float momentum;
        Vec3 velocity, position;
    };
    // Calc does not nest: one list serves every call.
    static std::vector<Work> work;
    work.clear();
    for (uint32_t p = lastCalced ? list.Next(lastCalced) : list.First(); p; p = list.Next(p)) {
        if (MemoryInline::FlatRead32(p + kLifeStatus) != kLifeActive || MemoryInline::FlatRead32(p + kParticleEval) != kEvalWait)
            continue;
        const uint16_t tick = MemoryInline::FlatRead16(p + kParticleTick);
        double age = tick, dt = step.particleStep;
        if (const Particle* found = step.particles.Find(p)) {
            const Particle& e = *found;
            if (e.seed == MemoryInline::FlatRead16(p + kParticleSeed) && uint32_t(e.age) == tick) {
                age = e.age;
                // Calc again in the same pass (calc remain: a particle emitted
                // with ticks to catch up) is a whole tick.
                if (e.generation == step.generation)
                    dt = 1.0;
            }
        }
        Work w{p, age, dt, MemoryInline::FlatReadFloat32(p + kParticleMomentum), ReadVec(p + kParticleVelocity), ReadVec(p + kParticlePosition)};
        if (dt != 1.0)
            MemoryInline::FlatWriteFloat32(p + kParticleMomentum, float(w.momentum * dt));
        if (std::floor(age + dt) == std::floor(age))
            betweenTicks[p] = true;
        work.push_back(w);
    }
    ParticleManagerCalcNative(ctx);
    for (const Work& w : work)
        betweenTicks.Erase(w.address);

    GuestCall game{ctx};
    for (const Work& w : work) {
        const uint32_t p = w.address;
        if (MemoryInline::FlatRead32(p + kLifeStatus) != kLifeActive) { // retired
            step.particles.Erase(p);
            curveCache.Erase(p);
            continue;
        }
        if (w.dt != 1.0) {
            MemoryInline::FlatWriteFloat32(p + kParticleMomentum, w.momentum);
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
        MemoryInline::FlatWrite16(p + kParticleTick, uint16_t(age));
        if (age == std::floor(age)) {
            step.particles.Erase(p);
            continue;
        }
        step.particles[p] = {MemoryInline::FlatRead16(p + kParticleSeed), age, step.generation};
        // After the tick from age k the Wii shows the curves at k: the curves
        // run one tick behind the age, here at age - 1.
        if (age <= 1.0)
            continue;
        CurvesAt(game, manager, p, age - 1.0, uint32_t(w.age));
    }
}
extern "C" void func_800AC35C(CpuContext* ctx);
extern "C" void MH3_ParticleManagerCalc(CpuContext* ctx) {
    PortCheck::Call(ctx, "ParticleManagerCalc", func_800AC35C, ParticleManagerCalcPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x800AC35C, MH3_ParticleManagerCalc, "ParticleManagerCalc");

// Child emission is a curve event: only when the particle's age crosses a tick.
extern "C" void func_800A1D0C(CpuContext* ctx);
static void AnimCurveExecuteChildPort(CpuContext* ctx) {
    if (!betweenTicks.Find(ctx->gpr[4]))
        func_800A1D0C(ctx);
}
extern "C" void MH3_AnimCurveExecuteChild(CpuContext* ctx) {
    PortCheck::Call(ctx, "AnimCurveExecuteChild", func_800A1D0C, AnimCurveExecuteChildPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x800A1D0C, MH3_AnimCurveExecuteChild, "AnimCurveExecuteChild");
