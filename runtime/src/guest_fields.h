// Reads and writes of guest structure fields, for the C reconstructions.
#pragma once

#include "memory.h"

#include <cstdint>

namespace GuestFields {

inline uint32_t R8(uint32_t a) {
    return MemoryInline::FlatRead8(a);
}

inline int32_t S8(uint32_t a) {
    return int8_t(MemoryInline::FlatRead8(a));
}

inline uint32_t R16(uint32_t a) {
    return MemoryInline::FlatRead16(a);
}

inline int32_t S16(uint32_t a) {
    return int16_t(MemoryInline::FlatRead16(a));
}

inline uint32_t R32(uint32_t a) {
    return MemoryInline::FlatRead32(a);
}

inline float RF(uint32_t a) {
    return MemoryInline::FlatReadFloat32(a);
}

inline void W8(uint32_t a, uint32_t v) {
    MemoryInline::FlatWrite8(a, uint8_t(v));
}

inline void W16(uint32_t a, uint32_t v) {
    MemoryInline::FlatWrite16(a, uint16_t(v));
}

inline void W32(uint32_t a, uint32_t v) {
    MemoryInline::FlatWrite32(a, v);
}

inline void WF(uint32_t a, float v) {
    MemoryInline::FlatWriteFloat32(a, v);
}

// A VEC3 by its words.
inline void Copy3(uint32_t to, uint32_t from) {
    for (uint32_t k = 0; k < 12; k += 4)
        W32(to + k, R32(from + k));
}

} // namespace GuestFields
