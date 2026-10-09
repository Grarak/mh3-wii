#pragma once

#include "ppc_isa_config.h"
#include "ppc_isa_context.h"
#include "ppc_isa_float.h"
#include "big_endian.h"

#include "ppc_isa_memory.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <type_traits>
#include <utility>

inline uint32_t PpcLoadPsqFloatBitsInline(uint32_t value) {
    const uint32_t magnitude = value & 0x7FFFFFFFu;

    if (magnitude > 0x7F800000u)
        return value | 0x00400000u;

    return value;
}

inline uint32_t PpcStorePsqFloatBitsInline(uint32_t value) {
    const uint32_t magnitude = value & 0x7FFFFFFFu;

    if (magnitude < 0x00800000u)
        return value & 0x80000000u;

    if (magnitude > 0x7F800000u)
        return value | 0x00400000u;

    return value;
}

inline uint64_t PpcLoadPairPsqFloatBitsPackedInline(uint64_t value) {
#if defined(__x86_64__)
    const __m128i lanes = _mm_cvtsi64_si128(static_cast<long long>(value));
    const __m128i magnitude = _mm_and_si128(lanes, _mm_set1_epi32(0x7FFFFFFF));
    const __m128i nanMask = _mm_cmpgt_epi32(magnitude, _mm_set1_epi32(0x7F800000));
    const __m128i result = _mm_or_si128(lanes, _mm_and_si128(nanMask, _mm_set1_epi32(0x00400000)));
    return static_cast<uint64_t>(_mm_cvtsi128_si64(result));
#elif defined(__aarch64__) || defined(__arm__) || defined(__wasm__)
    // Equivalent to applying PpcLoadPsqFloatBitsInline to each 32-bit lane: the
    // x86 body above only ever acts on these same two lanes (the upper 64 bits
    // _mm_cvtsi64_si128 zero-fills never survive the final truncating extract).
    const uint32_t lo = PpcLoadPsqFloatBitsInline(static_cast<uint32_t>(value));
    const uint32_t hi = PpcLoadPsqFloatBitsInline(static_cast<uint32_t>(value >> 32));
    return (static_cast<uint64_t>(hi) << 32) | lo;
#endif
}

inline uint64_t PpcStorePairPsqFloatBitsPackedInline(uint64_t value) {
#if defined(__x86_64__)
    const __m128i lanes = _mm_cvtsi64_si128(static_cast<long long>(value));
    const __m128i magnitude = _mm_and_si128(lanes, _mm_set1_epi32(0x7FFFFFFF));
    const __m128i subnormalMask = _mm_cmplt_epi32(magnitude, _mm_set1_epi32(0x00800000));
    const __m128i nanMask = _mm_cmpgt_epi32(magnitude, _mm_set1_epi32(0x7F800000));
    const __m128i quieted = _mm_or_si128(lanes, _mm_and_si128(nanMask, _mm_set1_epi32(0x00400000)));
    const __m128i signedZero = _mm_and_si128(lanes, _mm_set1_epi32(std::numeric_limits<int32_t>::min()));
    const __m128i result = _mm_or_si128(_mm_and_si128(subnormalMask, signedZero), _mm_andnot_si128(subnormalMask, quieted));
    return static_cast<uint64_t>(_mm_cvtsi128_si64(result));
#elif defined(__aarch64__) || defined(__arm__) || defined(__wasm__)
    // Equivalent to applying PpcStorePsqFloatBitsInline to each 32-bit lane;
    // same reasoning as the load-side port above.
    const uint32_t lo = PpcStorePsqFloatBitsInline(static_cast<uint32_t>(value));
    const uint32_t hi = PpcStorePsqFloatBitsInline(static_cast<uint32_t>(value >> 32));
    return (static_cast<uint64_t>(hi) << 32) | lo;
#endif
}

#if defined(__x86_64__)
inline __m128i PpcPsqSwapPairBytesInline(__m128i lanes) {
    const __m128i order = _mm_setr_epi8(static_cast<char>(7), static_cast<char>(6), static_cast<char>(5), static_cast<char>(4), static_cast<char>(3), static_cast<char>(2), static_cast<char>(1),
                                        static_cast<char>(0), static_cast<char>(0x80), static_cast<char>(0x80), static_cast<char>(0x80), static_cast<char>(0x80), static_cast<char>(0x80),
                                        static_cast<char>(0x80), static_cast<char>(0x80), static_cast<char>(0x80));
    return _mm_shuffle_epi8(lanes, order);
}

// Lane-local sNaN quieting; identical rule to PpcLoadPairPsqFloatBitsPackedInline.
inline __m128i PpcLoadPairPsqFloatBitsLanesInline(__m128i lanes) {
    const __m128i magnitude = _mm_and_si128(lanes, _mm_set1_epi32(0x7FFFFFFF));
    const __m128i nanMask = _mm_cmpgt_epi32(magnitude, _mm_set1_epi32(0x7F800000));
    return _mm_or_si128(lanes, _mm_and_si128(nanMask, _mm_set1_epi32(0x00400000)));
}

// Lane-local denormal flush + sNaN quieting; identical rule to
// PpcStorePairPsqFloatBitsPackedInline.
inline __m128i PpcStorePairPsqFloatBitsLanesInline(__m128i lanes) {
    const __m128i magnitude = _mm_and_si128(lanes, _mm_set1_epi32(0x7FFFFFFF));
    const __m128i subnormalMask = _mm_cmplt_epi32(magnitude, _mm_set1_epi32(0x00800000));
    const __m128i nanMask = _mm_cmpgt_epi32(magnitude, _mm_set1_epi32(0x7F800000));
    const __m128i quieted = _mm_or_si128(lanes, _mm_and_si128(nanMask, _mm_set1_epi32(0x00400000)));
    const __m128i signedZero = _mm_and_si128(lanes, _mm_set1_epi32(std::numeric_limits<int32_t>::min()));
    return _mm_or_si128(_mm_and_si128(subnormalMask, signedZero), _mm_andnot_si128(subnormalMask, quieted));
}
#endif // defined(__x86_64__)

// host -> packed FPR double, guard already proven by the caller.
inline double PpcLoadPairPsqFloatFromHostInline(const uint8_t* host) {
#if defined(__x86_64__)
    const __m128i raw = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(host));
    return PpcM128ToPsInline(_mm_castsi128_ps(PpcLoadPairPsqFloatBitsLanesInline(PpcPsqSwapPairBytesInline(raw))));
#elif defined(__aarch64__) || defined(__arm__) || defined(__wasm__)
    // The x86 path's full 8-byte pshufb reversal plus a same-endian load is,
    // taken together, exactly a 64-bit byteswap of a plain little-endian load:
    // it turns the on-disk [ps0 big-endian][ps1 big-endian] byte layout into a
    // native uint64 with low 32 bits = ps1, high 32 bits = ps0 (this file's
    // documented packed-double lane convention).
    uint64_t raw = 0;
    std::memcpy(&raw, host, sizeof(raw));
    const uint64_t swapped = __builtin_bswap64(raw);
    return PpcBitCastToDoubleInline(PpcLoadPairPsqFloatBitsPackedInline(swapped));
#endif
}

// packed FPR double -> host, guard already proven by the caller.
inline void PpcStorePairPsqFloatToHostInline(uint8_t* host, double value) {
#if defined(__x86_64__)
    const __m128i lanes = PpcStorePairPsqFloatBitsLanesInline(_mm_castps_si128(PpcPsToM128Inline(value)));
    _mm_storel_epi64(reinterpret_cast<__m128i*>(host), PpcPsqSwapPairBytesInline(lanes));
#elif defined(__aarch64__) || defined(__arm__) || defined(__wasm__)
    // Inverse of the load path above: bswap64 is its own inverse, so applying
    // it to the quieted packed value reproduces the on-disk big-endian bytes.
    const uint64_t quieted = PpcStorePairPsqFloatBitsPackedInline(PpcBitCastToU64Inline(value));
    const uint64_t swapped = __builtin_bswap64(quieted);
    std::memcpy(host, &swapped, sizeof(swapped));
#endif
}

template <typename SignedType>
inline SignedType PpcScaleAndClampPsqInline(float value, uint32_t scale) {
    if (scale == 0u) {
        constexpr float kMin = static_cast<float>(std::numeric_limits<SignedType>::min());
        constexpr float kMax = static_cast<float>(std::numeric_limits<SignedType>::max());
        return static_cast<SignedType>(std::clamp(value, kMin, kMax));
    }

    const float factor = scale < 32u ? std::ldexp(1.0f, static_cast<int>(scale)) : std::ldexp(1.0f, -(64 - static_cast<int>(scale)));
    const float scaled = value * factor;
    constexpr float kMin = static_cast<float>(std::numeric_limits<SignedType>::min());
    constexpr float kMax = static_cast<float>(std::numeric_limits<SignedType>::max());
    return static_cast<SignedType>(std::clamp(scaled, kMin, kMax));
}

template <typename SignedType>
inline float PpcDequantizePsqInline(SignedType value, uint32_t scale) {
    if (scale == 0u) {
        return static_cast<float>(value);
    }

    const float factor = scale < 32u ? std::ldexp(1.0f, -static_cast<int>(scale)) : std::ldexp(1.0f, static_cast<int>(64u - scale));
    return static_cast<float>(value) * factor;
}

// Paired-Single helpers (psq_l / psq_st). w=0 loads/stores both ps0/ps1 (paired), w=1 only
// ps0 (scalar); i is the GQR index (0-7) for quantization type and scale.
extern "C" double PPC_PsqL(uint32_t addr, uint32_t w, uint32_t i);
extern "C" void PPC_PsqSt(uint32_t addr, double value, uint32_t w, uint32_t i);

namespace PsqFlat {
#define MKW_PSQ_MEM MemoryInline
#include "ppc_isa_quantized_access.inc"
#undef MKW_PSQ_MEM
} // namespace PsqFlat

using namespace PsqFlat;

namespace MemoryHw {
#define MKW_PSQ_MEM MemoryHw
#include "ppc_isa_quantized_access.inc"
#undef MKW_PSQ_MEM
} // namespace MemoryHw

namespace MemoryAlias {
#define MKW_PSQ_MEM MemoryAlias
#include "ppc_isa_quantized_access.inc"
#undef MKW_PSQ_MEM
} // namespace MemoryAlias
