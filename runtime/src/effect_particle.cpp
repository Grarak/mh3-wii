// NW4R ef particle accessors, RMHP08: C reconstructions from the disassembly
// of the helpers the draw strategies call for every particle drawn. Each is a
// few loads (and the debug build's pointer assertions) behind a call that the
// translated code paid a register flush and a dispatch for; the native
// strategies in native_effect_draw.cpp call these directly.
//
//   0x800B5BA4 ParticleListLast        *(manager + 0x3C)
//   0x800B5B98 ParticleListPrev        link prev of particle (+ u16 manager+0x42)
//   0x800B5AC8 ParticleListLastAlive   the last particle with life status 1
//   0x800B5B30 ParticleListPrevAlive   the previous one with life status 1
//   0x800B9624 ParticleListFirstAlive  the first one with life status 1
//   0x800B8DAC ParticleListNextAlive   the next one with life status 1
//   0x800C8AE4/0x800C8C00 DrawStrategyFirstAlive/LastAlive (forward/reverse
//              GetFirst functions of DrawStrategyImpl, 0x800C8AAC picks)
//   0x800C8D1C/0x800C8E48 DrawStrategyNextAlive/PrevAlive (GetNext, 0x800C8AC8)
//   0x800AB0BC ParticleColorRgb        RGB of colour layer/index into out[3]
//   0x800AB284 ParticleColorAlpha      its alpha
//   0x800AB460 ParticleAlphaFlicker    the emitter's alpha flicker at the age
//   0x800AE70C ParticleManagerModulateColor  manager colour scale (+0x6C)
//   0x800B5518 ParticleRotation        rotation + random bytes + manager offset
//   0x800B56FC ParticleManagerRotationOffset  += manager +0x60..+0x68
//
// The ports drop the NW4R pointer assertions (valid callers never panic);
// assertions on argument ranges remain as panics. MH3_PORT_CHECK=Name checks a
// port against its original on every call.
#include "effect_particle.h"

#include "abi_bridge.h"
#include "guest_call.h"
#include "memory.h"
#include "port_check.h"
#include "ppc_runtime.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace {
using MemoryInline::FlatRead16;
using MemoryInline::FlatRead32;
using MemoryInline::FlatRead8;
using MemoryInline::FlatReadFloat32;
using MemoryInline::FlatWrite8;
using MemoryInline::FlatWriteFloat32;

constexpr uint32_t kLifeAlive = 1;
extern "C" void func_800AB460(CpuContext* ctx);

[[noreturn]] void Panic(const char* what, uint32_t value) {
    char text[128];
    std::snprintf(text, sizeof text, "NW4R panic: %s (%08x)", what, value);
    throw std::runtime_error(text);
}
bool Alive(uint32_t particle) {
    return FlatRead32(particle + 0x0C) == kLifeAlive;
}
uint32_t LinkOffset(uint32_t manager) {
    return FlatRead16(manager + 0x42);
}

// nw4r::math::SinFIdx (0x805014B4): |fidx| wrapped below 65536 (while above),
// cut to u16 by the quantized store, the 256-entry {sin, cos, dsin, dcos}
// table at 0x805744A0 interpolated with the fraction (fmuls, then fadds) and
// negated for a negative index.
float SinFIdx(float fidx) {
    float a = std::fabs(fidx);
    while (a > 65536.0f)
        a -= 65536.0f;
    const uint32_t whole = a >= 65535.0f ? 65535u : uint32_t(a);
    const float fraction = a - float(whole);
    const uint32_t entry = 0x805744A0 + 16 * (whole & 0xFF);
    const float slope = fraction * FlatReadFloat32(entry + 8);
    const float value = FlatReadFloat32(entry) + slope;
    return fidx < 0.0f ? -value : value;
}

} // namespace

namespace EffectParticle {
uint32_t First(uint32_t manager) {
    return FlatRead32(manager + 0x38);
}
uint32_t Last(uint32_t manager) {
    return FlatRead32(manager + 0x3C);
}
uint32_t Next(uint32_t manager, uint32_t particle) {
    return FlatRead32(particle + LinkOffset(manager) + 4);
}
uint32_t Prev(uint32_t manager, uint32_t particle) {
    return FlatRead32(particle + LinkOffset(manager));
}
uint32_t FirstAlive(uint32_t manager) {
    const uint32_t link = LinkOffset(manager) + 4;
    uint32_t p = First(manager);
    while (p && !Alive(p))
        p = FlatRead32(p + link);
    return p;
}
uint32_t NextAlive(uint32_t manager, uint32_t particle) {
    const uint32_t link = LinkOffset(manager) + 4;
    uint32_t p = FlatRead32(particle + link);
    while (p && !Alive(p))
        p = FlatRead32(p + link);
    return p;
}
uint32_t LastAlive(uint32_t manager) {
    const uint32_t link = LinkOffset(manager);
    uint32_t p = Last(manager);
    while (p && !Alive(p))
        p = FlatRead32(p + link);
    return p;
}
uint32_t PrevAlive(uint32_t manager, uint32_t particle) {
    const uint32_t link = LinkOffset(manager);
    uint32_t p = FlatRead32(particle + link);
    while (p && !Alive(p))
        p = FlatRead32(p + link);
    return p;
}
FirstFunction NativeFirst(uint32_t guest) {
    switch (guest) {
    case 0x800C8AE4:
    case 0x800B9624:
        return FirstAlive;
    case 0x800C8C00:
    case 0x800B5AC8:
        return LastAlive;
    default:
        return nullptr;
    }
}
NextFunction NativeNext(uint32_t guest) {
    switch (guest) {
    case 0x800C8D1C:
    case 0x800B8DAC:
        return NextAlive;
    case 0x800C8E48:
    case 0x800B5B30:
        return PrevAlive;
    default:
        return nullptr;
    }
}

void ColorRgb(uint32_t particle, uint32_t layer, uint32_t index, uint8_t out[3]) {
    if (layer > 1)
        Panic("particle colour layer > 1", layer);
    if (index > 1)
        Panic("particle colour index > 1", index);
    const uint32_t color = particle + 0x20 + layer * 8 + index * 4;
    for (uint32_t k = 0; k < 3; ++k)
        out[k] = FlatRead8(color + k);
}
uint8_t ColorAlpha(uint32_t particle, uint32_t layer, uint32_t index) {
    if (layer > 1)
        Panic("particle colour layer > 1", layer);
    if (index > 1)
        Panic("particle colour index > 1", index);
    return FlatRead8(particle + 0x23 + layer * 8 + index * 4);
}

// The flicker's settings are in the emitter resource (manager +0x24, +8):
// mode +0x105, period +0x106 (u16), period randomness +0x108, amplitude
// +0x109; the particle's random byte +0x99 and its age +0xDC.
uint32_t AlphaFlicker(uint32_t particle) {
    const uint32_t body = FlatRead32(FlatRead32(particle + 0xC8) + 0x24) + 8;
    const uint8_t mode = FlatRead8(body + 0x105);
    if (mode == 0)
        return 0xFF;
    const uint32_t base = FlatRead16(body + 0x106);
    const int32_t random = int32_t(FlatRead8(body + 0x108) * base) * int8_t(FlatRead8(particle + 0x99));
    const uint32_t sum = base + uint32_t(random / 12700);
    const int32_t period = int32_t(sum > 0xFFFF ? 0xFFFF : sum & 0xFFFF);
    const int32_t age = int32_t(FlatRead16(particle + 0xDC)) - 1;
    if (period == 0) {
        // The original divides by zero (no trap, an undefined quotient): keep
        // whatever its translation computes.
        // The registered port is a precise native: the caller's registers
        // all stay as they were.
        CpuContext* cpu = TryGetCpuContext();
        const CpuContext saved = *cpu;
        cpu->gpr[3] = particle;
        func_800AB460(cpu);
        const uint32_t result = cpu->gpr[3];
        *cpu = saved;
        return result;
    }
    const int32_t phase = int32_t(uint32_t(age - (age / period) * period) & 0xFFFF);
    const int32_t amplitude = FlatRead8(body + 0x109);
    int32_t value;
    switch (mode) {
    case 1:
        value = phase * 2 > period ? int16_t(3 * amplitude + -(amplitude * phase * 4) / period + 128) : int16_t((128 - amplitude) + (amplitude * phase * 4) / period);
        break;
    case 2:
        value = int16_t(amplitude + -(amplitude * phase * 2) / period + 128);
        break;
    case 3:
        value = int16_t((128 - amplitude) + (amplitude * phase * 2) / period);
        break;
    case 4:
        value = phase * 2 > period ? int16_t(128 - amplitude) : int16_t(amplitude + 128);
        break;
    case 5: {
        // MathSinRad (0x800AB6BC): SinFIdx(256 / 2 pi * x), x the phase's
        // share of 2 pi, in single precision; the result truncated (fctiwz).
        const float turn = FlatReadFloat32(0x80798560) * float(phase);
        const float x = turn / float(period);
        const float scaled = FlatReadFloat32(0x80798564) * x;
        const float product = float(amplitude) * SinFIdx(scaled);
        const float sum5 = FlatReadFloat32(0x80798538) + product;
        value = int16_t(int32_t(sum5));
        break;
    }
    default:
        Panic("alpha flicker mode", mode);
    }
    if (value < 0)
        value = 0;
    if (value > 255)
        value = 255;
    return uint32_t(value) & 0xFF;
}

void ModulateColors(uint32_t manager, uint32_t particle, uint8_t primary[4], uint8_t secondary[4]) {
    const uint8_t mode = FlatRead8(manager + 0x6C);
    if (mode != 1 && mode != 2)
        return;
    const auto scale = [&](uint8_t color[4]) {
        for (uint32_t k = 0; k < 4; ++k)
            color[k] = uint8_t((color[k] * FlatRead8(manager + 0x6D + k) + 128) >> 8);
    };
    if (mode == 2 && !(FlatReadFloat32(manager + 0x78) < FlatReadFloat32(0x807985A0))) {
        // Fade by the distance of the particle (in the manager's draw frame,
        // the matrix 0x800AE68C left at 0x806969E8) from the centre +0x7C:
        // from the colour +0x71 at the centre to +0x6D at the range +0x78.
        // Through the guest's vector helpers; the caller's registers all
        // stay as they were (the registered port is a precise native).
        CpuContext* cpu = TryGetCpuContext();
        const CpuContext saved = *cpu;
        float distance;
        {
            GuestFrame frame(cpu, 0x20);
            GuestCall game{cpu};
            const uint32_t moved = frame[8];
            game.Call<0x80051560>({moved, 0x806969E8, particle + 0xAC});
            game.Call<0x80050D58>({moved, moved, manager + 0x7C});
            game.Call<0x80050F88>({moved});
            distance = float(cpu->fpr[1].d);
        }
        *cpu = saved;
        const float range = FlatReadFloat32(manager + 0x78);
        if (!(distance > range)) {
            const float product = FlatReadFloat32(0x807985A4) * distance;
            const int32_t t = int32_t(product / range);
            uint32_t weight[4];
            for (uint32_t k = 0; k < 4; ++k) {
                const int32_t from = FlatRead8(manager + 0x71 + k), to = FlatRead8(manager + 0x6D + k);
                weight[k] = uint32_t((from << 8) + t * (to - from)) & 0xFFFF;
            }
            for (uint8_t* color : {primary, secondary})
                for (uint32_t k = 0; k < 4; ++k)
                    color[k] = uint8_t((color[k] * weight[k] + 128) >> 16);
            return;
        }
    }
    scale(primary);
    scale(secondary);
}

void Rotation(uint32_t particle, float out[3]) {
    const float step = FlatReadFloat32(0x807985F4);
    for (uint32_t k = 0; k < 3; ++k) {
        out[k] = FlatReadFloat32(particle + 0x40 + 4 * k);
        if (const uint8_t random = FlatRead8(particle + 0x9A + k)) {
            const float add = step * float(random);
            out[k] = out[k] + add;
        }
    }
    const uint32_t manager = FlatRead32(particle + 0xC8);
    for (uint32_t k = 0; k < 3; ++k)
        out[k] = out[k] + FlatReadFloat32(manager + 0x60 + 4 * k);
}
} // namespace EffectParticle


// ---- Registrations: the translated callers reach the ports too ----
// Each registered body passes argument registers to a typed function and
// writes its result register (a precise native). With MH3_PORT_CHECK naming
// it, the typed function runs the translated original first and compares.
extern "C" void func_800B5BA4(CpuContext* ctx);
extern "C" void func_800B5B98(CpuContext* ctx);
extern "C" void func_800B5AC8(CpuContext* ctx);
extern "C" void func_800B5B30(CpuContext* ctx);
extern "C" void func_800B9624(CpuContext* ctx);
extern "C" void func_800B8DAC(CpuContext* ctx);
extern "C" void func_800C8AE4(CpuContext* ctx);
extern "C" void func_800C8C00(CpuContext* ctx);
extern "C" void func_800C8D1C(CpuContext* ctx);
extern "C" void func_800C8E48(CpuContext* ctx);
extern "C" void func_800AB0BC(CpuContext* ctx);
extern "C" void func_800AB284(CpuContext* ctx);
extern "C" void func_800AE70C(CpuContext* ctx);
extern "C" void func_800B5518(CpuContext* ctx);
extern "C" void func_800B56FC(CpuContext* ctx);
namespace {
using namespace EffectParticle;
#ifdef MH3_DEVELOPMENT
struct Checks {
    bool listLast = PortCheck::Enabled("ParticleListLast"), listPrev = PortCheck::Enabled("ParticleListPrev");
    bool listLastAlive = PortCheck::Enabled("ParticleListLastAlive"), listPrevAlive = PortCheck::Enabled("ParticleListPrevAlive");
    bool listFirstAlive = PortCheck::Enabled("ParticleListFirstAlive"), listNextAlive = PortCheck::Enabled("ParticleListNextAlive");
    bool strategyFirst = PortCheck::Enabled("DrawStrategyFirstAlive"), strategyLast = PortCheck::Enabled("DrawStrategyLastAlive");
    bool strategyNext = PortCheck::Enabled("DrawStrategyNextAlive"), strategyPrev = PortCheck::Enabled("DrawStrategyPrevAlive");
    bool colorRgb = PortCheck::Enabled("ParticleColorRgb"), colorAlpha = PortCheck::Enabled("ParticleColorAlpha");
    bool alphaFlicker = PortCheck::Enabled("ParticleAlphaFlicker"), modulate = PortCheck::Enabled("ParticleManagerModulateColor");
    bool rotation = PortCheck::Enabled("ParticleRotation"), rotationOffset = PortCheck::Enabled("ParticleManagerRotationOffset");
    bool any = listLast || listPrev || listLastAlive || listPrevAlive || listFirstAlive || listNextAlive || strategyFirst || strategyLast || strategyNext || strategyPrev || colorRgb ||
               colorAlpha || alphaFlicker || modulate || rotation || rotationOffset;
};
const Checks& Checked() {
    static const Checks checks;
    return checks;
}
// The original and the port of a function returning r3, from r3 (and r4).
uint32_t Pure(const char* name, void (*original)(CpuContext*), void (*port)(CpuContext*), uint32_t r3, uint32_t r4 = 0, uint32_t r5 = 0) {
    CpuContext* c = TryGetCpuContext();
    c->gpr[3] = r3;
    c->gpr[4] = r4;
    c->gpr[5] = r5;
    PortCheck::RunPure(c, name, PortCheck::Returns::Gpr3, original, port);
    return c->gpr[3];
}
#endif
void ListLastPort(CpuContext* c) {
    c->gpr[3] = Last(c->gpr[3]);
}
void ListPrevPort(CpuContext* c) {
    c->gpr[3] = Prev(c->gpr[3], c->gpr[4]);
}
void ListLastAlivePort(CpuContext* c) {
    c->gpr[3] = LastAlive(c->gpr[3]);
}
void ListPrevAlivePort(CpuContext* c) {
    c->gpr[3] = PrevAlive(c->gpr[3], c->gpr[4]);
}
void ListFirstAlivePort(CpuContext* c) {
    c->gpr[3] = FirstAlive(c->gpr[3]);
}
void ListNextAlivePort(CpuContext* c) {
    c->gpr[3] = NextAlive(c->gpr[3], c->gpr[4]);
}
void ColorRgbPort(CpuContext* c) {
    uint8_t rgb[3];
    ColorRgb(c->gpr[3], c->gpr[4], c->gpr[5], rgb);
    for (uint32_t k = 0; k < 3; ++k)
        FlatWrite8(c->gpr[6] + k, rgb[k]);
}
void ColorAlphaPort(CpuContext* c) {
    c->gpr[3] = ColorAlpha(c->gpr[3], c->gpr[4], c->gpr[5]);
}
void AlphaFlickerPort(CpuContext* c) {
    c->gpr[3] = AlphaFlicker(c->gpr[3]);
}
void ModulateColorsAt(uint32_t manager, uint32_t particle, uint32_t primary, uint32_t secondary) {
    uint8_t a[4], b[4];
    for (uint32_t k = 0; k < 4; ++k) {
        a[k] = FlatRead8(primary + k);
        b[k] = FlatRead8(secondary + k);
    }
    ModulateColors(manager, particle, a, b);
    for (uint32_t k = 0; k < 4; ++k) {
        FlatWrite8(primary + k, a[k]);
        FlatWrite8(secondary + k, b[k]);
    }
}
void ModulateColorsPort(CpuContext* c) {
    ModulateColorsAt(c->gpr[3], c->gpr[4], c->gpr[5], c->gpr[6]);
}
void RotationAt(uint32_t particle, uint32_t out) {
    float rotation[3];
    Rotation(particle, rotation);
    for (uint32_t k = 0; k < 3; ++k)
        FlatWriteFloat32(out + 4 * k, rotation[k]);
}
void RotationPort(CpuContext* c) {
    RotationAt(c->gpr[3], c->gpr[4]);
}
void RotationOffsetAt(uint32_t manager, uint32_t out) {
    for (uint32_t k = 0; k < 3; ++k)
        FlatWriteFloat32(out + 4 * k, FlatReadFloat32(out + 4 * k) + FlatReadFloat32(manager + 0x60 + 4 * k));
}
void RotationOffsetPort(CpuContext* c) {
    RotationOffsetAt(c->gpr[3], c->gpr[5]);
}

uint32_t ListLastChecked(uint32_t manager) {
#ifdef MH3_DEVELOPMENT
    if (PortCheck::Set() && Checked().listLast) [[unlikely]]
        return Pure("ParticleListLast", func_800B5BA4, ListLastPort, manager);
#endif
    return Last(manager);
}
uint32_t ListPrevChecked(uint32_t manager, uint32_t particle) {
#ifdef MH3_DEVELOPMENT
    if (PortCheck::Set() && Checked().listPrev) [[unlikely]]
        return Pure("ParticleListPrev", func_800B5B98, ListPrevPort, manager, particle);
#endif
    return Prev(manager, particle);
}
uint32_t ListLastAliveChecked(uint32_t manager) {
#ifdef MH3_DEVELOPMENT
    if (PortCheck::Set() && Checked().listLastAlive) [[unlikely]]
        return Pure("ParticleListLastAlive", func_800B5AC8, ListLastAlivePort, manager);
#endif
    return LastAlive(manager);
}
uint32_t ListPrevAliveChecked(uint32_t manager, uint32_t particle) {
#ifdef MH3_DEVELOPMENT
    if (PortCheck::Set() && Checked().listPrevAlive) [[unlikely]]
        return Pure("ParticleListPrevAlive", func_800B5B30, ListPrevAlivePort, manager, particle);
#endif
    return PrevAlive(manager, particle);
}
uint32_t ListFirstAliveChecked(uint32_t manager) {
#ifdef MH3_DEVELOPMENT
    if (PortCheck::Set() && Checked().listFirstAlive) [[unlikely]]
        return Pure("ParticleListFirstAlive", func_800B9624, ListFirstAlivePort, manager);
#endif
    return FirstAlive(manager);
}
uint32_t ListNextAliveChecked(uint32_t manager, uint32_t particle) {
#ifdef MH3_DEVELOPMENT
    if (PortCheck::Set() && Checked().listNextAlive) [[unlikely]]
        return Pure("ParticleListNextAlive", func_800B8DAC, ListNextAlivePort, manager, particle);
#endif
    return NextAlive(manager, particle);
}
uint32_t StrategyFirstChecked(uint32_t manager) {
#ifdef MH3_DEVELOPMENT
    if (PortCheck::Set() && Checked().strategyFirst) [[unlikely]]
        return Pure("DrawStrategyFirstAlive", func_800C8AE4, ListFirstAlivePort, manager);
#endif
    return FirstAlive(manager);
}
uint32_t StrategyLastChecked(uint32_t manager) {
#ifdef MH3_DEVELOPMENT
    if (PortCheck::Set() && Checked().strategyLast) [[unlikely]]
        return Pure("DrawStrategyLastAlive", func_800C8C00, ListLastAlivePort, manager);
#endif
    return LastAlive(manager);
}
uint32_t StrategyNextChecked(uint32_t manager, uint32_t particle) {
#ifdef MH3_DEVELOPMENT
    if (PortCheck::Set() && Checked().strategyNext) [[unlikely]]
        return Pure("DrawStrategyNextAlive", func_800C8D1C, ListNextAlivePort, manager, particle);
#endif
    return NextAlive(manager, particle);
}
uint32_t StrategyPrevChecked(uint32_t manager, uint32_t particle) {
#ifdef MH3_DEVELOPMENT
    if (PortCheck::Set() && Checked().strategyPrev) [[unlikely]]
        return Pure("DrawStrategyPrevAlive", func_800C8E48, ListPrevAlivePort, manager, particle);
#endif
    return PrevAlive(manager, particle);
}
uint32_t ColorAlphaChecked(uint32_t particle, uint32_t layer, uint32_t index) {
#ifdef MH3_DEVELOPMENT
    if (PortCheck::Set() && Checked().colorAlpha) [[unlikely]]
        return Pure("ParticleColorAlpha", func_800AB284, ColorAlphaPort, particle, layer, index);
#endif
    return ColorAlpha(particle, layer, index);
}
uint32_t AlphaFlickerChecked(uint32_t particle) {
#ifdef MH3_DEVELOPMENT
    if (PortCheck::Set() && Checked().alphaFlicker) [[unlikely]]
        return Pure("ParticleAlphaFlicker", func_800AB460, AlphaFlickerPort, particle);
#endif
    return AlphaFlicker(particle);
}
void ColorRgbChecked(uint32_t particle, uint32_t layer, uint32_t index, uint32_t out) {
#ifdef MH3_DEVELOPMENT
    if (PortCheck::Set() && Checked().colorRgb) [[unlikely]] {
        CpuContext* c = TryGetCpuContext();
        c->gpr[3] = particle, c->gpr[4] = layer, c->gpr[5] = index, c->gpr[6] = out;
        PortCheck::RunOutput(c, "ParticleColorRgb", func_800AB0BC, ColorRgbPort, {{out, 3}});
        return;
    }
#endif
    uint8_t rgb[3];
    ColorRgb(particle, layer, index, rgb);
    for (uint32_t k = 0; k < 3; ++k)
        FlatWrite8(out + k, rgb[k]);
}
void ModulateColorsChecked(uint32_t manager, uint32_t particle, uint32_t primary, uint32_t secondary) {
#ifdef MH3_DEVELOPMENT
    if (PortCheck::Set() && Checked().modulate) [[unlikely]] {
        CpuContext* c = TryGetCpuContext();
        c->gpr[3] = manager, c->gpr[4] = particle, c->gpr[5] = primary, c->gpr[6] = secondary;
        PortCheck::RunOutput(c, "ParticleManagerModulateColor", func_800AE70C, ModulateColorsPort, {{primary, 4}, {secondary, 4}});
        return;
    }
#endif
    ModulateColorsAt(manager, particle, primary, secondary);
}
void RotationChecked(uint32_t particle, uint32_t out) {
#ifdef MH3_DEVELOPMENT
    if (PortCheck::Set() && Checked().rotation) [[unlikely]] {
        CpuContext* c = TryGetCpuContext();
        c->gpr[3] = particle, c->gpr[4] = out;
        PortCheck::RunOutput(c, "ParticleRotation", func_800B5518, RotationPort, {{out, 12}});
        return;
    }
#endif
    RotationAt(particle, out);
}
void RotationOffsetChecked(uint32_t manager, uint32_t particle, uint32_t out) {
#ifdef MH3_DEVELOPMENT
    if (PortCheck::Set() && Checked().rotationOffset) [[unlikely]] {
        CpuContext* c = TryGetCpuContext();
        c->gpr[3] = manager, c->gpr[4] = particle, c->gpr[5] = out;
        PortCheck::RunOutput(c, "ParticleManagerRotationOffset", func_800B56FC, RotationOffsetPort, {{out, 12}});
        return;
    }
#else
    (void)particle;
#endif
    RotationOffsetAt(manager, out);
}
} // namespace

bool EffectParticle::PortCheckedSlow() {
#ifdef MH3_DEVELOPMENT
    return Checked().any;
#else
    return false;
#endif
}

extern "C" void MH3_ParticleListLast(CpuContext* ctx) {
    ctx->gpr[3] = ListLastChecked(ctx->gpr[3]);
}
REGISTER_NATIVE_FUNCTION_AS(0x800B5BA4, MH3_ParticleListLast, "ParticleListLast");
extern "C" void MH3_ParticleListPrev(CpuContext* ctx) {
    ctx->gpr[3] = ListPrevChecked(ctx->gpr[3], ctx->gpr[4]);
}
REGISTER_NATIVE_FUNCTION_AS(0x800B5B98, MH3_ParticleListPrev, "ParticleListPrev");
extern "C" void MH3_ParticleListLastAlive(CpuContext* ctx) {
    ctx->gpr[3] = ListLastAliveChecked(ctx->gpr[3]);
}
REGISTER_NATIVE_FUNCTION_AS(0x800B5AC8, MH3_ParticleListLastAlive, "ParticleListLastAlive");
extern "C" void MH3_ParticleListPrevAlive(CpuContext* ctx) {
    ctx->gpr[3] = ListPrevAliveChecked(ctx->gpr[3], ctx->gpr[4]);
}
REGISTER_NATIVE_FUNCTION_AS(0x800B5B30, MH3_ParticleListPrevAlive, "ParticleListPrevAlive");
extern "C" void MH3_ParticleListFirstAlive(CpuContext* ctx) {
    ctx->gpr[3] = ListFirstAliveChecked(ctx->gpr[3]);
}
REGISTER_NATIVE_FUNCTION_AS(0x800B9624, MH3_ParticleListFirstAlive, "ParticleListFirstAlive");
extern "C" void MH3_ParticleListNextAlive(CpuContext* ctx) {
    ctx->gpr[3] = ListNextAliveChecked(ctx->gpr[3], ctx->gpr[4]);
}
REGISTER_NATIVE_FUNCTION_AS(0x800B8DAC, MH3_ParticleListNextAlive, "ParticleListNextAlive");
extern "C" void MH3_DrawStrategyFirstAlive(CpuContext* ctx) {
    ctx->gpr[3] = StrategyFirstChecked(ctx->gpr[3]);
}
REGISTER_NATIVE_FUNCTION_AS(0x800C8AE4, MH3_DrawStrategyFirstAlive, "DrawStrategyFirstAlive");
extern "C" void MH3_DrawStrategyLastAlive(CpuContext* ctx) {
    ctx->gpr[3] = StrategyLastChecked(ctx->gpr[3]);
}
REGISTER_NATIVE_FUNCTION_AS(0x800C8C00, MH3_DrawStrategyLastAlive, "DrawStrategyLastAlive");
extern "C" void MH3_DrawStrategyNextAlive(CpuContext* ctx) {
    ctx->gpr[3] = StrategyNextChecked(ctx->gpr[3], ctx->gpr[4]);
}
REGISTER_NATIVE_FUNCTION_AS(0x800C8D1C, MH3_DrawStrategyNextAlive, "DrawStrategyNextAlive");
extern "C" void MH3_DrawStrategyPrevAlive(CpuContext* ctx) {
    ctx->gpr[3] = StrategyPrevChecked(ctx->gpr[3], ctx->gpr[4]);
}
REGISTER_NATIVE_FUNCTION_AS(0x800C8E48, MH3_DrawStrategyPrevAlive, "DrawStrategyPrevAlive");
extern "C" void MH3_ParticleColorRgb(CpuContext* ctx) {
    ColorRgbChecked(ctx->gpr[3], ctx->gpr[4], ctx->gpr[5], ctx->gpr[6]);
}
REGISTER_NATIVE_FUNCTION_AS(0x800AB0BC, MH3_ParticleColorRgb, "ParticleColorRgb");
extern "C" void MH3_ParticleColorAlpha(CpuContext* ctx) {
    ctx->gpr[3] = ColorAlphaChecked(ctx->gpr[3], ctx->gpr[4], ctx->gpr[5]);
}
REGISTER_NATIVE_FUNCTION_AS(0x800AB284, MH3_ParticleColorAlpha, "ParticleColorAlpha");
extern "C" void MH3_ParticleAlphaFlicker(CpuContext* ctx) {
    ctx->gpr[3] = AlphaFlickerChecked(ctx->gpr[3]);
}
REGISTER_NATIVE_FUNCTION_AS(0x800AB460, MH3_ParticleAlphaFlicker, "ParticleAlphaFlicker");
extern "C" void MH3_ParticleManagerModulateColor(CpuContext* ctx) {
    ModulateColorsChecked(ctx->gpr[3], ctx->gpr[4], ctx->gpr[5], ctx->gpr[6]);
}
REGISTER_NATIVE_FUNCTION_AS(0x800AE70C, MH3_ParticleManagerModulateColor, "ParticleManagerModulateColor");
extern "C" void MH3_ParticleRotation(CpuContext* ctx) {
    RotationChecked(ctx->gpr[3], ctx->gpr[4]);
}
REGISTER_NATIVE_FUNCTION_AS(0x800B5518, MH3_ParticleRotation, "ParticleRotation");
extern "C" void MH3_ParticleManagerRotationOffset(CpuContext* ctx) {
    RotationOffsetChecked(ctx->gpr[3], ctx->gpr[4], ctx->gpr[5]);
}
REGISTER_NATIVE_FUNCTION_AS(0x800B56FC, MH3_ParticleManagerRotationOffset, "ParticleManagerRotationOffset");
