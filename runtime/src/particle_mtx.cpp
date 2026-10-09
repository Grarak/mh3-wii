// NW4R ef emitter and particle-manager matrices, RMHP08. C reconstructions
// from the disassembly of ef_emitter.cpp and ef_particlemanager.cpp, which the
// native effect calculation and draw (effect_calc.cpp, effect_draw_order.cpp,
// native_effect_draw.cpp) call for every emitter and manager:
//
//   0x800A9508 EmitterCalcGlobalMtx(emitter, out): the emitter's global
//              matrix (+0x124), recomputed when marked dirty (u8 +0x120):
//              its effect's matrix (+0xBC, +0x58) or, under a parent
//              emitter (+0xF4), the parent's inherited through 0x800A9110
//              by the emitter's inherit flags (u8 +0x64: 1 rotation, 2
//              scale, 4 translation; s8 +0x65 the share), then its
//              translation (+0x90), rotation (+0xA8, 0x8009CA94) and scale
//              (+0x9C, 0x8009CC04). Copied to out (returned) when given,
//              else the matrix itself is returned
//   0x800AE3C4 ParticleManagerLocalMtx(manager, out): the manager's matrix
//              (+0x8C) to out, recomputed when dirty (u8 +0x8A) from its
//              emitter's (+0x20) global matrix through 0x800A9110 by the
//              manager's flags (+0x28, s8 +0x88)
//
// The originals assert their pointers; the ports drop those debug checks.
// Clean matrices (the common case) are a copy without a guest frame.
#include "abi_bridge.h"
#include "g3d_native.h"
#include "guest_call.h"
#include "memory.h"
#include "port_check.h"

#include <cstdint>

namespace {

uint32_t Read32(uint32_t a) {
    return MemoryInline::FlatRead32(a);
}

constexpr uint32_t kGlobalDirty = 0x120, kGlobal = 0x124;

uint32_t EmitterGlobal(CpuContext* ctx, uint32_t emitter, uint32_t out);

// 0x800A9110 (out, parent, rotation, scale, share, translation): the inherited
// matrix.
void Inherit(GuestCall& game, uint32_t out, uint32_t parent, uint32_t flags, uint32_t share) {
    game.Call<0x800A9110>({out, parent, (flags & 1) != 0, (flags & 2) != 0, uint32_t(int32_t(int8_t(share))), (flags & 4) != 0});
}

void EmitterRecalc(CpuContext* ctx, uint32_t emitter) {
    GuestFrame frame(ctx, 0x70);
    GuestCall game{ctx};
    const uint32_t global = emitter + kGlobal, rotation = frame[0x38], parentMtx = frame[8];

    if (const uint32_t parent = Read32(emitter + 0xF4)) {
        EmitterGlobal(ctx, parent, parentMtx);
        Inherit(game, global, parentMtx, MemoryInline::FlatRead8(emitter + 0x64), MemoryInline::FlatRead8(emitter + 0x65));
    } else {
        G3dNative::MtxCopy(Read32(emitter + 0xBC) + 0x58, global);
    }

    game.Call<0x80501A10>({global, global, emitter + 0x90}); // MTX34MultTrans
    game.Call<0x8009CA94>({rotation},
                          {MemoryInline::FlatReadFloat32(emitter + 0xA8), MemoryInline::FlatReadFloat32(emitter + 0xAC), MemoryInline::FlatReadFloat32(emitter + 0xB0)});
    G3dNative::MtxConcat(global, rotation, global);
    game.Call<0x8009CC04>({global, global, emitter + 0x9C});
    MemoryInline::FlatWrite8(emitter + kGlobalDirty, 0);
}

uint32_t EmitterGlobal(CpuContext* ctx, uint32_t emitter, uint32_t out) {
    if (MemoryInline::FlatRead8(emitter + kGlobalDirty))
        EmitterRecalc(ctx, emitter);

    if (!out)
        return emitter + kGlobal;

    G3dNative::MtxCopy(emitter + kGlobal, out);
    return out;
}

void ManagerRecalc(CpuContext* ctx, uint32_t manager) {
    GuestFrame frame(ctx, 0x40);
    GuestCall game{ctx};
    const uint32_t emitterMtx = frame[8];
    EmitterGlobal(ctx, Read32(manager + 0x20), emitterMtx);
    Inherit(game, manager + 0x8C, emitterMtx, Read32(manager + 0x28), MemoryInline::FlatRead8(manager + 0x88));
    MemoryInline::FlatWrite8(manager + 0x8A, 0);
}

uint32_t ManagerLocal(CpuContext* ctx, uint32_t manager, uint32_t out) {
    if (MemoryInline::FlatRead8(manager + 0x8A))
        ManagerRecalc(ctx, manager);

    G3dNative::MtxCopy(manager + 0x8C, out);
    return out;
}

void EmitterGlobalPort(CpuContext* ctx) {
    ctx->gpr[3] = EmitterGlobal(ctx, ctx->gpr[3], ctx->gpr[4]);
}

void ManagerLocalPort(CpuContext* ctx) {
    ctx->gpr[3] = ManagerLocal(ctx, ctx->gpr[3], ctx->gpr[4]);
}

} // namespace

extern "C" void func_800A9508(CpuContext* ctx);
extern "C" void MH3_EmitterCalcGlobalMtx(CpuContext* ctx) {
    PortCheck::Call(ctx, "EmitterCalcGlobalMtx", func_800A9508, EmitterGlobalPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x800A9508, MH3_EmitterCalcGlobalMtx, "EmitterCalcGlobalMtx", kNativeKeepsNonvolatile);

extern "C" void func_800AE3C4(CpuContext* ctx);
extern "C" void MH3_ParticleManagerLocalMtx(CpuContext* ctx) {
    PortCheck::Call(ctx, "ParticleManagerLocalMtx", func_800AE3C4, ManagerLocalPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x800AE3C4, MH3_ParticleManagerLocalMtx, "ParticleManagerLocalMtx", kNativeKeepsNonvolatile);
