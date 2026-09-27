#pragma once

#include "isa/big_endian.h"

// AX/DSP fixed-point mix kernels: the inner loops of the AXWii command-list mix in ax_mix.cpp.
// Each exists as a scalar reference (a literal transcription of the loop it replaced) and an AVX2 form.

#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(__AVX2__)
#include <immintrin.h>
#define MKW_AX_MIX_AVX2 1
#else
#define MKW_AX_MIX_AVX2 0
#endif

namespace AxMixKernels {

inline int16_t ClampToS16(int32_t value) {
    if (value < -0x8000) {
        return static_cast<int16_t>(-0x8000);
    }
    if (value > 0x7fff) {
        return static_cast<int16_t>(0x7fff);
    }
    return static_cast<int16_t>(value);
}

// MixAdd: accumulates a voice into a 32-bit mix bus through a 16-bit volume ramp, remembering the
// last emitted sample as the de-pop value. Returns the ramp after `count` steps; `dpop` is left
// untouched when count == 0, matching the scalar loop.

inline uint16_t MixAddRampScalar(int32_t* out, const int16_t* input, uint32_t count,
                                 uint16_t volume, uint16_t delta, int16_t& dpop) {
    int16_t last = dpop;
    for (uint32_t i = 0; i < count; ++i) {
        const int32_t scaled =
            (static_cast<int32_t>(input[i]) * static_cast<int32_t>(volume)) >> 15;
        const int16_t sample = ClampToS16(scaled);
        out[i] += sample;
        volume = static_cast<uint16_t>(volume + delta);
        last = sample;
    }
    dpop = last;
    return volume;
}

#if MKW_AX_MIX_AVX2
inline uint16_t MixAddRampAvx2(int32_t* out, const int16_t* input, uint32_t count,
                               uint16_t volume, uint16_t delta, int16_t& dpop) {
    uint32_t i = 0;
    int16_t last = dpop;
    if (count >= 8) {
        const __m256i lanes = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
        const __m256i deltaVec = _mm256_set1_epi32(static_cast<int32_t>(delta));
        const __m256i wrapMask = _mm256_set1_epi32(0xFFFF);
        const __m256i clampLow = _mm256_set1_epi32(-0x8000);
        const __m256i clampHigh = _mm256_set1_epi32(0x7FFF);
        const __m256i blockStep = _mm256_set1_epi32(static_cast<int32_t>(delta) * 8);
        // volume + k*delta stays well inside int32 for every count the AX mix
        // uses (<= 96), so the wrap is applied only by the 16-bit mask below -
        // exactly where the scalar loop applies it.
        __m256i ramp = _mm256_add_epi32(_mm256_set1_epi32(static_cast<int32_t>(volume)),
                                        _mm256_mullo_epi32(lanes, deltaVec));
        __m256i lastBlock = _mm256_setzero_si256();
        for (; i + 8 <= count; i += 8) {
            const __m256i samples = _mm256_cvtepi16_epi32(
                _mm_loadu_si128(reinterpret_cast<const __m128i*>(input + i)));
            const __m256i vol = _mm256_and_si256(ramp, wrapMask);
            // int16 x uint16 always fits in int32, so the low half of the
            // 32x32 multiply is the exact product.
            __m256i scaled = _mm256_srai_epi32(_mm256_mullo_epi32(samples, vol), 15);
            scaled = _mm256_min_epi32(_mm256_max_epi32(scaled, clampLow), clampHigh);
            const __m256i acc =
                _mm256_loadu_si256(reinterpret_cast<const __m256i*>(out + i));
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(out + i),
                                _mm256_add_epi32(acc, scaled));
            lastBlock = scaled;
            ramp = _mm256_add_epi32(ramp, blockStep);
        }
        if (i != 0) {
            last = static_cast<int16_t>(_mm256_extract_epi32(lastBlock, 7));
            volume = static_cast<uint16_t>(
                static_cast<uint32_t>(_mm256_extract_epi32(ramp, 0)));
        }
    }
    for (; i < count; ++i) {
        const int32_t scaled =
            (static_cast<int32_t>(input[i]) * static_cast<int32_t>(volume)) >> 15;
        const int16_t sample = ClampToS16(scaled);
        out[i] += sample;
        volume = static_cast<uint16_t>(volume + delta);
        last = sample;
    }
    dpop = last;
    return volume;
}
#endif

inline uint16_t MixAddRamp(int32_t* out, const int16_t* input, uint32_t count,
                           uint16_t volume, uint16_t delta, int16_t& dpop) {
#if MKW_AX_MIX_AVX2
    return MixAddRampAvx2(out, input, count, volume, delta, dpop);
#else
    return MixAddRampScalar(out, input, count, volume, delta, dpop);
#endif
}

// ---------------------------------------------------------------------------
// Voice volume envelope: scale a 16-bit sample block in place through the same
// wrapping 16-bit ramp. Returns the ramp value after `count` steps.
// ---------------------------------------------------------------------------

inline uint16_t ScaleRampScalar(int16_t* samples, uint32_t count, uint16_t volume,
                                uint16_t delta) {
    for (uint32_t i = 0; i < count; ++i) {
        const int32_t scaled =
            (static_cast<int32_t>(samples[i]) * static_cast<int32_t>(volume)) >> 15;
        samples[i] = ClampToS16(scaled);
        volume = static_cast<uint16_t>(volume + delta);
    }
    return volume;
}

#if MKW_AX_MIX_AVX2
inline uint16_t ScaleRampAvx2(int16_t* samples, uint32_t count, uint16_t volume,
                              uint16_t delta) {
    uint32_t i = 0;
    if (count >= 8) {
        const __m256i lanes = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
        const __m256i deltaVec = _mm256_set1_epi32(static_cast<int32_t>(delta));
        const __m256i wrapMask = _mm256_set1_epi32(0xFFFF);
        const __m256i blockStep = _mm256_set1_epi32(static_cast<int32_t>(delta) * 8);
        __m256i ramp = _mm256_add_epi32(_mm256_set1_epi32(static_cast<int32_t>(volume)),
                                        _mm256_mullo_epi32(lanes, deltaVec));
        for (; i + 8 <= count; i += 8) {
            const __m256i block = _mm256_cvtepi16_epi32(
                _mm_loadu_si128(reinterpret_cast<const __m128i*>(samples + i)));
            const __m256i vol = _mm256_and_si256(ramp, wrapMask);
            const __m256i scaled =
                _mm256_srai_epi32(_mm256_mullo_epi32(block, vol), 15);
            // Signed 32 -> 16 saturation is exactly clamp(-0x8000, 0x7fff).
            const __m128i packed = _mm_packs_epi32(_mm256_castsi256_si128(scaled),
                                                   _mm256_extracti128_si256(scaled, 1));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(samples + i), packed);
            ramp = _mm256_add_epi32(ramp, blockStep);
        }
        if (i != 0) {
            volume = static_cast<uint16_t>(
                static_cast<uint32_t>(_mm256_extract_epi32(ramp, 0)));
        }
    }
    for (; i < count; ++i) {
        const int32_t scaled =
            (static_cast<int32_t>(samples[i]) * static_cast<int32_t>(volume)) >> 15;
        samples[i] = ClampToS16(scaled);
        volume = static_cast<uint16_t>(volume + delta);
    }
    return volume;
}
#endif

inline uint16_t ScaleRamp(int16_t* samples, uint32_t count, uint16_t volume,
                          uint16_t delta) {
#if MKW_AX_MIX_AVX2
    return ScaleRampAvx2(samples, count, volume, delta);
#else
    return ScaleRampScalar(samples, count, volume, delta);
#endif
}

// ---------------------------------------------------------------------------
// AUX return: accumulate a 32-bit aux bus into a 32-bit main bus through a
// per-sample 16-bit ramp table.
// ---------------------------------------------------------------------------

inline void MixAccumRamp32Scalar(int32_t* dst, const int32_t* src, const uint16_t* ramp,
                                 uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        dst[i] += static_cast<int32_t>(
            (static_cast<int64_t>(src[i]) * static_cast<int64_t>(ramp[i])) >> 15);
    }
}

#if MKW_AX_MIX_AVX2
inline void MixAccumRamp32Avx2(int32_t* dst, const int32_t* src, const uint16_t* ramp,
                               uint32_t count) {
    uint32_t i = 0;
    for (; i + 8 <= count; i += 8) {
        const __m256i source =
            _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + i));
        const __m256i gain = _mm256_cvtepu16_epi32(
            _mm_loadu_si128(reinterpret_cast<const __m128i*>(ramp + i)));
        // Even dwords (indices 0,2,4,6) and odd dwords (1,3,5,7) as two sets of
        // four signed 32x32 -> 64 products. |product| < 2^47, so shifting the
        // 64-bit lane logically by 15 leaves bits 15..46 of the product in the
        // low dword - the same bits the scalar (int32)(p >> 15) keeps.
        const __m256i even = _mm256_srli_epi64(_mm256_mul_epi32(source, gain), 15);
        const __m256i odd = _mm256_srli_epi64(
            _mm256_mul_epi32(_mm256_srli_epi64(source, 32), _mm256_srli_epi64(gain, 32)),
            15);
        const __m256i result =
            _mm256_blend_epi32(even, _mm256_slli_epi64(odd, 32), 0xAA);
        const __m256i acc = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(dst + i));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i),
                            _mm256_add_epi32(acc, result));
    }
    for (; i < count; ++i) {
        dst[i] += static_cast<int32_t>(
            (static_cast<int64_t>(src[i]) * static_cast<int64_t>(ramp[i])) >> 15);
    }
}
#endif

inline void MixAccumRamp32(int32_t* dst, const int32_t* src, const uint16_t* ramp,
                           uint32_t count) {
#if MKW_AX_MIX_AVX2
    MixAccumRamp32Avx2(dst, src, ramp, count);
#else
    MixAccumRamp32Scalar(dst, src, ramp, count);
#endif
}

// ---------------------------------------------------------------------------
// Guest mix-buffer marshalling: host int32 <-> big-endian guest words.
// ---------------------------------------------------------------------------

inline void StoreBigEndian32Scalar(uint8_t* dst, const int32_t* src, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        BigEndian::Write32(dst + i * sizeof(uint32_t), static_cast<uint32_t>(src[i]));
    }
}

inline void LoadBigEndian32Scalar(int32_t* dst, const uint8_t* src, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        dst[i] = static_cast<int32_t>(BigEndian::Read32(src + i * sizeof(uint32_t)));
    }
}

#if MKW_AX_MIX_AVX2
inline __m256i ByteSwapMask32() {
    return _mm256_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12,
                            3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
}

inline void StoreBigEndian32Avx2(uint8_t* dst, const int32_t* src, size_t count) {
    const __m256i mask = ByteSwapMask32();
    size_t i = 0;
    for (; i + 8 <= count; i += 8) {
        const __m256i value =
            _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + i));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i * sizeof(uint32_t)),
                            _mm256_shuffle_epi8(value, mask));
    }
    StoreBigEndian32Scalar(dst + i * sizeof(uint32_t), src + i, count - i);
}

inline void LoadBigEndian32Avx2(int32_t* dst, const uint8_t* src, size_t count) {
    const __m256i mask = ByteSwapMask32();
    size_t i = 0;
    for (; i + 8 <= count; i += 8) {
        const __m256i value = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(src + i * sizeof(uint32_t)));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i),
                            _mm256_shuffle_epi8(value, mask));
    }
    LoadBigEndian32Scalar(dst + i, src + i * sizeof(uint32_t), count - i);
}
#endif

inline void StoreBigEndian32(uint8_t* dst, const int32_t* src, size_t count) {
#if MKW_AX_MIX_AVX2
    StoreBigEndian32Avx2(dst, src, count);
#else
    StoreBigEndian32Scalar(dst, src, count);
#endif
}

inline void LoadBigEndian32(int32_t* dst, const uint8_t* src, size_t count) {
#if MKW_AX_MIX_AVX2
    LoadBigEndian32Avx2(dst, src, count);
#else
    LoadBigEndian32Scalar(dst, src, count);
#endif
}

} // namespace AxMixKernels
