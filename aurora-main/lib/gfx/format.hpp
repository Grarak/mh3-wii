#pragma once
#include <cstdint>

namespace aurora::gl {
enum class Format : uint8_t {
  RGBA8, R8, RG8, R16I, R32F, R32UI, Depth32F, BC1, BC3,
  U8_RRRR, U8_1RRR, U8_R000, U8_R111, U8U8_GRRR, RGB565,
  P4_ABGR, P8_ABGR, BC4_RRRR, BC5_GRRR,
};
}
