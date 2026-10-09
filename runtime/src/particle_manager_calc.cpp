// nw4r::ef::ParticleManager::Calc (0x800AC35C), native: one tick of each of a
// manager's particles after the last one calced (0x54): curve tracks, fields,
// the post-field (collision) step, then position += (field push + velocity) *
// momentum and age + 1. effect_step.cpp's port wraps it for variable steps.
//
// The port keeps the original's 0x430-byte guest frame and its locals at the
// same offsets, so the guest functions it still calls (matrix setup, texture
// and child tracks, field kinds, the post-field step, retirement) see the same
// buffers. What the translated code paid for per track - the resource's track
// count and table lookups, a register flush around every call - is done once
// per call; the curve executors are called as the native ports they are, and
// the vector arithmetic runs on host floats with the translated code's rounding.
//
// Offsets are nw4r ef's (MH3's matches Wii Sports' decompiled ef):
//   ParticleManager: 0x20 Emitter*, 0x24 ResEmitter, 0x38 ut::List of Particle
//     (0x42: the link offset), 0x54 the last particle calced.
//   Emitter: 0xBC Effect*, 0x114 VEC3 (field origin, NaN x when unset),
//     0x20 flags, 0x3C life, 0xE4 tick, 0xEA seed (emitter-age tracks).
//   Effect: 0x4C/0x50 the before/after Calc callbacks, 0x54 flags.
//   Particle: 0x0C life status, 0x20.. curve targets, 0x40 rotation, 0x96
//     texture flags, 0xA0 velocity, 0xAC position, 0xB8 previous position,
//     0xC4 momentum, 0xC8 ParticleManager*, 0xD8 evaluated, 0xDC tick, 0xDE
//     seed, 0xE0 life, 0xE2 pending child count.
#include "abi_bridge.h"
#include "guest_call.h"
#include "memory.h"
#include "ppc_runtime.h"

#include <cstdint>
#include <initializer_list>
#include <vector>

extern "C" void MH3_ResEmitterNumPtclTrack(CpuContext*);
extern "C" void MH3_ResEmitterGetPtclTrack(CpuContext*);
extern "C" void MH3_ResEmitterNumPtclInitTrack(CpuContext*);
extern "C" void MH3_AnimCurveExecuteU8(CpuContext*);
extern "C" void MH3_AnimCurveExecuteF32(CpuContext*);
extern "C" void MH3_AnimCurveExecuteRotate(CpuContext*);

namespace {

using MemoryInline::FlatRead16;
using MemoryInline::FlatRead32;
using MemoryInline::FlatRead8;
using MemoryInline::FlatReadFloat32;
using MemoryInline::FlatWrite16;
using MemoryInline::FlatWrite32;
using MemoryInline::FlatWrite8;
using MemoryInline::FlatWriteFloat32;

// Guest functions the port still calls.
enum : uint32_t {
    kPanic = 0x80501130,              // nw4r::db::Panic(file, line, message, ...)
    kManagerLocalMtx = 0x800AE3C4,    // (manager, MTX34* out)
    kMtxInverse = 0x80088428,         // (MTX34* out, MTX34* in)
    kEmitterGlobalMtx = 0x800A9508,   // (emitter, MTX34* out)
    kVecMag = 0x80050F88,             // (VEC3*) -> f1
    kMtxConcat = 0x80071120,          // (out, a, b): out = a * b
    kMtxMultVec = 0x80051560,         // (VEC3* out, MTX34*, VEC3*)
    kVecScale = 0x80051454,           // (VEC3* in/out, f1)
    kVecSubtract = 0x80050D58,        // (out, a, b): out = a - b
    kMtxIdentity = 0x80050538,        // (MTX34*)
    kMtxRotate = 0x8009CC84,          // (out, VEC3* angles, in)
    kMtxScale = 0x8009CA94,           // (MTX34* out, f1, f2, f3)
    kMtxTranslate = 0x8005AB6C,       // (out, in, VEC3*)
    kFmod = 0x80464590,               // (f1, f2) -> f1
    kResEmitterData = 0x800A89A8,     // (ResEmitter*) -> emit data
    kRetireParticle = 0x800AB8E4,     // (manager, particle)
    kExecuteTexture = 0x800A0D68,     // (track, particle, tick, seed, life, out, out, out)
    kTextureFlags = 0x800A1508,       // (particle, texture result)
    kTextureRotate = 0x800A1568,      // (particle, result, data, manager, a, b)
    kExecuteChild = 0x800A1D0C,       // (track, particle, tick, seed, life)
    kFieldOrigin = 0x800ADAC0,        // (out, emitterSpace, mtx, mtx, mtx, VEC3*)
    kPostField = 0x800B1CE4,          // (particle, params, header, collision, ...)
};
// The field kinds' guest functions, by kind (the field track's target); 5
// has none, 8 is inline.
constexpr uint32_t kFieldKind[8] = {0x800ADA30, 0x800ADA88, 0x800AD484, 0x800AD3E8,
                                    0x800AD2B8, 0,          0x800AD4E0, 0x800AD584};

// Panic messages (ef_particlemanager.cpp).
constexpr uint32_t kFile = 0x80593738, kBadManagerEmitter = 0x80593830, kBadEmitterEffect = 0x8059386C,
                   kAssertTexture = 0x80593970, kAssertRotate = 0x80593994, kAssertPostField = 0x805939C4;

// Frame offsets of the original's locals.
enum : uint32_t {
    kArgA = 0x08, kArgB = 0x0C,     // the stack arguments of 8+-argument calls
    kRetire = 0x10,                 // u8: the post-field step retired the particle
    kTexOutA = 0x14, kTexOutB = 0x18, kResSlot = 0x20,
    kPostPush = 0x24,               // VEC3 copy of kPush for the post-field step
    kFieldOriginOut = 0x30,
    kWrapLocal = 0x3C,              // VEC3
    kEmitterTranslate = 0x48,       // VEC3
    kPostVelocity = 0x54,           // VEC3
    kEmitterPosition = 0x60,        // VEC3
    kFieldForce = 0x6C,             // VEC3
    kFieldOriginCopy = 0x78,        // VEC3
    kTexResult = 0x88,
    kPush = 0x98,                   // VEC3: added to the position
    kAccel = 0xA4,                  // VEC3: added to the velocity
    kMovement = 0xB0,               // VEC3: position - previous position
    kVelocity = 0xBC,               // VEC3 copy
    kPosition = 0xC8,               // VEC3 copy
    kMagVec = 0xD4,
    kFieldParams = 0xE0,            // 0x1C bytes: u8 space, u8 target, .., f32 at +4
    kWrapInverse = 0x100, kWrapScale = 0x130, kWrapMtx = 0x160,
    kEmitterMtx = 0x190,
    kPostParams = 0x1C0,            // 0x24 bytes
    kLocalRotInv = 0x1E8, kWorldRotInv = 0x218, kLocalToWorldRot = 0x248, kWorldToLocalRot = 0x278,
    kWorldToEmitterInv = 0x2A8, kWorldToEmitter = 0x2D8, kGlobal = 0x308, kWorldToLocal = 0x338,
    kLocal = 0x368,
    kNext = 0x398, kFirst = 0x39C,
    kFrameSize = 0x430,
};

bool ValidPointer(uint32_t p) {
    return (p & 0xFF000000u) == 0x80000000u || (p & 0xFF800000u) == 0x81000000u ||
           (p & 0xF8000000u) == 0x90000000u || (p & 0xFF000000u) == 0xC0000000u ||
           (p & 0xFF800000u) == 0xC1000000u || (p & 0xF8000000u) == 0xD0000000u ||
           (p & 0xFFFFC000u) == 0xE0000000u;
}

// Single-precision add/subtract as the translated fadds/fsubs/ps_add round them.
float Add(double a, double b) { return PpcForceSingleValueInline(a + b); }
float Sub(double a, double b) { return PpcForceSingleValueInline(a - b); }

void Copy3(uint32_t to, uint32_t from) {
    for (uint32_t k = 0; k < 12; k += 4) FlatWrite32(to + k, FlatRead32(from + k));
}
void Zero3(uint32_t to, float zero) {
    for (uint32_t k = 0; k < 12; k += 4) FlatWriteFloat32(to + k, zero);
}
// a += b (PSVECAdd).
void AddTo(uint32_t a, uint32_t b) {
    for (uint32_t k = 0; k < 12; k += 4) FlatWriteFloat32(a + k, Add(FlatReadFloat32(a + k), FlatReadFloat32(b + k)));
}
// ParticleAddPosition (0x800AD130): position += v * momentum.
void AddPosition(uint32_t particle, uint32_t v) {
    const double momentum = FlatReadFloat32(particle + 0xC4);
    for (uint32_t k = 0; k < 12; k += 4) {
        const double moved = PpcFmulsInline(FlatReadFloat32(v + k), momentum);
        FlatWriteFloat32(particle + 0xAC + k, Add(FlatReadFloat32(particle + 0xAC + k), moved));
    }
}
// slw: shift counts 32..63 give 0.
uint32_t Slw(uint32_t v, uint32_t n) { return (n & 32) ? 0 : v << (n & 31); }

} // namespace

void ParticleManagerCalcNative(CpuContext* ctx) {
    GuestFrame frame(ctx, kFrameSize);
    const uint32_t sp = frame[0];
    const uint32_t manager = frame.Arg(3), r2 = frame.Arg(2);
    GuestCall game{ctx};
    auto native = [&](void (*fn)(CpuContext*), std::initializer_list<uint32_t> ints) {
        uint32_t r = 3;
        for (uint32_t v : ints) ctx->gpr[r++] = v;
        fn(ctx);
        return ctx->gpr[3];
    };
    auto panic = [&](uint32_t line, uint32_t message) { game(kPanic, {kFile, line, message}); };

    const uint32_t list = manager + 0x38;
    const uint32_t lastCalced = FlatRead32(manager + 0x54);
    const uint32_t first = lastCalced ? FlatRead32(lastCalced + FlatRead16(list + 0xA) + 4) : FlatRead32(list);
    FlatWrite32(sp + kFirst, first);
    if (!first) return;

    if (!ValidPointer(FlatRead32(manager + 0x20))) panic(0xE1, kBadManagerEmitter);
    if (!ValidPointer(FlatRead32(FlatRead32(manager + 0x20) + 0xBC))) panic(0xE2, kBadEmitterEffect);
    if (const uint32_t before = FlatRead32(FlatRead32(FlatRead32(manager + 0x20) + 0xBC) + 0x4C))
        game(before, {manager, list, first});

    // The manager's frames: local (its emitter's space, as the manager
    // computes it), the emitter's global matrix, and those derived from them.
    const float zero = FlatReadFloat32(r2 - 0x79F4);
    game(kManagerLocalMtx, {manager, sp + kLocal});
    game(kMtxInverse, {sp + kWorldToLocal, sp + kLocal});
    game(kEmitterGlobalMtx, {FlatRead32(manager + 0x20), sp + kGlobal});
    for (uint32_t row = 0; row < 3; ++row) {
        const uint32_t m = sp + kGlobal + row * 16;
        FlatWriteFloat32(sp + kMagVec + row * 4,
                         Add(FlatReadFloat32(m + 8), Add(FlatReadFloat32(m), FlatReadFloat32(m + 4))));
    }
    game(kVecMag, {sp + kMagVec});
    const float scaleMag =
        static_cast<float>(PpcFmulsInline(FlatReadFloat32(r2 - 0x79F0), ctx->fpr[1].d)); // * 1/sqrt(3)
    game(kMtxConcat, {sp + kWorldToEmitter, sp + kWorldToLocal, sp + kGlobal});
    game(kMtxInverse, {sp + kWorldToEmitterInv, sp + kWorldToEmitter});
    // Rotation-only copies (translation zeroed).
    auto rotationOf = [&](uint32_t to, uint32_t from) {
        for (uint32_t k = 0; k < 48; k += 4) FlatWrite32(to + k, FlatRead32(from + k));
        for (uint32_t row = 0; row < 3; ++row) FlatWriteFloat32(to + row * 16 + 12, zero);
    };
    rotationOf(sp + kWorldToLocalRot, sp + kWorldToLocal);
    game(kMtxInverse, {sp + kLocalToWorldRot, sp + kWorldToLocalRot});
    rotationOf(sp + kWorldRotInv, sp + kWorldToEmitterInv);
    rotationOf(sp + kLocalRotInv, sp + kWorldToEmitter);
    const float unset = FlatReadFloat32(r2 - 0x79EC), one = FlatReadFloat32(r2 - 0x79F8),
                minusOne = FlatReadFloat32(r2 - 0x79E4), period = FlatReadFloat32(r2 - 0x79E8);

    // Per call rather than per particle and track: the resource's tracks and
    // its emit data (the original looks both up again each time).
    const uint32_t res = FlatRead32(manager + 0x24);
    std::vector<uint32_t> tracks;
    tracks.reserve(32);
    uint32_t initTracks = 0, data = 0;
    bool resolved = false;
    auto resolve = [&] {
        resolved = true;
        const uint32_t count = native(MH3_ResEmitterNumPtclTrack, {res}) & 0xFFFF;
        for (uint32_t i = 0; i < count; ++i) tracks.push_back(native(MH3_ResEmitterGetPtclTrack, {res, i}));
        initTracks = native(MH3_ResEmitterNumPtclInitTrack, {res}) & 0xFFFF;
        FlatWrite32(sp + kResSlot, res);
        data = game(kResEmitterData, {sp + kResSlot});
    };

    uint32_t header = frame.Arg(31); // the post-field header: kept across particles, as r31 was
    for (uint32_t p = first; p;) {
        const uint32_t next = FlatRead32(p + FlatRead16(list + 0xA) + 4);
        FlatWrite32(sp + kNext, next);
        if (FlatRead32(p + 0x0C) != 1 || FlatRead32(p + 0xD8) != 0) {
            p = next;
            continue;
        }
        FlatWrite32(p + 0xD8, 1);
        if (FlatRead16(p + 0xE2) != 0) {
            const uint32_t effect = FlatRead32(FlatRead32(manager + 0x20) + 0xBC);
            FlatWrite32(effect + 0x54, FlatRead32(effect + 0x54) | 0x10000);
        }
        Copy3(sp + kPosition, p + 0xAC);
        Copy3(sp + kVelocity, p + 0xA0);
        for (uint32_t k = 0; k < 12; k += 4)
            FlatWriteFloat32(sp + kMovement + k, Sub(FlatReadFloat32(p + 0xAC + k), FlatReadFloat32(p + 0xB8 + k)));
        Copy3(p + 0xB8, p + 0xAC);
        if (!resolved) resolve();
        if ((FlatRead8(data + 3) & 1) == 0 && FlatRead16(p + 0xE0) <= FlatRead16(p + 0xDC)) {
            game(kRetireParticle, {manager, p});
            p = FlatRead32(sp + kNext);
            continue;
        }
        Zero3(sp + kAccel, zero);
        Zero3(sp + kPush, zero);
        bool postField = false;
        uint32_t collision = 0;
        const uint32_t count = uint32_t(tracks.size());
        for (uint32_t i = FlatRead16(p + 0xDC) == 0 ? 0 : initTracks; (i & 0xFFFF) < count; ++i) {
            const uint32_t track = tracks[i & 0xFFFF];
            const uint8_t flags = FlatRead8(track + 4);
            if (flags & 0x08) continue;
            uint32_t tick, life, seed;
            if (flags & 0x10) { // on the emitter's age
                const uint32_t emitter = FlatRead32(FlatRead32(p + 0xC8) + 0x20);
                tick = FlatRead32(emitter + 0xE4);
                life = (FlatRead32(emitter + 0x20) & 4) ? 0xFFFFFFFFu : FlatRead16(emitter + 0x3C);
                seed = FlatRead16(emitter + 0xEA);
            } else {
                tick = FlatRead16(p + 0xDC);
                life = FlatRead16(p + 0xE0);
                seed = FlatRead16(p + 0xDE);
            }
            const uint8_t magic = FlatRead8(track);
            if (uint8_t(magic + 0x55) > 1) continue; // 0xAB, 0xAC
            const uint32_t target = FlatRead8(track + 1);
            switch (FlatRead8(track + 2)) {
            case 0: // u8
                native(MH3_AnimCurveExecuteU8, {track, p + 0x20 + target, tick, seed, life});
                break;
            case 3: // f32
                native(MH3_AnimCurveExecuteF32, {track, p + 0x20 + target, tick, seed, life});
                break;
            case 6: // rotation
                if (target != 0x20) panic(0x19D, kAssertRotate);
                if (FlatRead8(track) != 0xAC) panic(0x19E, kAssertTexture);
                native(MH3_AnimCurveExecuteRotate, {track, p + 0x40, tick, seed, life});
                break;
            case 4: { // texture
                if (magic != 0xAC) panic(0x181, kAssertTexture);
                game(kExecuteTexture, {track, p, tick, seed, life, sp + kTexOutB, sp + kTexOutA, sp + kTexResult});
                if (FlatRead8(data + 2) & 1) {
                    game(kTextureFlags, {p, sp + kTexResult});
                } else {
                    const uint32_t shift = FlatRead32(sp + kTexResult + 4) << 1;
                    FlatWrite8(p + 0x96, FlatRead8(p + 0x96) & (~Slw(3, shift) & 0xFF));
                    FlatWrite8(p + 0x96, FlatRead8(p + 0x96) | (Slw(FlatRead8(sp + kTexResult + 8) & 3, shift) & 0xFF));
                }
                if (FlatRead8(data + 3) & 2)
                    game(kTextureRotate, {p, sp + kTexResult, data, manager, FlatRead32(sp + kTexOutB),
                                          FlatRead32(sp + kTexOutA)});
                break;
            }
            case 5: // child emission
                if (magic != 0xAC) panic(0x198, kAssertTexture);
                game(kExecuteChild, {track, p, tick, seed, life});
                break;
            case 7: { // field
                const uint32_t params = track + FlatRead32(track + 0xC) + FlatRead32(track + 0x10) +
                                        FlatRead32(track + 0x14) + FlatRead32(track + 0x18) + 0x20;
                for (uint32_t k = 0; k < 0x1C; ++k) FlatWrite8(sp + kFieldParams + k, FlatRead8(params + k));
                if (FlatRead32(track + 0xC) != 0)
                    native(MH3_AnimCurveExecuteF32, {track, sp + kFieldParams + 4, tick, seed, life});
                if (target == 6 || uint8_t(target - 2) <= 2) {
                    game(kFieldOrigin, {sp + kFieldOriginOut, FlatRead8(sp + kFieldParams) == 1 ? 1u : 0u,
                                        sp + kWorldToEmitterInv, sp + kLocal, sp + kGlobal, sp + kPosition});
                    Copy3(sp + kFieldOriginCopy, sp + kFieldOriginOut);
                }
                const uint32_t force = sp + kFieldForce, fieldParams = sp + kFieldParams,
                               origin = sp + kFieldOriginCopy;
                Zero3(force, zero);
                switch (target) {
                case 0: game(kFieldKind[0], {force, fieldParams}); break;
                case 1: game(kFieldKind[1], {force, fieldParams, sp + kVelocity}); break;
                case 2: case 3: case 4: case 6: game(kFieldKind[target], {force, fieldParams, origin}); break;
                case 7:
                    FlatWrite32(sp + kArgA, sp + kMovement);
                    FlatWrite32(sp + kArgB, sp + kWorldRotInv);
                    game(kFieldKind[7], {force, fieldParams, track, track, p, tick, seed, sp + kVelocity});
                    break;
                case 8: { // toward the emitter's movement
                    const uint32_t emitter = FlatRead32(manager + 0x20);
                    if (!(unset == FlatReadFloat32(emitter + 0x114))) {
                        game(kEmitterGlobalMtx, {emitter, sp + kEmitterMtx});
                        FlatWrite32(sp + kEmitterPosition, FlatRead32(sp + kEmitterMtx + 0x0C));
                        FlatWrite32(sp + kEmitterPosition + 4, FlatRead32(sp + kEmitterMtx + 0x1C));
                        FlatWrite32(sp + kEmitterPosition + 8, FlatRead32(sp + kEmitterMtx + 0x2C));
                        game(kVecSubtract, {force, sp + kEmitterPosition, FlatRead32(manager + 0x20) + 0x114});
                        game(kVecScale, {force}, {FlatReadFloat32(sp + kFieldParams + 4)});
                    }
                    break;
                }
                default: break;
                }
                switch (FlatRead8(sp + kFieldParams)) { // the force's space
                case 1: game(kMtxMultVec, {force, sp + kLocalRotInv, force}); break;
                case 0: game(kMtxMultVec, {force, sp + kWorldToLocalRot, force}); break;
                case 3:
                    game(kVecScale, {force}, {scaleMag});
                    game(kMtxMultVec, {force, sp + kWorldToLocalRot, force});
                    break;
                default: break;
                }
                switch (FlatRead8(sp + kFieldParams + 1)) { // what it moves
                case 0: AddTo(sp + kAccel, force); break;
                case 1: AddTo(sp + kPush, force); break;
                default: break;
                }
                break;
            }
            case 2: { // post-field (collision) parameters
                if (FlatRead32(track + 0x1C) != 0) {
                    const uint32_t table = track + FlatRead32(track + 0xC) + 0x20 + FlatRead32(track + 0x10) +
                                           FlatRead32(track + 0x14);
                    header = table + FlatRead32(track + 0x18);
                    for (uint32_t k = 0; k < 0x24; ++k) FlatWrite8(sp + kPostParams + k, FlatRead8(header + k));
                    if (FlatRead32(track + 0x18) > 4) {
                        if (!(FlatRead16(header + 0x46) < FlatRead16(table))) panic(0x226, kAssertPostField);
                        collision = FlatRead32(table + FlatRead16(header + 0x46) * 4 + 4);
                    }
                }
                if (FlatRead32(track + 0xC) != 0)
                    native(MH3_AnimCurveExecuteF32, {track, sp + kPostParams + target, tick, seed, life});
                postField = true;
                break;
            }
            default: break;
            }
        }

        if (!postField) {
            AddTo(p + 0xA0, sp + kAccel);
            AddPosition(p, sp + kPush);
            AddPosition(p, p + 0xA0);
        } else {
            Copy3(sp + kPostVelocity, p + 0xA0);
            AddTo(sp + kPostVelocity, sp + kAccel);
            FlatWrite8(sp + kRetire, 0);
            Copy3(sp + kPostPush, sp + kPush);
            FlatWrite32(sp + kArgA, sp + kPostVelocity);
            FlatWrite32(sp + kArgB, sp + kRetire);
            const uint32_t moved = game(kPostField, {p, sp + kPostParams, header, collision, sp + kGlobal,
                                                     sp + kWorldToLocal, sp + kPosition, sp + kPostPush});
            if (FlatRead8(sp + kRetire) != 0) {
                game(kRetireParticle, {manager, p});
                p = FlatRead32(sp + kNext);
                continue;
            }
            if (moved) {
                Copy3(p + 0xA0, sp + kPostVelocity);
                AddPosition(p, sp + kPush);
                AddPosition(p, p + 0xA0);
            }
            if (FlatRead8(header + 0x48) & 1) {
                // Wrap the position into the collision box, [-1, 1] on each
                // axis of its frame.
                const uint32_t m = sp + kWrapMtx;
                game(kMtxIdentity, {m});
                game(kMtxRotate, {m, header + 0x4C, m});
                game(kMtxScale, {sp + kWrapScale},
                     {FlatReadFloat32(header + 0x58), FlatReadFloat32(header + 0x5C), FlatReadFloat32(header + 0x60)});
                game(kMtxConcat, {m, sp + kWrapScale, m});
                game(kMtxTranslate, {m, header + 0x64, m});
                if (FlatRead8(header + 0x48) & 2) {
                    FlatWrite32(sp + kEmitterTranslate, FlatRead32(sp + kGlobal + 0x0C));
                    FlatWrite32(sp + kEmitterTranslate + 4, FlatRead32(sp + kGlobal + 0x1C));
                    FlatWrite32(sp + kEmitterTranslate + 8, FlatRead32(sp + kGlobal + 0x2C));
                    game(kMtxTranslate, {m, sp + kEmitterTranslate, m});
                }
                game(kMtxConcat, {m, sp + kWorldToLocal, m});
                game(kMtxInverse, {sp + kWrapInverse, m});
                game(kMtxMultVec, {sp + kWrapLocal, sp + kWrapInverse, p + 0xAC});
                bool wrapped = false;
                for (uint32_t k = 0; k < 12; k += 4) {
                    const float v = FlatReadFloat32(sp + kWrapLocal + k);
                    if (v > one) {
                        game(kFmod, {}, {Add(one, v), period});
                        FlatWriteFloat32(sp + kWrapLocal + k, Sub(ctx->fpr[1].d, one));
                        wrapped = true;
                    } else if (v < minusOne) {
                        game(kFmod, {}, {Sub(v, one), period});
                        FlatWriteFloat32(sp + kWrapLocal + k, Add(one, ctx->fpr[1].d));
                        wrapped = true;
                    }
                }
                if (wrapped) game(kMtxMultVec, {p + 0xAC, m, sp + kWrapLocal});
            }
        }
        FlatWrite16(p + 0xDC, uint16_t(FlatRead16(p + 0xDC) + 1));
        p = FlatRead32(sp + kNext);
    }

    FlatWrite32(manager + 0x54, FlatRead32(list + 4)); // the list's last
    if (const uint32_t after = FlatRead32(FlatRead32(FlatRead32(manager + 0x20) + 0xBC) + 0x50))
        game(after, {manager, list, FlatRead32(sp + kFirst)});
}
