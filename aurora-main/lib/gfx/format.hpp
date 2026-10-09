#pragma once
#include <cstdint>

namespace aurora::gl {
enum class Format : uint8_t {
  RGBA8, R8, RG8, R16I, R32F, R32UI, Depth32F,
};
}
