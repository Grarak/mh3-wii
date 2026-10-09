#pragma once
// Shared offline/GXM layout contract. Format tags preserve sampled RGBA, not
// just channel count. No texture bytes, palette lookup or compression occurs here.
#include <aurora/fatal.hpp>
#include "format.hpp"
#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace aurora::gfx {
struct NativeTextureLevel {
  size_t offset;
  uint32_t width, height, pitch;
};

struct NativeTextureLayout {
  std::vector<NativeTextureLevel> levels;
  size_t image_bytes = 0, palette_offset = 0, palette_bytes = 0, total_bytes = 0;
};

constexpr size_t texture_align(size_t value, size_t alignment = 64) {
  return (value + alignment - 1) & ~(alignment - 1);
}

constexpr uint32_t native_pixel_bytes(gl::Format f) {
  switch (f) {
  case gl::Format::R8:
  case gl::Format::U8_RRRR:
  case gl::Format::U8_1RRR:
  case gl::Format::U8_R000:
  case gl::Format::U8_R111:
    return 1;
  case gl::Format::RG8:
  case gl::Format::U8U8_GRRR:
  case gl::Format::RGB565:
    return 2;
  case gl::Format::RGBA8:
  case gl::Format::R16I:
  case gl::Format::R32F:
  case gl::Format::R32UI:
  case gl::Format::Depth32F:
    return 4;
  default:
    return 0;
  }
}

constexpr uint32_t native_block_bytes(gl::Format f) {
  return f == gl::Format::BC1 || f == gl::Format::BC4_RRRR ? 8 : f == gl::Format::BC3 || f == gl::Format::BC5_GRRR ? 16 : 0;
}

constexpr uint32_t native_palette_entries(gl::Format f) {
  return f == gl::Format::P4_ABGR ? 16 : f == gl::Format::P8_ABGR ? 256 : 0;
}

constexpr bool native_swizzled(gl::Format f) {
  return native_block_bytes(f) || native_palette_entries(f);
}

inline NativeTextureLayout native_texture_layout(uint32_t width, uint32_t height, uint32_t mips, gl::Format f) {
  if (!width || !height || width > 4096 || height > 4096 || !mips || mips > uint32_t(std::bit_width(std::max(width, height))) || (f == gl::Format::Depth32F && mips != 1))
    AURORA_THROW(std::runtime_error("Invalid native texture dimensions/mips"));

  const uint32_t block = native_block_bytes(f), palette = native_palette_entries(f), pixel = native_pixel_bytes(f);

  if (!block && !palette && !pixel)
    AURORA_THROW(std::runtime_error("Unknown native texture layout"));

  if (native_swizzled(f) && (!std::has_single_bit(width) || !std::has_single_bit(height)))
    AURORA_THROW(std::runtime_error("Native swizzled assets require power-of-two dimensions"));

  NativeTextureLayout out;

  for (uint32_t l = 0; l < mips; l++) {
    const uint32_t w = std::max(width >> l, 1u), h = std::max(height >> l, 1u);
    uint32_t pitch = 0;
    size_t advance;

    if (block)
      advance = size_t(std::max(w / 4, 1u)) * std::max(h / 4, 1u) * block;
    else if (palette)
      advance = size_t(h) * (f == gl::Format::P4_ABGR ? (w + 1) / 2 : w);
    else {
      pitch = uint32_t(texture_align(w, f == gl::Format::Depth32F ? 32 : 8)) * pixel;
      advance = f == gl::Format::Depth32F ? size_t(pitch) * texture_align(h, 32)
                : l + 1 < mips            ? size_t(std::max(std::bit_ceil(width) >> l, 8u)) * std::max(std::bit_ceil(height) >> l, 1u) * pixel
                                          : size_t(pitch) * h;
    }

    out.levels.push_back({out.image_bytes, w, h, pitch});
    out.image_bytes += advance;
  }

  if (palette) {
    out.palette_offset = texture_align(out.image_bytes);
    out.palette_bytes = palette * 4;
  }

  out.total_bytes = palette ? out.palette_offset + out.palette_bytes : out.image_bytes;
  return out;
}

// File tags are stable independently of renderer enum values. Version1 uses1..3.
inline gl::Format native_texture_format(uint32_t tag) {
  switch (tag) {
  case 1:
    return gl::Format::RGBA8;
  case 2:
    return gl::Format::BC1;
  case 3:
    return gl::Format::BC3;
  case 4:
    return gl::Format::P4_ABGR;
  case 5:
    return gl::Format::P8_ABGR;
  case 6:
    return gl::Format::U8_RRRR;
  case 7:
    return gl::Format::U8_1RRR;
  case 8:
    return gl::Format::U8_R000;
  case 9:
    return gl::Format::U8_R111;
  case 10:
    return gl::Format::U8U8_GRRR;
  case 11:
    return gl::Format::RGB565;
  case 12:
    return gl::Format::BC4_RRRR;
  case 13:
    return gl::Format::BC5_GRRR;
  default:
    AURORA_THROW(std::runtime_error("Unsupported offline native texture tag"));
  }
}
} // namespace aurora::gfx
