// MHchar's per-step model update, RMHP08. C reconstructions from the
// disassembly of mhchar.cpp:
//
//   0x800E078C MHchar::move(u16 arg): the owner's pre-move hook, the model's
//              scale, the root motion (MHcharRootMotion 0x800E19D4), the
//              world matrix (0x800E0978) and the follow-up effect
//   0x800E0978 MHcharSetWorld(ch): the model's root matrix (ScnObj +0xC)
//              from the angles (ZXY) and position plus offset
//   0x800E3154 MHchar::setVisibility(node, visible): the node's visibility
//              byte in the model's copy (+0x148, by node); a change marks
//              the model (+0x13C | 1)
//
// The MHchar: +0x4 position, +0x10 position offset, +0x1C scale, +0x28 u32
// angles x, y, z (16-bit angles, cpSetRotMatrixZXY), u8 +0x34 effect on, u8
// +0x35 its kind, u32 +0x38 flags (1 no effect, 2 no world matrix), +0x3C
// effect argument, +0x40 owner object, +0xEC owner argument, +0x118 the
// model (ScnMdl), +0x160 hooks on.
#include "abi_bridge.h"
#include "g3d_native.h"
#include "guest_call.h"
#include "memory.h"
#include "nw4r_math.h"
#include "port_check.h"

#include <cstdint>

namespace {

uint32_t Read32(uint32_t a) {
    return MemoryInline::FlatRead32(a);
}

void Write32(uint32_t a, uint32_t v) {
    MemoryInline::FlatWrite32(a, v);
}

constexpr uint32_t kModel = 0x118;

// Memos of the virtual calls (GuestCall::VirtualAt).
const RawDispatchRecord* modelMemo = nullptr;
const RawDispatchRecord* ownerMemo = nullptr;

// 0x800E086C: the model's scale (+0xDC) from the character's.
void CopyScale(uint32_t model, uint32_t ch) {
    for (uint32_t k = 0; k < 12; k += 4)
        Write32(model + 0xDC + k, Read32(ch + 0x1C + k));
}

// `m` is a 0x30-byte guest buffer for the matrix.
void SetWorld(GuestCall& game, uint32_t ch, uint32_t m) {
    const uint32_t model = Read32(ch + kModel);

    if (!model)
        return;

    CopyScale(model, ch);
    Nw4rMath::RotMatrixZXY(ch + 0x28, m);

    for (uint32_t k = 0; k < 3; ++k) {
        const float sum = MemoryInline::FlatReadFloat32(ch + 4 + 4 * k) + MemoryInline::FlatReadFloat32(ch + 0x10 + 4 * k);
        MemoryInline::FlatWriteFloat32(m + 0xC + 0x10 * k, sum);
    }

    // ScnObj::SetMtx(0, m) (0x800816CC): the world matrix set, its "ignore"
    // flag (+0xCC & 0x80000000) cleared.
    Write32(model + 0xCC, Read32(model + 0xCC) & ~0x80000000u);
    G3dNative::MtxCopy(m, model + 0xC);

    if (Read32(ch + 0x160))
        game.VirtualAt(modelMemo, model, 0xC, {1, 0, 0});
}

void SetWorldPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x40);
    GuestCall game{ctx};
    SetWorld(game, frame.Arg(3), frame[8]);
}

void Move(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x40);
    GuestCall game{ctx};
    const uint32_t ch = frame.Arg(3), arg = frame.Arg(4);

    if (!Read32(ch + kModel))
        return;

    if (Read32(ch + 0x160)) {
        if (const uint32_t owner = Read32(ch + 0x40))
            game.VirtualAt(ownerMemo, owner, 0x18, {Read32(ch + kModel), 1, Read32(ch + 0xEC)});
    }

    CopyScale(Read32(ch + kModel), ch);
    game.Call<0x800E19D4>({ch}); // MHcharRootMotion

    if (!(Read32(ch + 0x38) & 2))
        SetWorld(game, ch, frame[8]);

    if (MemoryInline::FlatRead8(ch + 0x34) && !(Read32(ch + 0x38) & 1)) {
        const uint32_t effect = game.Call<0x800E3BF0>({0, MemoryInline::FlatRead8(ch + 0x35), Read32(ch + kModel), 0, arg & 0xFFFF, 0, Read32(ch + 0x3C)});
        Write32(effect + 0x2C, ch);
    }
}

void SetVisibility(uint32_t ch, uint32_t node, uint32_t visible) {
    const uint32_t model = Read32(ch + kModel);

    if (!model || !G3dNative::ResDicAtOffset14(model + 0xE8, node))
        return;

    const uint32_t copy = Read32(model + 0x148);

    if (!copy)
        return;

    const uint32_t flag = copy + node;
    const bool shown = MemoryInline::FlatRead8(flag) != 0;

    if (shown == (visible != 0))
        return;

    Write32(model + 0x13C, Read32(model + 0x13C) | 1);
    MemoryInline::FlatWrite8(flag, visible ? 1 : 0);
}

void SetVisibilityPort(CpuContext* p) {
    SetVisibility(p->gpr[3], p->gpr[4], p->gpr[5]);
}

} // namespace

extern "C" void func_800E078C(CpuContext* ctx);
extern "C" void MH3_MHcharMove(CpuContext* ctx) {
    PortCheck::Call(ctx, "MHcharMove", func_800E078C, Move);
}
REGISTER_NATIVE_FUNCTION_AS(0x800E078C, MH3_MHcharMove, "MHcharMove");

extern "C" void func_800E0978(CpuContext* ctx);
extern "C" void MH3_MHcharSetWorld(CpuContext* ctx) {
    PortCheck::Call(ctx, "MHcharSetWorld", func_800E0978, SetWorldPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x800E0978, MH3_MHcharSetWorld, "MHcharSetWorld");

extern "C" void func_800E3154(CpuContext* ctx);
static void SetVisibilityChecked([[maybe_unused]] uint32_t r1, uint32_t r3, uint32_t r4, uint32_t r5) {
#ifdef MH3_DEVELOPMENT
    if (PortCheck::Enabled("MHcharSetVisibility")) [[unlikely]] {
        CpuContext* c = TryGetCpuContext();
        c->gpr[1] = r1;
        c->gpr[3] = r3;
        c->gpr[4] = r4;
        c->gpr[5] = r5;
        PortCheck::Run(c, "MHcharSetVisibility", func_800E3154, SetVisibilityPort);
        return;
    }
#endif
    SetVisibility(r3, r4, r5);
}

extern "C" void MH3_MHcharSetVisibility(CpuContext* ctx) {
    SetVisibilityChecked(ctx->gpr[1], ctx->gpr[3], ctx->gpr[4], ctx->gpr[5]);
}
REGISTER_NATIVE_FUNCTION_AS(0x800E3154, MH3_MHcharSetVisibility, "MHcharSetVisibility");
