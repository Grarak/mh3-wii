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
#include "g3d_native.h"
#include "guest_call.h"
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

#ifdef MH3_DEVELOPMENT
void MH3FrameSplitCall(unsigned kind); // game_frame.cpp
#endif
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
    return (p & 0xFF000000u) == 0x80000000u || (p & 0xFF800000u) == 0x81000000u || (p & 0xF8000000u) == 0x90000000u || (p & 0xFF000000u) == 0xC0000000u || (p & 0xFF800000u) == 0xC1000000u ||
           (p & 0xF8000000u) == 0xD0000000u || (p & 0xFFFFC000u) == 0xE0000000u;
}

// nw4r::db::Panic ends the game (it halts); so does the port.
[[noreturn]] void Panic(const char* what, uint32_t value) {
    char text[128];
    std::snprintf(text, sizeof text, "NW4R panic (g3d_resanmchr.cpp): %s (%08x)", what, value);
    throw std::runtime_error(text);
}

uint32_t Gqr(unsigned index) {
    return TryGetCpuContext()->gqr[index];
}

// psq_l with W=1 (ps0 the value, ps1 1.0), and its ps0.
template <uint32_t I>
double PsqLoad1(uint32_t gqr, uint32_t address) {
    return PPC_PsqLGqrInline<1u, I>(gqr, address);
}
template <uint32_t I>
double PsqLoadScalar(uint32_t gqr, uint32_t address) {
    return PPC_PsToScalarInline(PsqLoad1<I>(gqr, address));
}

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
    if (!ValidPointer(anm))
        Panic("NW4R:Pointer Error pAnm is not valid", anm);
    const uint32_t scratch = r1 - 8;
    Track t;
    static_cast<TrackBase&>(t) = TrackBase{anm, scratch, r1 - 16, Gqr(2), Gqr(3), Gqr(6), Gqr(7)};
    const uint32_t count = MemoryInline::FlatRead16(anm + kKeyCount);
    const uint32_t first = t.Key(0), last = t.Key(count - 1);

    if (frame <= t.Frame(first))
        return t.Value(first);
    if (t.Frame(last) <= frame)
        return t.Value(last);

    // The key estimate: (frame - first frame) * count * (count / span), as u16.
    const double past = static_cast<double>(PpcForceSingleValueInline(frame - t.Frame(first)));
    MemoryInline::FlatWriteRam16(scratch, uint16_t(count));
    const double countF = ThroughSingle(scratch, PsqLoadScalar<3u>(t.gqr3, scratch));
    double estimate = PpcFmulsInline(past, countF);
    estimate = PpcFmulsInline(MemoryInline::FlatReadFloat32(anm + kInvSpan), estimate);
    PsqStore1<3u>(t.gqr3, scratch, ThroughSingle(scratch, estimate));
    const uint32_t index = MemoryInline::FlatRead16(scratch);
    if (int32_t(index) > int32_t(count) - 1)
        Panic("estimatePos is out of bounds", index);

    // Walk to the key at or before the frame.
    const auto walk = t.Walk(frame);
    uint32_t key = t.Key(index);
    if (walk < t.Raw(key)) {
        do
            key -= Track::kSize;
        while (walk < t.Raw(key));
    } else {
        do
            key += Track::kSize;
        while (t.Raw(key) <= walk);
        key -= Track::kSize;
    }

    if (frame == t.Frame(key))
        return t.Value(key);
    const uint32_t next = key + Track::kSize;
    const double v0 = t.Value(key), t0 = t.Slope(key);
    const double v1 = t.Value(next), t1 = t.Slope(next);
    const double f0 = t.Frame(key), f1 = t.Frame(next);
    if (!(f0 <= frame && frame <= f1))
        Panic("frame is out of bounds", 0);
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
float Pow2(int e) {
    return std::bit_cast<float>(uint32_t(127 + e) << 23);
}
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
float Madd(float a, float b, float c) {
    return float(double(a) * double(b) + double(c));
}

// GQR types (bits 16-18 load, 0-2 store) the lean tracks assume: u8 for GQR2,
// u16 for GQR3, s16 for GQR6 and GQR7.
bool LeanGqrs(uint32_t gqr2, uint32_t gqr3, uint32_t gqr6, uint32_t gqr7) {
    auto type = [](uint32_t gqr, uint32_t t) {
        return (gqr >> 16 & 7) == t && (gqr & 7) == t;
    };
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
    float Slope(uint32_t key) const { return float(int32_t(MemoryInline::FlatRead32(key) << 20) >> 20) * s16Load6; }
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

template <typename Track>
struct Lean;
template <>
struct Lean<Fvs48> {
    using Type = LeanFvs48;
};
template <>
struct Lean<Fvs32> {
    using Type = LeanFvs32;
};
template <>
struct Lean<Fvs96> {
    using Type = LeanFvs96;
};

template <typename Track>
double EvaluateLean(uint32_t r2, uint32_t anm, float frame, const CpuContext& cpu) {
    if (!ValidPointer(anm))
        Panic("NW4R:Pointer Error pAnm is not valid", anm);
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
    if (frame <= firstFrame)
        return t.Value(first);
    if (t.Frame(last) <= frame)
        return t.Value(last);

    const float estimate = MemoryInline::FlatReadFloat32(anm + kInvSpan) * ((frame - firstFrame) * (float(count) * b.u16Load));
    const uint32_t index = Quantize<uint16_t>(estimate, b.u16Store);
    if (int32_t(index) > int32_t(count) - 1)
        Panic("estimatePos is out of bounds", index);

    const auto walk = t.Walk(frame);
    uint32_t key = t.Key(index);
    if (walk < t.Raw(key)) {
        do
            key -= Track::kSize;
        while (walk < t.Raw(key));
    } else {
        do
            key += Track::kSize;
        while (t.Raw(key) <= walk);
        key -= Track::kSize;
    }

    const float f0 = t.Frame(key);
    if (frame == f0)
        return t.Value(key);
    const uint32_t next = key + Track::kSize;
    const float v0 = t.Value(key), t0 = t.Slope(key);
    const float v1 = t.Value(next), t1 = t.Slope(next);
    const float f1 = t.Frame(next);
    if (!(f0 <= frame && frame <= f1))
        Panic("frame is out of bounds", 0);
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
            std::fprintf(stderr, "MH3 anm keys: GQRs %08x %08x %08x %08x not the lean types, Gekko path\n", cpu.gqr[2], cpu.gqr[3], cpu.gqr[6], cpu.gqr[7]);
        }
#endif
        return Evaluate<Track>(r1, r2, anm, frame);
    }
    const double lean = EvaluateLean<Track>(r2, anm, float(frame), cpu);
    return lean;
}

} // namespace

// The ports, or with MH3_PORT_CHECK naming one the (Gekko path) port checked
// against the original on every call (registers from the current context).
template <typename Track>
double Checked(const char* name, void (*original)(CpuContext*), void (*port)(CpuContext*), bool check, uint32_t r1, uint32_t r2, uint32_t r3, double f1) {
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

#define MH3_ANM_CHR_KEYS(address, track, name)                                                                                                                                                         \
    extern "C" void func_##address(CpuContext* ctx);                                                                                                                                                   \
    static void name##Port(CpuContext* p) {                                                                                                                                                            \
        p->fpr[1].d = Evaluate<track>(p->gpr[1], p->gpr[2], p->gpr[3], p->fpr[1].d);                                                                                                                   \
    }                                                                                                                                                                                                  \
    static double name##Checked(uint32_t r1, uint32_t r2, uint32_t r3, double f1) {                                                                                                                    \
        static const bool check = PortCheck::Enabled(#name);                                                                                                                                           \
        return Checked<track>(#name, func_##address, name##Port, check, r1, r2, r3, f1);                                                                                                               \
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

// 0x8008D660: ResAnmChr::GetAnmResult(result, node index, frame). The node's
// data comes from the animation's node dictionary (data +0x10); its flags
// (+4) give the result's flags (low 13 bits) and pick one of eight handlers
// (bits 22-24, the table at 0x8056FED0) by which of scale, rotation and
// translation it animates, whose data follows from node +8 in that order:
//   scale (0x8008A6C8) to result +4: format bits 25-26 (constant f32, FVS32,
//     FVS48, FVS96), per-axis constant bits 13-15, uniform bit 4 (one value);
//   rotation (0x8008C09C) as degrees to result +0x10 and the matrix at +0x1C
//     (MTX34RotXYZFIdx of the degrees times 256/360), format bits 27-29
//     (also three baked formats, one value per frame, below), per-axis
//     constant bits 16-18, and flag bit 31 set;
//   translation (0x8008CF90) to the matrix's +0xC/+0x1C/+0x2C: format bits
//     30-31, per-axis constant bits 19-21.
// Unanimated scale is 1, an unanimated rotation the identity, translation 0.
// A constant component is the f32 in the data, an animated one a track at the
// node plus the data word (0x8008A99C/B110/B86C), evaluated by the key ports
// above. The debug build's pointer and float checks panic as the original.
namespace {

enum : uint32_t {
    kResultScale = 0x04,
    kResultRotate = 0x10,
    kResultMtx = 0x1C,
};

struct ResultPort {
    CpuContext* ctx;
    uint32_t r1, r2, node, flags, frames; // frames: the u16 frame count's address
    double frame;

    float Load(uint32_t a) const { return MemoryInline::FlatReadFloat32(a); }
    void Store(uint32_t a, float v) const { MemoryInline::FlatWriteFloat32(a, v); }

    // Formats 1-3: FVS32, FVS48, FVS96 (0x8008B86C, 0x8008B110, 0x8008A99C).
    float Key(uint32_t format, uint32_t data, bool constant) const {
        if (constant)
            return Load(data);
        const uint32_t track = node + MemoryInline::FlatRead32(data);
        if (!ValidPointer(track))
            Panic("NW4R:Pointer Error pAnm is not valid", track);
        switch (format) {
        case 1:
            return float(EvaluateFast<Fvs32>(*ctx, r1, r2, track, frame));
        case 2:
            return float(EvaluateFast<Fvs48>(*ctx, r1, r2, track, frame));
        default:
            return float(EvaluateFast<Fvs96>(*ctx, r1, r2, track, frame));
        }
    }
    // Three components from data (format 0 constant f32s, 1-3 keys, constant
    // bits from `constantBit` up), returning the data past them.
    uint32_t Vector(uint32_t format, uint32_t data, uint32_t constantBit, float out[3]) const {
        for (int i = 0; i < 3; ++i, data += 4)
            out[i] = format == 0 ? Load(data) : Key(format, data, flags & constantBit << i);
        return data;
    }

    // Formats 4-6, baked (0x8008CBFC, 0x8008C864, 0x8008C4FC): a value per
    // whole frame from the track at node + data word, frame count + 1 of them,
    // read by 0x8008CD64, 0x8008C9CC and 0x8008C664. Format 4 stores u8s and
    // 5 u16s from track +8, each scale (+0) * value + offset (+4)
    // (0x8008CF54, 0x8008CBBC); 6 stores f32s from +0. Between frames the value
    // is interpolated linearly; at or before frame 0 it is the first, from the
    // frame count on the last. The frame's index is a GQR3 u16 store
    // (0x80082F7C), which the caller has checked is u16.
    float Baked(uint32_t format, uint32_t data, bool constant) const {
        if (constant)
            return Load(data);
        const uint32_t track = node + MemoryInline::FlatRead32(data); // 0x8008B0DC
        if (!ValidPointer(track))
            Panic("NW4R:Pointer Error pAnm is not valid", track);
        auto value = [&](uint32_t i) {
            switch (format) {
            case 4:
                return Madd(Load(track), float(MemoryInline::FlatRead8(track + 8 + i)), Load(track + 4));
            case 5:
                return Madd(Load(track), float(MemoryInline::FlatRead16(track + 8 + 2 * i)), Load(track + 4));
            default:
                return Load(track + 4 * i);
            }
        };
        const float zero = Load(r2 - 0x7BD0);
        if (frame <= zero)
            return value(0);
        const uint32_t index = Quantize<uint16_t>(float(frame), StoreFactor(ctx->gqr[3]));
        const uint32_t count = MemoryInline::FlatRead16(frames);
        if (index >= count)
            return value(count);
        const float t = float(frame - double(index));
        const float v = value(index);
        if (t == zero)
            return v;
        return Madd(t, value((index + 1) & 0xFFFF) - v, v);
    }

    float BakedOriginal(uint32_t format, uint32_t data, bool constant) const {
        static constexpr uint32_t kBaked[3] = {0x8008CBFCu, 0x8008C864u, 0x8008C4FCu};
        GuestCall game{ctx};
        ctx->gpr[1] = r1;
        game(kBaked[format - 4], {node, data, MemoryInline::FlatRead16(frames), uint32_t(constant)}, {frame});
        return float(ctx->fpr[1].d);
    }

    // 0x8008A6C8.
    uint32_t Scale(uint32_t result, uint32_t data) const {
        const uint32_t format = flags >> 25 & 3;
        float s[3];
        if (flags & 0x10) {
            s[0] = s[1] = s[2] = format == 0 ? Load(data) : Key(format, data, flags & 0x2000);
            data += 4;
        } else {
            data = Vector(format, data, 0x2000, s);
        }
        for (int i = 0; i < 3; ++i)
            Store(result + kResultScale + 4 * i, s[i]);
        return data;
    }
    // 0x8008C09C.
    uint32_t Rotate(uint32_t result, uint32_t data) const {
        const uint32_t format = flags >> 27 & 7;
        float r[3];
        if (format <= 3) {
            data = Vector(format, data, 0x10000, r);
        } else if (format <= 6 && (ctx->gqr[3] & 7) == 5) {
            for (int i = 0; i < 3; ++i)
                r[i] = Baked(format, data + 4 * i, flags & 0x10000u << i);
            data += 12;
        } else if (format <= 6) { // GQR3 not u16: the originals
            for (int i = 0; i < 3; ++i, data += 4)
                r[i] = BakedOriginal(format, data, flags & 0x10000u << i);
        } else {
            Panic("NW4R:Internal error ResAnmChrNodeData rotation format", flags);
        }
        const float k = Load(r2 - 0x7BAC); // 256/360
        G3dNative::MtxRotXYZFIdx(result + kResultMtx, k * r[0], k * r[1], k * r[2], r2);
        for (int i = 0; i < 3; ++i)
            Store(result + kResultRotate + 4 * i, r[i]);
        MemoryInline::FlatWrite32(result, MemoryInline::FlatRead32(result) | 0x80000000u);
        return data;
    }
    // 0x8008CF90, into the matrix's translation column.
    void Translate(uint32_t result, uint32_t data) const {
        float t[3];
        Vector(flags >> 30, data, 0x80000, t);
        for (int i = 0; i < 3; ++i)
            Store(result + kResultMtx + 0xC + 0x10 * i, t[i]);
    }
    void UnitScale(uint32_t result) const {
        const float one = Load(r2 - 0x7BB8);
        for (int i = 0; i < 3; ++i)
            Store(result + kResultScale + 4 * i, one);
    }
    void Identity(uint32_t result) const { // 0x80050538 (PSMTXIdentity)
        for (int row = 0; row < 3; ++row)
            for (int col = 0; col < 4; ++col)
                Store(result + kResultMtx + 0x10 * row + 4 * col, row == col ? 1.0f : 0.0f);
    }
    void ZeroTranslation(uint32_t result) const {
        const float zero = Load(r2 - 0x7BD0);
        for (int i = 0; i < 3; ++i)
            Store(result + kResultMtx + 0xC + 0x10 * i, zero);
    }
};

// 0x8008DA1C: the node dictionary's entry `index` (0x800627B4, whose checks it keeps).
uint32_t NodeData(uint32_t data, uint32_t index) {
    const uint32_t offset = MemoryInline::FlatRead32(data + 0x10);
    const uint32_t dic = offset ? data + offset : 0;
    if (dic & 3)
        Panic("NW4R:Failed assertion ptr is not aligned", dic);
    if (!dic)
        return 0;
    const int32_t i = int32_t(index);
    if (!(i >= 0 && i <= int32_t(MemoryInline::FlatRead32(dic + 4)) - 1))
        Panic("idx is out of bounds", index);
    const uint32_t entry = MemoryInline::FlatRead32(dic + (index + 1) * 16 + 0x14);
    return entry ? dic + entry : 0;
}

void GetAnmResult(CpuContext* ctx) {
    const uint32_t anm = ctx->gpr[3], result = ctx->gpr[4], index = ctx->gpr[5];
    const double frame = ctx->fpr[1].d;
    if (!ValidPointer(anm))
        Panic("NW4R:Pointer Error this is not valid", anm);
    if (!ValidPointer(result))
        Panic("NW4R:Pointer Error pResult is not valid", result);
    if (!std::isfinite(frame))
        Panic("NW4R:Floating Point Value Error frame", 0);
    const uint32_t data = MemoryInline::FlatRead32(anm);
    if (!data)
        Panic("NW4R:Failed assertion IsValid()", anm);
    const uint32_t node = NodeData(data, index);
    if (!ValidPointer(node))
        Panic("NW4R:Pointer Error nodeData is not valid", node);
    const uint32_t flags = MemoryInline::FlatRead32(node + 4);
    MemoryInline::FlatWrite32(result, flags & 0x1FFF);

    const ResultPort p{ctx, ctx->gpr[1], ctx->gpr[2], node, flags, data + 0x20, frame};
    const uint32_t first = node + 8;
    switch (flags >> 22 & 7) {
    case 0:
        p.UnitScale(result), p.Identity(result);
        break; // 0x8008D210
    case 1:
        p.Scale(result, first), p.Identity(result);
        break; // 0x8008D330
    case 2:
        p.UnitScale(result), p.Rotate(result, first), p.ZeroTranslation(result);
        break; // 0x8008D2C0
    case 3:
        p.Rotate(result, p.Scale(result, first)), p.ZeroTranslation(result);
        break; // 0x8008D440
    case 4:
        p.UnitScale(result), p.Identity(result), p.Translate(result, first);
        break; // 0x8008D228
    case 5: {  // 0x8008D4E0
        const uint32_t next = p.Scale(result, first);
        p.Identity(result), p.Translate(result, next);
        break;
    }
    case 6:
        p.UnitScale(result), p.Translate(result, p.Rotate(result, first));
        break; // 0x8008D370
    default:
        p.Translate(result, p.Rotate(result, p.Scale(result, first)));
        break; // 0x8008D58C
    }
}

void GetAnmResultPort(CpuContext* ctx) {
    const CpuContext saved = *ctx; // the baked formats' guest calls clobber volatiles
    GetAnmResult(ctx);
    const uint32_t r3 = ctx->gpr[3];
    *ctx = saved;
    ctx->gpr[3] = r3;
}

} // namespace

// The port, or with MH3_PORT_CHECK=AnmChrGetAnmResult the port checked against
// the original (whose key evaluation is the key ports above too).
extern "C" void func_8008D660(CpuContext* ctx);
extern "C" void MH3_AnmChrGetAnmResult(CpuContext* ctx) {
#ifdef MH3_DEVELOPMENT
    MH3FrameSplitCall(1);
#endif
#ifdef MH3_DEVELOPMENT
    static const bool check = PortCheck::Enabled("AnmChrGetAnmResult");
    if (check) [[unlikely]] {
        PortCheck::Run(ctx, "AnmChrGetAnmResult", func_8008D660, GetAnmResultPort);
        return;
    }
#endif
    GetAnmResult(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x8008D660, MH3_AnmChrGetAnmResult, "AnmChrGetAnmResult");
