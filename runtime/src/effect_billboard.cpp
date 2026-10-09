// nw4r::ef::DrawBillboardStrategy::Draw (0x800B9E5C), native: a manager's
// particles as camera-facing quads. Once per call the GX setup (0x800BB7AC)
// and the view * local matrix stay guest calls; per particle the port walks
// the manager's list, culls on the particle's scaled size, calls the draw
// state setup (0x800C694C) and emits the quad (0x800BA244 / 0x800BA798: four
// positions, each with a texture coordinate index when the strategy has
// one) straight into the GX FIFO, with GXBegin's fast path inline.
//
// The quad math runs on host floats with the original's single-precision
// rounding: its paired-single form (no rotation about z) with the fused
// ps_madd/ps_nmsub as fused lanes, the rotated form as its fmuls/fadds.
//
// Offsets (nw4r ef):
//   DrawBillboardStrategy: 0xD0 u8: vertices carry a texture coordinate index.
//   ParticleManager: 0x24 ResEmitter (its data at +0x9C: 0x00 u16 flags,
//     0xA9/0xAA s8 pivot x/y (/100), 0xAD u8 billboard kind), 0x38 the
//     particle list (0x3C last, 0x42 link offset), 0x58/0x5C size scale.
//   Particle: 0x0C life status (1: alive), 0x30/0x34 size, 0x38/0x3C scale,
//     0x40 rotation, 0xAC position, 0xC8 ParticleManager*.
#include "abi_bridge.h"
#include "guest_call.h"
#include "memory.h"
#include "port_check.h"
#include "ppc_runtime.h"

#include <cstdint>
#include <cstring>

extern "C" void func_800B9E5C(CpuContext* ctx);

namespace {

using MemoryInline::FlatRead16;
using MemoryInline::FlatRead32;
using MemoryInline::FlatRead8;
using MemoryInline::FlatReadFloat32;
using MemoryInline::FlatWrite32;
using MemoryInline::FlatWriteFloat32;

enum : uint32_t {
    kPanic = 0x80501130,          // nw4r::db::Panic(file, line, message, ...)
    kViewMtx = 0x800B7E14,        // (draw info, MTX34* out)
    kSetupGx = 0x800BB7AC,        // (strategy, draw info, manager)
    kManagerLocalMtx = 0x800AE3C4,// (manager, MTX34* out)
    kMtxConcat = 0x80071120,      // (out, a, b): out = a * b
    kColumnLength = 0x8009CDC8,   // (MTX34*, column) -> f1
    kSqrt = 0x80050C24,           // (f1) -> f1
    kReciprocal = 0x80061110,     // (f1) -> f1, as a paired single
    kDrawState = 0x800C694C,      // (strategy, particle, emitter data, draw info, first, 0)
    kRotation = 0x800B5518,       // (particle, VEC3* out)
    kMtxMultVec = 0x80051560,     // (VEC3* out, MTX34*, VEC3*)
    kSinCos = 0x8009C7C4,         // (f32* sin, f32* cos, f1 angle)
    kGXBegin = 0x804B6EF0,        // (primitive, vertex format, count)
    kFirstAlive = 0x800B5AC8,     // (manager) -> particle: the list's last alive
    kNextAlive = 0x800B5B30,      // (manager, particle) -> the alive one before
};
constexpr uint32_t kFile = 0x80594648, kBadPointer = 0x80594668;           // ef_drawbillboardstrategy.cpp
constexpr uint32_t kParticleFile = 0x80594810, kBadParticle = 0x805947D8;

// The original's frame: 0x08 the pivot (f32 x, y), 0x10 view * local, 0x40
// the view matrix. The quad's guest buffers go above them.
enum : uint32_t {
    kPivot = 0x08, kMtx = 0x10, kView = 0x40,
    kRotationOut = 0x80, kWorld = 0x90, kSin = 0xA0, kCos = 0xA4,
    kFrameSize = 0xB0,
};

bool ValidPointer(uint32_t p) {
    return (p & 0xFF000000u) == 0x80000000u || (p & 0xFF800000u) == 0x81000000u ||
           (p & 0xF8000000u) == 0x90000000u || (p & 0xFF000000u) == 0xC0000000u ||
           (p & 0xFF800000u) == 0xC1000000u || (p & 0xF8000000u) == 0xD0000000u ||
           (p & 0xFFFFC000u) == 0xE0000000u;
}

// fmuls/fadds/fsubs of single operands: the host float ops round the same.
float Mul(float a, float c) { return float(PpcFmulsInline(a, c)); }
float Fma(float a, float c, float b) { return PpcFmaFloatInline(a, c, b); } // a * c + b, fused

struct Quad {
    float x[4], y[4], z[4];
};

// One billboard's corners, as 0x800BA244 computes them.
// f1/f2: the matrix's column lengths; f4/f5: the billboard's facing (0/1
// when upright); f6/f7: the particle's scaled size.
class QuadMath {
public:
    QuadMath(float f1, float f2, float f4, float f5) : a_(Mul(f1, f4)), b_(Mul(f1, f5)), c_(Mul(f2, f4)), d_(Mul(f2, f5)) {}

    // The paired-single form: no rotation about z.
    void Upright(uint32_t mtx, uint32_t position, float pivotX, float pivotY, float w, float h, Quad& q) const {
        const float px = FlatReadFloat32(position), py = FlatReadFloat32(position + 4),
                    pz = FlatReadFloat32(position + 8);
        float row[3];
        for (uint32_t r = 0; r < 3; ++r) {
            const uint32_t m = mtx + r * 16;
            // ps_mul, ps_madd with (z, 1), ps_sum0.
            const float lane0 = Fma(FlatReadFloat32(m + 8), pz, Mul(FlatReadFloat32(m), px));
            const float lane1 = Fma(FlatReadFloat32(m + 12), 1.0f, Mul(FlatReadFloat32(m + 4), py));
            row[r] = lane0 + lane1;
        }
        // ps_nmsub: pivot - size * pivot, fused.
        const float ox = -Fma(w, pivotX, -pivotX), oy = -Fma(h, pivotY, -pivotY);
        const float cx = Fma(d_, oy, Fma(a_, ox, row[0])), cy = Fma(-c_, oy, Fma(b_, ox, row[1]));
        const float ux = Mul(a_, w), uy = Mul(b_, w), vx = Mul(d_, h), vy = Mul(-c_, h);
        const float sx = ux + vx, sy = uy + vy, tx = ux + -vx, ty = uy + -vy;
        q.x[0] = cx - tx, q.y[0] = cy - ty;
        q.x[1] = cx - sx, q.y[1] = cy - sy;
        q.x[2] = cx + tx, q.y[2] = cy + ty;
        q.x[3] = cx + sx, q.y[3] = cy + sy;
        q.z[0] = q.z[1] = q.z[2] = q.z[3] = row[2];
    }

    // Rotated by `angle` about z; world: the transformed position, s/c the
    // angle's sine and cosine.
    void Rotated(uint32_t world, float s, float c, float pivotX, float pivotY, float w, float h, Quad& q) const {
        const float cw = Mul(c, w), sw = Mul(s, w), ch = Mul(c, h), sh = Mul(s, h);
        const float ox = (pivotX - Mul(cw, pivotX)) - Mul(sh, pivotY);
        const float oy = (pivotY + Mul(sw, pivotX)) - Mul(ch, pivotY);
        const float cx = FlatReadFloat32(world) + (Mul(a_, ox) + Mul(d_, oy));
        const float cy = FlatReadFloat32(world + 4) + (Mul(b_, ox) - Mul(c_, oy));
        const float acw = Mul(a_, cw), ash = Mul(a_, sh), dsw = Mul(d_, sw), dch = Mul(d_, ch);
        const float bcw = Mul(b_, cw), bsh = Mul(b_, sh), csw = Mul(c_, sw), cch = Mul(c_, ch);
        const float ax = ((acw - ash) - dsw) - dch, ay = ((bcw - bsh) + csw) + cch;
        const float bx = ((acw + ash) - dsw) + dch, by = ((bcw + bsh) + csw) - cch;
        // VEC3Sub / VEC3Add (0x800513DC), lane by lane.
        q.x[0] = cx - ax, q.y[0] = cy - ay;
        q.x[1] = cx - bx, q.y[1] = cy - by;
        q.x[2] = cx + ax, q.y[2] = cy + ay;
        q.x[3] = cx + bx, q.y[3] = cy + by;
        // The corner offsets' z is +0: center - 0 and center + 0 (-0 + 0 is +0).
        q.z[0] = q.z[1] = FlatReadFloat32(world + 8);
        q.z[2] = q.z[3] = q.z[0] + 0.0f;
    }

private:
    float a_, b_, c_, d_;
};

void Put32(uint8_t*& out, uint32_t v) {
    v = __builtin_bswap32(v);
    std::memcpy(out, &v, 4);
    out += 4;
}
void PutFloat(uint8_t*& out, float f) {
    uint32_t v;
    std::memcpy(&v, &f, 4);
    Put32(out, v);
}

// GXBegin(GX_QUADS, GX_VTXFMT0, 4) and the quad's vertices: one FIFO write.
void EmitQuad(GuestCall& game, uint32_t r2, const Quad& q, bool texCoord) {
    uint8_t bytes[3 + 4 * 13];
    uint8_t* out = bytes;
    const uint32_t gx = FlatRead32(r2 - 2480);
    if (FlatRead32(gx + 0x5FC) != 0 || FlatRead32(gx) == 0) [[unlikely]] {
        game(kGXBegin, {0x80, 0, 4}); // dirty state, or the pad draw first
    } else {
        *out++ = 0x80;
        *out++ = 0;
        *out++ = 4;
    }
    for (int i = 0; i < 4; ++i) {
        PutFloat(out, q.x[i]);
        PutFloat(out, q.y[i]);
        PutFloat(out, q.z[i]);
        if (texCoord) *out++ = uint8_t(i);
    }
    GX_HLE_FIFO_WriteBytesInline(bytes, uint32_t(out - bytes));
}

void DrawBillboard(CpuContext* ctx) {
    GuestFrame frame(ctx, kFrameSize);
    const uint32_t sp = frame[0];
    const uint32_t strategy = frame.Arg(3), drawInfo = frame.Arg(4), manager = frame.Arg(5), r2 = frame.Arg(2);
    GuestCall game{ctx};

    if (!ValidPointer(manager)) game(kPanic, {kFile, 0x210, kBadPointer, manager});
    game(kViewMtx, {drawInfo, sp + kView});
    const uint32_t data = FlatRead32(manager + 0x24) + 0x9C;
    game(kSetupGx, {strategy, drawInfo, manager});
    const bool texCoord = FlatRead8(strategy + 0xD0) != 0;
    const float hundred = FlatReadFloat32(r2 - 31056);
    const float pivotX = float(int8_t(FlatRead8(data + 0xA9))) / hundred;
    const float pivotY = float(int8_t(FlatRead8(data + 0xAA))) / hundred;
    FlatWriteFloat32(sp + kPivot, pivotX);
    FlatWriteFloat32(sp + kPivot + 4, pivotY);
    game(kManagerLocalMtx, {manager, sp + kMtx});
    game(kMtxConcat, {sp + kMtx, sp + kView, sp + kMtx});
    game(kColumnLength, {sp + kMtx, 0});
    const float f1 = float(ctx->fpr[1].d);
    game(kColumnLength, {sp + kMtx, 1});
    const float f2 = float(ctx->fpr[1].d);
    const float zero = FlatReadFloat32(r2 - 31052);
    // The facing: the view's y axis in its xy plane, normalized.
    const float m01 = FlatReadFloat32(sp + kView + 4), m11 = FlatReadFloat32(sp + kView + 0x14);
    game(kSqrt, {}, {Mul(m01, m01) + Mul(m11, m11)});
    float f4 = FlatReadFloat32(r2 - 31048), f5 = zero;
    if (FlatRead8(data + 0xAD) != 3 && !(zero == float(ctx->fpr[1].d))) {
        game(kReciprocal, {}, {ctx->fpr[1].d});
        const float inv = float(PPC_PsToScalarInline(ctx->fpr[1].d)); // a paired result
        f5 = Mul(-m01, inv);
        f4 = Mul(m11, inv);
    }
    const QuadMath math(f1, f2, f4, f5);

    const uint32_t order = FlatRead16(data) & 0x800;
    const uint32_t first = game.Virtual(strategy, 0x10, {order});
    const uint32_t next = game.Virtual(strategy, 0x14, {order});
    // The list walk: the stock (last to first) iterators natively.
    const bool stock = first == kFirstAlive && next == kNextAlive;
    const uint32_t link = FlatRead16(manager + 0x42);
    auto aliveFrom = [&](uint32_t p) {
        while (p && FlatRead32(p + 0x0C) != 1) p = FlatRead32(p + link);
        return p;
    };
    const float minSize = FlatReadFloat32(r2 - 31072);
    uint32_t firstDraw = 1;
    for (uint32_t p = stock ? aliveFrom(FlatRead32(manager + 0x3C)) : game(first, {manager}); p;
         p = stock ? aliveFrom(FlatRead32(p + link)) : game(next, {manager, p})) {
        const uint32_t owner = FlatRead32(p + 0xC8);
        const float w = Mul(Mul(FlatReadFloat32(p + 0x30), FlatReadFloat32(p + 0x38)), FlatReadFloat32(owner + 0x58));
        if (w < minSize) continue;
        float h;
        switch (FlatRead16(FlatRead32(owner + 0x24) + 0x9C) & 0x6000) {
        case 0x4000: h = Mul(FlatReadFloat32(p + 0x34), FlatReadFloat32(p + 0x38)); break;
        case 0x2000: h = Mul(FlatReadFloat32(p + 0x30), FlatReadFloat32(p + 0x3C)); break;
        case 0x6000: h = Mul(FlatReadFloat32(p + 0x30), FlatReadFloat32(p + 0x38)); break;
        default: h = Mul(FlatReadFloat32(p + 0x34), FlatReadFloat32(p + 0x3C)); break;
        }
        h = Mul(h, FlatReadFloat32(owner + 0x5C));
        if (h < minSize) continue;
        game(kDrawState, {strategy, p, data, drawInfo, firstDraw, 0});
        firstDraw = 0;

        // 0x800BA244.
        if (!ValidPointer(p)) game(kPanic, {kParticleFile, 0x88, kBadParticle, p});
        game(kRotation, {p, sp + kRotationOut});
        Quad q;
        const float angle = FlatReadFloat32(sp + kRotationOut + 8);
        if (zero == angle) {
            math.Upright(sp + kMtx, p + 0xAC, pivotX, pivotY, w, h, q);
        } else {
            game(kMtxMultVec, {sp + kWorld, sp + kMtx, p + 0xAC});
            game(kSinCos, {sp + kSin, sp + kCos}, {-angle});
            math.Rotated(sp + kWorld, FlatReadFloat32(sp + kSin), FlatReadFloat32(sp + kCos), pivotX, pivotY, w, h, q);
        }
        EmitQuad(game, r2, q, texCoord);
    }
}

} // namespace

extern "C" void MH3_DrawBillboard(CpuContext* ctx) { PortCheck::Call(ctx, "DrawBillboard", func_800B9E5C, DrawBillboard); }
REGISTER_NATIVE_FUNCTION_AS(0x800B9E5C, MH3_DrawBillboard, "DrawBillboard");
