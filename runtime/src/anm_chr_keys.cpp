// Character-animation keys, RMHP08. C reconstructions of NW4R g3d's
// ResAnmChr hermite-key evaluation (g3d_resanmchr.cpp), decompiled with
// Ghidra and checked against the Gekko disassembly.
//
// 0x8008B264, 0x8008B9C0 and 0x8008AAF0 evaluate FVS48, FVS32 and FVS96
// tracks. 0x8008B264 evaluates an FVS48 track at a frame: a header (+0 u16 key count,
// +4 f32 key count over the frame span, +8 f32 value scale, +0xC f32 value
// offset) and 6-byte keys from +0x10 (s16 frame through GQR6, u16 value
// through GQR3, s16 slope through GQR7). Before the first key or after the
// last the value is that key's; otherwise the key count times the frame's
// share of the span estimates the key, a walk on the raw s16 frames (against
// the frame quantized the same way) finds the one at or before the frame, and
// on a key the value is its own, between two keys their hermite blend.
//
// The debug NW4R build calls a function for every accessor: a key's frame,
// value, slope, the next key, the u16 conversions, the reciprocal, each through
// the full guest call glue, several levels deep. The ports inline all of it
// and keep the Gekko arithmetic (the same helpers the translated code uses),
// so they match the originals bit for bit.
//
// The registered bodies only move registers, so callers keep the precise call
// boundary the translated functions had.
#include "abi_bridge.h"
#include "memory.h"
#include "port_check.h"
#include "ppc_runtime.h"

#include <cstdint>
#include <stdexcept>
#include <string>

namespace {

enum : uint32_t {
    kKeyCount = 0x00, // u16
    kInvSpan = 0x04,  // f32: key count / frame span
    kScale = 0x08,    // f32, FVS32 and FVS48
    kOffset = 0x0C,   // f32, FVS32 and FVS48
};

// NW4R's pointer check (NW4R_ASSERT_CHECK_POINTER): MEM1, MEM2 or the locked
// cache, cached or not.
bool ValidPointer(uint32_t p) {
    return (p & 0xFF000000u) == 0x80000000u || (p & 0xFF800000u) == 0x81000000u ||
           (p & 0xF8000000u) == 0x90000000u || (p & 0xFF000000u) == 0xC0000000u ||
           (p & 0xFF800000u) == 0xC1000000u || (p & 0xF8000000u) == 0xD0000000u ||
           (p & 0xFFFFC000u) == 0xE0000000u;
}

// nw4r::db::Panic ends the game (it halts); so does the port.
[[noreturn]] void Panic(const char* what, uint32_t value) {
    char text[128];
    std::snprintf(text, sizeof text, "NW4R panic (g3d_resanmchr.cpp): %s (%08x)", what, value);
    throw std::runtime_error(text);
}

uint32_t Gqr(unsigned index) { return TryGetCpuContext()->gqr[index]; }

// psq_l with W=1 (ps0 the value, ps1 1.0), and its ps0.
template <uint32_t I>
double PsqLoad1(uint32_t gqr, uint32_t address) { return PPC_PsqLGqrInline<1u, I>(gqr, address); }
template <uint32_t I>
double PsqLoadScalar(uint32_t gqr, uint32_t address) { return PPC_PsToScalarInline(PsqLoad1<I>(gqr, address)); }

// psq_st with W=1 of a scalar, to a word of the guest stack below the caller's
// frame (where the original's helper frames store it too).
template <uint32_t I>
void PsqStore1(uint32_t gqr, uint32_t scratch, double scalar) {
    PPC_PsqStStateInline<1u, I, true>(gqr, scratch, PPC_PsFromScalarInline(scalar));
}

// stfs then lfs.
double ThroughSingle(uint32_t scratch, double value) {
    MemoryInline::FlatWriteRamFloat32(scratch, value);
    return MemoryInline::FlatReadFloat32(scratch);
}

// 0x80061110: fres refined by one Newton step, in paired arithmetic.
double Reciprocal(double x) {
    const double estimate = PPC_Fres(PPC_PsFromScalarInline(x));
    const double twice = PPC_PsAddInline(estimate, estimate);
    const double squared = PPC_PsMulInline(estimate, estimate);
    return PPC_PsNmsubInline(PPC_PsFromScalarInline(x), squared, twice);
}

// 0x8008AF34: the hermite blend of (v0, t0) and (v1, t1) at s (the frame's
// share of the span) over d (the frame past the first key). Constants from the
// small-data area.
double Hermite(uint32_t r2, double v0, double t0, double v1, double t1, double s, double d) {
    double f0 = MemoryInline::FlatReadFloat32(r2 - 31672);
    const double f7 = static_cast<double>(PpcForceSingleValueInline(s - f0));
    const double f6 = PpcFmulsInline(d, f7);
    f0 = PpcFmulsInline(s, t1);
    const double f4 = PPC_Fmadds(f7, t0, f0);
    double f2 = MemoryInline::FlatReadFloat32(r2 - 31668);
    f0 = MemoryInline::FlatReadFloat32(r2 - 31664);
    f2 = PPC_Fmsubs(f2, s, f0);
    f0 = static_cast<double>(PpcForceSingleValueInline(v0 - v1));
    f0 = PpcFmulsInline(f2, f0);
    f0 = PpcFmulsInline(s, f0);
    f0 = PPC_Fmadds(s, f0, v0);
    return PPC_Fmadds(f6, f4, f0);
}

// The key formats. Each gives a key's address, frame, value and slope, the
// frame as the walk compares it (Walk) and a key's frame that way (Raw).
struct TrackBase {
    uint32_t anm, scratch, scratch2, gqr2, gqr3, gqr6, gqr7;
    // scale * value + offset, the value a u16 through GQR3 (0x8008B780, 0x8008BF6C).
    double ScaledU16(uint32_t address) const {
        const double value = ThroughSingle(scratch2, PsqLoadScalar<3u>(gqr3, address));
        return PPC_Fmadds(MemoryInline::FlatReadFloat32(anm + kScale), value, MemoryInline::FlatReadFloat32(anm + kOffset));
    }
};

// FVS48: 6-byte keys from +0x10, s16 frame (GQR6), u16 value, s16 slope (GQR7).
struct Fvs48 : TrackBase {
    static constexpr uint32_t kSize = 6;
    uint32_t Key(uint32_t index) const { return anm + 0x10 + index * kSize; }
    double Frame(uint32_t key) const { return PsqLoadScalar<6u>(gqr6, key); }
    double Value(uint32_t key) const { return ScaledU16(key + 2); }
    double Slope(uint32_t key) const { return PsqLoadScalar<7u>(gqr7, key + 4); }
    int32_t Walk(double frame) const {
        PsqStore1<6u>(gqr6, scratch, frame);
        return int16_t(MemoryInline::FlatRead16(scratch));
    }
    int32_t Raw(uint32_t key) const { return int16_t(MemoryInline::FlatRead16(key)); }
};

// FVS32: 4-byte keys from +0x10, a word of u8 frame (GQR2), 12-bit value and
// signed 12-bit slope (through GQR6).
struct Fvs32 : TrackBase {
    static constexpr uint32_t kSize = 4;
    uint32_t Key(uint32_t index) const { return anm + 0x10 + index * kSize; }
    double Frame(uint32_t key) const { return ThroughSingle(scratch, PsqLoadScalar<2u>(gqr2, key)); }
    double Value(uint32_t key) const {
        MemoryInline::FlatWriteRam16(scratch, uint16_t((MemoryInline::FlatRead32(key) >> 12) & 0xFFF));
        return ScaledU16(scratch);
    }
    double Slope(uint32_t key) const {
        const int32_t slope = int32_t(MemoryInline::FlatRead32(key) << 20) >> 20;
        MemoryInline::FlatWriteRam16(scratch, uint16_t(slope));
        return PsqLoadScalar<6u>(gqr6, scratch);
    }
    uint32_t Walk(double frame) const {
        PsqStore1<2u>(gqr2, scratch, ThroughSingle(scratch, frame));
        return MemoryInline::FlatRead8(scratch);
    }
    uint32_t Raw(uint32_t key) const { return MemoryInline::FlatRead8(key); }
};

// FVS96: 12-byte keys from +8 of f32 frame, value and slope; the walk compares
// the frames themselves.
struct Fvs96 : TrackBase {
    static constexpr uint32_t kSize = 12;
    uint32_t Key(uint32_t index) const { return anm + 8 + index * kSize; }
    double Frame(uint32_t key) const { return MemoryInline::FlatReadFloat32(key); }
    double Value(uint32_t key) const { return MemoryInline::FlatReadFloat32(key + 4); }
    double Slope(uint32_t key) const { return MemoryInline::FlatReadFloat32(key + 8); }
    double Walk(double frame) const { return frame; }
    double Raw(uint32_t key) const { return Frame(key); }
};

template <typename Track>
double Evaluate(uint32_t r1, uint32_t r2, uint32_t anm, double frame) {
    if (!ValidPointer(anm)) Panic("NW4R:Pointer Error pAnm is not valid", anm);
    const uint32_t scratch = r1 - 8;
    Track t;
    static_cast<TrackBase&>(t) = TrackBase{anm, scratch, r1 - 16, Gqr(2), Gqr(3), Gqr(6), Gqr(7)};
    const uint32_t count = MemoryInline::FlatRead16(anm + kKeyCount);
    const uint32_t first = t.Key(0), last = t.Key(count - 1);

    if (frame <= t.Frame(first)) return t.Value(first);
    if (t.Frame(last) <= frame) return t.Value(last);

    // The key estimate: (frame - first frame) * count * (count / span), as u16.
    const double past = static_cast<double>(PpcForceSingleValueInline(frame - t.Frame(first)));
    MemoryInline::FlatWriteRam16(scratch, uint16_t(count));
    const double countF = ThroughSingle(scratch, PsqLoadScalar<3u>(t.gqr3, scratch));
    double estimate = PpcFmulsInline(past, countF);
    estimate = PpcFmulsInline(MemoryInline::FlatReadFloat32(anm + kInvSpan), estimate);
    PsqStore1<3u>(t.gqr3, scratch, ThroughSingle(scratch, estimate));
    const uint32_t index = MemoryInline::FlatRead16(scratch);
    if (int32_t(index) > int32_t(count) - 1) Panic("estimatePos is out of bounds", index);

    // Walk to the key at or before the frame.
    const auto walk = t.Walk(frame);
    uint32_t key = t.Key(index);
    if (walk < t.Raw(key)) {
        do key -= Track::kSize;
        while (walk < t.Raw(key));
    } else {
        do key += Track::kSize;
        while (t.Raw(key) <= walk);
        key -= Track::kSize;
    }

    if (frame == t.Frame(key)) return t.Value(key);
    const uint32_t next = key + Track::kSize;
    const double v0 = t.Value(key), t0 = t.Slope(key);
    const double v1 = t.Value(next), t1 = t.Slope(next);
    const double f0 = t.Frame(key), f1 = t.Frame(next);
    if (!(f0 <= frame && frame <= f1)) Panic("frame is out of bounds", 0);
    const double d = static_cast<double>(PpcForceSingleValueInline(frame - f0));
    const double span = static_cast<double>(PpcForceSingleValueInline(f1 - f0));
    const double s = PpcFmulsInline(d, PPC_PsToScalarInline(Reciprocal(span)));
    return Hermite(r2, v0, t0, v1, t1, s, d);
}

} // namespace

// The ports, or with MH3_PORT_CHECK naming one the port checked against the
// original on every call (registers from the current context).
template <typename Track>
double Checked(const char* name, void (*original)(CpuContext*), void (*port)(CpuContext*), bool check,
               uint32_t r1, uint32_t r2, uint32_t r3, double f1) {
#ifdef MH3_DEVELOPMENT
    if (check) [[unlikely]] {
        CpuContext* c = TryGetCpuContext();
        c->gpr[1] = r1;
        c->gpr[2] = r2;
        c->gpr[3] = r3;
        c->fpr[1].d = f1;
        PortCheck::RunPure(c, name, PortCheck::Returns::Fpr1, original, port);
        return c->fpr[1].d;
    }
#else
    (void)name, (void)original, (void)port, (void)check;
#endif
    return Evaluate<Track>(r1, r2, r3, f1);
}

#define MH3_ANM_CHR_KEYS(address, track, name)                                                              \
    extern "C" void func_##address(CpuContext* ctx);                                                        \
    static void name##Port(CpuContext* p) {                                                                 \
        p->fpr[1].d = Evaluate<track>(p->gpr[1], p->gpr[2], p->gpr[3], p->fpr[1].d);                        \
    }                                                                                                       \
    static double name##Checked(uint32_t r1, uint32_t r2, uint32_t r3, double f1) {                        \
        static const bool check = PortCheck::Enabled(#name);                                                \
        return Checked<track>(#name, func_##address, name##Port, check, r1, r2, r3, f1);                    \
    }

MH3_ANM_CHR_KEYS(8008B264, Fvs48, AnmChrKeyEvalFVS48)
MH3_ANM_CHR_KEYS(8008B9C0, Fvs32, AnmChrKeyEvalFVS32)
MH3_ANM_CHR_KEYS(8008AAF0, Fvs96, AnmChrKeyEvalFVS96)

extern "C" void MH3_AnmChrKeyEvalFVS48(CpuContext* ctx) {
    ctx->fpr[1].d = AnmChrKeyEvalFVS48Checked(ctx->gpr[1], ctx->gpr[2], ctx->gpr[3], ctx->fpr[1].d);
}
REGISTER_NATIVE_FUNCTION_AS(0x8008B264, MH3_AnmChrKeyEvalFVS48, "AnmChrKeyEvalFVS48");

extern "C" void MH3_AnmChrKeyEvalFVS32(CpuContext* ctx) {
    ctx->fpr[1].d = AnmChrKeyEvalFVS32Checked(ctx->gpr[1], ctx->gpr[2], ctx->gpr[3], ctx->fpr[1].d);
}
REGISTER_NATIVE_FUNCTION_AS(0x8008B9C0, MH3_AnmChrKeyEvalFVS32, "AnmChrKeyEvalFVS32");

extern "C" void MH3_AnmChrKeyEvalFVS96(CpuContext* ctx) {
    ctx->fpr[1].d = AnmChrKeyEvalFVS96Checked(ctx->gpr[1], ctx->gpr[2], ctx->gpr[3], ctx->fpr[1].d);
}
REGISTER_NATIVE_FUNCTION_AS(0x8008AAF0, MH3_AnmChrKeyEvalFVS96, "AnmChrKeyEvalFVS96");
