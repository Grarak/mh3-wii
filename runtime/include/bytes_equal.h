// Byte equality of two buffers, for the state snapshots and publications the
// renderer compares per draw (hundreds of bytes to ~1.3 KB). Only equality is
// reported, so NEON compares 64 bytes per step and stops at the first
// differing block; newlib's memcmp on the Vita compares a word per iteration.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif

inline bool BytesEqual(const void* a, const void* b, size_t size) {
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    const auto* x = static_cast<const uint8_t*>(a);
    const auto* y = static_cast<const uint8_t*>(b);

    for (; size >= 64; x += 64, y += 64, size -= 64) {
        uint8x16_t d = veorq_u8(vld1q_u8(x), vld1q_u8(y));
        d = vorrq_u8(d, veorq_u8(vld1q_u8(x + 16), vld1q_u8(y + 16)));
        d = vorrq_u8(d, veorq_u8(vld1q_u8(x + 32), vld1q_u8(y + 32)));
        d = vorrq_u8(d, veorq_u8(vld1q_u8(x + 48), vld1q_u8(y + 48)));
        const uint64x2_t lanes = vreinterpretq_u64_u8(d);

        if (vgetq_lane_u64(lanes, 0) | vgetq_lane_u64(lanes, 1))
            return false;
    }

    for (; size >= 16; x += 16, y += 16, size -= 16) {
        const uint64x2_t lanes = vreinterpretq_u64_u8(veorq_u8(vld1q_u8(x), vld1q_u8(y)));

        if (vgetq_lane_u64(lanes, 0) | vgetq_lane_u64(lanes, 1))
            return false;
    }

    return !size || !std::memcmp(x, y, size);
#else
    return !std::memcmp(a, b, size);
#endif
}
