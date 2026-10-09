// nw4r::ef animation curves, RMHP08: the particle curve executors (u8, f32,
// rotation, baked) and their helpers, C reconstructions from the disassembly.
// Each registered body reads only argument registers and runs host code, so
// ParticleManager::Calc keeps precise call boundaries around them.
//
// 0x8009EFEC (AnimCurveKeySearch):
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
#include "fatal.h"
#include "memory.h"
#include "port_check.h"
#include "ppc_runtime.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace {

struct Args {
    uint32_t index, exact, lower, upper;
    int32_t frame;
    double time;
    uint32_t keys, stride;
    int32_t lo, hi;
    uint32_t r2;
};

[[noreturn]] void Panic(const char* what, uint32_t p) {
    char text[112];
    std::snprintf(text, sizeof text, "NW4R panic (ef animation curve): bad %s pointer %08x", what, p);
    MH3_THROW(std::runtime_error(text));
}

// NW4R's pointer check always passes in the port; the pointer is folded to
// its cached alias instead.
void FoldPointer(uint32_t& p) {
    p = GuestFlat::CachedAlias(p);
}

// The search's outputs, written as the original writes them (a sink either
// stores to guest memory or keeps them on the host).
struct GuestKeyOutputs {
    uint32_t indexAddress, exactAddress, lowerAddress, upperAddress;
    void Index(int32_t v) const { MemoryInline::FlatWrite32(indexAddress, uint32_t(v)); }
    void Exact(uint8_t v) const { MemoryInline::FlatWrite8(exactAddress, v); }
    void Lower(int32_t v) const { MemoryInline::FlatWrite16(lowerAddress, uint16_t(v)); }
    void Upper(int32_t v) const { MemoryInline::FlatWrite16(upperAddress, uint16_t(v)); }
};

struct HostKeyOutputs {
    uint32_t index = 0;
    uint8_t exact = 0;
    uint16_t lower = 0, upper = 0;
    void Index(int32_t v) { index = uint32_t(v); }
    void Exact(uint8_t v) { exact = v; }
    void Lower(int32_t v) { lower = uint16_t(v); }
    void Upper(int32_t v) { upper = uint16_t(v); }
};

template <class Out>
void SearchKeys(int32_t frame, double time, uint32_t keys, uint32_t stride, int32_t lo, int32_t hi, uint32_t r2, Out& out) {
    FoldPointer(keys);
    auto key = [&](int32_t i) {
        return int32_t(MemoryInline::FlatRead16(keys + uint32_t(i) * stride));
    };

    // float(frame) through the lfd of 0x43300000:frame^0x80000000 less the
    // bias, then |float(frame) - time| < epsilon.
    uint64_t magic = uint64_t(0x43300000u) << 32 | (uint32_t(frame) ^ 0x80000000u);
    double biased;
    std::memcpy(&biased, &magic, 8);
    const uint64_t biasBits = uint64_t(MemoryInline::FlatRead32(r2 - 0x7AD8)) << 32 | MemoryInline::FlatRead32(r2 - 0x7AD8 + 4);
    double bias;
    std::memcpy(&bias, &biasBits, 8);
    const double frameF = double(float(biased - bias));
    const float distance = std::fabs(float(frameF - time));
    const uint32_t epsilonBits = MemoryInline::FlatRead32(r2 - 0x7AB4);
    float epsilon;
    std::memcpy(&epsilon, &epsilonBits, 4);
    const uint8_t onTime = double(distance) < double(epsilon);

    int32_t mid = (lo + hi) / 2;
    const int32_t first = key(lo);
    out.Lower(first);

    if (frame < first) {
        out.Index(lo);
        out.Exact(1);
        return;
    }

    if (frame == first) {
        out.Index(lo);

        if (lo == hi) {
            out.Exact(1);
            return;
        }

        out.Exact(onTime);

        if (!onTime)
            out.Upper(key(lo + 1));

        return;
    }

    const int32_t last = key(hi);
    out.Upper(last);

    if (last <= frame) {
        out.Index(hi);
        out.Exact(1);
        return;
    }

    int32_t k = key(mid);

    while (lo < mid) {
        if (frame == k) {
            out.Index(mid);
            out.Exact(onTime);

            if (!onTime) {
                out.Lower(k);
                out.Upper(key(mid + 1));
            }

            return;
        }

        if (k < frame) {
            lo = mid;
            out.Lower(k);
        } else {
            hi = mid;
            out.Upper(k);
        }

        mid = (lo + hi) / 2;
        k = key(mid);
    }

    out.Index(lo);
    out.Exact(0);
}

void Search(const Args& a) {
    GuestKeyOutputs out{a.index, a.exact, a.lower, a.upper};
    SearchKeys(a.frame, a.time, a.keys, a.stride, a.lo, a.hi, a.r2, out);
}

float Sda2Float(uint32_t r2, int32_t offset) {
    const uint32_t bits = MemoryInline::FlatRead32(r2 + uint32_t(offset));
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

uint32_t ToInt(float v) {
    return PPC_FprLowWordInline(PPC_Fctiwz(v));
}

// 0x8009E8B8 (AnimCurveBlendU8): the u8 value between keys a (r4) and b (r5)
// at t (r3, a 16.16 fraction for the linear mode, else frames of r2 - 0x7ACC
// per key span), by the key's interpolation (r6): 0 linear, 1 ease (with
// only bit 0 set) or hermite with the ends' slopes from bits 2 and 3 (clamped
// to 0-255), 2 step. Mode 3 panics. Equal keys give r4 as it came.
uint32_t BlendU8(uint32_t t, uint32_t r4, uint32_t r5, uint32_t r6, uint32_t r2) {
    const uint32_t a = r4 & 0xFF, b = r5 & 0xFF, mode = r6 & 0xFF;

    if (a == b)
        return r4;

    switch (mode & 3) {
    case 0:
        return (a + ((t * (b - a)) >> 16 & 0xFFFF)) & 0xFF;
    case 1: {
        auto k = [&](int32_t offset) {
            return Sda2Float(r2, offset);
        };

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

        if (!(v >= k(-0x7AE8)))
            return 0;

        if (v > k(-0x7AB8))
            return 0xFF;

        return ToInt(v);
    }
    case 2:
        return r4;
    default: {
        char text[96];
        std::snprintf(text, sizeof text, "NW4R panic (ef animation curve): interpolation %u", mode);
        MH3_THROW(std::runtime_error(text));
    }
    }
}

Args FromContext(const CpuContext* c) {
    return Args{c->gpr[3], c->gpr[4], c->gpr[5], c->gpr[6], int32_t(c->gpr[7]), c->fpr[1].d, c->gpr[8], c->gpr[9], int32_t(c->gpr[10]), int32_t(MemoryInline::FlatRead32(c->gpr[1] + 8)), c->gpr[2]};
}

} // namespace

extern "C" void func_8009EFEC(CpuContext* ctx);

static void SearchPort(CpuContext* p) {
    Search(FromContext(p));
}

static void SearchChecked(uint32_t r1, const Args& a) {
#ifdef MH3_DEVELOPMENT
    const bool check = PortCheck::Enabled("AnimCurveKeySearch");

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
        PortCheck::RunOutput(c, "AnimCurveKeySearch", func_8009EFEC, SearchPort, {{a.index, 4}, {a.exact, 1}, {a.lower, 2}, {a.upper, 2}});
        return;
    }
#else
    (void)r1;
#endif
    Search(a);
}

extern "C" void MH3_AnimCurveKeySearch(CpuContext* ctx) {
    SearchChecked(ctx->gpr[1], Args{ctx->gpr[3], ctx->gpr[4], ctx->gpr[5], ctx->gpr[6], int32_t(ctx->gpr[7]), ctx->fpr[1].d, ctx->gpr[8], ctx->gpr[9], int32_t(ctx->gpr[10]),
                                    int32_t(MemoryInline::FlatRead32(ctx->gpr[1] + 8)), ctx->gpr[2]});
}
REGISTER_NATIVE_FUNCTION_AS(0x8009EFEC, MH3_AnimCurveKeySearch, "AnimCurveKeySearch");

extern "C" void func_8009E8B8(CpuContext* ctx);
static void BlendU8Port(CpuContext* p) {
    p->gpr[3] = BlendU8(p->gpr[3], p->gpr[4], p->gpr[5], p->gpr[6], p->gpr[2]);
}

static uint32_t BlendU8Checked(uint32_t r1, uint32_t r2, uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6) {
#ifdef MH3_DEVELOPMENT
    const bool check = PortCheck::Enabled("AnimCurveBlendU8");

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

// ---------------------------------------------------------------------------
// The u8 curve executor (0x8009DF14) and the helpers it runs, reconstructed
// from the disassembly. A curve track (the ResAnimCurve) is a 0x20-byte header
// followed by its tables:
//   +0  u8 magic: 0xAC keyframed, 0xAB baked (one value set per frame)
//   +3  u8 channel mask (which of the target's bytes it animates)
//   +4  u8 flags: 0x80 frames stretched over the particle's life, 0x40
//       ping-pong loop, 0x20 loop forever, 0x04 one random pick for every
//       channel (else the pick advances per channel)
//   +5  u8 loop count, +6 u16 random salt, +8 u16 frames
//   +C/+10/+14 u32 sizes of the key, random and random-pool tables
// A key is {u16 frame, u8 random (bit 1: from the pool), u8, u8 interpolation
// per channel [8], then the channel values or a u16 random-set index}; a
// random set is {u32, then per channel u8 base, s8-scaled u8 range}.
namespace {

uint32_t Lcg(uint32_t s) {
    return s * 0x343FDu + 0x269EC3u;
}

// 0x8009EF58: the random seed of a key's pick.
uint32_t PickSeed(uint32_t particleSeed, uint32_t salt, uint32_t set, uint32_t loop) {
    const uint32_t w = loop * 0x7B929u + (set & 0xFFFF) * 0x371097E7u + ((particleSeed & 0xFFFF) * 0x3F81F635u + (salt & 0xFFFF) * 0x30A74193u + 0x4BF53u);
    uint8_t b0 = uint8_t(w >> 24), b1 = uint8_t(w >> 16), b2 = uint8_t(w >> 8);
    const uint8_t b3 = uint8_t(w);
    b2 ^= b3;
    b1 ^= b2;
    b0 ^= b1;
    return uint32_t(b0) << 24 | uint32_t(b1) << 16 | uint32_t(b2) << 8 | b3;
}

// 0x8009F898 / 0x8009F8AC: channels in a mask, and its highest channel bit.
uint32_t ChannelCount(uint32_t mask) {
    return MemoryInline::FlatRead8(0x80592428u + (mask & 0xFF));
}

uint32_t ChannelBound(uint32_t mask) {
    return MemoryInline::FlatRead8(0x80592528u + (mask & 0xFF));
}

// __cvt_fp2unsigned (0x80457360).
uint32_t CvtToUnsigned(double v) {
    if (v < 0.0)
        return 0;

    if (!(v < 4294967296.0))
        return 0xFFFFFFFFu;

    if (v < 2147483648.0)
        return PPC_FprLowWordInline(PPC_Fctiwz(v));

    return PPC_FprLowWordInline(PPC_Fctiwz(v - 2147483648.0)) + 0x80000000u;
}

struct Tables {
    uint32_t header, keys, random, pool, names;
};

// 0x8009F290: the track's tables.
Tables ResolveTables(uint32_t track) {
    FoldPointer(track);
    Tables t;
    t.header = track;
    t.keys = track + 0x20;
    t.random = t.keys + MemoryInline::FlatRead32(track + 0xC);
    t.pool = t.random + MemoryInline::FlatRead32(track + 0x10);
    t.names = t.pool + MemoryInline::FlatRead32(track + 0x14);
    return t;
}

struct TickFrame {
    uint16_t tick;
    float frame;
    uint32_t loop;
};

// 0x8009CE20 (AnimCurveTickToFrame): the curve frame at particle tick `tick`
// of `life`, as a whole frame, a float frame and the loop it is in.
TickFrame TickToFrame(uint32_t life, uint32_t header, uint32_t tick) {
    FoldPointer(header);
    TickFrame r{0, 0.0f, 0};
    const uint32_t frames = MemoryInline::FlatRead16(header + 8);

    if (frames <= 1) {
        r.loop = tick;
        return r;
    }

    const uint32_t flags = MemoryInline::FlatRead8(header + 4);
    const uint32_t loops = MemoryInline::FlatRead8(header + 5);
    const uint32_t last = frames - 1;

    if ((flags & 0x20) == 0 && loops <= 1) {
        if ((flags & 0x80) == 0) {
            r.tick = uint16_t(tick < last ? tick : last);
            r.frame = float(uint32_t(r.tick));
            return r;
        }

        float f;

        if (life == 1) {
            f = float(int32_t(last));
        } else {
            f = float(tick) * (float(int32_t(last)) / float(life - 1));
            const float end = float(int32_t(last));

            if (!(f <= end))
                f = end;
        }

        r.frame = f;
        r.tick = uint16_t(ToInt(f));
        return r;
    }

    if ((flags & 0x80) == 0) {
        const uint32_t loop = tick / last;
        r.loop = loop;

        if ((flags & 0x20) == 0 && loop >= loops) {
            r.tick = uint16_t((flags & 0x40) != 0 && (loops & 1) == 0 ? 0 : last);
            r.loop = (loops - 1) & 0xFF;
        } else if ((flags & 0x40) == 0 || (loop & 1) == 0) {
            r.tick = uint16_t(tick - loop * last);
        } else {
            r.tick = uint16_t(last * (loop + 1) - tick);
        }

        r.frame = float(uint32_t(r.tick));
        return r;
    }

    if (tick >= life - 1) {
        r.tick = uint16_t((flags & 0x40) != 0 && (loops & 1) == 0 ? 0 : last);
        r.frame = float(uint32_t(r.tick));
        r.loop = (loops - 1) & 0xFF;
        return r;
    }

    const float span = float(loops) * (float(int32_t(last)) / float(life - 1));
    const uint32_t loop = CvtToUnsigned(double((float(tick) * span) / float(int32_t(last))));
    r.loop = loop;
    const float into = float(tick) * span - float(loop * last);
    r.frame = (flags & 0x40) == 0 || (loop & 1) == 0 ? into : float(int32_t(last)) - into;
    r.tick = uint16_t(ToInt(r.frame));
    return r;
}

// 0x8009EAB0: the loops of the keys either side of key `index` (a ping-pong
// loop's turn crosses into the next loop).
void AdjustLoops(uint32_t header, uint32_t keys, int32_t index, uint32_t& low, uint32_t& high) {
    high = low;
    const uint32_t flags = MemoryInline::FlatRead8(header + 4);

    if ((flags & 0x40) == 0 || ((flags & 0x20) == 0 && MemoryInline::FlatRead8(header + 5) <= 1))
        return;

    if ((low & 1) == 0 && index + 1 >= int32_t(MemoryInline::FlatRead16(keys)) - 1 && ((flags & 0x20) != 0 || low < MemoryInline::FlatRead8(header + 5) - 1u))
        ++high;

    if ((low & 1) != 0 && index == 0 && low != 0)
        ++low;
}

// 0x800A2568 (u8) / 0x800A2D54 (f32): a baked track, the frame's values
// copied (`width` bytes each).
template <uint32_t width>
void ExecuteBaked(uint32_t track, uint32_t target, uint32_t tick, uint32_t life) {
    FoldPointer(track);
    FoldPointer(target);
    const uint32_t mask = MemoryInline::FlatRead8(track + 3);
    const int32_t bound = int32_t(ChannelBound(mask));
    const uint32_t count = ChannelCount(mask);

    if (count == 0)
        return;

    const Tables t = ResolveTables(track);
    uint32_t src = t.keys + count * TickToFrame(life, t.header, tick).tick * width;

    for (int32_t bit = 1; bit <= bound; bit = (bit << 1) & 0xFFFF, target += width) {
        if ((mask & uint32_t(bit)) == 0)
            continue;

        if constexpr (width == 1)
            MemoryInline::FlatWrite8(target, MemoryInline::FlatRead8(src));
        else
            MemoryInline::FlatWrite32(target, MemoryInline::FlatRead32(src));

        src += width;
    }
}

uint32_t Divide(uint32_t a, uint32_t b) {
    return b != 0 ? a / b : 0;
}

// A random channel value: base + range * (seed's high half as s16) / 32768,
// clamped to a byte.
uint32_t RandomChannel(uint32_t& values, uint32_t seed) {
    const int32_t base = MemoryInline::FlatRead8(values), range = MemoryInline::FlatRead8(values + 1);
    values += 2;
    const int32_t v = base + range * int32_t(int16_t(seed >> 16)) / 32768;
    return uint32_t(v < 0 ? 0 : v > 0xFF ? 0xFF : v);
}

// 0x8009DF14 (AnimCurveExecuteU8): the track's values at particle tick `tick`
// of `life` into the target's channel bytes.
void ExecuteU8(uint32_t track, uint32_t target, uint32_t tick, uint32_t particleSeed, uint32_t life, uint32_t r2) {
    FoldPointer(track);
    FoldPointer(target);

    if (MemoryInline::FlatRead8(track) == 0xAB) {
        ExecuteBaked<1>(track, target, tick, life);
        return;
    }

    const uint32_t mask = MemoryInline::FlatRead8(track + 3);
    const int32_t bound = int32_t(ChannelBound(mask));
    const uint32_t count = ChannelCount(mask) & 0xFF;

    if (count == 0)
        return;

    const uint32_t stride = (count + 0xD) & ~1u;
    const uint32_t setBytes = count * 2;
    const Tables t = ResolveTables(track);
    const TickFrame at = TickToFrame(life, t.header, tick);
    const uint32_t keys = t.keys + 4;
    HostKeyOutputs key;
    SearchKeys(int32_t(at.tick), double(at.frame), keys, stride, 0, int32_t(MemoryInline::FlatRead16(t.keys)) - 1, r2, key);
    const bool advance = (MemoryInline::FlatRead8(t.header + 4) & 4) == 0;
    const uint32_t salt = MemoryInline::FlatRead16(t.header + 6);

    // The values of a random key: its set, or a set picked from the pool.
    auto randomSet = [&](uint32_t k, uint32_t loop, uint32_t& seed) {
        const uint32_t set = MemoryInline::FlatRead16(k + 0xC);
        seed = PickSeed(particleSeed, salt, set, loop);

        if ((MemoryInline::FlatRead8(k + 2) & 2) == 0)
            return t.random + set * setBytes + 4;

        const uint32_t hi = seed >> 16, pool = MemoryInline::FlatRead16(t.pool);
        const uint32_t values = t.pool + setBytes * (hi - Divide(hi, pool) * pool) + 4;
        seed = Lcg(seed);
        return values;
    };

    if (key.exact) {
        const uint32_t k = keys + key.index * stride;

        if (MemoryInline::FlatRead8(k + 2) == 0) {
            uint32_t values = k + 0xC;

            for (int32_t bit = 1; bit <= bound; bit = (bit << 1) & 0xFFFF, ++target)
                if (mask & uint32_t(bit))
                    MemoryInline::FlatWrite8(target, MemoryInline::FlatRead8(values++));
            return;
        }

        uint32_t seed;
        uint32_t values = randomSet(k, at.loop, seed);

        for (int32_t bit = 1; bit <= bound; bit = (bit << 1) & 0xFFFF, ++target) {
            if (mask & uint32_t(bit))
                MemoryInline::FlatWrite8(target, RandomChannel(values, seed));

            if (advance)
                seed = Lcg(seed);
        }

        return;
    }

    uint32_t lowLoop = at.loop, highLoop;
    AdjustLoops(t.header, t.keys, int32_t(key.index), lowLoop, highLoop);
    const uint32_t lower = key.lower;
    const uint32_t into = CvtToUnsigned(double(65536.0f * (at.frame - float(lower))));
    const uint32_t blend = Divide(into, uint32_t(key.upper) - lower);
    const uint32_t lowKey = keys + key.index * stride, highKey = keys + (key.index + 1) * stride;
    uint32_t low = lowKey + 0xC, high = highKey + 0xC, modes = lowKey + 4;
    const bool lowRandom = MemoryInline::FlatRead8(lowKey + 2) != 0;
    const bool highRandom = MemoryInline::FlatRead8(highKey + 2) != 0;
    auto put = [&](uint32_t a, uint32_t b) {
        MemoryInline::FlatWrite8(target, uint8_t(BlendU8(blend, a, b, MemoryInline::FlatRead8(modes), r2)));
    };

    if (!lowRandom && !highRandom) {
        for (int32_t bit = 1; bit <= bound; bit = (bit << 1) & 0xFFFF, ++target, ++modes)
            if (mask & uint32_t(bit))
                put(MemoryInline::FlatRead8(low++), MemoryInline::FlatRead8(high++));
    } else if (lowRandom && !highRandom) {
        uint32_t seed;
        low = randomSet(lowKey, lowLoop, seed);

        for (int32_t bit = 1; bit <= bound; bit = (bit << 1) & 0xFFFF, ++target, ++modes) {
            if (mask & uint32_t(bit)) {
                const uint32_t a = RandomChannel(low, seed);
                put(a, MemoryInline::FlatRead8(high++));
            }

            if (advance)
                seed = Lcg(seed);
        }
    } else if (!lowRandom) {
        uint32_t seed;
        high = randomSet(highKey, highLoop, seed);

        for (int32_t bit = 1; bit <= bound; bit = (bit << 1) & 0xFFFF, ++target, ++modes) {
            if (mask & uint32_t(bit)) {
                const uint32_t b = RandomChannel(high, seed);
                put(MemoryInline::FlatRead8(low++), b);
            }

            if (advance)
                seed = Lcg(seed);
        }
    } else {
        uint32_t lowSeed, highSeed;
        low = randomSet(lowKey, lowLoop, lowSeed);
        high = randomSet(highKey, highLoop, highSeed);

        for (int32_t bit = 1; bit <= bound; bit = (bit << 1) & 0xFFFF, ++target, ++modes) {
            if (mask & uint32_t(bit)) {
                const uint32_t a = RandomChannel(low, lowSeed);

                if (advance)
                    lowSeed = Lcg(lowSeed);

                const uint32_t b = RandomChannel(high, highSeed);

                if (advance)
                    highSeed = Lcg(highSeed);

                put(a, b);
            } else if (advance) {
                lowSeed = Lcg(lowSeed);
                highSeed = Lcg(highSeed);
            }
        }
    }
}

// 0x800A0228 (AnimCurveBlendF32): the value between keys a and b at t (0-1)
// by the interpolation mode, as BlendU8 without the byte rounding.
float BlendF32(float t, float a, float b, uint32_t mode, uint32_t r2) {
    if (a == b)
        return a;

    mode &= 0xFF;
    auto k = [&](int32_t offset) {
        return Sda2Float(r2, offset);
    };

    switch (mode & 3) {
    case 0:
        return a + t * (b - a);
    case 1: {
        if (mode == 1)
            return a + t * (t * ((k(-0x7AC8) - k(-0x7AC4) * t) * (b - a)));

        const float f6 = (mode >> 3 & 1) ? k(-0x7AC0) : k(-0x7AE8);
        const float f7 = (mode >> 2 & 1) ? k(-0x7AC0) : k(-0x7AE8);
        const float f5 = t * (b - a);
        const float f4 = t * (t * ((f6 + f7) - k(-0x7AC4)));
        const float f0 = t * (k(-0x7AC8) + (k(-0x7ABC) * f6 - f7));
        return a + f5 * (f6 + (f4 + f0));
    }
    case 2:
        return a;
    default: {
        char text[96];
        std::snprintf(text, sizeof text, "NW4R panic (ef animation curve): interpolation %u", mode);
        MH3_THROW(std::runtime_error(text));
    }
    }
}

float ReadFloat(uint32_t address) {
    const uint32_t bits = MemoryInline::FlatRead32(address);
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

void WriteFloat(uint32_t address, float f) {
    uint32_t bits;
    std::memcpy(&bits, &f, 4);
    MemoryInline::FlatWrite32(address, bits);
}

// 0x8009F8C0 (AnimCurveExecuteF32) / 0x800A035C (AnimCurveExecuteRotate): as
// ExecuteU8 for f32 channels (blended by BlendF32, random values unclamped).
// The f32 executor advances a random pick only on the channels the track
// animates and has baked tracks; the rotation's random sets end with a byte
// that, when set, negates the set's values on half the picks (a pick of its
// own), and its picks advance on every channel bit.
template <bool rotate>
void ExecuteFloat(uint32_t track, uint32_t target, uint32_t tick, uint32_t particleSeed, uint32_t life, uint32_t r2) {
    FoldPointer(track);
    FoldPointer(target);

    if (!rotate && MemoryInline::FlatRead8(track) == 0xAB) {
        ExecuteBaked<4>(track, target, tick, life);
        return;
    }

    const uint32_t mask = MemoryInline::FlatRead8(track + 3);
    const Tables t = ResolveTables(track);
    const TickFrame at = TickToFrame(life, t.header, tick);
    const int32_t bound = int32_t(ChannelBound(mask));
    const uint32_t count = ChannelCount(mask) & 0xFF;

    if (count == 0)
        return;

    const uint32_t stride = count * 4 + 0xC;
    const uint32_t valueBytes = count * 8;
    const uint32_t setBytes = rotate ? valueBytes + 4 : valueBytes;
    const uint32_t keys = t.keys + 4;
    HostKeyOutputs key;
    SearchKeys(int32_t(at.tick), double(at.frame), keys, stride, 0, int32_t(MemoryInline::FlatRead16(t.keys)) - 1, r2, key);
    const bool advance = (MemoryInline::FlatRead8(t.header + 4) & 4) == 0;
    const uint32_t salt = MemoryInline::FlatRead16(t.header + 6);
    struct Pick {
        uint32_t values, seed;
        bool negate;
        float Next() {
            const float base = ReadFloat(values), range = ReadFloat(values + 4);
            values += 8;
            const float v = base + range * float(seed >> 16);
            return negate ? -v : v;
        }
    };

    auto randomSet = [&](uint32_t k, uint32_t loop) {
        Pick p{0, 0, false};
        const uint32_t set = MemoryInline::FlatRead16(k + 0xC);
        p.seed = PickSeed(particleSeed, salt, set, loop);

        if ((MemoryInline::FlatRead8(k + 2) & 2) == 0) {
            p.values = t.random + set * setBytes + 4;
        } else {
            const uint32_t hi = p.seed >> 16, pool = MemoryInline::FlatRead16(t.pool);
            p.values = t.pool + setBytes * (hi - Divide(hi, pool) * pool) + 4;
            p.seed = Lcg(p.seed);
        }

        if (rotate && MemoryInline::FlatRead8(p.values + valueBytes) != 0) {
            p.negate = (p.seed & 0x10000u) == 0;
            p.seed = Lcg(p.seed);
        }

        return p;
    };

    if (key.exact) {
        const uint32_t k = keys + key.index * stride;

        if (MemoryInline::FlatRead8(k + 2) == 0) {
            uint32_t values = k + 0xC;

            for (int32_t bit = 1; bit <= bound; bit = (bit << 1) & 0xFFFF, target += 4) {
                if ((mask & uint32_t(bit)) == 0)
                    continue;

                MemoryInline::FlatWrite32(target, MemoryInline::FlatRead32(values));
                values += 4;
            }

            return;
        }

        Pick p = randomSet(k, at.loop);

        for (int32_t bit = 1; bit <= bound; bit = (bit << 1) & 0xFFFF, target += 4) {
            const bool present = (mask & uint32_t(bit)) != 0;

            if (present)
                WriteFloat(target, p.Next());

            if (advance && (rotate || present))
                p.seed = Lcg(p.seed);
        }

        return;
    }

    uint32_t lowLoop = at.loop, highLoop;
    AdjustLoops(t.header, t.keys, int32_t(key.index), lowLoop, highLoop);
    const float blend = (at.frame - float(uint32_t(key.lower))) / float(int32_t(uint32_t(key.upper) - key.lower));
    const uint32_t lowKey = keys + key.index * stride, highKey = keys + (key.index + 1) * stride;
    uint32_t low = lowKey + 0xC, high = highKey + 0xC, modes = lowKey + 4;
    const bool lowRandom = MemoryInline::FlatRead8(lowKey + 2) != 0;
    const bool highRandom = MemoryInline::FlatRead8(highKey + 2) != 0;
    Pick lowPick{0, 0, false}, highPick{0, 0, false};

    if (lowRandom)
        lowPick = randomSet(lowKey, lowLoop);

    if (highRandom)
        highPick = randomSet(highKey, highLoop);

    for (int32_t bit = 1; bit <= bound; bit = (bit << 1) & 0xFFFF, target += 4, ++modes) {
        const bool present = (mask & uint32_t(bit)) != 0;

        if (present) {
            float a, b;

            if (lowRandom) {
                a = lowPick.Next();
            } else {
                a = ReadFloat(low);
                low += 4;
            }

            if (highRandom) {
                b = highPick.Next();
            } else {
                b = ReadFloat(high);
                high += 4;
            }

            WriteFloat(target, BlendF32(blend, a, b, MemoryInline::FlatRead8(modes), r2));
        }

        if (advance && (rotate || present)) {
            if (lowRandom)
                lowPick.seed = Lcg(lowPick.seed);

            if (highRandom)
                highPick.seed = Lcg(highPick.seed);
        }
    }
}

} // namespace

// For the field kinds (particle_manager_calc.cpp): the key pick's seed.
uint32_t EfPickSeed(uint32_t particleSeed, uint32_t salt, uint32_t set, uint32_t loop) {
    return PickSeed(particleSeed, salt, set, loop);
}

extern "C" void func_8009CE20(CpuContext* ctx);
extern "C" void func_800A2818(CpuContext* ctx);
extern "C" void func_8009F290(CpuContext* ctx);
extern "C" void func_8009EAB0(CpuContext* ctx);
extern "C" void func_800A2568(CpuContext* ctx);
extern "C" void func_8009DF14(CpuContext* ctx);
extern "C" void func_800A0228(CpuContext* ctx);
extern "C" void func_800A2D54(CpuContext* ctx);
extern "C" void func_8009F8C0(CpuContext* ctx);
extern "C" void func_800A035C(CpuContext* ctx);

static void TickToFramePort(CpuContext* p) {
    const TickFrame r = TickToFrame(p->gpr[3], p->gpr[4], p->gpr[5]);
    MemoryInline::FlatWrite16(p->gpr[6], r.tick);
    uint32_t bits;
    std::memcpy(&bits, &r.frame, 4);
    MemoryInline::FlatWrite32(p->gpr[7], bits);
    MemoryInline::FlatWrite32(p->gpr[8], r.loop);
}

static void TickToFrameChecked(uint32_t r1, uint32_t r2, uint32_t life, uint32_t header, uint32_t tick, uint32_t outTick, uint32_t outFrame, uint32_t outLoop) {
#ifdef MH3_DEVELOPMENT
    const bool check = PortCheck::Enabled("AnimCurveTickToFrame");

    if (check) [[unlikely]] {
        CpuContext* c = TryGetCpuContext();
        c->gpr[1] = r1;
        c->gpr[2] = r2;
        c->gpr[3] = life;
        c->gpr[4] = header;
        c->gpr[5] = tick;
        c->gpr[6] = outTick;
        c->gpr[7] = outFrame;
        c->gpr[8] = outLoop;
        PortCheck::RunOutput(c, "AnimCurveTickToFrame", func_8009CE20, TickToFramePort, {{outTick, 2}, {outFrame, 4}, {outLoop, 4}});
        return;
    }
#else
    (void)r1;
    (void)r2;
#endif
    FoldPointer(outTick);
    FoldPointer(outFrame);
    FoldPointer(outLoop);
    const TickFrame r = TickToFrame(life, header, tick);
    MemoryInline::FlatWrite16(outTick, r.tick);
    uint32_t bits;
    std::memcpy(&bits, &r.frame, 4);
    MemoryInline::FlatWrite32(outFrame, bits);
    MemoryInline::FlatWrite32(outLoop, r.loop);
}

extern "C" void MH3_AnimCurveTickToFrame(CpuContext* ctx) {
    TickToFrameChecked(ctx->gpr[1], ctx->gpr[2], ctx->gpr[3], ctx->gpr[4], ctx->gpr[5], ctx->gpr[6], ctx->gpr[7], ctx->gpr[8]);
}
REGISTER_NATIVE_FUNCTION_AS(0x8009CE20, MH3_AnimCurveTickToFrame, "AnimCurveTickToFrame");

// 0x800A2818 (AnimCurveFrameIndex): TickToFrame's whole frame alone, for
// (header r3, tick r4, u16* out r5, life r6).
static void FrameIndexPort(CpuContext* p) {
    MemoryInline::FlatWrite16(p->gpr[5], TickToFrame(p->gpr[6], p->gpr[3], p->gpr[4]).tick);
}

static void FrameIndexChecked(uint32_t r1, uint32_t header, uint32_t tick, uint32_t out, uint32_t life) {
#ifdef MH3_DEVELOPMENT
    const bool check = PortCheck::Enabled("AnimCurveFrameIndex");

    if (check) [[unlikely]] {
        CpuContext* c = TryGetCpuContext();
        c->gpr[1] = r1;
        c->gpr[3] = header;
        c->gpr[4] = tick;
        c->gpr[5] = out;
        c->gpr[6] = life;
        PortCheck::RunOutput(c, "AnimCurveFrameIndex", func_800A2818, FrameIndexPort, {{out, 2}});
        return;
    }
#else
    (void)r1;
#endif
    FoldPointer(out);
    MemoryInline::FlatWrite16(out, TickToFrame(life, header, tick).tick);
}

extern "C" void MH3_AnimCurveFrameIndex(CpuContext* ctx) {
    FrameIndexChecked(ctx->gpr[1], ctx->gpr[3], ctx->gpr[4], ctx->gpr[5], ctx->gpr[6]);
}
REGISTER_NATIVE_FUNCTION_AS(0x800A2818, MH3_AnimCurveFrameIndex, "AnimCurveFrameIndex");

static void StoreTables(const Tables& t, uint32_t header, uint32_t keys, uint32_t random, uint32_t pool, uint32_t names) {
    MemoryInline::FlatWrite32(header, t.header);
    MemoryInline::FlatWrite32(keys, t.keys);
    MemoryInline::FlatWrite32(random, t.random);
    MemoryInline::FlatWrite32(pool, t.pool);
    MemoryInline::FlatWrite32(names, t.names);
}

static void ResolveTablesPort(CpuContext* p) {
    StoreTables(ResolveTables(p->gpr[3]), p->gpr[4], p->gpr[5], p->gpr[6], p->gpr[7], p->gpr[8]);
}

static void ResolveTablesChecked(uint32_t r1, uint32_t track, uint32_t header, uint32_t keys, uint32_t random, uint32_t pool, uint32_t names) {
#ifdef MH3_DEVELOPMENT
    const bool check = PortCheck::Enabled("AnimCurveTables");

    if (check) [[unlikely]] {
        CpuContext* c = TryGetCpuContext();
        c->gpr[1] = r1;
        c->gpr[3] = track;
        c->gpr[4] = header;
        c->gpr[5] = keys;
        c->gpr[6] = random;
        c->gpr[7] = pool;
        c->gpr[8] = names;
        PortCheck::RunOutput(c, "AnimCurveTables", func_8009F290, ResolveTablesPort, {{header, 4}, {keys, 4}, {random, 4}, {pool, 4}, {names, 4}});
        return;
    }
#else
    (void)r1;
#endif
    const Tables t = ResolveTables(track);
    FoldPointer(header);
    FoldPointer(keys);
    FoldPointer(random);
    FoldPointer(pool);
    FoldPointer(names);
    StoreTables(t, header, keys, random, pool, names);
}

extern "C" void MH3_AnimCurveTables(CpuContext* ctx) {
    ResolveTablesChecked(ctx->gpr[1], ctx->gpr[3], ctx->gpr[4], ctx->gpr[5], ctx->gpr[6], ctx->gpr[7], ctx->gpr[8]);
}
REGISTER_NATIVE_FUNCTION_AS(0x8009F290, MH3_AnimCurveTables, "AnimCurveTables");

static void AdjustLoopsStore(uint32_t header, uint32_t keys, uint32_t index, uint32_t lowAddress, uint32_t highAddress) {
    uint32_t low = MemoryInline::FlatRead32(lowAddress), high;
    AdjustLoops(header, keys, int32_t(index), low, high);
    MemoryInline::FlatWrite32(highAddress, high);
    MemoryInline::FlatWrite32(lowAddress, low);
}

static void AdjustLoopsPort(CpuContext* p) {
    AdjustLoopsStore(p->gpr[3], p->gpr[4], p->gpr[5], p->gpr[6], p->gpr[7]);
}

static void AdjustLoopsChecked(uint32_t r1, uint32_t header, uint32_t keys, uint32_t index, uint32_t low, uint32_t high) {
#ifdef MH3_DEVELOPMENT
    const bool check = PortCheck::Enabled("AnimCurveKeyLoops");

    if (check) [[unlikely]] {
        CpuContext* c = TryGetCpuContext();
        c->gpr[1] = r1;
        c->gpr[3] = header;
        c->gpr[4] = keys;
        c->gpr[5] = index;
        c->gpr[6] = low;
        c->gpr[7] = high;
        PortCheck::RunOutput(c, "AnimCurveKeyLoops", func_8009EAB0, AdjustLoopsPort, {{low, 4}, {high, 4}});
        return;
    }
#else
    (void)r1;
#endif
    FoldPointer(header);
    FoldPointer(keys);
    FoldPointer(low);
    FoldPointer(high);
    AdjustLoopsStore(header, keys, index, low, high);
}

extern "C" void MH3_AnimCurveKeyLoops(CpuContext* ctx) {
    AdjustLoopsChecked(ctx->gpr[1], ctx->gpr[3], ctx->gpr[4], ctx->gpr[5], ctx->gpr[6], ctx->gpr[7]);
}
REGISTER_NATIVE_FUNCTION_AS(0x8009EAB0, MH3_AnimCurveKeyLoops, "AnimCurveKeyLoops");

static void BakedU8Port(CpuContext* p) {
    ExecuteBaked<1>(p->gpr[3], p->gpr[4], p->gpr[5], p->gpr[6]);
}

static void BakedU8Checked(uint32_t r1, uint32_t track, uint32_t target, uint32_t tick, uint32_t life) {
#ifdef MH3_DEVELOPMENT
    const bool check = PortCheck::Enabled("AnimCurveExecuteBakedU8");

    if (check) [[unlikely]] {
        CpuContext* c = TryGetCpuContext();
        c->gpr[1] = r1;
        c->gpr[3] = track;
        c->gpr[4] = target;
        c->gpr[5] = tick;
        c->gpr[6] = life;
        PortCheck::RunOutput(c, "AnimCurveExecuteBakedU8", func_800A2568, BakedU8Port, {{target, 8}});
        return;
    }
#else
    (void)r1;
#endif
    ExecuteBaked<1>(track, target, tick, life);
}

extern "C" void MH3_AnimCurveExecuteBakedU8(CpuContext* ctx) {
    BakedU8Checked(ctx->gpr[1], ctx->gpr[3], ctx->gpr[4], ctx->gpr[5], ctx->gpr[6]);
}
REGISTER_NATIVE_FUNCTION_AS(0x800A2568, MH3_AnimCurveExecuteBakedU8, "AnimCurveExecuteBakedU8");

static void ExecuteU8Port(CpuContext* p) {
    ExecuteU8(p->gpr[3], p->gpr[4], p->gpr[5], p->gpr[6], p->gpr[7], p->gpr[2]);
}

static void ExecuteU8Checked(uint32_t r1, uint32_t r2, uint32_t track, uint32_t target, uint32_t tick, uint32_t seed, uint32_t life) {
#ifdef MH3_DEVELOPMENT
    const bool check = PortCheck::Enabled("AnimCurveExecuteU8");

    if (check) [[unlikely]] {
        CpuContext* c = TryGetCpuContext();
        c->gpr[1] = r1;
        c->gpr[2] = r2;
        c->gpr[3] = track;
        c->gpr[4] = target;
        c->gpr[5] = tick;
        c->gpr[6] = seed;
        c->gpr[7] = life;
        PortCheck::RunOutput(c, "AnimCurveExecuteU8", func_8009DF14, ExecuteU8Port, {{target, 8}});
        return;
    }
#else
    (void)r1;
#endif
    ExecuteU8(track, target, tick, seed, life, r2);
}

extern "C" void MH3_AnimCurveExecuteU8(CpuContext* ctx) {
    ExecuteU8Checked(ctx->gpr[1], ctx->gpr[2], ctx->gpr[3], ctx->gpr[4], ctx->gpr[5], ctx->gpr[6], ctx->gpr[7]);
}
REGISTER_NATIVE_FUNCTION_AS(0x8009DF14, MH3_AnimCurveExecuteU8, "AnimCurveExecuteU8");

static void BlendF32Port(CpuContext* p) {
    p->fpr[1].d = double(BlendF32(float(p->fpr[1].d), float(p->fpr[2].d), float(p->fpr[3].d), p->gpr[3], p->gpr[2]));
}

static double BlendF32Checked(uint32_t r1, uint32_t r2, uint32_t mode, double t, double a, double b) {
#ifdef MH3_DEVELOPMENT
    const bool check = PortCheck::Enabled("AnimCurveBlendF32");

    if (check) [[unlikely]] {
        CpuContext* c = TryGetCpuContext();
        c->gpr[1] = r1;
        c->gpr[2] = r2;
        c->gpr[3] = mode;
        c->fpr[1].d = t;
        c->fpr[2].d = a;
        c->fpr[3].d = b;
        PortCheck::RunPure(c, "AnimCurveBlendF32", PortCheck::Returns::Fpr1, func_800A0228, BlendF32Port);
        return c->fpr[1].d;
    }
#else
    (void)r1;
#endif
    return double(BlendF32(float(t), float(a), float(b), mode, r2));
}

extern "C" void MH3_AnimCurveBlendF32(CpuContext* ctx) {
    ctx->fpr[1].d = BlendF32Checked(ctx->gpr[1], ctx->gpr[2], ctx->gpr[3], ctx->fpr[1].d, ctx->fpr[2].d, ctx->fpr[3].d);
}
REGISTER_NATIVE_FUNCTION_AS(0x800A0228, MH3_AnimCurveBlendF32, "AnimCurveBlendF32");

static void BakedF32Port(CpuContext* p) {
    ExecuteBaked<4>(p->gpr[3], p->gpr[4], p->gpr[5], p->gpr[6]);
}

static void BakedF32Checked(uint32_t r1, uint32_t track, uint32_t target, uint32_t tick, uint32_t life) {
#ifdef MH3_DEVELOPMENT
    const bool check = PortCheck::Enabled("AnimCurveExecuteBakedF32");

    if (check) [[unlikely]] {
        CpuContext* c = TryGetCpuContext();
        c->gpr[1] = r1;
        c->gpr[3] = track;
        c->gpr[4] = target;
        c->gpr[5] = tick;
        c->gpr[6] = life;
        PortCheck::RunOutput(c, "AnimCurveExecuteBakedF32", func_800A2D54, BakedF32Port, {{target, 32}});
        return;
    }
#else
    (void)r1;
#endif
    ExecuteBaked<4>(track, target, tick, life);
}

extern "C" void MH3_AnimCurveExecuteBakedF32(CpuContext* ctx) {
    BakedF32Checked(ctx->gpr[1], ctx->gpr[3], ctx->gpr[4], ctx->gpr[5], ctx->gpr[6]);
}
REGISTER_NATIVE_FUNCTION_AS(0x800A2D54, MH3_AnimCurveExecuteBakedF32, "AnimCurveExecuteBakedF32");

static void ExecuteF32Port(CpuContext* p) {
    ExecuteFloat<false>(p->gpr[3], p->gpr[4], p->gpr[5], p->gpr[6], p->gpr[7], p->gpr[2]);
}

static void ExecuteF32Checked(uint32_t r1, uint32_t r2, uint32_t track, uint32_t target, uint32_t tick, uint32_t seed, uint32_t life) {
#ifdef MH3_DEVELOPMENT
    const bool check = PortCheck::Enabled("AnimCurveExecuteF32");

    if (check) [[unlikely]] {
        CpuContext* c = TryGetCpuContext();
        c->gpr[1] = r1;
        c->gpr[2] = r2;
        c->gpr[3] = track;
        c->gpr[4] = target;
        c->gpr[5] = tick;
        c->gpr[6] = seed;
        c->gpr[7] = life;
        PortCheck::RunOutput(c, "AnimCurveExecuteF32", func_8009F8C0, ExecuteF32Port, {{target, 32}});
        return;
    }
#else
    (void)r1;
#endif
    ExecuteFloat<false>(track, target, tick, seed, life, r2);
}

extern "C" void MH3_AnimCurveExecuteF32(CpuContext* ctx) {
    ExecuteF32Checked(ctx->gpr[1], ctx->gpr[2], ctx->gpr[3], ctx->gpr[4], ctx->gpr[5], ctx->gpr[6], ctx->gpr[7]);
}
REGISTER_NATIVE_FUNCTION_AS(0x8009F8C0, MH3_AnimCurveExecuteF32, "AnimCurveExecuteF32");

static void ExecuteRotatePort(CpuContext* p) {
    ExecuteFloat<true>(p->gpr[3], p->gpr[4], p->gpr[5], p->gpr[6], p->gpr[7], p->gpr[2]);
}

static void ExecuteRotateChecked(uint32_t r1, uint32_t r2, uint32_t track, uint32_t target, uint32_t tick, uint32_t seed, uint32_t life) {
#ifdef MH3_DEVELOPMENT
    const bool check = PortCheck::Enabled("AnimCurveExecuteRotate");

    if (check) [[unlikely]] {
        CpuContext* c = TryGetCpuContext();
        c->gpr[1] = r1;
        c->gpr[2] = r2;
        c->gpr[3] = track;
        c->gpr[4] = target;
        c->gpr[5] = tick;
        c->gpr[6] = seed;
        c->gpr[7] = life;
        PortCheck::RunOutput(c, "AnimCurveExecuteRotate", func_800A035C, ExecuteRotatePort, {{target, 32}});
        return;
    }
#else
    (void)r1;
#endif
    ExecuteFloat<true>(track, target, tick, seed, life, r2);
}

extern "C" void MH3_AnimCurveExecuteRotate(CpuContext* ctx) {
    ExecuteRotateChecked(ctx->gpr[1], ctx->gpr[2], ctx->gpr[3], ctx->gpr[4], ctx->gpr[5], ctx->gpr[6], ctx->gpr[7]);
}
REGISTER_NATIVE_FUNCTION_AS(0x800A035C, MH3_AnimCurveExecuteRotate, "AnimCurveExecuteRotate");
