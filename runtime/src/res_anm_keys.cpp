// NW4R g3d ResAnm float keys (g3d_resanm.cpp), RMHP08. C reconstructions from
// the disassembly of the debug-build evaluation the material, texture SRT
// and other non-character animations use:
//
//   0x80089914 ResAnmKeyEvalFVS(f1 frame, r3 track) -> f1
//   0x80089BEC ResAnmHermite(f1 v0, f2 t0, f3 v1, f4 t1, f5 d, f6 span) -> f1
//
// A track: u16 key count +0, f32 key count over the frame span +4, then
// 12-byte keys {f32 frame, value, slope} from +8. Before the first key or
// after the last the value is that key's; otherwise the key count times the
// frame's share of the span (u16 through GQR3, truncating) estimates the
// key, a walk finds the one at or before the frame, and on a key the value
// is its own, between two keys their hermite blend. The products and sums
// are the originals' float operations in their order; the reciprocal is
// FastReciprocal's (0x80061110: fres refined by one Newton step).
#include "abi_bridge.h"
#include "fatal.h"
#include "memory.h"
#include "nw4r_math.h"
#include "port_check.h"
#include "ppc_runtime.h"

#include <cstdint>
#include <stdexcept>

namespace {

constexpr uint32_t kC0 = 0x80798368u; // r2 - 31736: s - c0
constexpr uint32_t kC1 = 0x8079836Cu; // r2 - 31732: c1 s - c2
constexpr uint32_t kC2 = 0x80798370u; // r2 - 31728

float ReadFloat(uint32_t a) {
    return MemoryInline::FlatReadFloat32(a);
}

float Hermite(float v0, float t0, float v1, float t1, float d, double span) {
    const float s = d * Nw4rMath::FastReciprocal(span);
    const float f5 = s - ReadFloat(kC0);
    const float f1 = ReadFloat(kC1) * s - ReadFloat(kC2);
    const float blend = v0 + s * (s * (f1 * (v0 - v1)));
    return blend + (d * f5) * (f5 * t0 + s * t1);
}

float Evaluate(float frame, uint32_t track) {
    const uint32_t count = MemoryInline::FlatRead16(track);
    const uint32_t first = track + 8, last = track + (count - 1) * 12 + 8;

    if (frame <= ReadFloat(first))
        return ReadFloat(first + 4);

    if (ReadFloat(last) <= frame)
        return ReadFloat(last + 4);

    const float estimate = ReadFloat(track + 4) * ((frame - ReadFloat(first)) * float(count));
    const uint32_t index = estimate >= 65535.0f ? 65535u : estimate > 0.0f ? uint32_t(estimate) : 0u;

    if (int32_t(index) > int32_t(count) - 1)
        MH3_THROW(std::runtime_error("NW4R panic (g3d_resanm.cpp:135): estimatePos is out of bounds"));

    uint32_t key = first + index * 12;

    if (frame < ReadFloat(key)) {
        do
            key -= 12;
        while (frame < ReadFloat(key));
    } else {
        do
            key += 12;
        while (ReadFloat(key) <= frame);
        key -= 12;
    }

    const float f0 = ReadFloat(key);

    if (f0 == frame)
        return ReadFloat(key + 4);

    const float f1 = ReadFloat(key + 12);

    if (!(f0 <= frame && frame <= f1))
        MH3_THROW(std::runtime_error("NW4R panic (g3d_resanm.cpp:176): frame is out of bounds"));

    return Hermite(ReadFloat(key + 4), ReadFloat(key + 8), ReadFloat(key + 16), ReadFloat(key + 20), frame - f0, f1 - f0);
}

} // namespace

extern "C" void func_80089914(CpuContext* ctx);
static void EvaluatePort(CpuContext* p) {
    p->fpr[1].d = Evaluate(float(p->fpr[1].d), p->gpr[3]);
}

static double EvaluateChecked([[maybe_unused]] uint32_t r1, uint32_t r3, double f1) {
#ifdef MH3_DEVELOPMENT
    if (PortCheck::Enabled("ResAnmKeyEvalFVS")) [[unlikely]] {
        CpuContext* c = TryGetCpuContext();
        c->gpr[1] = r1;
        c->gpr[2] = 0x8079FF60u;
        c->gpr[3] = r3;
        c->fpr[1].d = f1;
        PortCheck::RunPure(c, "ResAnmKeyEvalFVS", PortCheck::Returns::Fpr1, func_80089914, EvaluatePort);
        return c->fpr[1].d;
    }
#endif
    return Evaluate(float(f1), r3);
}

extern "C" void MH3_ResAnmKeyEvalFVS(CpuContext* ctx) {
    ctx->fpr[1].d = EvaluateChecked(ctx->gpr[1], ctx->gpr[3], ctx->fpr[1].d);
}
REGISTER_NATIVE_FUNCTION_AS(0x80089914, MH3_ResAnmKeyEvalFVS, "ResAnmKeyEvalFVS");

extern "C" void func_80089BEC(CpuContext* ctx);
static void HermitePort(CpuContext* p) {
    p->fpr[1].d = Hermite(float(p->fpr[1].d), float(p->fpr[2].d), float(p->fpr[3].d), float(p->fpr[4].d), float(p->fpr[5].d), p->fpr[6].d);
}

static double HermiteChecked([[maybe_unused]] uint32_t r1, double f1, double f2, double f3, double f4, double f5, double f6) {
#ifdef MH3_DEVELOPMENT
    if (PortCheck::Enabled("ResAnmHermite")) [[unlikely]] {
        CpuContext* c = TryGetCpuContext();
        c->gpr[1] = r1;
        c->gpr[2] = 0x8079FF60u;
        c->fpr[1].d = f1;
        c->fpr[2].d = f2;
        c->fpr[3].d = f3;
        c->fpr[4].d = f4;
        c->fpr[5].d = f5;
        c->fpr[6].d = f6;
        PortCheck::RunPure(c, "ResAnmHermite", PortCheck::Returns::Fpr1, func_80089BEC, HermitePort);
        return c->fpr[1].d;
    }
#endif
    return Hermite(float(f1), float(f2), float(f3), float(f4), float(f5), f6);
}

extern "C" void MH3_ResAnmHermite(CpuContext* ctx) {
    ctx->fpr[1].d = HermiteChecked(ctx->gpr[1], ctx->fpr[1].d, ctx->fpr[2].d, ctx->fpr[3].d, ctx->fpr[4].d, ctx->fpr[5].d, ctx->fpr[6].d);
}
REGISTER_NATIVE_FUNCTION_AS(0x80089BEC, MH3_ResAnmHermite, "ResAnmHermite");
