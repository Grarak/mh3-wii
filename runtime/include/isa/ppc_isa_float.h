// SPDX-License-Identifier: GPL-2.0-or-later
//
// Copyright 2018 Dolphin Emulator Project
//
// Portions of this file are derived from the Dolphin Emulator
// (https://github.com/dolphin-emu/dolphin):
//
//   * Source/Core/Common/FloatUtils.cpp - the Gekko/Broadway fres and frsqrte
//     estimate tables (kPpcFresEstimateInline / kPpcFrsqrteEstimateInline,
//     upstream fres_expected / frsqrte_expected) together with the
//     PpcApproximateReciprocalInline and PpcApproximateReciprocalSquareRootInline
//     interpolation routines, which are ports of upstream ApproximateReciprocal
//     and ApproximateReciprocalSquareRoot.
//
// The remainder of this header is original to this project. It is
// GPL-2.0-or-later as a consequence of the above; see THIRD-PARTY-NOTICES.md.

#pragma once
// Pure floating-point and paired-single PowerPC semantics: the scalar single
// and double families, every ps_* arithmetic form, the NI flush rules and the
// Gekko fres/frsqrte estimates. Nothing in this header touches guest memory -
// the psq_l/psq_st tier that does lives in ppc_isa_quantized.h.

#include "ppc_isa_config.h"
#include "ppc_isa_context.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

inline uint64_t PpcBitCastToU64Inline(double value)
{
    uint64_t integral = 0;
    std::memcpy(&integral, &value, sizeof(integral));
    return integral;
}

inline uint32_t PPC_FprLowWordInline(double value)
{
    return static_cast<uint32_t>(PpcBitCastToU64Inline(value));
}

inline double PpcBitCastToDoubleInline(uint64_t value)
{
    double result = 0.0;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}

// The double an lfd reads from two stored words (high word first, as in guest memory);
// the translator forwards CodeWarrior's integer-to-float stack idiom through this.
inline double PpcDoubleFromWordsInline(uint32_t high, uint32_t low)
{
    return PpcBitCastToDoubleInline(static_cast<uint64_t>(high) << 32 | low);
}

inline uint32_t PpcBitCastToU32Inline(float value)
{
    uint32_t integral = 0;
    std::memcpy(&integral, &value, sizeof(integral));
    return integral;
}

inline float PpcBitCastToFloatInline(uint32_t value)
{
    float result = 0.0f;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}

// NOTE: PpcGetPs0Inline / PpcGetPs1Inline / PpcPackPairedInline are defined
// below the paired-single helper block (PpcPsToM128Inline and
// friends) because their register-domain implementations are written in terms
// of those helpers. Nothing between here and there uses them.

inline double PpcGetPairedFprInline(const PPC_FPR& fpr)
{
    return fpr.d;
}

inline void PpcSetPairedFprInline(PPC_FPR& fpr, double packed)
{
    fpr.d = packed;
}

#if defined(__x86_64__)
using PpcPairVec = __m128;
#elif defined(__aarch64__)
// Only 2 lanes are ever meaningful (a PPC paired-single register), so a 2-lane float32x2_t
// (one 64-bit D register) is a more natural fit than mirroring x86's 128-bit register usage.
using PpcPairVec = float32x2_t;
#elif defined(__arm__)
// AArch32 Advanced SIMD always flushes denormals and returns the default NaN, ignoring FPSCR, so
// it cannot implement Gekko paired singles. Lanes are therefore two scalars handled by VFP, which
// honors FPSCR.FZ (the NI mirror) and propagates NaNs by the same rules as AArch64. A D register
// is the S-register pair {lane0, lane1}, so moving a packed double in and out is register-only.
struct PpcPairVec
{
    float lane0; // ps1
    float lane1; // ps0
};

inline uint32_t PpcPairLaneBitsInline(float value)
{
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

inline float PpcPairLaneFromBitsInline(uint32_t bits)
{
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}
#endif

// Must stay inside the vector register domain. Bitcasting through a 64-bit GPR added a movq
// domain crossing on every paired-single op (630 in the THP IDCT region alone); a double
// local already lives in a vector register, so these casts compile to nothing.
inline PpcPairVec PpcPsToM128Inline(double value)
{
#if defined(__x86_64__)
    return _mm_castpd_ps(_mm_set_sd(value));
#elif defined(__aarch64__)
    return vreinterpret_f32_f64(vdup_n_f64(value));
#elif defined(__arm__)
    PpcPairVec lanes;
    std::memcpy(&lanes, &value, sizeof(lanes));
    return lanes;
#endif
}

inline double PpcM128ToPsInline(PpcPairVec value)
{
#if defined(__x86_64__)
    return _mm_cvtsd_f64(_mm_castps_pd(value));
#elif defined(__aarch64__)
    return vget_lane_f64(vreinterpret_f64_f32(value), 0);
#elif defined(__arm__)
    double packed;
    std::memcpy(&packed, &value, sizeof(packed));
    return packed;
#endif
}

inline PpcPairVec PpcBroadcastPs0Inline(double value)
{
#if defined(__x86_64__)
    const __m128 lanes = PpcPsToM128Inline(value);
    return _mm_shuffle_ps(lanes, lanes, _MM_SHUFFLE(1, 1, 1, 1));
#elif defined(__aarch64__)
    // ps0 lives in lane 1 (see the lane-accessor comment below).
    return vdup_lane_f32(PpcPsToM128Inline(value), 1);
#elif defined(__arm__)
    const PpcPairVec lanes = PpcPsToM128Inline(value);
    return {lanes.lane1, lanes.lane1};
#endif
}

inline PpcPairVec PpcBroadcastPs1Inline(double value)
{
#if defined(__x86_64__)
    const __m128 lanes = PpcPsToM128Inline(value);
    return _mm_shuffle_ps(lanes, lanes, _MM_SHUFFLE(0, 0, 0, 0));
#elif defined(__aarch64__)
    // ps1 is already lane 0.
    return vdup_lane_f32(PpcPsToM128Inline(value), 0);
#elif defined(__arm__)
    const PpcPairVec lanes = PpcPsToM128Inline(value);
    return {lanes.lane0, lanes.lane0};
#endif
}

inline PpcPairVec PpcNegateNonNanLanesInline(PpcPairVec value)
{
#if defined(__x86_64__)
    const __m128 signMask = _mm_castsi128_ps(_mm_set1_epi32(static_cast<int>(0x80000000u)));
    const __m128 negated = _mm_xor_ps(value, signMask);
    const __m128 ordered = _mm_cmpord_ps(value, value);
    return _mm_or_ps(_mm_and_ps(ordered, negated), _mm_andnot_ps(ordered, value));
#elif defined(__aarch64__)
    const uint32x2_t signMask = vdup_n_u32(0x80000000u);
    const float32x2_t negated = vreinterpret_f32_u32(veor_u32(vreinterpret_u32_f32(value), signMask));
    // NEON has no direct "ordered compare"; a value compares equal to itself iff it's not NaN.
    const uint32x2_t ordered = vceq_f32(value, value);
    return vbsl_f32(ordered, negated, value);
#elif defined(__arm__)
    // Integer sign flips: no FP operation, so NaN and denormal bits pass through untouched.
    const auto negate = [](float lane) {
        const uint32_t bits = PpcPairLaneBitsInline(lane);
        return (bits & 0x7FFFFFFFu) > 0x7F800000u ? lane : PpcPairLaneFromBitsInline(bits ^ 0x80000000u);
    };
    return {negate(value.lane0), negate(value.lane1)};
#endif
}

// Paired-single lane accessors. The packed double's LOW 32 bits hold ps1 and HIGH 32 bits
// hold ps0, so in the SSE float-lane view lane 0 == ps1 and lane 1 == ps0. Must stay pure
// register-domain shuffles (no arithmetic/conversion, so NaN/denormal bits pass through);
// the former union-based forms store-then-reloaded through memory, a guaranteed
// store-to-load-forwarding stall on every scalar-lane op.
inline float PpcGetPs0Inline(double value)
{
    // ps0 lives in lane 1; PpcBroadcastPs0Inline already splats it.
#if defined(__x86_64__)
    return _mm_cvtss_f32(PpcBroadcastPs0Inline(value));
#elif defined(__aarch64__)
    return vget_lane_f32(PpcBroadcastPs0Inline(value), 0);
#elif defined(__arm__)
    return PpcPsToM128Inline(value).lane1;
#endif
}

inline float PpcGetPs1Inline(double value)
{
    // ps1 is already lane 0 of the packed representation.
#if defined(__x86_64__)
    return _mm_cvtss_f32(PpcPsToM128Inline(value));
#elif defined(__aarch64__)
    return vget_lane_f32(PpcPsToM128Inline(value), 0);
#elif defined(__arm__)
    return PpcPsToM128Inline(value).lane0;
#endif
}

inline double PpcPackPairedInline(float ps0, float ps1)
{
#if defined(__x86_64__)
    // _mm_unpacklo_ps(x, y) -> { x[0], y[0], x[1], y[1] }, so lane 0 becomes
    // ps1 and lane 1 becomes ps0, matching the union layout bit for bit.
    return PpcM128ToPsInline(_mm_unpacklo_ps(_mm_set_ss(ps1), _mm_set_ss(ps0)));
#elif defined(__aarch64__)
    // Lane 0 = ps1, lane 1 = ps0, matching the union layout bit for bit.
    const float32x2_t lane0 = vdup_n_f32(ps1);
    return PpcM128ToPsInline(vset_lane_f32(ps0, lane0, 1));
#elif defined(__arm__)
    return PpcM128ToPsInline(PpcPairVec{ps1, ps0});
#endif
}

// FPSCR[NI] is modeled by MXCSR FTZ/DAZ, so arithmetic output flushing compiles to nothing.
// Two cases still need a software check against the mirrored bit (not STMXCSR, too hot):
// double->single conversion (CVTSD2SS isn't covered by FTZ) and raw lane pass-through moves.
inline bool MkwHostNiActiveInline() noexcept
{
    return g_mkwHostNiActive;
}

inline float PpcForceSingleValueInline(double value)
{
    // FPSCR[NI] flushes an exact pre-round single-subnormal even when rounding would promote it
    // to the smallest normal. On x86-64, g_mkwNiFlushThreshold (2^-126 active, 0.0 inactive)
    // turns the flush into a branchless mask: a set compare lane keeps just the sign bit, a clear
    // lane passes the value to the double->float conversion. DAZ/FZ (set exactly when NI is)
    // makes the compare itself treat a subnormal as zero, matching the mask's answer.
#if defined(__x86_64__)
    const __m128d v = _mm_set_sd(value);
    const __m128d signMask = _mm_set_sd(-0.0);
    const __m128d magnitude = _mm_andnot_pd(signMask, v);
    const __m128d flush = _mm_cmplt_sd(magnitude, _mm_set_sd(g_mkwNiFlushThreshold));
    const __m128d kept = _mm_andnot_pd(_mm_andnot_pd(signMask, flush), v);
    return static_cast<float>(_mm_cvtsd_f64(kept));
#else
    // ARM needs no mask: host FZ is set exactly when NI is (MkwApplyHostNiMode), and FZ
    // flushes on the exponent of the unrounded result (FPRound in the Arm ARM, AArch64 and
    // AArch32 alike), which is NI's rule. The mask sat on the critical path of every
    // single-precision result, on AArch32 as a VFP compare whose flags move to the core.
    return static_cast<float>(value);
#endif
}

inline float PpcFlushSingleForNiInline(float value)
{
    if (!MkwHostNiActiveInline())
        return value;
    const uint32_t bits = PpcBitCastToU32Inline(value);
    if ((bits & 0x7FFFFFFFu) < 0x00800000u) [[unlikely]]
        return PpcBitCastToFloatInline(bits & 0x80000000u);
    return value;
}

inline double PpcFlushPairedForNiInline(double value)
{
    // Callers pass results of SSE arithmetic; MXCSR.FTZ already flushed them.
    return value;
}

inline double PpcForce25BitInline(double value)
{
    // A widened single (an lfs load, a rounded single result) has its low 29 fraction bits
    // clear, so the rounding below leaves it unchanged, NaNs included. Where the compiler can
    // see the value came from a float, float(value) folds back to that float and this test
    // becomes a constant: the rounding and its round trip through integer registers go away.
    // Anywhere else the test is not constant and costs nothing. (The expression has to be
    // written out inside __builtin_constant_p; through a helper call it never folds.)
    if (__builtin_constant_p(value != value || static_cast<double>(static_cast<float>(value)) == value) &&
        (value != value || static_cast<double>(static_cast<float>(value)) == value))
        return value;
    constexpr uint64_t kDoubleExpMask = 0x7FF0000000000000ULL;
    constexpr uint64_t kDoubleFracMask = 0x000FFFFFFFFFFFFFULL;
    constexpr int kDoubleFracWidth = 52;

    uint64_t integral = PpcBitCastToU64Inline(value);

    const uint64_t exponent = integral & kDoubleExpMask;
    const uint64_t fraction = integral & kDoubleFracMask;

    if (exponent == 0 && fraction != 0)
    {
        int64_t keepMask = 0xFFFFFFFFF8000000LL;
        uint64_t round = 0x8000000ULL;
        uint32_t leadingZeros = 0;
        uint64_t normalizedFraction = fraction;
        while ((normalizedFraction & (1ULL << 63)) == 0)
        {
            normalizedFraction <<= 1;
            ++leadingZeros;
        }
        const uint32_t shift = leadingZeros - (63 - kDoubleFracWidth);
        keepMask >>= shift;
        round >>= shift;
        integral = (integral & static_cast<uint64_t>(keepMask)) + (integral & round);
    }
    else
    {
        integral = (integral & 0xFFFFFFFFF8000000ULL) + (integral & 0x8000000ULL);
    }

    return PpcBitCastToDoubleInline(integral);
}

struct PpcEstimateEntryInline
{
    int32_t base;
    int32_t decrement;
};

inline constexpr std::array<PpcEstimateEntryInline, 32> kPpcFresEstimateInline = {{
    {0x7ff800, 0x3e1}, {0x783800, 0x3a7}, {0x70ea00, 0x371}, {0x6a0800, 0x340},
    {0x638800, 0x313}, {0x5d6200, 0x2ea}, {0x579000, 0x2c4}, {0x520800, 0x2a0},
    {0x4cc800, 0x27f}, {0x47ca00, 0x261}, {0x430800, 0x245}, {0x3e8000, 0x22a},
    {0x3a2c00, 0x212}, {0x360800, 0x1fb}, {0x321400, 0x1e5}, {0x2e4a00, 0x1d1},
    {0x2aa800, 0x1be}, {0x272c00, 0x1ac}, {0x23d600, 0x19b}, {0x209e00, 0x18b},
    {0x1d8800, 0x17c}, {0x1a9000, 0x16e}, {0x17ae00, 0x15b}, {0x14f800, 0x15b},
    {0x124400, 0x143}, {0x0fbe00, 0x143}, {0x0d3800, 0x12d}, {0x0ade00, 0x12d},
    {0x088400, 0x11a}, {0x065000, 0x11a}, {0x041c00, 0x108}, {0x020c00, 0x106},
}};

inline double PpcApproximateReciprocalInline(double value)
{
    constexpr uint64_t kSign = 0x8000000000000000ULL;
    constexpr uint64_t kExponent = 0x7FF0000000000000ULL;
    constexpr uint64_t kFraction = 0x000FFFFFFFFFFFFFULL;
    constexpr uint64_t kQuietBit = 0x0008000000000000ULL;
    const uint64_t input = PpcBitCastToU64Inline(value);
    const uint64_t mantissa = input & kFraction;
    const uint64_t sign = input & kSign;
    uint64_t exponent = input & kExponent;

    if (mantissa == 0 && exponent == 0)
        return PpcBitCastToDoubleInline(sign | kExponent);
    if (exponent == kExponent)
    {
        if (mantissa == 0)
            return PpcBitCastToDoubleInline(sign);
        return PpcBitCastToDoubleInline(input | kQuietBit);
    }
    if (exponent < (uint64_t{895} << 52))
        return std::copysign(static_cast<double>(std::numeric_limits<float>::max()), value);
    if (exponent >= (uint64_t{1149} << 52))
        return std::copysign(0.0, value);

    exponent = (uint64_t{0x7FD} << 52) - exponent;
    const int index = static_cast<int>(mantissa >> 37);
    const auto& entry = kPpcFresEstimateInline[static_cast<size_t>(index / 1024)];
    const int64_t estimate = static_cast<int64_t>(entry.base) -
        (static_cast<int64_t>(entry.decrement) * (index % 1024) + 1) / 2;
    return PpcBitCastToDoubleInline(
        sign | exponent | (static_cast<uint64_t>(estimate) << 29));
}

// Broadway/Gekko frsqrte lookup table. These constants and the interpolation
// below match the algorithm used by the checked-in Dolphin reference rather
// than substituting an exact host reciprocal square root. This lives in the
// header because frsqrte is emitted at 281 translated sites and an out-of-line
// call is a full register barrier at each of them.
inline constexpr std::array<PpcEstimateEntryInline, 32> kPpcFrsqrteEstimateInline = {{
    {0x1a7e800, -0x568}, {0x17cb800, -0x4f3}, {0x1552800, -0x48d}, {0x130c000, -0x435},
    {0x10f2000, -0x3e7}, {0x0eff000, -0x3a2}, {0x0d2e000, -0x365}, {0x0b7c000, -0x32e},
    {0x09e5000, -0x2fc}, {0x0867000, -0x2d0}, {0x06ff000, -0x2a8}, {0x05ab800, -0x283},
    {0x046a000, -0x261}, {0x0339800, -0x243}, {0x0218800, -0x226}, {0x0105800, -0x20b},
    {0x3ffa000, -0x7a4}, {0x3c29000, -0x700}, {0x38aa000, -0x670}, {0x3572000, -0x5f2},
    {0x3279000, -0x584}, {0x2fb7000, -0x524}, {0x2d26000, -0x4cc}, {0x2ac0000, -0x47e},
    {0x2881000, -0x43a}, {0x2665000, -0x3fa}, {0x2468000, -0x3c2}, {0x2287000, -0x38e},
    {0x20c1000, -0x35e}, {0x1f12000, -0x332}, {0x1d79000, -0x30a}, {0x1bf4000, -0x2e6},
}};

inline double PpcApproximateReciprocalSquareRootInline(double value)
{
    constexpr uint64_t kSign = 0x8000000000000000ULL;
    constexpr uint64_t kExponent = 0x7FF0000000000000ULL;
    constexpr uint64_t kFraction = 0x000FFFFFFFFFFFFFULL;
    constexpr uint64_t kQuietBit = 0x0008000000000000ULL;
    constexpr uint64_t kCanonicalQuietNaN = kExponent | kQuietBit;

    const uint64_t input = PpcBitCastToU64Inline(value);
    uint64_t mantissa = input & kFraction;
    const uint64_t sign = input & kSign;
    int64_t exponent = static_cast<int64_t>(input & kExponent);

    if (mantissa == 0 && exponent == 0)
    {
        return PpcBitCastToDoubleInline(sign | kExponent);
    }

    if (static_cast<uint64_t>(exponent) == kExponent)
    {
        if (mantissa == 0)
        {
            return sign ? PpcBitCastToDoubleInline(kCanonicalQuietNaN) : 0.0;
        }
        return PpcBitCastToDoubleInline(input | kQuietBit);
    }

    if (sign != 0)
    {
        return PpcBitCastToDoubleInline(kCanonicalQuietNaN);
    }

    if (exponent == 0)
    {
        // Normalize a subnormal while allowing the signed exponent to extend
        // below the IEEE-754 encoded range, exactly as the hardware estimate
        // interpolation expects.
        do
        {
            exponent -= int64_t{1} << 52;
            mantissa <<= 1;
        } while ((mantissa & (uint64_t{1} << 52)) == 0);
        mantissa &= kFraction;
        exponent += int64_t{1} << 52;
    }

    const int64_t exponentLsb = exponent & (int64_t{1} << 52);
    exponent = (((int64_t{0x3FF} << 52) -
                 ((exponent - (int64_t{0x3FE} << 52)) / 2)) &
                static_cast<int64_t>(kExponent));

    const int index = static_cast<int>(
        (static_cast<uint64_t>(exponentLsb) | mantissa) >> 37);
    const auto& entry = kPpcFrsqrteEstimateInline[static_cast<size_t>(index / 2048)];
    const int64_t estimate =
        static_cast<int64_t>(entry.base) +
        static_cast<int64_t>(entry.decrement) * (index % 2048);
    const uint64_t result = sign | static_cast<uint64_t>(exponent) |
                            (static_cast<uint64_t>(estimate) << 26);
    return PpcBitCastToDoubleInline(result);
}

// fctiwz: convert to a 32-bit signed integer with round-toward-zero and place
// the result in the LOW 32 bits of the FPR. The upper 32 bits are
// architecturally undefined; zero is what the hardware leaves in practice and
// what stfd/lwz+4 sequences in guest code expect to read back.
inline int32_t PpcClampIntegerWordInline(double value)
{
    if (std::isnan(value)) {
        return static_cast<int32_t>(0x80000000u);
    }
    if (value >= 2147483647.0) {
        return 2147483647;
    }
    if (value <= -2147483648.0) {
        return static_cast<int32_t>(0x80000000u);
    }
    return static_cast<int32_t>(value);
}

inline double PpcPackIntegerWordInline(int32_t value)
{
    return PpcBitCastToDoubleInline(
        static_cast<uint64_t>(static_cast<uint32_t>(value)));
}

// Correctly rounded float a*b + c without a fused instruction: the product of
// two floats is exact in double, so the only rounding before the final one is
// the double add. That double rounding can only go wrong when the double sum
// lands exactly on a float halfway point; the exact TwoSum error then says
// which way the true value lies. Float-subnormal results (another rounding
// position) and NaNs take std::fma.
inline float PpcFmaFloatViaDoubleInline(float a, float b, float c)
{
    const double product = static_cast<double>(a) * static_cast<double>(b);
    const double addend = static_cast<double>(c);
    double sum = product + addend;
    const double magnitude = sum < 0.0 ? -sum : sum;
    if (!(magnitude >= 0x1p-126) && sum != 0.0) [[unlikely]]  // float subnormal, or NaN
        return std::fma(a, b, c);
    uint64_t bits = PpcBitCastToU64Inline(sum);
    if ((bits & 0x1FFFFFFFull) == 0x10000000ull) [[unlikely]]
    {
        const double addendPart = sum - product;
        const double error = (product - (sum - addendPart)) + (addend - addendPart);
        if (error != 0.0)
            sum = PpcBitCastToDoubleInline((error > 0.0) == (sum > 0.0) ? bits + 1 : bits - 1);
    }
    return static_cast<float>(sum);
}

// Guest fused multiply-adds round once. Where the host has a fused
// instruction (AArch64, x86 with FMA) that costs nothing, so it is used. The
// Cortex-A9 (VFPv3/NEONv1) has none, and there the default is a separate
// multiply and add, rounding twice: an exact result needs libm's software fma
// for doubles and a double-precision detour for floats, about 0.56 ms of the
// Pi 5 game thread per frame, and in play the inexact results showed no
// difference in movement, collision, audio or frames beyond 1-level pixels.
// MH3_EXACT_FMA keeps the exact forms on such a host.
#if defined(MH3_EXACT_FMA) || !defined(__arm__) || defined(__ARM_FEATURE_FMA)
#define MH3_FUSED_MADD 1
#endif

inline double PpcFmaDoubleInline(double a, double b, double c)
{
#if defined(MH3_FUSED_MADD)
    return std::fma(a, b, c);
#else
    return a * b + c;
#endif
}

// Float fused multiply-add: one instruction where the host has it; with
// MH3_EXACT_FMA on the Cortex-A9 (whose vmla rounds twice), the exact double
// form instead of glibc's software fmaf; otherwise a multiply and an add.
inline float PpcFmaFloatInline(float a, float b, float c)
{
#if !defined(MH3_FUSED_MADD)
    return a * b + c;
#elif defined(__arm__) && !defined(__ARM_FEATURE_FMA)
    return PpcFmaFloatViaDoubleInline(a, b, c);
#else
    return std::fma(a, b, c);
#endif
}

template <bool Subtract>
inline float PpcAccuratePsMaddLaneInline(float a, float c, float b)
{
    const float signedB = Subtract ? -b : b;
    // Paired values are stored as exact float32 lanes in this runtime.  An
    // explicit float FMA therefore matches Gekko's single rounding point;
    // multiplying and adding separately can differ by an ULP on ordinary
    // vector and matrix workloads.
    return PpcFlushSingleForNiInline(PpcFmaFloatInline(a, c, signedB));
}

template <bool Subtract>
inline float PpcAccuratePsMaddLaneNoNiInline(float a, float c, float b)
{
    const float signedB = Subtract ? -b : b;
    return PpcFmaFloatInline(a, c, signedB);
}

template <bool Subtract>
inline double PpcAccurateSingleMaddIntermediateInline(double a, double c, double b)
{
    // Gekko single-precision fused operations keep the full precision of A
    // and B, round C to a 25-bit significand, and round only the final result
    // to float32.  A double FMA is almost sufficient, but an exact result just
    // beyond a float32 halfway point can be rounded to a double tie first.
    // Recover the direction of that discarded error for those tie cases.
    const double roundedC = PpcForce25BitInline(c);
    const double signedB = Subtract ? -b : b;
    double result = PpcFmaDoubleInline(a, roundedC, signedB);
    const uint64_t resultBits = PpcBitCastToU64Inline(result);
    constexpr uint64_t kDiscardedMask = 0x000000001FFFFFFFULL;
    constexpr uint64_t kEvenTie = 0x0000000010000000ULL;
    if ((resultBits & kDiscardedMask) == kEvenTie)
    {
        const double aPrime = signedB - result;
        const double bPrime = result + aPrime;
        const double deltaA = PpcFmaDoubleInline(a, roundedC, aPrime);
        const double deltaB = signedB - bPrime;
        const double error = deltaA + deltaB;
        if (error != 0.0)
        {
            result = PpcBitCastToDoubleInline(
                (error > 0.0) == (result > 0.0) ? resultBits + 1 : resultBits - 1);
        }
    }
    return result;
}

inline double PpcFmulsInline(double a, double c)
{
    return static_cast<double>(
        PpcForceSingleValueInline(a * PpcForce25BitInline(c)));
}

inline double PpcFmulsNoNiInline(double a, double c)
{
    return static_cast<double>(static_cast<float>(a * PpcForce25BitInline(c)));
}

inline double PpcFmaddInline(double a, double c, double b)
{
    return PpcFmaDoubleInline(a, c, b);
}

inline double PpcFmsubInline(double a, double c, double b)
{
    return PpcFmaDoubleInline(a, c, -b);
}

inline double PpcFnmaddInline(double a, double c, double b)
{
    const double result = PpcFmaDoubleInline(a, c, b);
    return std::isnan(result) ? result : -result;
}

inline double PpcFnmsubInline(double a, double c, double b)
{
    const double result = PpcFmaDoubleInline(a, c, -b);
    return std::isnan(result) ? result : -result;
}

inline double PpcFmaddsInline(double a, double c, double b)
{
    return static_cast<double>(
        PpcForceSingleValueInline(PpcAccurateSingleMaddIntermediateInline<false>(a, c, b)));
}

inline double PpcFmsubsInline(double a, double c, double b)
{
    return static_cast<double>(
        PpcForceSingleValueInline(PpcAccurateSingleMaddIntermediateInline<true>(a, c, b)));
}

inline double PpcFnmaddsInline(double a, double c, double b)
{
    const float result = PpcForceSingleValueInline(
        PpcAccurateSingleMaddIntermediateInline<false>(a, c, b));
    return static_cast<double>(std::isnan(result) ? result : -result);
}

inline double PpcFnmsubsInline(double a, double c, double b)
{
    const float result = PpcForceSingleValueInline(
        PpcAccurateSingleMaddIntermediateInline<true>(a, c, b));
    return static_cast<double>(std::isnan(result) ? result : -result);
}

#if defined(__aarch64__)
// x86 resolves a single NaN source operand (SNaN or QNaN alike) to that operand quieted with
// sign and payload kept, and an invalid op with no NaN input to 0xFFC00000; NEON prefers SNaNs,
// checks the accumulator first, and yields the positive default NaN 0x7FC00000. NaN result
// lanes (rare: one branch on the pair) are therefore rewritten to the x86 answer, keeping the
// two hosts bit-identical for every case where x86 itself is deterministic. With two or more
// NaN operands even x86 is not: the winning operand depends on which FMA form / commuted
// operand order clang picked per call site, so this resolver's fixed first-operand priority is
// one of the answers a real x86 build can give, not a guaranteed match.
inline uint64_t PpcPairNanLaneBitsInline(PpcPairVec value)
{
    return vget_lane_u64(vreinterpret_u64_u32(vmvn_u32(vceq_f32(value, value))), 0);
}

inline PpcPairVec PpcQuietPairInline(PpcPairVec value)
{
    return vreinterpret_f32_u32(vorr_u32(vreinterpret_u32_f32(value), vdup_n_u32(0x00400000u)));
}

inline PpcPairVec PpcResolveNanLanesInline(PpcPairVec result, PpcPairVec op1, PpcPairVec op2)
{
    const uint32x2_t op1Nan = vmvn_u32(vceq_f32(op1, op1));
    const uint32x2_t op2Nan = vmvn_u32(vceq_f32(op2, op2));
    PpcPairVec replacement = vreinterpret_f32_u32(vdup_n_u32(0xFFC00000u));
    replacement = vbsl_f32(op2Nan, PpcQuietPairInline(op2), replacement);
    replacement = vbsl_f32(op1Nan, PpcQuietPairInline(op1), replacement);
    const uint32x2_t resultNan = vmvn_u32(vceq_f32(result, result));
    return vbsl_f32(resultNan, replacement, result);
}

inline PpcPairVec PpcResolveNanLanes3Inline(
    PpcPairVec result, PpcPairVec op1, PpcPairVec op2, PpcPairVec op3)
{
    const uint32x2_t op3Nan = vmvn_u32(vceq_f32(op3, op3));
    PpcPairVec replacement = vreinterpret_f32_u32(vdup_n_u32(0xFFC00000u));
    replacement = vbsl_f32(op3Nan, PpcQuietPairInline(op3), replacement);
    const uint32x2_t op2Nan = vmvn_u32(vceq_f32(op2, op2));
    replacement = vbsl_f32(op2Nan, PpcQuietPairInline(op2), replacement);
    const uint32x2_t op1Nan = vmvn_u32(vceq_f32(op1, op1));
    replacement = vbsl_f32(op1Nan, PpcQuietPairInline(op1), replacement);
    const uint32x2_t resultNan = vmvn_u32(vceq_f32(result, result));
    return vbsl_f32(resultNan, replacement, result);
}
#endif // defined(__aarch64__)

#if defined(__arm__)
// Scalar-lane counterpart of the AArch64 resolver above: VFP follows the same ARM NaN rules, so
// NaN result lanes are rewritten to the x86 answer in the same operand priority.
inline bool PpcPairLaneIsNanInline(float value)
{
    return (PpcPairLaneBitsInline(value) & 0x7FFFFFFFu) > 0x7F800000u;
}

inline float PpcResolveNanLaneInline(float result, float op1, float op2, float op3)
{
    if (!PpcPairLaneIsNanInline(result)) [[likely]]
        return result;
    if (PpcPairLaneIsNanInline(op1))
        return PpcPairLaneFromBitsInline(PpcPairLaneBitsInline(op1) | 0x00400000u);
    if (PpcPairLaneIsNanInline(op2))
        return PpcPairLaneFromBitsInline(PpcPairLaneBitsInline(op2) | 0x00400000u);
    if (PpcPairLaneIsNanInline(op3))
        return PpcPairLaneFromBitsInline(PpcPairLaneBitsInline(op3) | 0x00400000u);
    return PpcPairLaneFromBitsInline(0xFFC00000u);
}

[[gnu::cold, gnu::noinline]] inline PpcPairVec PpcResolveNanPairSlow(
    PpcPairVec result, PpcPairVec op1, PpcPairVec op2, PpcPairVec op3)
{
    return {PpcResolveNanLaneInline(result.lane0, op1.lane0, op2.lane0, op3.lane0),
            PpcResolveNanLaneInline(result.lane1, op1.lane1, op2.lane1, op3.lane1)};
}

// Every paired op ends here, so the common case must stay small: comparing the two result
// lanes with each other is unordered exactly when either is a NaN, one VFP compare and branch
// instead of moving each lane to a core register to test its bits.
inline PpcPairVec PpcResolveNanLanesInline(PpcPairVec result, PpcPairVec op1, PpcPairVec op2)
{
    if (!__builtin_isunordered(result.lane0, result.lane1)) [[likely]]
        return result;
    // A non-NaN value in the op3 slot never wins.
    return PpcResolveNanPairSlow(result, op1, op2, PpcPairVec{0.0f, 0.0f});
}

inline PpcPairVec PpcResolveNanLanes3Inline(
    PpcPairVec result, PpcPairVec op1, PpcPairVec op2, PpcPairVec op3)
{
    if (!__builtin_isunordered(result.lane0, result.lane1)) [[likely]]
        return result;
    return PpcResolveNanPairSlow(result, op1, op2, op3);
}
#endif // defined(__arm__)

inline PpcPairVec PpcMulPairInline(PpcPairVec lhs, PpcPairVec rhs)
{
#if defined(__x86_64__)
    return _mm_mul_ps(lhs, rhs);
#elif defined(__aarch64__)
    const PpcPairVec result = vmul_f32(lhs, rhs);
    if (PpcPairNanLaneBitsInline(result) != 0) [[unlikely]]
        return PpcResolveNanLanesInline(result, lhs, rhs);
    return result;
#elif defined(__arm__)
    const PpcPairVec result{lhs.lane0 * rhs.lane0, lhs.lane1 * rhs.lane1};
    return PpcResolveNanLanesInline(result, lhs, rhs);
#endif
}

inline double PPC_PsMulInline(double lhs, double rhs)
{
    return PpcFlushPairedForNiInline(
        PpcM128ToPsInline(PpcMulPairInline(PpcPsToM128Inline(lhs), PpcPsToM128Inline(rhs))));
}

inline double PPC_PsMulNoNiInline(double lhs, double rhs)
{
    return PpcM128ToPsInline(PpcMulPairInline(PpcPsToM128Inline(lhs), PpcPsToM128Inline(rhs)));
}

// The paired madd family lowers to one hardware FMA. Semantics match the scalar lanes exactly: a
// single fused rounding per lane (std::fma(float) == vfmaddps per lane), and
// the negate-unless-NaN behavior of the nmadd/nmsub forms is expressed with
// PpcNegateNonNanLanesInline. NI flushing is handled by MXCSR (see
// MkwApplyHostNiMode), so the NI and NoNi entry points are identical here.

// Fused multiply-add/subtract on a pair. NEON's vfma_f32(acc, a, b) = acc + a*b has an
// accumulator-first operand order, unlike x86's _mm_fmadd_ps(a, b, c) = a*b + c - msub is
// therefore an fma against a negated accumulator. That vneg would flip a sole-NaN subtractor's
// sign, which x86's vfmsub does not do, so the NaN resolver receives the original subtractor.
inline PpcPairVec PpcFmaddPairInline(PpcPairVec multiplicand, PpcPairVec multiplier, PpcPairVec addend)
{
#if defined(__x86_64__)
    return _mm_fmadd_ps(multiplicand, multiplier, addend);
#elif defined(__aarch64__)
    const PpcPairVec result = vfma_f32(addend, multiplicand, multiplier);
    if (PpcPairNanLaneBitsInline(result) != 0) [[unlikely]]
        return PpcResolveNanLanes3Inline(result, multiplicand, multiplier, addend);
    return result;
#elif defined(__arm__)
    const PpcPairVec result{PpcFmaFloatInline(multiplicand.lane0, multiplier.lane0, addend.lane0), PpcFmaFloatInline(multiplicand.lane1, multiplier.lane1, addend.lane1)};
    return PpcResolveNanLanes3Inline(result, multiplicand, multiplier, addend);
#endif
}

inline PpcPairVec PpcFmsubPairInline(PpcPairVec multiplicand, PpcPairVec multiplier, PpcPairVec subtractor)
{
#if defined(__x86_64__)
    return _mm_fmsub_ps(multiplicand, multiplier, subtractor);
#elif defined(__aarch64__)
    const PpcPairVec result = vfma_f32(vneg_f32(subtractor), multiplicand, multiplier);
    if (PpcPairNanLaneBitsInline(result) != 0) [[unlikely]]
        return PpcResolveNanLanes3Inline(result, multiplicand, multiplier, subtractor);
    return result;
#elif defined(__arm__)
    const PpcPairVec result{PpcFmaFloatInline(multiplicand.lane0, multiplier.lane0, -subtractor.lane0), PpcFmaFloatInline(multiplicand.lane1, multiplier.lane1, -subtractor.lane1)};
    return PpcResolveNanLanes3Inline(result, multiplicand, multiplier, subtractor);
#endif
}

inline double PPC_PsMsubInline(double multiplicand, double multiplier, double subtractor)
{
    return PpcM128ToPsInline(PpcFmsubPairInline(
        PpcPsToM128Inline(multiplicand), PpcPsToM128Inline(multiplier), PpcPsToM128Inline(subtractor)));
}

inline double PPC_PsMsubNoNiInline(double multiplicand, double multiplier, double subtractor)
{
    return PPC_PsMsubInline(multiplicand, multiplier, subtractor);
}

inline double PPC_PsMaddInline(double multiplicand, double multiplier, double addend)
{
    return PpcM128ToPsInline(PpcFmaddPairInline(
        PpcPsToM128Inline(multiplicand), PpcPsToM128Inline(multiplier), PpcPsToM128Inline(addend)));
}

inline double PPC_PsMaddNoNiInline(double multiplicand, double multiplier, double addend)
{
    return PPC_PsMaddInline(multiplicand, multiplier, addend);
}

inline double PPC_PsMadds0Inline(double multiplicand, double multiplier, double addend)
{
    return PpcM128ToPsInline(PpcFmaddPairInline(
        PpcPsToM128Inline(multiplicand), PpcBroadcastPs0Inline(multiplier), PpcPsToM128Inline(addend)));
}

inline double PPC_PsMadds1Inline(double multiplicand, double multiplier, double addend)
{
    return PpcM128ToPsInline(PpcFmaddPairInline(
        PpcPsToM128Inline(multiplicand), PpcBroadcastPs1Inline(multiplier), PpcPsToM128Inline(addend)));
}

inline double PPC_PsNmsubInline(double multiplicand, double multiplier, double subtractor)
{
    return PpcM128ToPsInline(PpcNegateNonNanLanesInline(PpcFmsubPairInline(
        PpcPsToM128Inline(multiplicand), PpcPsToM128Inline(multiplier), PpcPsToM128Inline(subtractor))));
}

inline double PPC_PsNmsubNoNiInline(double multiplicand, double multiplier, double subtractor)
{
    return PPC_PsNmsubInline(multiplicand, multiplier, subtractor);
}

inline double PPC_PsNmaddInline(double multiplicand, double multiplier, double addend)
{
    return PpcM128ToPsInline(PpcNegateNonNanLanesInline(PpcFmaddPairInline(
        PpcPsToM128Inline(multiplicand), PpcPsToM128Inline(multiplier), PpcPsToM128Inline(addend))));
}

inline double PPC_PsMuls0Inline(double aValue, double cValue)
{
    return PpcFlushPairedForNiInline(PpcM128ToPsInline(
        PpcMulPairInline(PpcPsToM128Inline(aValue), PpcBroadcastPs0Inline(cValue))));
}

inline double PPC_PsMuls1Inline(double aValue, double cValue)
{
    return PpcFlushPairedForNiInline(PpcM128ToPsInline(
        PpcMulPairInline(PpcPsToM128Inline(aValue), PpcBroadcastPs1Inline(cValue))));
}

inline PPC_FPR PpcMakePairedResultInline(float ps0, float ps1);

inline double PPC_PsFromScalarInline(double value)
{
    // Representation conversion, not an architectural operation: Gekko has no
    // "scalar to paired" instruction, so there is no NI rounding point here.
    // If the scalar is a single-denormal it stays one; MXCSR.DAZ flushes it as
    // an input at the next real arithmetic op, exactly like the hardware.
    const float single = static_cast<float>(value);
    return PpcPackPairedInline(single, single);
}

inline double PPC_PsFromScalarNoNiInline(double value)
{
    const float single = static_cast<float>(value);
    return PpcPackPairedInline(single, single);
}

inline double PPC_PsToScalarInline(double value)
{
    return static_cast<double>(PpcGetPs0Inline(value));
}

// ps_merge* are pure lane selections (result.ps0 from frA, result.ps1 from frB). On x86 two
// shuffles build the result bit-exact without round-tripping through the pack helper; NEON has
// no equally cheap 2-lane general shuffle, so its branch expresses the exact same selection
// (verified against the x86 comments below, lane for lane) directly in terms of the portable
// Get/Pack accessors instead.
inline double PPC_PsMerge00Inline(double aValue, double bValue)
{
    // result.ps0 = a.ps0, result.ps1 = b.ps0 (lane0 = b.ps0/b lane1, lane1 = a.ps0/a lane1)
#if defined(__x86_64__)
    const __m128 gathered = _mm_shuffle_ps(
        PpcPsToM128Inline(bValue), PpcPsToM128Inline(aValue), _MM_SHUFFLE(1, 1, 1, 1));
    return PpcM128ToPsInline(_mm_shuffle_ps(gathered, gathered, _MM_SHUFFLE(0, 0, 2, 0)));
#elif defined(__aarch64__) || defined(__arm__)
    return PpcPackPairedInline(PpcGetPs0Inline(aValue), PpcGetPs0Inline(bValue));
#endif
}

inline double PPC_PsMerge01Inline(double aValue, double bValue)
{
    // result.ps0 = a.ps0, result.ps1 = b.ps1 (lane0 = b.ps1/b lane0, lane1 = a.ps0/a lane1)
#if defined(__x86_64__)
    const __m128 gathered = _mm_shuffle_ps(
        PpcPsToM128Inline(bValue), PpcPsToM128Inline(aValue), _MM_SHUFFLE(1, 1, 0, 0));
    return PpcM128ToPsInline(_mm_shuffle_ps(gathered, gathered, _MM_SHUFFLE(0, 0, 2, 0)));
#elif defined(__aarch64__) || defined(__arm__)
    return PpcPackPairedInline(PpcGetPs0Inline(aValue), PpcGetPs1Inline(bValue));
#endif
}

inline double PPC_PsMerge10Inline(double aValue, double bValue)
{
    // result.ps0 = a.ps1, result.ps1 = b.ps0 (lane0 = b.ps0/b lane1, lane1 = a.ps1/a lane0)
#if defined(__x86_64__)
    const __m128 gathered = _mm_shuffle_ps(
        PpcPsToM128Inline(bValue), PpcPsToM128Inline(aValue), _MM_SHUFFLE(0, 0, 1, 1));
    return PpcM128ToPsInline(_mm_shuffle_ps(gathered, gathered, _MM_SHUFFLE(0, 0, 2, 0)));
#elif defined(__aarch64__) || defined(__arm__)
    return PpcPackPairedInline(PpcGetPs1Inline(aValue), PpcGetPs0Inline(bValue));
#endif
}

inline double PPC_PsMerge11Inline(double aValue, double bValue)
{
    // result.ps0 = a.ps1, result.ps1 = b.ps1 (lane0 = b.ps1/b lane0, lane1 = a.ps1/a lane0):
    // plain unpcklps on x86.
#if defined(__x86_64__)
    return PpcM128ToPsInline(
        _mm_unpacklo_ps(PpcPsToM128Inline(bValue), PpcPsToM128Inline(aValue)));
#elif defined(__aarch64__) || defined(__arm__)
    return PpcPackPairedInline(PpcGetPs1Inline(aValue), PpcGetPs1Inline(bValue));
#endif
}

inline PpcPairVec PpcAddPairInline(PpcPairVec lhs, PpcPairVec rhs)
{
#if defined(__x86_64__)
    return _mm_add_ps(lhs, rhs);
#elif defined(__aarch64__)
    const PpcPairVec result = vadd_f32(lhs, rhs);
    if (PpcPairNanLaneBitsInline(result) != 0) [[unlikely]]
        return PpcResolveNanLanesInline(result, lhs, rhs);
    return result;
#elif defined(__arm__)
    const PpcPairVec result{lhs.lane0 + rhs.lane0, lhs.lane1 + rhs.lane1};
    return PpcResolveNanLanesInline(result, lhs, rhs);
#endif
}

inline PpcPairVec PpcSubPairInline(PpcPairVec lhs, PpcPairVec rhs)
{
#if defined(__x86_64__)
    return _mm_sub_ps(lhs, rhs);
#elif defined(__aarch64__)
    const PpcPairVec result = vsub_f32(lhs, rhs);
    if (PpcPairNanLaneBitsInline(result) != 0) [[unlikely]]
        return PpcResolveNanLanesInline(result, lhs, rhs);
    return result;
#elif defined(__arm__)
    const PpcPairVec result{lhs.lane0 - rhs.lane0, lhs.lane1 - rhs.lane1};
    return PpcResolveNanLanesInline(result, lhs, rhs);
#endif
}

inline PpcPairVec PpcDivPairInline(PpcPairVec lhs, PpcPairVec rhs)
{
#if defined(__x86_64__)
    return _mm_div_ps(lhs, rhs);
#elif defined(__aarch64__)
    const PpcPairVec result = vdiv_f32(lhs, rhs);
    if (PpcPairNanLaneBitsInline(result) != 0) [[unlikely]]
        return PpcResolveNanLanesInline(result, lhs, rhs);
    return result;
#elif defined(__arm__)
    const PpcPairVec result{lhs.lane0 / rhs.lane0, lhs.lane1 / rhs.lane1};
    return PpcResolveNanLanesInline(result, lhs, rhs);
#endif
}

inline double PPC_PsAddInline(double aValue, double bValue)
{
    return PpcFlushPairedForNiInline(
        PpcM128ToPsInline(PpcAddPairInline(PpcPsToM128Inline(aValue), PpcPsToM128Inline(bValue))));
}

inline double PPC_PsAddNoNiInline(double aValue, double bValue)
{
    return PpcM128ToPsInline(
        PpcAddPairInline(PpcPsToM128Inline(aValue), PpcPsToM128Inline(bValue)));
}

inline double PPC_PsSelInline(double lhsValue, double controlValue, double rhsValue)
{
    const float lhs0 = PpcGetPs0Inline(lhsValue);
    const float lhs1 = PpcGetPs1Inline(lhsValue);
    const float control0 = PpcGetPs0Inline(controlValue);
    const float control1 = PpcGetPs1Inline(controlValue);
    const float rhs0 = PpcGetPs0Inline(rhsValue);
    const float rhs1 = PpcGetPs1Inline(rhsValue);
    return PpcPackPairedInline(
        control0 >= -0.0f ? lhs0 : rhs0,
        control1 >= -0.0f ? lhs1 : rhs1);
}

inline double PPC_PsSubInline(double aValue, double bValue)
{
    return PpcFlushPairedForNiInline(
        PpcM128ToPsInline(PpcSubPairInline(PpcPsToM128Inline(aValue), PpcPsToM128Inline(bValue))));
}

inline double PPC_PsSubNoNiInline(double aValue, double bValue)
{
    return PpcM128ToPsInline(
        PpcSubPairInline(PpcPsToM128Inline(aValue), PpcPsToM128Inline(bValue)));
}

inline double PPC_PsDivInline(double aValue, double bValue)
{
    return PpcFlushPairedForNiInline(
        PpcM128ToPsInline(PpcDivPairInline(PpcPsToM128Inline(aValue), PpcPsToM128Inline(bValue))));
}

inline double PPC_PsNegInline(double value)
{
    return PpcPackPairedInline(-PpcGetPs0Inline(value), -PpcGetPs1Inline(value));
}

inline double PPC_PsAbsInline(double value)
{
    return PpcPackPairedInline(std::abs(PpcGetPs0Inline(value)), std::abs(PpcGetPs1Inline(value)));
}

inline double PPC_PsSum0Inline(double aValue, double bValue, double cValue)
{
    return PpcPackPairedInline(
        PpcForceSingleValueInline(static_cast<double>(PpcGetPs0Inline(aValue)) + PpcGetPs1Inline(bValue)),
        PpcFlushSingleForNiInline(PpcGetPs1Inline(cValue)));
}

inline double PPC_PsSum1Inline(double aValue, double bValue, double cValue)
{
    return PpcPackPairedInline(
        PpcFlushSingleForNiInline(PpcGetPs0Inline(cValue)),
        PpcForceSingleValueInline(static_cast<double>(PpcGetPs0Inline(aValue)) + PpcGetPs1Inline(bValue)));
}

inline uint32_t PpcConvertToSingleFTZInline(uint64_t value)
{
    const uint32_t exp = static_cast<uint32_t>((value >> 52) & 0x7FFu);
    if (exp > 896u || (value & 0x7FFFFFFFFFFFFFFFULL) == 0)
    {
        return static_cast<uint32_t>(((value >> 32) & 0xC0000000ULL) |
                                     ((value >> 29) & 0x3FFFFFFFULL));
    }

    return static_cast<uint32_t>((value >> 32) & 0x80000000ULL);
}

inline uint64_t PpcConvertToDoubleBitsInline(uint32_t value)
{
    uint64_t x = value;
    uint64_t exp = (x >> 23) & 0xFFu;
    uint64_t frac = x & 0x007FFFFFu;

    if (exp > 0 && exp < 255)
    {
        const uint64_t y = !(exp >> 7);
        const uint64_t z = (y << 61) | (y << 60) | (y << 59);
        return ((x & 0xC0000000ULL) << 32) | z | ((x & 0x3FFFFFFFULL) << 29);
    }

    if (exp == 0 && frac != 0)
    {
        exp = 1023 - 126;
        do
        {
            frac <<= 1;
            --exp;
        } while ((frac & 0x00800000u) == 0);

        return ((x & 0x80000000ULL) << 32) | (exp << 52) | ((frac & 0x007FFFFFULL) << 29);
    }

    const uint64_t y = exp >> 7;
    const uint64_t z = (y << 61) | (y << 60) | (y << 59);
    return ((x & 0xC0000000ULL) << 32) | z | ((x & 0x3FFFFFFFULL) << 29);
}

inline double PpcPackPairedBitsInline(uint32_t ps0, uint32_t ps1)
{
    return PpcBitCastToDoubleInline((static_cast<uint64_t>(ps0) << 32) | ps1);
}

inline PPC_FPR PpcMakePairedResultInline(float ps0, float ps1)
{
    PPC_FPR result{};
    result.paired.ps0 = ps0;
    result.paired.ps1 = ps1;
    return result;
}

extern "C" void PPC_Mtfsf(uint32_t fieldMask, double source);
extern "C" void PPC_Mtfsfi(uint32_t field, uint32_t value);
extern "C" void PPC_Mtfsb0(uint32_t bit);
extern "C" void PPC_Mtfsb1(uint32_t bit);
extern "C" double PPC_Mffs();

extern "C" double PPC_PsAdd(double lhs, double rhs);
extern "C" double PPC_PsSub(double lhs, double rhs);
extern "C" double PPC_PsDiv(double lhs, double rhs);
extern "C" double PPC_PsNeg(double value);
extern "C" double PPC_PsMul(double lhs, double rhs);
extern "C" double PPC_PsMsub(double lhs, double mul, double sub);
extern "C" double PPC_PsMadd(double lhs, double mul, double add);
extern "C" double PPC_PsNmsub(double lhs, double mul, double sub);
extern "C" double PPC_PsMadds0(double lhs, double mul, double add);
extern "C" double PPC_PsMadds1(double lhs, double mul, double add);
extern "C" double PPC_PsNmadd(double lhs, double mul, double add);
extern "C" double PPC_PsSel(double lhs, double control, double rhs);
// ps_res is hot in the THP dequant path (148 static call sites in the IDCT
// region); the out-of-line definition cost a call + full spill barrier per
// use, so it is defined inline here. The estimate logic is byte-identical to
// the old ppc_helpers.cpp body (both used PpcApproximateReciprocalInline).
extern "C" inline double PPC_PsRes(double value)
{
    const float ps0 = static_cast<float>(
        PpcApproximateReciprocalInline(static_cast<double>(PpcGetPs0Inline(value))));
    const float ps1 = static_cast<float>(
        PpcApproximateReciprocalInline(static_cast<double>(PpcGetPs1Inline(value))));
    return PpcPackPairedInline(ps0, ps1);
}

extern "C" double PPC_PsRsqrte(double value);
// Pack a scalar double into a paired-single FPR value (ps0=ps1=float(value)).
extern "C" double PPC_PsFromScalar(double value);
// Extract the ps0 lane as a scalar double (used to feed single-precision ops).
extern "C" double PPC_PsToScalar(double value);
extern "C" double PPC_PsMerge00(double a, double b);
extern "C" double PPC_PsMerge01(double a, double b);
extern "C" double PPC_PsMerge10(double a, double b);
extern "C" double PPC_PsMerge11(double a, double b);
extern "C" double PPC_Fsqrt(double value);

// Hot scalar float helpers, defined inline so the compiler can see through them instead of
// taking a cross-TU caller-saved register barrier at ~2,500 call sites in the hottest float
// code in the game (bodies moved verbatim from ppc_helpers.cpp/fpu_helpers.cpp). PPC_PsRes
// above uses the same pattern.

extern "C" inline double PPC_Fres(double value)
{
    // The generated paired/scalar boundary presents fres as a packed input and
    // expects its architecturally replicated single result in the same format.
    const float result = static_cast<float>(
        PpcApproximateReciprocalInline(static_cast<double>(PpcGetPs0Inline(value))));
    return PpcPackPairedInline(result, result);
}

extern "C" inline double PPC_Frsqrte(double value)
{
    // frsqrte (opcode 63) takes the full scalar-double input (generated code normalizes
    // scalar/paired ownership beforehand) and returns exact Gekko estimate bits only; FPSCR/
    // Rc/FPRF updates need instruction-level context this value-only helper doesn't have.
    return PpcApproximateReciprocalSquareRootInline(value);
}

extern "C" inline double PPC_Fsel(double control, double negative, double positive)
{
    // Dolphin models fsel/ps_sel as "fra >= -0.0 ? frC : frB".
    // That comparison deliberately sends unordered/NaN controls to frB.
    return (control >= -0.0) ? positive : negative;
}

// PowerPC fctiwz instruction: Float Convert to Integer Word with Round toward
// Zero. The result goes in the LOWER 32 bits of the FPR (bits 32-63); the upper
// 32 bits are undefined. When the value is stored via stfd and reloaded via lwz
// at offset +4, the integer is correctly retrieved.
extern "C" inline double PPC_Fctiwz(double value)
{
    return PpcPackIntegerWordInline(PpcClampIntegerWordInline(value));
}

extern "C" double PPC_Fmadd(double multiplicand, double multiplier, double addend);
extern "C" double PPC_Fmsub(double multiplicand, double multiplier, double subtractor);
extern "C" double PPC_Fnmadd(double multiplicand, double multiplier, double addend);
extern "C" double PPC_Fnmsub(double multiplicand, double multiplier, double subtractor);
// Single-precision scalar helpers (fadds/fsubs/fmuls/fdivs and fused variants).
extern "C" double PPC_Fadds(double a, double b);
extern "C" double PPC_Fsubs(double a, double b);
extern "C" double PPC_Fmuls(double a, double b);
extern "C" double PPC_Fdivs(double a, double b);
// The fused single-precision family is a thin wrapper over the Ppc*Inline
// bodies above; keeping the wrapper out of line meant the wrapper itself was
// the register barrier. See the block at PPC_Fres for why there is no LTO to
// fall back on.
extern "C" inline double PPC_Fmadds(double multiplicand, double multiplier, double addend)
{
    return PpcFmaddsInline(multiplicand, multiplier, addend);
}

extern "C" inline double PPC_Fmsubs(double multiplicand, double multiplier, double subtractor)
{
    return PpcFmsubsInline(multiplicand, multiplier, subtractor);
}

extern "C" inline double PPC_Fnmadds(double multiplicand, double multiplier, double addend)
{
    return PpcFnmaddsInline(multiplicand, multiplier, addend);
}

extern "C" inline double PPC_Fnmsubs(double multiplicand, double multiplier, double subtractor)
{
    return PpcFnmsubsInline(multiplicand, multiplier, subtractor);
}

extern "C" double PPC_Fctiw(double value);   // Convert float to int using FPSCR rounding mode
extern "C" void PPC_Stfiwx(uint32_t addr, double fprValue);  // Store Float as Integer Word (indexed)
extern "C" double PPC_PsSum0(double a, double b, double c);
extern "C" double PPC_PsSum1(double a, double b, double c);
extern "C" double PPC_PsMuls0(double a, double c);
extern "C" double PPC_PsMuls1(double a, double c);
extern "C" double PPC_PsAbs(double value);

// Floating-point comparison helper (fcmpu/fcmpo)
extern "C" void PPC_Fcmp(uint32_t crField, double a, double b);
extern "C" void PPC_PsCmpo0(uint32_t crField, double a, double b);
extern "C" void PPC_PsCmpu0(uint32_t crField, double a, double b);
extern "C" void PPC_PsCmpo1(uint32_t crField, double a, double b);
extern "C" void PPC_PsCmpu1(uint32_t crField, double a, double b);
extern "C" double PPC_PsNabs(double value);
extern "C" double PPC_PsMr(double value);
