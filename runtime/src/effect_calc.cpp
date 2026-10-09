// nw4r::ef Effect::Calc and the list walks around ParticleManager::Calc,
// RMHP08: C reconstructions from the Ghidra decompilation. The translated
// originals spent their time on stack frames, register flushes and indirect
// dispatch around a few list walks; the ports walk the lists on host values and
// call the emitters' and managers' virtual functions directly where they are
// native ports.
//
//   0x800A4C20 EffectCalc               (effect_step.cpp runs it per pass)
//   0x800A8D94 EmitterCalcParticle      Calc of each of the emitter's managers
//   0x800AE564 ParticleManagerBeginCalc pending child counts down, evaluated
//                                       particles (+0xD8 1) back to waiting (0)
//   0x800AE614 ParticleManagerEndCalc   held particles (+0xD8 3) back to 1
//   0x8009B3D8 EffUtilListToArray       a ut::List's objects into an array
//
// Offsets (nw4r ef, as Wii Sports' decompiled ef):
//   Effect: 0x20 EffectSystem*, 0x24 ut::List of Emitter, 0x54 flags (1 stop
//     calc, 0x10000 more emission pending this calc).
//   Emitter: 0x0C life status, 0x1C vtable, 0x20 flags (0x200 no particle
//     calc), 0x24 flags (0x8000, 0x10000 billboard calc), 0xB4 emission state,
//     0xC0 ut::List of ParticleManager, 0xE4 tick, 0xE8 pending emissions,
//     0x108 translation of the last calc, 0x114 its copy before this one.
//   ParticleManager: 0x1C vtable, 0x38 ut::List of Particle (link offset at
//     0x42), 0x54 last particle calced.
//   Particle: 0x0C life status, 0xD8 evaluation state, 0xE2 pending children.
#include "abi_bridge.h"
#include "guest_call.h"
#include "memory.h"
#include "port_check.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <stdexcept>

extern "C" void MH3_EmitterCalcEmitter(CpuContext* ctx);   // 0x800A8AC0, effect_step.cpp
extern "C" void MH3_EmitterCalcEmission(CpuContext* ctx);  // 0x800A8E5C, effect_step.cpp
extern "C" void MH3_ParticleManagerCalc(CpuContext* ctx);  // 0x800AC35C, effect_step.cpp
extern "C" void MH3_EmitterCalcParticle(CpuContext* ctx);  // 0x800A8D94, below

namespace {
using MemoryInline::FlatRead16;
using MemoryInline::FlatRead32;
using MemoryInline::FlatWrite16;
using MemoryInline::FlatWrite32;

[[noreturn]] void Panic(const char* what, uint32_t value) {
    char text[128];
    std::snprintf(text, sizeof text, "NW4R panic: %s (%u)", what, value);
    throw std::runtime_error(text);
}

// nw4r::ut::List_GetNext (0x805022E0): the head for 0, else the link's next.
uint32_t ListNext(uint32_t list, uint32_t object) {
    return object ? FlatRead32(object + FlatRead16(list + 0xA) + 4) : FlatRead32(list);
}

uint32_t ListCount(uint32_t list) {
    return FlatRead16(list + 8);
}

// EffUtilListToArray (0x8009B3D8): the objects in list order, up to `limit`
// (at least one: the original stores before it tests). The u16 count.
template <size_t N>
uint32_t ListToArray(uint32_t list, std::array<uint32_t, N>& out, uint32_t limit) {
    uint32_t n = 0;

    for (uint32_t object = ListNext(list, 0); object && n < N; object = ListNext(list, object)) {
        out[n++] = object;

        if (!(int32_t(n & 0xFFFF) < int32_t(limit)))
            break;
    }

    return n & 0xFFFF;
}

// A virtual function of an emitter or manager (vtable at +0x1C), calling the
// native ports of the common targets directly.
void CallVirtual(CpuContext* cpu, uint32_t object, uint32_t slot) {
    const uint32_t fn = FlatRead32(FlatRead32(object + 0x1C) + slot);
    cpu->gpr[3] = object;

    switch (fn) {
    case 0x800A8AC0:
        MH3_EmitterCalcEmitter(cpu);
        return;
    case 0x800A8D94:
        MH3_EmitterCalcParticle(cpu);
        return;
    case 0x800A8E5C:
        MH3_EmitterCalcEmission(cpu);
        return;
    case 0x800AC35C:
        MH3_ParticleManagerCalc(cpu);
        return;
    default:
        GuestCall{cpu}(fn, {object});
    }
}

void BeginCalc(uint32_t manager, bool pendingOnly) {
    FlatWrite32(manager + 0x54, 0);
    const uint32_t link = FlatRead16(manager + 0x42) + 4;

    for (uint32_t p = FlatRead32(manager + 0x38); p; p = FlatRead32(p + link)) {
        const uint16_t pending = FlatRead16(p + 0xE2);

        if (pendingOnly && pending == 0)
            continue;

        if (pending != 0)
            FlatWrite16(p + 0xE2, uint16_t(pending - 1));

        if (FlatRead32(p + 0x0C) == 1 && FlatRead32(p + 0xD8) == 1)
            FlatWrite32(p + 0xD8, 0);
    }
}

void EndCalc(uint32_t manager) {
    const uint32_t link = FlatRead16(manager + 0x42) + 4;

    for (uint32_t p = FlatRead32(manager + 0x38); p; p = FlatRead32(p + link))
        if (FlatRead32(p + 0x0C) == 1 && FlatRead32(p + 0xD8) == 3)
            FlatWrite32(p + 0xD8, 1);
}

void ForEachManager(uint32_t emitter, void (*fn)(uint32_t manager, bool), bool flag) {
    const uint32_t list = emitter + 0xC0;

    for (uint32_t m = ListNext(list, 0); m; m = ListNext(list, m))
        fn(m, flag);
}

bool LiveEmitter(uint32_t emitter) {
    return FlatRead32(emitter + 0x0C) - 1 < 2;
}

bool BillboardEmitter(uint32_t emitter) {
    return (FlatRead32(emitter + 0x24) & 0x18000) != 0;
}

} // namespace

// Effect::Calc(r4 bit 0: billboard emitters only; bit 1: no begin pass).
void EffectCalcNative(CpuContext* cpu) {
    const uint32_t effect = cpu->gpr[3], flags = cpu->gpr[4];
    const uint32_t emitters = effect + 0x24;

    if (flags & 1) {
        for (uint32_t e = ListNext(emitters, 0); e; e = ListNext(emitters, e))
            if (LiveEmitter(e) && BillboardEmitter(e))
                CallVirtual(cpu, e, 0x24);
        return;
    }

    if (FlatRead32(effect + 0x54) & 1)
        return;

    GuestFrame frame(cpu, 0x40);

    if (!(flags & 2)) {
        for (uint32_t e = ListNext(emitters, 0); e; e = ListNext(emitters, e)) {
            if (LiveEmitter(e) && FlatRead32(e + 0xB4) == 1)
                FlatWrite32(e + 0xB4, 0);

            for (uint32_t k = 0; k < 12; k += 4) // Vec3Copy(e + 0x114, e + 0x108)
                FlatWrite32(e + 0x114 + k, FlatRead32(e + 0x108 + k));

            ForEachManager(e, BeginCalc, false);
        }
    }

    FlatWrite32(effect + 0x54, FlatRead32(effect + 0x54) | 0x10000); // EffectSetPendingFlag(1)
    std::array<uint32_t, 0x200> array;
    const auto snapshot = [&](uint32_t line) {
        const uint32_t count = ListCount(emitters);

        if (count > 0x1FF)
            Panic("ef_effect.cpp: UtlistSize(&mListEmitter) <= EMITTER_MAX", line);

        return ListToArray(emitters, array, count);
    };

    while (FlatRead32(effect + 0x54) & 0x10000) {
        FlatWrite32(effect + 0x54, FlatRead32(effect + 0x54) & ~0x10000u);

        for (;;) {
            uint32_t n = snapshot(0x10B);

            for (uint32_t i = 0; i < n; ++i)
                CallVirtual(cpu, array[i], 0x18); // CalcEmitter
            for (uint32_t e = ListNext(emitters, 0); e; e = ListNext(emitters, e))
                if (LiveEmitter(e) && BillboardEmitter(e))
                    CallVirtual(cpu, e, 0x24); // CalcBillboard
            n = snapshot(0x124);

            for (uint32_t i = 0; i < n; ++i) {
                const uint32_t e = array[i];

                // CalcParticle at the tick this calc's emission will reach.
                if (FlatRead32(e + 0xB4) == 0)
                    FlatWrite32(e + 0xE4, FlatRead32(e + 0xE4) + 1);

                CallVirtual(cpu, e, 0x1C);

                if (FlatRead32(e + 0xB4) == 0)
                    FlatWrite32(e + 0xE4, FlatRead32(e + 0xE4) - 1);
            }

            n = snapshot(0x138);

            for (uint32_t i = 0; i < n; ++i)
                CallVirtual(cpu, array[i], 0x20); // CalcEmission
            n = snapshot(0x143);

            for (uint32_t i = 0; i < n; ++i)
                CallVirtual(cpu, array[i], 0x1C); // CalcParticle of the new particles
            const uint32_t queue = FlatRead32(effect + 0x20) + 0x10;

            if (FlatRead32(queue) == 0)
                break;

            GuestCall{cpu}.Call<0x800A377C>({queue}); // the queued child emissions
        }

        if (FlatRead32(effect + 0x54) & 0x10000)
            for (uint32_t e = ListNext(emitters, 0); e; e = ListNext(emitters, e)) {
                if (FlatRead32(e + 0x0C) == 1) {
                    const uint16_t pending = FlatRead16(e + 0xE8);

                    if (pending != 0) {
                        FlatWrite16(e + 0xE8, uint16_t(pending - 1));

                        if (FlatRead32(e + 0xB4) == 1)
                            FlatWrite32(e + 0xB4, 0);
                    }
                }

                ForEachManager(e, BeginCalc, true);
            }
    }

    const uint32_t matrix = frame[0x10];

    for (uint32_t e = ListNext(emitters, 0); e; e = ListNext(emitters, e)) {
        if (LiveEmitter(e) && FlatRead32(e + 0xB4) == 3)
            FlatWrite32(e + 0xB4, 1);

        const uint32_t list = e + 0xC0;

        for (uint32_t m = ListNext(list, 0); m; m = ListNext(list, m))
            EndCalc(m);

        GuestCall{cpu}.Call<0x800A9508>({e, matrix}); // EmitterCalcGlobalMtx
        FlatWrite32(e + 0x108, FlatRead32(matrix + 0x0C));
        FlatWrite32(e + 0x10C, FlatRead32(matrix + 0x1C));
        FlatWrite32(e + 0x110, FlatRead32(matrix + 0x2C));
    }
}

// Emitter::CalcParticle: each manager's Calc, unless the emitter stops them.
static void EmitterCalcParticlePort(CpuContext* cpu) {
    const uint32_t emitter = cpu->gpr[3];

    if (FlatRead32(emitter + 0x20) & 0x200)
        return;

    const uint32_t list = emitter + 0xC0, count = ListCount(list);

    if (count > 0x3FF)
        Panic("ef_emitter.cpp: UtlistSize(&mManagerEP) <= MANAGER_MAX", count);

    std::array<uint32_t, 0x400> managers;
    const uint32_t n = ListToArray(list, managers, count);

    for (uint32_t i = 0; i < n; ++i)
        CallVirtual(cpu, managers[i], 0x18);
}

extern "C" void func_800A8D94(CpuContext* ctx);
extern "C" void MH3_EmitterCalcParticle(CpuContext* ctx) {
    PortCheck::Call(ctx, "EmitterCalcParticle", func_800A8D94, EmitterCalcParticlePort);
}
REGISTER_NATIVE_FUNCTION_AS(0x800A8D94, MH3_EmitterCalcParticle, "EmitterCalcParticle");

static void ParticleManagerBeginCalcPort(CpuContext* cpu) {
    BeginCalc(cpu->gpr[3], cpu->gpr[4] != 0);
}

extern "C" void func_800AE564(CpuContext* ctx);
extern "C" void MH3_ParticleManagerBeginCalc(CpuContext* ctx) {
    PortCheck::Call(ctx, "ParticleManagerBeginCalc", func_800AE564, ParticleManagerBeginCalcPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x800AE564, MH3_ParticleManagerBeginCalc, "ParticleManagerBeginCalc");

static void ParticleManagerEndCalcPort(CpuContext* cpu) {
    EndCalc(cpu->gpr[3]);
}

extern "C" void func_800AE614(CpuContext* ctx);
extern "C" void MH3_ParticleManagerEndCalc(CpuContext* ctx) {
    PortCheck::Call(ctx, "ParticleManagerEndCalc", func_800AE614, ParticleManagerEndCalcPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x800AE614, MH3_ParticleManagerEndCalc, "ParticleManagerEndCalc");

// EffUtilListToArray(list, u32* array, s32 limit) -> u16 count.
static void ListToArrayPort(CpuContext* cpu) {
    const uint32_t list = cpu->gpr[3], array = cpu->gpr[4];
    const int32_t limit = int32_t(cpu->gpr[5]);

    if (!list)
        Panic("ef_util.cpp: list != NULL", 0x25);

    if (!array)
        Panic("ef_util.cpp: array != NULL", 0x26);

    uint32_t n = 0;

    for (uint32_t object = ListNext(list, 0); object;) {
        FlatWrite32(array + (n & 0xFFFF) * 4, object);
        ++n;

        if (!(int32_t(n & 0xFFFF) < limit))
            break;

        object = ListNext(list, object);
    }

    cpu->gpr[3] = n;
}

extern "C" void func_8009B3D8(CpuContext* ctx);
extern "C" void MH3_EffUtilListToArray(CpuContext* ctx) {
    PortCheck::Call(ctx, "EffUtilListToArray", func_8009B3D8, ListToArrayPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8009B3D8, MH3_EffUtilListToArray, "EffUtilListToArray");
