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

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <type_traits>
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

// The lean evaluation: the same walk in host float arithmetic. Quantized loads
// and single multiplies and subtracts are exact in float, so they and the key
// choice match the Gekko path; the fused multiply-adds (no FMA unit on the
// Cortex-A9, so libm) are a double multiply-add rounded to single and the
// reciprocal is a divide, which can differ in the last bits.
float Pow2(int e) { return std::bit_cast<float>(uint32_t(127 + e) << 23); }
// A GQR's load factor (2^-scale) and store factor (2^scale), scales 6-bit signed.
float LoadFactor(uint32_t gqr) {
    const int scale = int(gqr >> 24 & 0x3F);
    return Pow2(scale < 32 ? -scale : 64 - scale);
}
float StoreFactor(uint32_t gqr) {
    const int scale = int(gqr >> 8 & 0x3F);
    return Pow2(scale < 32 ? scale : scale - 64);
}
// psq_st to an integer type: scale, clamp and truncate.
template <typename T>
T Quantize(float value, float factor) {
    return T(std::clamp(value * factor, float(std::numeric_limits<T>::min()), float(std::numeric_limits<T>::max())));
}
float Madd(float a, float b, float c) { return float(double(a) * double(b) + double(c)); }

// GQR types (bits 16-18 load, 0-2 store) the lean tracks assume: u8 for GQR2,
// u16 for GQR3, s16 for GQR6 and GQR7.
bool LeanGqrs(uint32_t gqr2, uint32_t gqr3, uint32_t gqr6, uint32_t gqr7) {
    auto type = [](uint32_t gqr, uint32_t t) { return (gqr >> 16 & 7) == t && (gqr & 7) == t; };
    return type(gqr2, 4) && type(gqr3, 5) && type(gqr6, 7) && type(gqr7, 7);
}

struct LeanBase {
    uint32_t anm;
    float scale, offset, u8Load, u8Store, u16Load, u16Store, s16Load6, s16Store6, s16Load7;
    float ScaledU16(uint32_t raw) const { return Madd(scale, float(raw) * u16Load, offset); }
};

struct LeanFvs48 : LeanBase {
    static constexpr uint32_t kSize = 6;
    uint32_t Key(uint32_t index) const { return anm + 0x10 + index * kSize; }
    float Frame(uint32_t key) const { return float(int16_t(MemoryInline::FlatRead16(key))) * s16Load6; }
    float Value(uint32_t key) const { return ScaledU16(MemoryInline::FlatRead16(key + 2)); }
    float Slope(uint32_t key) const { return float(int16_t(MemoryInline::FlatRead16(key + 4))) * s16Load7; }
    int32_t Walk(float frame) const { return Quantize<int16_t>(frame, s16Store6); }
    int32_t Raw(uint32_t key) const { return int16_t(MemoryInline::FlatRead16(key)); }
};

struct LeanFvs32 : LeanBase {
    static constexpr uint32_t kSize = 4;
    uint32_t Key(uint32_t index) const { return anm + 0x10 + index * kSize; }
    float Frame(uint32_t key) const { return float(MemoryInline::FlatRead8(key)) * u8Load; }
    float Value(uint32_t key) const { return ScaledU16(MemoryInline::FlatRead32(key) >> 12 & 0xFFF); }
    float Slope(uint32_t key) const {
        return float(int32_t(MemoryInline::FlatRead32(key) << 20) >> 20) * s16Load6;
    }
    uint32_t Walk(float frame) const { return Quantize<uint8_t>(frame, u8Store); }
    uint32_t Raw(uint32_t key) const { return MemoryInline::FlatRead8(key); }
};

struct LeanFvs96 : LeanBase {
    static constexpr uint32_t kSize = 12;
    uint32_t Key(uint32_t index) const { return anm + 8 + index * kSize; }
    float Frame(uint32_t key) const { return MemoryInline::FlatReadFloat32(key); }
    float Value(uint32_t key) const { return MemoryInline::FlatReadFloat32(key + 4); }
    float Slope(uint32_t key) const { return MemoryInline::FlatReadFloat32(key + 8); }
    float Walk(float frame) const { return frame; }
    float Raw(uint32_t key) const { return Frame(key); }
};

template <typename Track> struct Lean;
template <> struct Lean<Fvs48> { using Type = LeanFvs48; };
template <> struct Lean<Fvs32> { using Type = LeanFvs32; };
template <> struct Lean<Fvs96> { using Type = LeanFvs96; };

template <typename Track>
double EvaluateLean(uint32_t r2, uint32_t anm, float frame, const CpuContext& cpu) {
    if (!ValidPointer(anm)) Panic("NW4R:Pointer Error pAnm is not valid", anm);
    typename Lean<Track>::Type t;
    LeanBase& b = t;
    b.anm = anm;
    if constexpr (!std::is_same_v<Track, Fvs96>) {
        b.scale = MemoryInline::FlatReadFloat32(anm + kScale);
        b.offset = MemoryInline::FlatReadFloat32(anm + kOffset);
        b.u8Load = LoadFactor(cpu.gqr[2]), b.u8Store = StoreFactor(cpu.gqr[2]);
        b.s16Load6 = LoadFactor(cpu.gqr[6]), b.s16Store6 = StoreFactor(cpu.gqr[6]);
        b.s16Load7 = LoadFactor(cpu.gqr[7]);
    }
    b.u16Load = LoadFactor(cpu.gqr[3]), b.u16Store = StoreFactor(cpu.gqr[3]);
    const uint32_t count = MemoryInline::FlatRead16(anm + kKeyCount);
    const uint32_t first = t.Key(0), last = t.Key(count - 1);

    const float firstFrame = t.Frame(first);
    if (frame <= firstFrame) return t.Value(first);
    if (t.Frame(last) <= frame) return t.Value(last);

    const float estimate = MemoryInline::FlatReadFloat32(anm + kInvSpan) *
                           ((frame - firstFrame) * (float(count) * b.u16Load));
    const uint32_t index = Quantize<uint16_t>(estimate, b.u16Store);
    if (int32_t(index) > int32_t(count) - 1) Panic("estimatePos is out of bounds", index);

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

    const float f0 = t.Frame(key);
    if (frame == f0) return t.Value(key);
    const uint32_t next = key + Track::kSize;
    const float v0 = t.Value(key), t0 = t.Slope(key);
    const float v1 = t.Value(next), t1 = t.Slope(next);
    const float f1 = t.Frame(next);
    if (!(f0 <= frame && frame <= f1)) Panic("frame is out of bounds", 0);
    const float d = frame - f0;
    const float s = d * (1.0f / (f1 - f0));

    // Hermite, as 0x8008AF34.
    const float c0 = MemoryInline::FlatReadFloat32(r2 - 31672);
    const float c1 = MemoryInline::FlatReadFloat32(r2 - 31668);
    const float c2 = MemoryInline::FlatReadFloat32(r2 - 31664);
    const float f7 = s - c0;
    const float f6 = d * f7;
    const float f4 = Madd(f7, t0, s * t1);
    const float f2 = float(double(c1) * double(s) - double(c2));
    const float blend = Madd(s, s * (f2 * (v0 - v1)), v0);
    return Madd(f6, f4, blend);
}

// The evaluation the ports use: lean when the GQRs have the types it assumes.
// MH3_ANM_KEYS_CHECK=1 (debugging): compare it with the Gekko path and report
// the calls that differ by more than 1e-4 relative.
template <typename Track>
double EvaluateFast(const CpuContext& cpu, uint32_t r1, uint32_t r2, uint32_t anm, double frame) {
    if (!LeanGqrs(cpu.gqr[2], cpu.gqr[3], cpu.gqr[6], cpu.gqr[7])) [[unlikely]] {
#ifdef MH3_DEVELOPMENT
        static bool logged = false;
        if (!logged) {
            logged = true;
            std::fprintf(stderr, "MH3 anm keys: GQRs %08x %08x %08x %08x not the lean types, Gekko path\n",
                         cpu.gqr[2], cpu.gqr[3], cpu.gqr[6], cpu.gqr[7]);
        }
#endif
        return Evaluate<Track>(r1, r2, anm, frame);
    }
    const double lean = EvaluateLean<Track>(r2, anm, float(frame), cpu);
#ifdef MH3_DEVELOPMENT
    static const bool check = std::getenv("MH3_ANM_KEYS_CHECK") != nullptr;
    if (check) [[unlikely]] {
        static uint64_t checked = 0, differing = 0, far = 0;
        const double exact = Evaluate<Track>(r1, r2, anm, frame);
        ++checked;
        if (lean != exact) {
            ++differing;
            if (std::fabs(lean - exact) > 1e-4 * std::max(1.0, std::fabs(exact)) && ++far <= 16)
                std::fprintf(stderr, "MH3 anm keys: track %08x frame %.6f lean %.9g exact %.9g\n", anm, frame, lean,
                             exact);
        }
        if (checked % 200000 == 0)
            std::fprintf(stderr, "MH3 anm keys: %llu checked, %llu differ in the last bits, %llu beyond 1e-4\n",
                         (unsigned long long)checked, (unsigned long long)differing, (unsigned long long)far);
    }
#endif
    return lean;
}

} // namespace

// The ports, or with MH3_PORT_CHECK naming one the (Gekko path) port checked
// against the original on every call (registers from the current context).
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
    return EvaluateFast<Track>(*TryGetCpuContext(), r1, r2, r3, f1);
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
