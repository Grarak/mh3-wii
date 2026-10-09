// Effect texture setup, RMHP08. C reconstruction of EffectDrawTextureSetup
// (0x800C7D44) from the disassembly.
//
// Setup(draw, emitter, mtxFlags, view, force) loads the emitter's up to three
// textures (+0x88) into the GX texture maps draw+0xD4.. (negative: unused). A
// slot caches, at draw + 0x30 * slot, the texture and wrap modes it last
// loaded (+4, +0x18, +0x1C) and the texture matrix inputs (+8..+0x30); only
// when they differ (or force) does it init and load the texture object or
// build and load the texture matrix:
//
//   wrap modes    (u16 emitter+0x94 >> 4*slot) & 3 for S, >> 4*slot+2 for T
//   scale, trans  from (u8 emitter+0x96 >> 2*slot) & 3 (mirror bits) and the
//                 wrap modes: 1 or 2 (mirrored wrap), negated when mirrored;
//                 0 or 1, doubled when mirrored wrap
//   matrix        identity, rotated about (0.5, 0.5) by emitter+0x64+4*slot,
//                 scaled by scale * emitter+0x4C+8*slot about 0.5, translated;
//                 slots 0 and 1 then flip S/T by the resource's flag bits;
//                 postmultiplied by the view matrix for mtxFlags bit 7+slot
//
// Returns whether anything was loaded. The GX calls and the sine/cosine table
// lookup stay guest calls; the arithmetic keeps the Gekko single-precision
// operations of the translated code, so the port matches it bit for bit.
#include "abi_bridge.h"
#include "g3d_native.h"
#include "guest_call.h"
#include "memory.h"
#include "port_check.h"
#include "ppc_runtime.h"

#include <cstdint>

extern "C" void func_800C7D44(CpuContext* ctx);

namespace {

uint32_t Read32(uint32_t a) { return MemoryInline::FlatRead32(a); }
uint32_t Read16(uint32_t a) { return MemoryInline::FlatRead16(a); }
uint32_t Read8(uint32_t a) { return MemoryInline::FlatRead8(a); }
double Load(uint32_t a) { return MemoryInline::FlatReadFloat32(a); }
void Store(uint32_t a, double v) { MemoryInline::FlatWriteRamFloat32(a, v); }
double Single(double v) { return static_cast<double>(PpcForceSingleValueInline(v)); }
double Fadds(double a, double b) { return Single(a + b); }
double Fsubs(double a, double b) { return Single(a - b); }
double Fmuls(double a, double b) { return PpcFmulsInline(a, b); }

enum : uint32_t {
    GXInitTlutObj = 0x804B8E00,
    GXLoadTlut = 0x804B8E30,
    GXInitTexObjCI = 0x804B88B0,
    GXInitTexObj = 0x804B86A0,
    GXInitTexObjLOD = 0x804B8900,
    GXLoadTexObj = 0x804B8DA0,
    GXLoadTexMtxImm = 0x804BACD0,
    GXSetTexCoordGen2 = 0x804B61B0,
    SinCosIdx = 0x8009C7C4,
};

// The matrix element offsets of the 3x4 texture matrix.
enum : uint32_t { M00 = 0, M01 = 4, M03 = 0xC, M10 = 0x10, M11 = 0x14, M13 = 0x1C };

uint32_t Setup(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x190);
    const uint32_t draw = frame.Arg(3), emitter = frame.Arg(4), mtxFlags = frame.Arg(5);
    const uint32_t view = frame.Arg(6), force = frame.Arg(7);
    GuestCall game{ctx};

    constexpr uint32_t r2 = 0x8079FF60u;
    const double one = Load(r2 - 0x78C0), zero = Load(r2 - 0x78BC);
    const double minusOne = Load(r2 - 0x78B4), half = Load(r2 - 0x78B0);
    const uint32_t tlut = frame[0x1C], texObj = frame[0x28], mtx = frame[0x48];

    uint32_t changed = 0;
    for (uint32_t slot = 0; slot < 3; ++slot) {
        const int32_t id = int32_t(Read32(draw + 0xD4 + 4 * slot));
        if (id < 0) continue;
        const uint32_t tex = Read32(emitter + 0x88 + 4 * slot);
        if (tex == 0) continue;
        const uint32_t cache = draw + 0x30 * slot, params = emitter + 8 * slot;

        const uint32_t wrapBits = Read16(emitter + 0x94);
        const uint32_t wrapS = (wrapBits >> (4 * slot)) & 3, wrapT = (wrapBits >> (4 * slot + 2)) & 3;
        if (force != 0 || Read32(cache + 4) != tex || Read32(cache + 0x18) != wrapS ||
            Read32(cache + 0x1C) != wrapT) {
            changed = 1;
            MemoryInline::FlatWrite32(cache + 4, tex);
            MemoryInline::FlatWrite32(cache + 0x18, wrapS);
            MemoryInline::FlatWrite32(cache + 0x1C, wrapT);
            const uint32_t format = Read8(tex + 0xC);
            const uint32_t mipmap = Read8(tex + 0x14) > 1 ? 1 : 0;
            if (format <= 6 || format == 14) {
                game.Call<GXInitTexObj>({texObj, Read32(tex + 0x1C), Read16(tex + 4), Read16(tex + 6), format, wrapS,
                                    wrapT, mipmap});
            } else if (format - 8 <= 2) {
                game.Call<GXInitTlutObj>({tlut, Read32(tex + 0x20), Read8(tex + 0xD), Read16(tex + 0xE)});
                game.Call<GXLoadTlut>({tlut, uint32_t(id)});
                MemoryInline::FlatWrite32(frame[8], uint32_t(id));
                game.Call<GXInitTexObjCI>({texObj, Read32(tex + 0x1C), Read16(tex + 4), Read16(tex + 6), format, wrapS,
                                      wrapT, mipmap});
            }
            const double maxLod = Fadds(minusOne, Single(double(Read8(tex + 0x14))));
            game.Call<GXInitTexObjLOD>({texObj, Read8(tex + 0x15), Read8(tex + 0x16), 0, 0, 0},
                 {zero, maxLod, Load(tex + 0x18)});
            game.Call<GXLoadTexObj>({texObj, uint32_t(id)});
        }

        const uint32_t mirror = (Read8(emitter + 0x96) >> (2 * slot)) & 3;
        const bool mirrorS = mirror == 1 || mirror == 3, mirrorT = mirror == 2 || mirror == 3;
        const int32_t scaleSi = mirrorS ? -int32_t(wrapS == 2 ? 2 : 1) : int32_t(wrapS == 2 ? 2 : 1);
        const int32_t scaleTi = mirrorT ? -int32_t(wrapT == 2 ? 2 : 1) : int32_t(wrapT == 2 ? 2 : 1);
        const int32_t transSi = int32_t(mirrorS ? 1 : 0) << (wrapS == 2 ? 1 : 0);
        const int32_t transTi = int32_t(mirrorT ? 1 : 0) << (wrapT == 2 ? 1 : 0);
        const double scaleS = Single(double(scaleSi)), scaleT = Single(double(scaleTi));
        const double transS = Single(double(transSi)), transT = Single(double(transTi));
        const double tileS = Load(params + 0x4C), tileT = Load(params + 0x50);
        const double rotation = Load(emitter + 0x64 + 4 * slot);
        const double offsetS = Load(params + 0x70), offsetT = Load(params + 0x74);
        if (force == 0 && scaleS == Load(cache + 8) && scaleT == Load(cache + 0xC) && transS == Load(cache + 0x10) &&
            transT == Load(cache + 0x14) && tileS == Load(cache + 0x20) && tileT == Load(cache + 0x24) &&
            rotation == Load(cache + 0x28) && offsetS == Load(cache + 0x2C) && offsetT == Load(cache + 0x30))
            continue;

        changed = 1;
        Store(cache + 8, scaleS);
        Store(cache + 0xC, scaleT);
        Store(cache + 0x10, transS);
        Store(cache + 0x14, transT);
        Store(cache + 0x20, tileS);
        Store(cache + 0x24, tileT);
        Store(cache + 0x28, rotation);
        Store(cache + 0x2C, offsetS);
        Store(cache + 0x30, offsetT);

        // PSMTXIdentity (0x804C6290) with its constants.
        const uint32_t mtxZero = Read32(0x8079FF60u - 0x824), mtxOne = Read32(0x8079FF60u - 0x828);
        for (uint32_t k = 0; k < 12; ++k) MemoryInline::FlatWrite32(mtx + 4 * k, k % 5 == 0 ? mtxOne : mtxZero);
        Store(mtx + M03, offsetS);
        Store(mtx + M13, offsetT);
        if (rotation != zero) {
            const double centeredS = Fsubs(offsetS, half), centeredT = Fsubs(offsetT, half);
            game.Call<SinCosIdx>({frame[0x18], frame[0x14]}, {rotation});
            const double c = Load(frame[0x14]), s = Load(frame[0x18]);
            Store(mtx + M11, c);
            Store(mtx + M00, c);
            Store(mtx + M01, -s);
            Store(mtx + M10, s);
            Store(mtx + M03, Fsubs(Fadds(half, Fmuls(c, centeredS)), Fmuls(s, centeredT)));
            Store(mtx + M13, Fadds(Fadds(half, Fmuls(s, centeredS)), Fmuls(c, centeredT)));
        }
        if (!(one == scaleS && one == scaleT && one == tileS && one == tileT && zero == transS && zero == transT)) {
            const double s = Fmuls(scaleS, tileS), t = Fmuls(scaleT, tileT);
            Store(mtx + M00, Fmuls(Load(mtx + M00), s));
            Store(mtx + M01, Fmuls(Load(mtx + M01), s));
            Store(mtx + M03, Fadds(transS, Fmuls(scaleS, Fadds(half, Fmuls(Fsubs(Load(mtx + M03), half), tileS)))));
            Store(mtx + M10, Fmuls(Load(mtx + M10), t));
            Store(mtx + M11, Fmuls(Load(mtx + M11), t));
            Store(mtx + M13, Fadds(transT, Fmuls(scaleT, Fadds(half, Fmuls(Fsubs(Load(mtx + M13), half), tileT)))));
        }

        const auto flipS = [&] {
            Store(mtx + M00, Fmuls(Load(mtx + M00), minusOne));
            Store(mtx + M03, Fadds(Load(mtx + M03), Load(params + 0x4C)));
        };
        const auto flipT = [&] {
            Store(mtx + M11, Fmuls(Load(mtx + M11), minusOne));
            Store(mtx + M13, Fadds(Load(mtx + M13), Load(params + 0x50)));
        };
        const uint32_t res = Read32(Read32(emitter + 0xC8) + 0x24);
        MemoryInline::FlatWrite32(frame[0x10], res);
        const uint32_t resFlags = res + 0x94;
        const bool doubleSided = (Read8(emitter + 0xE5) & 1) != 0;
        if (slot == 0) {
            const uint32_t bits = Read8(resFlags + 2);
            if ((bits & 4) && doubleSided) {
                flipS();
                flipT();
            } else {
                if (bits & 8) flipS();
                if (bits & 0x10) flipT();
            }
        } else if (slot == 1) {
            const uint32_t bits = Read8(resFlags + 2), low = Read8(resFlags + 1);
            if ((bits & 0x20) && doubleSided) {
                flipS();
                flipT();
            } else {
                if (low & 0x40) flipS();
                if (low & 0x80) flipT();
            }
        }

        if (((int32_t(Read16(mtxFlags)) >> (slot + 7)) & 1) == 0) {
            const uint32_t mtxId = 3 * uint32_t(id) + 0x1E;
            game.Call<GXLoadTexMtxImm>({mtx, mtxId, 1});
            if (force != 0) game.Call<GXSetTexCoordGen2>({uint32_t(id), 1, 4, mtxId, 0, 0x7D});
        } else {
            G3dNative::MtxConcat(view + 0x30, mtx, mtx);
            const uint32_t mtxId = 3 * uint32_t(id) + 0x40;
            game.Call<GXLoadTexMtxImm>({mtx, mtxId, 0});
            if (force != 0) game.Call<GXSetTexCoordGen2>({uint32_t(id), 0, 0, 0, 0, mtxId});
        }
    }
    return changed;
}

void SetupPort(CpuContext* ctx) {
    const uint32_t r3 = Setup(ctx);
    ctx->gpr[3] = r3;
}

} // namespace

extern "C" void MH3_EffectDrawTextureSetup(CpuContext* ctx) {
#ifdef MH3_DEVELOPMENT
    static const bool check = PortCheck::Enabled("EffectDrawTextureSetup");
    if (check) [[unlikely]] {
        PortCheck::Run(ctx, "EffectDrawTextureSetup", func_800C7D44, SetupPort);
        return;
    }
#endif
    SetupPort(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x800C7D44, MH3_EffectDrawTextureSetup, "EffectDrawTextureSetup");
