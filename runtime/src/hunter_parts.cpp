// The hunter's equipment part models' world-matrix callbacks, RMHP08. C
// reconstructions from the disassembly of the two node callbacks G3dCalcWorld
// (g3d_calc_world.cpp) calls for every node of a part model:
//
//   0x80229508 PartNodeCallbackA(part, ChrAnmResult*, mdl, callback): before
//              a node's world matrix. Node 0 gets a zero translation; nodes
//              2, 3, 12 and 13 turn about Y by the hunter's s16 angle
//              (+0xB2 of the hunter, part +4) scaled per node
//   0x802296FC PartNodeCallbackB(part, CalcWorldCallbackArgs*, mdl,
//              callback): after it. The node's world matrix becomes the
//              hunter model's matrix the part maps it to (part +0x10, -1
//              none); the callback's node counter (u16 +6) advances; for the
//              swinging parts (part +0xC kind 4 or 5) the swing node that
//              drives this node turns it (about X, or by its swing angles
//              through 0x802AD830/0x802AD79C)
//
// The part: +4 the hunter, +8 its model holder (+0x118 the ScnMdl, whose
// +0xEC holds the world matrices), +0xC the kind, +0x10 the node map (s32
// per node), the swing nodes 0x90 bytes each from the hunter's +0x674 (u8
// live +0, u8 mode +1, u8 on +3, u8 seen +6, nodes +0x10/+0x14, angles
// +0x78/+0x80), the kinds' swing node ranges in the u8 pairs at 0x805BB308.
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

float ReadFloat(uint32_t a) {
    return MemoryInline::FlatReadFloat32(a);
}

constexpr uint32_t kSwingRanges = 0x805BB308u;

void CallbackB(CpuContext* ctx) {
    const uint32_t part = ctx->gpr[3], args = ctx->gpr[4], callback = ctx->gpr[6];
    const uint32_t node = MemoryInline::FlatRead16(callback + 6);
    const int32_t mapped = int32_t(Read32(part + 0x10 + 4 * node));

    // 0x80139AC8: the callback's world matrix (*args) set.
    if (mapped != -1) {
        const uint32_t matrices = Read32(Read32(Read32(part + 8) + 0x118) + 0xEC);
        G3dNative::MtxCopy(matrices + uint32_t(mapped) * 0x30, Read32(args));
    }

    MemoryInline::FlatWrite16(callback + 6, uint16_t(node + 1));
    const uint32_t kind = Read32(part + 0xC);

    if ((kind != 5 && kind != 4) || node == 0)
        return;

    const uint32_t first = MemoryInline::FlatRead8(kSwingRanges + 2 * kind), end = MemoryInline::FlatRead8(kSwingRanges + 2 * kind + 1);

    for (uint32_t i = first; uint8_t(i) < end; ++i) {
        const uint32_t base = Read32(part + 4) + uint8_t(i) * 0x90;
        const uint32_t swing = base + 0x674;

        if (!MemoryInline::FlatRead8(swing))
            continue;

        uint32_t side;

        if (node == Read32(base + 0x684))
            side = 0;
        else if (node == Read32(base + 0x688))
            side = 1;
        else
            continue;

        MemoryInline::FlatWrite8(swing + 6, 1);

        if (!MemoryInline::FlatRead8(swing + 3))
            return;

        GuestFrame frame(ctx, 0xA0);
        GuestCall game{ctx};
        const uint32_t m = frame[0x20];
        G3dNative::MtxCopy(Read32(args), m); // 0x80139AE0

        if (MemoryInline::FlatRead8(swing + 1) == 1) {
            // About X by the swing's angle: m's rotation times R (0x800515D8).
            const uint32_t r = frame[0x50];

            for (uint32_t k = 0; k < 0x30; k += 4)
                MemoryInline::FlatWrite32(r + k, k == 0x00 || k == 0x14 || k == 0x28 ? 0x3F800000u : 0u);

            Nw4rMath::RotMatrixX(Read32(swing + 0x78), r);
            game.Call<0x800515D8>({m, r});
            G3dNative::MtxCopy(m, Read32(args));
        } else {
            game.Call<0x802AD830>({swing, m, swing + 0x78, swing + 0x80, side}); // YureNodeAngles
            game.Call<0x802AD79C>({swing, args});
        }

        return;
    }
}

// The matrix and scales go where the original's frame keeps them (r1 - 0xA0
// + 0x38, and the SetMtx scratch below), below the caller's stack pointer.
void CallbackA(uint32_t r1, uint32_t part, uint32_t result, uint32_t callback) {
    const uint32_t node = MemoryInline::FlatRead16(callback + 6);

    if (node == 0) {
        const uint32_t t = r1 - 0xA0 + 8, zero = Read32(0x8079C19Cu); // r2 - 15812

        for (uint32_t k = 0; k < 12; k += 4)
            MemoryInline::FlatWrite32(t + k, zero);

        G3dNative::ChrAnmResultSetTranslation(result, t);
        return;
    }

    if (node != 2 && node != 3 && node != 12 && node != 13)
        return;

    const float a = ReadFloat(0x8079C28Cu), b = ReadFloat(0x8079C290u); // r2 - 15572, r2 - 15568
    const float k = ReadFloat(node == 13 ? 0x8079C238u : 0x8079C2B4u);  // r2 - 15656, r2 - 15532
    const float angle = float(int16_t(MemoryInline::FlatRead16(Read32(part + 4) + 0xB2)));
    const float scaled = k * ((a * angle) / b);
    const uint32_t m = r1 - 0xA0 + 0x38;
    G3dNative::ChrAnmResultGetMtx(result, m);
    const double turn = double(ReadFloat(0x8079C2B8u) + (b * scaled) / a); // r2 - 15528
    const uint32_t units = uint32_t(int32_t(PpcClampIntegerWordInline(turn))); // fctiwz
    Nw4rMath::RotMatrixY(units & 0xFFFF, m);
    G3dNative::ChrAnmResultSetMtx(result, m, r1 - 0xA0 - 0x50 + 8);
}

void CallbackAPort(CpuContext* p) {
    CallbackA(p->gpr[1], p->gpr[3], p->gpr[4], p->gpr[6]);
}

} // namespace

extern "C" void func_802296FC(CpuContext* ctx);
extern "C" void MH3_PartNodeCallbackB(CpuContext* ctx) {
    PortCheck::Call(ctx, "PartNodeCallbackB", func_802296FC, CallbackB);
}
REGISTER_NATIVE_FUNCTION_AS(0x802296FC, MH3_PartNodeCallbackB, "PartNodeCallbackB", kNativeKeepsNonvolatile);

extern "C" void func_80229508(CpuContext* ctx);
static void CallbackAChecked(uint32_t r1, uint32_t r3, uint32_t r4, uint32_t r6) {
#ifdef MH3_DEVELOPMENT
    if (PortCheck::Enabled("PartNodeCallbackA")) [[unlikely]] {
        CpuContext* c = TryGetCpuContext();
        c->gpr[1] = r1;
        c->gpr[2] = 0x8079FF60u;
        c->gpr[3] = r3;
        c->gpr[4] = r4;
        c->gpr[6] = r6;
        PortCheck::RunOutput(c, "PartNodeCallbackA", func_80229508, CallbackAPort, {{r4, 0x4C}});
        return;
    }
#endif
    CallbackA(r1, r3, r4, r6);
}

extern "C" void MH3_PartNodeCallbackA(CpuContext* ctx) {
    CallbackAChecked(ctx->gpr[1], ctx->gpr[3], ctx->gpr[4], ctx->gpr[6]);
}
REGISTER_NATIVE_FUNCTION_AS(0x80229508, MH3_PartNodeCallbackA, "PartNodeCallbackA");
