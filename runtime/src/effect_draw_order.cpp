// nw4r ef drawing above the draw strategies, RMHP08: C reconstructions from
// the Ghidra decompilation. Per effect and per particle manager each was a
// translated frame and an indirect dispatch around a few loads; the ports call
// the next level's native ports directly.
//
//   0x800A51B8 EffectDraw                  the effect's draw order, unless +0x54 bit 2
//   0x800A3A08 DrawOrderDraw               each manager of the effect (+0x94):
//                                          its draw matrix, then its Draw
//   0x800AE68C ParticleManagerDrawMtx      0x806969E8 = inverse(emitter global) *
//                                          manager local
//   0x800ADF3C ParticleManagerDraw         a copy of the DrawInfo (with the
//                                          emitter's depth offset), then the
//                                          draw strategy's Draw
//   0x800AE308 DrawInfoCopy                the DrawInfo fields DrawInfo's copy
//                                          assignment copies
//   0x800B9AE4 DrawBillboardStrategyDraw   by the resource's billboard kind:
//                                          DrawBillboard (0, 3) or the two
//                                          translated other kinds (1, 2)
//
// Offsets: Effect 0x54 flags, 0x94 ut::List of ParticleManager (draw order),
// 0xA0 DrawOrder*; ParticleManager 0x1C vtable, 0x20 Emitter*, 0x24
// ResEmitter, 0x2C DrawStrategy*; the emitter resource's body is at +0x9C
// (flags u16 at +0x94 of +8, depth offset f32 at +0xB4, billboard kind at
// +0xAD of the body).
#include "abi_bridge.h"
#include "guest_call.h"
#include "memory.h"
#include "port_check.h"

#include <cstdint>

extern "C" void MH3_DrawBillboard(CpuContext* ctx); // 0x800B9E5C, effect_billboard.cpp

namespace {
using MemoryInline::FlatRead16;
using MemoryInline::FlatRead32;
using MemoryInline::FlatRead8;
using MemoryInline::FlatReadFloat32;
using MemoryInline::FlatWrite32;
using MemoryInline::FlatWrite8;
using MemoryInline::FlatWriteFloat32;

uint32_t ListNext(uint32_t list, uint32_t object) {
    return object ? FlatRead32(object + FlatRead16(list + 0xA) + 4) : FlatRead32(list);
}

void DrawInfoCopy(uint32_t to, uint32_t from) {
    for (uint32_t k = 0; k < 0x60; k += 4) // two Mtx34
        FlatWrite32(to + k, FlatRead32(from + k));
    FlatWrite8(to + 0x60, FlatRead8(from + 0x60));
    FlatWrite32(to + 0x64, FlatRead32(from + 0x64));
    FlatWrite32(to + 0x68, FlatRead32(from + 0x68));
    FlatWrite8(to + 0x6C, FlatRead8(from + 0x6C));

    for (uint32_t k = 0x70; k < 0x98; k += 4) // 0x70..0x88, then the VEC3 at 0x8C
        FlatWrite32(to + k, FlatRead32(from + k));
    FlatWrite32(to + 0x98, FlatRead32(from + 0x98));
    FlatWrite32(to + 0x9C, FlatRead32(from + 0x9C));
}

void DrawMtx(CpuContext* cpu, uint32_t manager) {
    GuestFrame frame(cpu, 0x70);
    GuestCall game{cpu};
    const uint32_t global = frame[0x38], local = frame[0x08];
    game.Call<0x800A9508>({FlatRead32(manager + 0x20), global}); // EmitterCalcGlobalMtx
    game.Call<0x800AE3C4>({manager, local});                     // ParticleManagerLocalMtx
    game.Call<0x80088428>({global, global});                     // MtxInverseWrapper
    game.Call<0x80071120>({0x806969E8, global, local});          // MtxConcatWrapper
}

void BillboardStrategyDraw(CpuContext* cpu, uint32_t strategy, uint32_t info, uint32_t manager) {
    const uint32_t body = FlatRead32(manager + 0x24) + 0x9C;

    switch (FlatRead8(body + 0xAD)) {
    case 0:
    case 3:
        cpu->gpr[3] = strategy, cpu->gpr[4] = info, cpu->gpr[5] = manager;
        MH3_DrawBillboard(cpu);
        break;
    case 1:
        GuestCall{cpu}.Call<0x800BA8B8>({strategy, info, manager});
        break;
    case 2:
        GuestCall{cpu}.Call<0x800BB020>({strategy, info, manager});
        break;
    default:
        break;
    }
}

void ManagerDraw(CpuContext* cpu, uint32_t manager, uint32_t info) {
    const uint32_t resource = FlatRead32(manager + 0x24);

    if ((FlatRead16(resource + 8 + 0x94) & 0x400) || (FlatRead32(FlatRead32(manager + 0x20) + 0x20) & 2))
        return;

    GuestFrame frame(cpu, 0x110);
    const uint32_t copy = frame[0x48], matrix = frame[0x18], position = frame[0x08];
    DrawInfoCopy(copy, info);
    const float offset = FlatReadFloat32(resource + 0x9C + 0xB4);

    if (FlatReadFloat32(0x8079856C) != offset) {
        GuestCall{cpu}.Call<0x800A9508>({FlatRead32(manager + 0x20), matrix}); // EmitterCalcGlobalMtx
        // The emitter's position (Vec3Set 0x80041EF0 from its global matrix)
        // becomes the copy's depth anchor (0x800AE2FC).
        FlatWrite32(position, FlatRead32(matrix + 0x0C));
        FlatWrite32(position + 4, FlatRead32(matrix + 0x1C));
        FlatWrite32(position + 8, FlatRead32(matrix + 0x2C));
        FlatWriteFloat32(copy + 0x88, offset);

        for (uint32_t k = 0; k < 12; k += 4)
            FlatWrite32(copy + 0x8C + k, FlatRead32(position + k));
    }

    const uint32_t strategy = FlatRead32(manager + 0x2C);
    const uint32_t fn = FlatRead32(FlatRead32(strategy) + 0xC);

    if (fn == 0x800B9AE4)
        BillboardStrategyDraw(cpu, strategy, copy, manager);
    else
        GuestCall{cpu}(fn, {strategy, copy, manager});
}

void DrawOrderDraw(CpuContext* cpu, uint32_t effect, uint32_t info) {
    const uint32_t list = effect + 0x94;

    for (uint32_t m = ListNext(list, 0); m; m = ListNext(list, m)) {
        DrawMtx(cpu, m);
        const uint32_t fn = FlatRead32(FlatRead32(m + 0x1C) + 0x1C);

        if (fn == 0x800ADF3C)
            ManagerDraw(cpu, m, info);
        else
            GuestCall{cpu}(fn, {m, info});
    }
}

void EffectDraw(CpuContext* cpu, uint32_t effect, uint32_t info) {
    if (FlatRead32(effect + 0x54) & 2)
        return;

    const uint32_t order = FlatRead32(effect + 0xA0);
    const uint32_t fn = FlatRead32(FlatRead32(order) + 0x10);

    if (fn == 0x800A3A08)
        DrawOrderDraw(cpu, effect, info);
    else
        GuestCall{cpu}(fn, {order, effect, info});
}

void EffectDrawPort(CpuContext* cpu) {
    EffectDraw(cpu, cpu->gpr[3], cpu->gpr[4]);
}

void DrawOrderDrawPort(CpuContext* cpu) {
    DrawOrderDraw(cpu, cpu->gpr[4], cpu->gpr[5]);
}

void DrawMtxPort(CpuContext* cpu) {
    DrawMtx(cpu, cpu->gpr[3]);
}

void ManagerDrawPort(CpuContext* cpu) {
    ManagerDraw(cpu, cpu->gpr[3], cpu->gpr[4]);
}

void DrawInfoCopyPort(CpuContext* cpu) {
    DrawInfoCopy(cpu->gpr[3], cpu->gpr[4]);
}

void BillboardStrategyDrawPort(CpuContext* cpu) {
    BillboardStrategyDraw(cpu, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5]);
}
} // namespace

extern "C" void func_800A51B8(CpuContext* ctx);
extern "C" void MH3_EffectDraw(CpuContext* ctx) {
    PortCheck::Call(ctx, "EffectDraw", func_800A51B8, EffectDrawPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x800A51B8, MH3_EffectDraw, "EffectDraw");

extern "C" void func_800A3A08(CpuContext* ctx);
extern "C" void MH3_DrawOrderDraw(CpuContext* ctx) {
    PortCheck::Call(ctx, "DrawOrderDraw", func_800A3A08, DrawOrderDrawPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x800A3A08, MH3_DrawOrderDraw, "DrawOrderDraw");

extern "C" void func_800AE68C(CpuContext* ctx);
extern "C" void MH3_ParticleManagerDrawMtx(CpuContext* ctx) {
    PortCheck::Call(ctx, "ParticleManagerDrawMtx", func_800AE68C, DrawMtxPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x800AE68C, MH3_ParticleManagerDrawMtx, "ParticleManagerDrawMtx");

extern "C" void func_800ADF3C(CpuContext* ctx);
extern "C" void MH3_ParticleManagerDraw(CpuContext* ctx) {
    PortCheck::Call(ctx, "ParticleManagerDraw", func_800ADF3C, ManagerDrawPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x800ADF3C, MH3_ParticleManagerDraw, "ParticleManagerDraw");

extern "C" void func_800AE308(CpuContext* ctx);
extern "C" void MH3_DrawInfoCopy(CpuContext* ctx) {
    PortCheck::Call(ctx, "DrawInfoCopy", func_800AE308, DrawInfoCopyPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x800AE308, MH3_DrawInfoCopy, "DrawInfoCopy");

extern "C" void func_800B9AE4(CpuContext* ctx);
extern "C" void MH3_DrawBillboardStrategyDraw(CpuContext* ctx) {
    PortCheck::Call(ctx, "DrawBillboardStrategyDraw", func_800B9AE4, BillboardStrategyDrawPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x800B9AE4, MH3_DrawBillboardStrategyDraw, "DrawBillboardStrategyDraw");
