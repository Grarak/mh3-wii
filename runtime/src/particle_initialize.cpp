// NW4R ef Particle::Initialize and its parameter copy, RMHP08: C reconstructions
// from the Ghidra decompilation and the translation of ef_particle.cpp. Every
// particle an emitter emits starts here (the base camp waterfall's vapor: a
// dozen a tick).
//
//   0x800AA804 ParticleInitialize(particle, life, position, velocity, manager,
//              mtx, f1 scale, inherit, parent): alive (+0xC 1, +0x10 0), its
//              manager (+0xC8, the manager's reference count +0x10 up), the
//              manager resource's parameters copied (0x800AA334), the scale
//              (+0xC4), three draws of the emitter's random seed (+0xEC): the
//              particle's seed (u16 +0xDE), a random byte (+0xE5) and a signed
//              byte in -126..127 (+0x99); the life (+0xE0, 0 made 1), the
//              position (+0xAC, previous +0xB8) and velocity (+0xA0), plus the
//              effect's velocity (+0x88, into the emitter's frame) when not
//              zero; with mtx both moved by it (the velocity without its
//              translation); +0xCC = (0x80798520, 0, 0). A child (inherit and
//              parent given) takes shares of its parent's velocity (s16 +0,
//              in hundredths), size (u8 +2), alpha (u8 +3: layer flags u8 +8,
//              +9), colour (u8 +4) and rotation (u8 +7 bit 2). Returns 1
//   0x800AA334 ParticleParameterCopy(dst, src, manager): the resource's
//              particle parameters (0x80 bytes), the three initial rotation
//              angles as bytes (0x800AA764) with the random share (src +0x79)
//              of each drawn from the emitter's seed
//   0x800AA764 EfAngleByte(f1 radians): the angle in 256ths of a turn, the
//              ceiling's low byte
//   0x800AA7C0 Vec2Copy(dst, src): returns dst
//
// The originals assert their pointers; the ports drop those debug checks. The
// matrix and vector helpers stay the guest's (MTX inverse 0x80088428, MultVec
// 0x80051560, VEC3Add 0x80073FCC). Float steps are single operations as the
// originals' fmuls, fadds and fdivs (no fused multiply-add).
#include "abi_bridge.h"
#include "effect_particle.h"
#include "guest_call.h"
#include "memory.h"
#include "port_check.h"

#include <cmath>
#include <cstdint>

namespace {
using MemoryInline::FlatRead16;
using MemoryInline::FlatRead32;
using MemoryInline::FlatRead8;
using MemoryInline::FlatReadFloat32;
using MemoryInline::FlatWrite16;
using MemoryInline::FlatWrite32;
using MemoryInline::FlatWrite8;
using MemoryInline::FlatWriteFloat32;

constexpr uint32_t kOne = 0x80798520, kRandomScale = 0x80798524, kRandomShare = 0x80798528, kZero = 0x8079854C;
constexpr uint32_t kHundredth = 0x80798550, kRotationByte = 0x80798554;
constexpr uint32_t kAngleUnit = 0x80798538, kAngleTurn = 0x8079853C, kAngleBias = 0x80798540;
constexpr uint32_t kRandomDivisor = 0x807984E4;

void CopyWords(uint32_t dst, uint32_t src, uint32_t count) {
    for (uint32_t k = 0; k < count; ++k)
        FlatWrite32(dst + 4 * k, FlatRead32(src + 4 * k));
}

// The ef random generator (0x800A6F08) at `seed`: the next state's high half.
uint32_t RandomU16(uint32_t seed) {
    const uint32_t next = FlatRead32(seed) * 0x343FDu + 0x269EC3u;
    FlatWrite32(seed, next);
    return next >> 16;
}

// 0x800A8A6C: the next random number in 0..1.
float RandomFloat(uint32_t seed) {
    const float value = float(RandomU16(seed));
    return value / FlatReadFloat32(kRandomDivisor);
}

// 0x800AA764: ceil(radians / turn * unit - bias), its low byte (as the
// original's signed remainder).
uint8_t AngleByte(float radians) {
    float turns = radians / FlatReadFloat32(kAngleTurn);
    turns = FlatReadFloat32(kAngleUnit) * turns;
    turns = turns - FlatReadFloat32(kAngleBias);
    const float ceiling = float(std::ceil(double(turns)));
    const int32_t whole = std::isnan(ceiling) ? int32_t(0x80000000) : ceiling >= 2147483648.0f ? 0x7FFFFFFF : ceiling < -2147483648.0f ? int32_t(0x80000000) : int32_t(ceiling);
    const uint32_t sign = uint32_t(whole) >> 31;
    const uint32_t shifted = (uint32_t(whole) << 24) - sign;
    return uint8_t((shifted >> 24) + sign);
}

// 0x800AA334: dst = src's particle parameters, the rotation angles as bytes.
void ParameterCopy(uint32_t dst, uint32_t src, uint32_t manager) {
    CopyWords(dst, src, 4);
    CopyWords(dst + 0x10, src + 0x10, 2);
    CopyWords(dst + 0x18, src + 0x18, 2);
    CopyWords(dst + 0x20, src + 0x20, 3);
    const uint32_t seed = FlatRead32(manager + 0x20) + 0xEC;

    for (uint32_t i = 0; i < 3; ++i) {
        float angle = FlatReadFloat32(src + 0x7C + 4 * i);

        if (const uint8_t share = FlatRead8(src + 0x79 + i)) {
            float random = FlatReadFloat32(kOne) * RandomFloat(seed);
            random = random - FlatReadFloat32(kRandomScale);
            random = angle * random;
            float extra = float(share) * random;
            extra = extra / FlatReadFloat32(kRandomShare);
            angle = angle + extra;
        }

        FlatWrite8(dst + 0x7A + i, AngleByte(angle));
    }

    for (uint32_t i = 0; i < 3; ++i) {
        FlatWrite32(dst + 0x68 + 4 * i, FlatRead32(src + 0x68 + 4 * i));
        CopyWords(dst + 0x2C + 8 * i, src + 0x2C + 8 * i, 2);
        FlatWrite32(dst + 0x44 + 4 * i, FlatRead32(src + 0x44 + 4 * i));
        CopyWords(dst + 0x50 + 8 * i, src + 0x50 + 8 * i, 2);
    }

    FlatWrite16(dst + 0x74, FlatRead16(src + 0x74));
    FlatWrite8(dst + 0x76, FlatRead8(src + 0x76));
    FlatWrite8(dst + 0x77, FlatRead8(src + 0x77));
    FlatWrite8(dst + 0x78, FlatRead8(src + 0x78));
    FlatWrite8(dst + 0x7D, 0);
}

// A 3x4 matrix's translation column set to zero.
void ClearTranslation(uint32_t mtx) {
    const uint32_t zero = FlatRead32(kZero);
    FlatWrite32(mtx + 0x0C, zero);
    FlatWrite32(mtx + 0x1C, zero);
    FlatWrite32(mtx + 0x2C, zero);
}

// The share of a parent's alpha by the layer flags (1 the primary alpha, 2
// the secondary, 4 the flicker and the modulated alpha `modulated`), in 255ths.
uint32_t InheritedAlpha(uint32_t parent, uint32_t layer, uint8_t flags, uint8_t modulated) {
    uint32_t alpha = 255;

    if (flags & 1) {
        const uint32_t primary = EffectParticle::ColorAlpha(parent, layer, 0);
        alpha = ((primary << 8) - primary + 255) >> 8;
    }

    if (flags & 2)
        alpha = (alpha * EffectParticle::ColorAlpha(parent, layer, 1) + 255) >> 8;

    if (flags & 4)
        alpha = ((alpha * (EffectParticle::AlphaFlicker(parent) & 0xFF) + 255) * modulated + 255) >> 16;

    return alpha;
}

// The child's shares of its parent (inherit != 0, parent != 0).
void Inherit(CpuContext* cpu, const GuestFrame& frame, uint32_t particle, uint32_t inherit, uint32_t parent) {
    GuestCall game{cpu};
    const uint32_t parentManager = FlatRead32(parent + 0xC8);

    if (const int16_t share = int16_t(FlatRead16(inherit))) {
        const uint32_t velocity = frame[0x20], mtx = frame[0x38];
        game.Call<0x800AE3C4>({parentManager, mtx}); // ParticleManagerLocalMtx
        ClearTranslation(mtx);
        game.Call<0x80051560>({velocity, mtx, parent + 0xA0});
        game.Call<0x800AE3C4>({FlatRead32(particle + 0xC8), mtx});
        ClearTranslation(mtx);
        game.Call<0x80088428>({mtx, mtx});
        game.Call<0x80051560>({velocity, mtx, velocity});
        const float scale = float(share), hundredth = FlatReadFloat32(kHundredth);

        for (uint32_t k = 0; k < 3; ++k) {
            float add = FlatReadFloat32(velocity + 4 * k) * scale;
            add = hundredth * add;
            FlatWriteFloat32(particle + 0xA0 + 4 * k, FlatReadFloat32(particle + 0xA0 + 4 * k) + add);
        }
    }

    if (const uint8_t share = FlatRead8(inherit + 2)) {
        float width = FlatReadFloat32(parent + 0x30) * FlatReadFloat32(parent + 0x38);
        width = width * FlatReadFloat32(parentManager + 0x58);
        float size = float(share) * width;
        FlatWriteFloat32(particle + 0x30, FlatReadFloat32(kHundredth) * size);
        game.Call<0x800AB340>({parent}); // the parent's scaled height
        size = float(share) * game.F1();
        FlatWriteFloat32(particle + 0x34, FlatReadFloat32(kHundredth) * size);
    }

    if (const uint8_t share = FlatRead8(inherit + 3)) {
        uint8_t primary[4], secondary[4];

        for (uint32_t k = 0; k < 4; ++k) {
            primary[k] = FlatRead8(0x80798544 + k);
            secondary[k] = FlatRead8(0x80798548 + k);
        }

        EffectParticle::ModulateColors(parentManager, parent, primary, secondary);

        for (uint32_t layer = 0; layer < 2; ++layer) {
            const uint32_t colors = particle + 8 * layer;

            if (const uint8_t flags = FlatRead8(inherit + 8))
                FlatWrite8(colors + 0x23, uint8_t(InheritedAlpha(parent, layer, flags, primary[3]) * share / 100));

            if (const uint8_t flags = FlatRead8(inherit + 9))
                FlatWrite8(colors + 0x27, uint8_t(InheritedAlpha(parent, layer, flags, primary[3]) * share / 100));
        }
    }

    if (const uint8_t share = FlatRead8(inherit + 4))
        for (uint32_t layer = 0; layer < 2; ++layer)
            for (uint32_t index = 0; index < 2; ++index) {
                uint8_t rgb[3];
                EffectParticle::ColorRgb(parent, layer, index, rgb);
                const uint32_t color = particle + 0x20 + 8 * layer + 4 * index;

                for (uint32_t k = 0; k < 3; ++k)
                    FlatWrite8(color + k, uint8_t(rgb[k] * share / 100));
            }

    if (FlatRead8(inherit + 7) & 2)
        for (uint32_t k = 0; k < 3; ++k) {
            float random = FlatReadFloat32(kRotationByte) * float(FlatRead8(parent + 0x9A + k));
            random = FlatReadFloat32(parent + 0x40 + 4 * k) + random;
            FlatWrite8(particle + 0x9A + k, AngleByte(random));
        }
}

// 0x800AA804 (its arguments in the guest registers).
void Initialize(CpuContext* cpu) {
    GuestFrame frame(cpu, 0x110);
    GuestCall game{cpu};
    const uint32_t particle = frame.Arg(3), position = frame.Arg(5), velocity = frame.Arg(6), manager = frame.Arg(7), mtx = frame.Arg(8);
    const uint32_t inherit = frame.Arg(9), parent = frame.Arg(10);
    const float scale = float(cpu->fpr[1].d);
    uint32_t life = frame.Arg(4) & 0xFFFF;

    FlatWrite32(particle + 0x10, 0);
    FlatWrite32(particle + 0x0C, 1);

    if (life == 0)
        life = 1;

    FlatWrite32(particle + 0xC8, manager);
    FlatWrite32(manager + 0x10, FlatRead32(manager + 0x10) + 1);
    FlatWrite32(particle + 0xD8, 0);
    FlatWrite16(particle + 0xE2, 0);
    const uint32_t parameters = game.Call<0x800AB43C>({FlatRead32(manager + 0x24)});
    ParameterCopy(particle + 0x20, parameters, manager);
    FlatWriteFloat32(particle + 0xC4, scale);
    FlatWrite16(particle + 0xDC, 0);
    const uint32_t emitter = FlatRead32(manager + 0x20), seed = emitter + 0xEC;
    FlatWrite16(particle + 0xDE, uint16_t(RandomU16(seed)));
    FlatWrite16(particle + 0xE0, uint16_t(life));
    FlatWrite8(particle + 0xE5, uint8_t(RandomU16(seed)));
    FlatWrite8(particle + 0x99, uint8_t(int32_t(RandomU16(seed) % 254) - 126));
    CopyWords(particle + 0xAC, position, 3);
    CopyWords(particle + 0xB8, particle + 0xAC, 3);
    CopyWords(particle + 0xA0, velocity, 3);

    // The effect's velocity, into the emitter's frame.
    const uint32_t effectVelocity = FlatRead32(emitter + 0xBC) + 0x88;
    const float zero = FlatReadFloat32(kZero);

    if (zero != FlatReadFloat32(effectVelocity) || zero != FlatReadFloat32(effectVelocity + 4) || zero != FlatReadFloat32(effectVelocity + 8)) {
        const uint32_t inverse = frame[0x98], moved = frame[0x2C];
        game.Call<0x800A9508>({emitter, inverse}); // EmitterCalcGlobalMtx
        game.Call<0x80088428>({inverse, inverse});
        ClearTranslation(inverse);
        game.Call<0x80051560>({moved, inverse, effectVelocity});
        game.Call<0x80073FCC>({particle + 0xA0, moved});
    }

    if (mtx) {
        const uint32_t rotation = frame[0x68];
        game.Call<0x80051560>({particle + 0xAC, mtx, particle + 0xAC});
        CopyWords(rotation, mtx, 12);
        ClearTranslation(rotation);
        game.Call<0x80051560>({particle + 0xA0, rotation, particle + 0xA0});
    }

    FlatWrite32(particle + 0xCC, FlatRead32(kOne));
    FlatWrite32(particle + 0xD0, FlatRead32(kZero));
    FlatWrite32(particle + 0xD4, FlatRead32(kZero));

    if (parent && inherit)
        Inherit(cpu, frame, particle, inherit, parent);
}

void InitializePort(CpuContext* cpu) {
    Initialize(cpu);
    cpu->gpr[3] = 1; // after the frame restored the GPRs
}

void ParameterCopyPort(CpuContext* cpu) {
    const uint32_t dst = cpu->gpr[3], src = cpu->gpr[4], manager = cpu->gpr[5];
    ParameterCopy(dst, src, manager);
}

void AngleBytePort(CpuContext* cpu) {
    cpu->gpr[3] = AngleByte(float(cpu->fpr[1].d));
}

void Vec2CopyPort(CpuContext* cpu) {
    CopyWords(cpu->gpr[3], cpu->gpr[4], 2);
}

} // namespace

extern "C" void func_800AA804(CpuContext* ctx);
extern "C" void MH3_ParticleInitialize(CpuContext* ctx) {
    PortCheck::Call(ctx, "ParticleInitialize", func_800AA804, InitializePort);
}
REGISTER_NATIVE_FUNCTION_AS(0x800AA804, MH3_ParticleInitialize, "ParticleInitialize");

extern "C" void func_800AA334(CpuContext* ctx);
extern "C" void MH3_ParticleParameterCopy(CpuContext* ctx) {
    PortCheck::Call(ctx, "ParticleParameterCopy", func_800AA334, ParameterCopyPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x800AA334, MH3_ParticleParameterCopy, "ParticleParameterCopy");

extern "C" void func_800AA764(CpuContext* ctx);
extern "C" void MH3_EfAngleByte(CpuContext* ctx) {
    PortCheck::Call(ctx, "EfAngleByte", func_800AA764, AngleBytePort);
}
REGISTER_NATIVE_FUNCTION_AS(0x800AA764, MH3_EfAngleByte, "EfAngleByte");

extern "C" void func_800AA7C0(CpuContext* ctx);
extern "C" void MH3_Vec2Copy(CpuContext* ctx) {
    PortCheck::Call(ctx, "Vec2Copy", func_800AA7C0, Vec2CopyPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x800AA7C0, MH3_Vec2Copy, "Vec2Copy");
