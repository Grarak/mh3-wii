#pragma once
// Host diagnostics only. Vita reads the prepared image/palette allocation
// directly; it never links this CPU expansion or a compression encoder.
#include <aurora/fatal.hpp>
#include "native_texture_pack.hpp"
#include <cstring>

namespace aurora::gfx {
inline size_t native_texture_morton(uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
  size_t result = 0;
  unsigned shift = 0;

  for (unsigned bit = 0; (1u << bit) < std::max(width, height); bit++) {
    if ((1u << bit) < height)
      result |= size_t((y >> bit) & 1) << shift++;

    if ((1u << bit) < width)
      result |= size_t((x >> bit) & 1) << shift++;
  }

  return result;
}

inline std::array<uint8_t, 4> native_color565(uint16_t v) {
  const unsigned r = v >> 11, g = v >> 5 & 63, b = v & 31;
  return {uint8_t(r << 3 | r >> 2), uint8_t(g << 2 | g >> 4), uint8_t(b << 3 | b >> 2), 255};
}

inline uint8_t native_bc_channel(const uint8_t* block, unsigned texel) {
  uint8_t palette[8]{block[0], block[1]};
  const unsigned divisor = block[0] > block[1] ? 7 : 5;

  for (unsigned i = 2; i < divisor + 1; i++)
    palette[i] = uint8_t(((divisor + 1 - i) * block[0] + (i - 1) * block[1] + divisor / 2) / divisor);

  if (divisor == 5) {
    palette[6] = 0;
    palette[7] = 255;
  }

  uint64_t indices = 0;

  for (unsigned b = 0; b < 6; b++)
    indices |= uint64_t(block[2 + b]) << (8 * b);

  return palette[indices >> (3 * texel) & 7];
}

inline std::vector<uint8_t> native_texture_rgba_level(const NativeTextureRecord& record, std::span<const uint8_t> payload, uint32_t mip) {
  const auto f = native_texture_format(record.format);
  const auto layout = native_texture_layout(record.width, record.height, record.mips, f);

  if (payload.size() != layout.total_bytes || mip >= layout.levels.size())
    AURORA_THROW(std::runtime_error("Invalid diagnostic native texture payload/mip"));

  const auto& level = layout.levels[mip];
  const auto* image = payload.data() + level.offset;
  std::vector<uint8_t> out(size_t(level.width) * level.height * 4);

  for (uint32_t y = 0; y < level.height; y++)
    for (uint32_t x = 0; x < level.width; x++) {
      std::array<uint8_t, 4> color{0, 0, 0, 255};

      if (native_palette_entries(f)) {
        const auto at = native_texture_morton(x, y, level.width, level.height);
        const unsigned index = f == gl::Format::P4_ABGR ? image[at / 2] >> (4 * (at & 1)) & 15 : image[at];
        std::memcpy(color.data(), payload.data() + layout.palette_offset + index * 4, 4);
      } else if (const auto block_bytes = native_block_bytes(f)) {
        const auto* block = image + native_texture_morton(x / 4, y / 4, std::max(level.width / 4, 1u), std::max(level.height / 4, 1u)) * block_bytes;
        const unsigned texel = (y % 4) * 4 + x % 4;

        if (f == gl::Format::BC4_RRRR) {
          const auto v = native_bc_channel(block, texel);
          color = {v, v, v, v};
        } else if (f == gl::Format::BC5_GRRR) {
          const auto v = native_bc_channel(block, texel);
          color = {v, v, v, native_bc_channel(block + 8, texel)};
        } else {
          const auto* rgb = block + (f == gl::Format::BC3 ? 8 : 0);
          const uint16_t a = uint16_t(rgb[0]) | uint16_t(rgb[1]) << 8, b = uint16_t(rgb[2]) | uint16_t(rgb[3]) << 8;
          const auto first = native_color565(a), second = native_color565(b);
          const unsigned index = pack_u32(rgb + 4) >> (2 * texel) & 3;

          if (index < 2)
            color = index ? second : first;
          else {
            for (unsigned c = 0; c < 3; c++)
              color[c] = uint8_t(a > b ? ((index == 2 ? 2 : 1) * first[c] + (index == 2 ? 1 : 2) * second[c] + 1) / 3 : (first[c] + second[c] + 1) / 2);

            if (a <= b && index == 3)
              color[3] = 0;
          }

          if (f == gl::Format::BC3)
            color[3] = native_bc_channel(block, texel);
        }
      } else {
        const auto* p = image + size_t(y) * level.pitch + x * native_pixel_bytes(f);

        switch (f) {
        case gl::Format::RGBA8:
          std::memcpy(color.data(), p, 4);
          break;
        case gl::Format::U8_RRRR:
          color = {p[0], p[0], p[0], p[0]};
          break;
        case gl::Format::U8_1RRR:
          color = {p[0], p[0], p[0], 255};
          break;
        case gl::Format::U8_R000:
          color = {0, 0, 0, p[0]};
          break;
        case gl::Format::U8_R111:
          color = {255, 255, 255, p[0]};
          break;
        case gl::Format::U8U8_GRRR:
          color = {p[0], p[0], p[0], p[1]};
          break;
        case gl::Format::RGB565:
          color = native_color565(uint16_t(p[0]) | uint16_t(p[1]) << 8);
          break;
        default:
          AURORA_THROW(std::runtime_error("Unsupported host diagnostic texture format"));
        }
      }

      std::memcpy(out.data() + (size_t(y) * level.width + x) * 4, color.data(), 4);
    }
  return out;
}
} // namespace aurora::gfx
