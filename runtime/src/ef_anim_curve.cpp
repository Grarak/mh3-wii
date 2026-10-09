// Effect animation-curve key search, RMHP08. C reconstruction of 0x8009EFEC
// (AnimCurveKeySearch), decompiled from the disassembly.
//
// Binary search of frame among a curve's keys: keys (r8) is an array of
// stride (r9)-byte records whose first half-word is the key's frame, searched
// between the key indices lo (r10) and hi (the ninth argument, at the caller's
// r1 + 8). Its results:
//   index (r3, u32)  the key at or before frame (lo before the first key, hi
//                    after the last);
//   exact (r4, u8)   1 when the value is the key's own (before the first key,
//                    after the last, on the last), 0 between keys; on a key
//                    frame it is |frame - time| < epsilon (time, f1, being the
//                    current time the frame was taken from);
//   lower (r5, u16), upper (r6, u16): the frames of the keys around frame, as
//                    far as the search wrote them.
// A keys pointer outside the Wii's address ranges panics (nw4r::db::Panic).
//
// The port does the search on host integers; the float test uses the same
// double-then-single rounding as the translated code. Its registered body only
// reads argument registers, so callers keep a precise call boundary.
#include "abi_bridge.h"
#include "memory.h"
#include "port_check.h"
#include "ppc_runtime.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace {

bool ValidPointer(uint32_t p) {
    return (p & 0xFF000000u) == 0x80000000u || (p & 0xFF800000u) == 0x81000000u ||
           (p & 0xF8000000u) == 0x90000000u || (p & 0xFF000000u) == 0xC0000000u ||
           (p & 0xFF800000u) == 0xC1000000u || (p & 0xF8000000u) == 0xD0000000u ||
           (p & 0xFFFFC000u) == 0xE0000000u;
}

struct Args {
    uint32_t index, exact, lower, upper;
    int32_t frame;
    double time;
    uint32_t keys, stride;
    int32_t lo, hi;
    uint32_t r2;
};

void Search(const Args& a) {
    if (!ValidPointer(a.keys)) {
        char text[96];
        std::snprintf(text, sizeof text, "NW4R panic (ef animation curve): bad key pointer %08x", a.keys);
        throw std::runtime_error(text);
    }
    auto key = [&](int32_t i) { return int32_t(MemoryInline::FlatRead16(a.keys + uint32_t(i) * a.stride)); };
    auto store16 = [](uint32_t address, int32_t v) { MemoryInline::FlatWrite16(address, uint16_t(v)); };

    // float(frame) through the lfd of 0x43300000:frame^0x80000000 less the
    // bias, then |float(frame) - time| < epsilon.
    uint64_t magic = uint64_t(0x43300000u) << 32 | (uint32_t(a.frame) ^ 0x80000000u);
    double biased;
    std::memcpy(&biased, &magic, 8);
    const uint64_t biasBits = uint64_t(MemoryInline::FlatRead32(a.r2 - 0x7AD8)) << 32 |
                              MemoryInline::FlatRead32(a.r2 - 0x7AD8 + 4);
    double bias;
    std::memcpy(&bias, &biasBits, 8);
    const double frameF = double(float(biased - bias));
    const float distance = std::fabs(float(frameF - a.time));
    const uint32_t epsilonBits = MemoryInline::FlatRead32(a.r2 - 0x7AB4);
    float epsilon;
    std::memcpy(&epsilon, &epsilonBits, 4);
    const uint8_t onTime = double(distance) < double(epsilon);

    int32_t lo = a.lo, hi = a.hi, mid = (lo + hi) / 2;
    const int32_t first = key(lo);
    store16(a.lower, first);
    if (a.frame < first) {
        MemoryInline::FlatWrite32(a.index, uint32_t(lo));
        MemoryInline::FlatWrite8(a.exact, 1);
        return;
    }
    if (a.frame == first) {
        MemoryInline::FlatWrite32(a.index, uint32_t(lo));
        if (lo == hi) {
            MemoryInline::FlatWrite8(a.exact, 1);
            return;
        }
        MemoryInline::FlatWrite8(a.exact, onTime);
        if (!onTime) store16(a.upper, key(lo + 1));
        return;
    }
    const int32_t last = key(hi);
    store16(a.upper, last);
    if (last <= a.frame) {
        MemoryInline::FlatWrite32(a.index, uint32_t(hi));
        MemoryInline::FlatWrite8(a.exact, 1);
        return;
    }
    int32_t k = key(mid);
    while (lo < mid) {
        if (a.frame == k) {
            MemoryInline::FlatWrite32(a.index, uint32_t(mid));
            MemoryInline::FlatWrite8(a.exact, onTime);
            if (!onTime) {
                store16(a.lower, k);
                store16(a.upper, key(mid + 1));
            }
            return;
        }
        if (k < a.frame) {
            lo = mid;
            store16(a.lower, k);
        } else {
            hi = mid;
            store16(a.upper, k);
        }
        mid = (lo + hi) / 2;
        k = key(mid);
    }
    MemoryInline::FlatWrite32(a.index, uint32_t(lo));
    MemoryInline::FlatWrite8(a.exact, 0);
}

float Sda2Float(uint32_t r2, int32_t offset) {
    const uint32_t bits = MemoryInline::FlatRead32(r2 + uint32_t(offset));
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}
uint32_t ToInt(float v) { return PPC_FprLowWordInline(PPC_Fctiwz(v)); }

// 0x8009E8B8 (AnimCurveBlendU8): the u8 value between keys a (r4) and b (r5)
// at t (r3, a 16.16 fraction for the linear mode, else frames of r2 - 0x7ACC
// per key span), by the key's interpolation (r6): 0 linear, 1 ease (with
// only bit 0 set) or hermite with the ends' slopes from bits 2 and 3 (clamped
// to 0-255), 2 step. Mode 3 panics. Equal keys give r4 as it came.
uint32_t BlendU8(uint32_t t, uint32_t r4, uint32_t r5, uint32_t r6, uint32_t r2) {
    const uint32_t a = r4 & 0xFF, b = r5 & 0xFF, mode = r6 & 0xFF;
    if (a == b) return r4;
    switch (mode & 3) {
    case 0:
        return (a + ((t * (b - a)) >> 16 & 0xFFFF)) & 0xFF;
    case 1: {
        auto k = [&](int32_t offset) { return Sda2Float(r2, offset); };
        const float f5 = float(t) / k(-0x7ACC);
        const float span = float(int32_t(b - a));
        if (mode == 1) {
            const float ease = k(-0x7AC8) - k(-0x7AC4) * f5;
            return ToInt(float(a) + f5 * (f5 * (ease * span)));
        }
        const float f7 = (mode >> 2 & 1) ? k(-0x7AC0) : k(-0x7AE8);
        const float f6 = (mode >> 3 & 1) ? k(-0x7AC0) : k(-0x7AE8);
        const float f3 = f5 * span;
        const float f2 = f5 * (f5 * ((f6 + f7) - k(-0x7AC4)));
        const float f0 = f5 * (k(-0x7AC8) + (k(-0x7ABC) * f6 - f7));
        const float v = float(a) + f3 * (f6 + (f2 + f0));
        if (!(v >= k(-0x7AE8))) return 0;
        if (v > k(-0x7AB8)) return 0xFF;
        return ToInt(v);
    }
    case 2:
        return r4;
    default: {
        char text[96];
        std::snprintf(text, sizeof text, "NW4R panic (ef animation curve): interpolation %u", mode);
        throw std::runtime_error(text);
    }
    }
}

Args FromContext(const CpuContext* c) {
    return Args{c->gpr[3], c->gpr[4], c->gpr[5], c->gpr[6], int32_t(c->gpr[7]), c->fpr[1].d, c->gpr[8], c->gpr[9],
                int32_t(c->gpr[10]), int32_t(MemoryInline::FlatRead32(c->gpr[1] + 8)), c->gpr[2]};
}

} // namespace

extern "C" void func_8009EFEC(CpuContext* ctx);

static void SearchPort(CpuContext* p) { Search(FromContext(p)); }

static void SearchChecked(uint32_t r1, const Args& a) {
#ifdef MH3_DEVELOPMENT
    static const bool check = PortCheck::Enabled("AnimCurveKeySearch");
    if (check) [[unlikely]] {
        CpuContext* c = TryGetCpuContext();
        c->gpr[1] = r1;
        c->gpr[2] = a.r2;
        c->gpr[3] = a.index;
        c->gpr[4] = a.exact;
        c->gpr[5] = a.lower;
        c->gpr[6] = a.upper;
        c->gpr[7] = uint32_t(a.frame);
        c->fpr[1].d = a.time;
        c->gpr[8] = a.keys;
        c->gpr[9] = a.stride;
        c->gpr[10] = uint32_t(a.lo);
        PortCheck::RunOutput(c, "AnimCurveKeySearch", func_8009EFEC, SearchPort,
                             {{a.index, 4}, {a.exact, 1}, {a.lower, 2}, {a.upper, 2}});
        return;
    }
#else
    (void)r1;
#endif
    Search(a);
}

extern "C" void MH3_AnimCurveKeySearch(CpuContext* ctx) {
    SearchChecked(ctx->gpr[1], Args{ctx->gpr[3], ctx->gpr[4], ctx->gpr[5], ctx->gpr[6], int32_t(ctx->gpr[7]),
                                    ctx->fpr[1].d, ctx->gpr[8], ctx->gpr[9], int32_t(ctx->gpr[10]),
                                    int32_t(MemoryInline::FlatRead32(ctx->gpr[1] + 8)), ctx->gpr[2]});
}
REGISTER_NATIVE_FUNCTION_AS(0x8009EFEC, MH3_AnimCurveKeySearch, "AnimCurveKeySearch");

extern "C" void func_8009E8B8(CpuContext* ctx);
static void BlendU8Port(CpuContext* p) { p->gpr[3] = BlendU8(p->gpr[3], p->gpr[4], p->gpr[5], p->gpr[6], p->gpr[2]); }
static uint32_t BlendU8Checked(uint32_t r1, uint32_t r2, uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6) {
#ifdef MH3_DEVELOPMENT
    static const bool check = PortCheck::Enabled("AnimCurveBlendU8");
    if (check) [[unlikely]] {
        CpuContext* c = TryGetCpuContext();
        c->gpr[1] = r1;
        c->gpr[2] = r2;
        c->gpr[3] = r3;
        c->gpr[4] = r4;
        c->gpr[5] = r5;
        c->gpr[6] = r6;
        PortCheck::RunPure(c, "AnimCurveBlendU8", PortCheck::Returns::Gpr3, func_8009E8B8, BlendU8Port);
        return c->gpr[3];
    }
#else
    (void)r1;
#endif
    return BlendU8(r3, r4, r5, r6, r2);
}
extern "C" void MH3_AnimCurveBlendU8(CpuContext* ctx) {
    ctx->gpr[3] = BlendU8Checked(ctx->gpr[1], ctx->gpr[2], ctx->gpr[3], ctx->gpr[4], ctx->gpr[5], ctx->gpr[6]);
}
REGISTER_NATIVE_FUNCTION_AS(0x8009E8B8, MH3_AnimCurveBlendU8, "AnimCurveBlendU8");
