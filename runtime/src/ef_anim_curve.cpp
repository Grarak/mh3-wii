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
