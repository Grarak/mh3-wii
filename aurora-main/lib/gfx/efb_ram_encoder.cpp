#include "efb_ram_encoder.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace aurora::gfx::efb_ram {
namespace {

struct Pixel {
  uint8_t r;
  uint8_t g;
  uint8_t b;
  uint8_t a;
};

struct BlockInfo {
  uint32_t width;
  uint32_t height;
  uint32_t bytes;
};

enum class Layout {
  I4,
  I8,
  IA4,
  IA8,
  RGB565,
  RGB5A3,
  RGBA8,
};

bool describe(GXTexFmt format, Layout& layout, BlockInfo& block) noexcept {
  switch (format) {
  case GX_TF_I4:
  case GX_CTF_R4:
    layout = Layout::I4;
    block = {8, 8, 32};
    return true;
  case GX_TF_I8:
  case GX_CTF_A8:
  case GX_CTF_R8:
  case GX_CTF_G8:
  case GX_CTF_B8:
    layout = Layout::I8;
    block = {8, 4, 32};
    return true;
  case GX_TF_IA4:
  case GX_CTF_RA4:
    layout = Layout::IA4;
    block = {8, 4, 32};
    return true;
  case GX_TF_IA8:
  case GX_TF_Z16:
  case GX_CTF_RA8:
  case GX_CTF_RG8:
  case GX_CTF_GB8:
    layout = Layout::IA8;
    block = {4, 4, 32};
    return true;
  case GX_TF_RGB565:
    layout = Layout::RGB565;
    block = {4, 4, 32};
    return true;
  case GX_TF_RGB5A3:
    layout = Layout::RGB5A3;
    block = {4, 4, 32};
    return true;
  case GX_TF_RGBA8:
  case GX_TF_Z24X8:
    layout = Layout::RGBA8;
    block = {4, 4, 64};
    return true;
  default:
    return false;
  }
}

Pixel read_pixel(const uint8_t* pixels, uint32_t hostWidth, uint32_t hostHeight, uint32_t bytesPerRow,
                 HostPixelOrder order, uint32_t guestX, uint32_t guestY, uint32_t guestWidth,
                 uint32_t guestHeight) noexcept {
  const uint32_t clampedGuestX = std::min(guestX, guestWidth - 1);
  const uint32_t clampedGuestY = std::min(guestY, guestHeight - 1);
  const uint64_t sampleXNumerator = (static_cast<uint64_t>(clampedGuestX) * 2 + 1) * hostWidth;
  const uint64_t sampleYNumerator = (static_cast<uint64_t>(clampedGuestY) * 2 + 1) * hostHeight;
  const uint32_t hostX =
      std::min<uint32_t>(static_cast<uint32_t>(sampleXNumerator / (guestWidth * 2ull)), hostWidth - 1);
  const uint32_t hostY =
      std::min<uint32_t>(static_cast<uint32_t>(sampleYNumerator / (guestHeight * 2ull)), hostHeight - 1);
  const auto* src = pixels + static_cast<size_t>(hostY) * bytesPerRow + static_cast<size_t>(hostX) * 4;
  if (order == HostPixelOrder::BGRA) {
    return {src[2], src[1], src[0], src[3]};
  }
  return {src[0], src[1], src[2], src[3]};
}

void write_be16(uint8_t*& dst, uint16_t value) noexcept {
  *dst++ = static_cast<uint8_t>(value >> 8);
  *dst++ = static_cast<uint8_t>(value);
}

} // namespace

bool supports_format(GXTexFmt format) noexcept {
  Layout layout{};
  BlockInfo block{};
  return describe(format, layout, block);
}

size_t encoded_size(GXTexFmt format, uint32_t width, uint32_t height) noexcept {
  Layout layout{};
  BlockInfo block{};
  if (width == 0 || height == 0 || !describe(format, layout, block)) {
    return 0;
  }
  const uint64_t blocksX = (static_cast<uint64_t>(width) + block.width - 1) / block.width;
  const uint64_t blocksY = (static_cast<uint64_t>(height) + block.height - 1) / block.height;
  const uint64_t size = blocksX * blocksY * block.bytes;
  return size <= SIZE_MAX ? static_cast<size_t>(size) : 0;
}

bool encode(void* dstValue, size_t dstSize, GXTexFmt format, uint32_t guestWidth, uint32_t guestHeight,
            const uint8_t* hostPixels, uint32_t hostWidth, uint32_t hostHeight, uint32_t hostBytesPerRow,
            HostPixelOrder order) noexcept {
  Layout layout{};
  BlockInfo block{};
  const size_t required = encoded_size(format, guestWidth, guestHeight);
  if (dstValue == nullptr || hostPixels == nullptr || required == 0 || dstSize < required || hostWidth == 0 ||
      hostHeight == 0 || hostBytesPerRow < hostWidth * 4 || !describe(format, layout, block)) {
    return false;
  }

  auto* dst = static_cast<uint8_t*>(dstValue);
  std::memset(dst, 0, required);
  const uint32_t blocksX = (guestWidth + block.width - 1) / block.width;
  const uint32_t blocksY = (guestHeight + block.height - 1) / block.height;

  for (uint32_t blockY = 0; blockY < blocksY; ++blockY) {
    for (uint32_t blockX = 0; blockX < blocksX; ++blockX) {
      uint8_t* blockDst = dst + (static_cast<size_t>(blockY) * blocksX + blockX) * block.bytes;
      if (layout == Layout::RGBA8) {
        uint8_t* ar = blockDst;
        uint8_t* gb = blockDst + 32;
        for (uint32_t y = 0; y < 4; ++y) {
          for (uint32_t x = 0; x < 4; ++x) {
            const Pixel pixel = read_pixel(hostPixels, hostWidth, hostHeight, hostBytesPerRow, order, blockX * 4 + x,
                                           blockY * 4 + y, guestWidth, guestHeight);
            *ar++ = pixel.a;
            *ar++ = pixel.r;
            *gb++ = pixel.g;
            *gb++ = pixel.b;
          }
        }
        continue;
      }

      uint8_t* out = blockDst;
      for (uint32_t y = 0; y < block.height; ++y) {
        if (layout == Layout::I4) {
          for (uint32_t x = 0; x < block.width; x += 2) {
            const Pixel first =
                read_pixel(hostPixels, hostWidth, hostHeight, hostBytesPerRow, order, blockX * block.width + x,
                           blockY * block.height + y, guestWidth, guestHeight);
            const Pixel second =
                read_pixel(hostPixels, hostWidth, hostHeight, hostBytesPerRow, order, blockX * block.width + x + 1,
                           blockY * block.height + y, guestWidth, guestHeight);
            *out++ = static_cast<uint8_t>((first.r & 0xf0) | (second.r >> 4));
          }
          continue;
        }

        for (uint32_t x = 0; x < block.width; ++x) {
          const Pixel pixel = read_pixel(hostPixels, hostWidth, hostHeight, hostBytesPerRow, order,
                                         blockX * block.width + x, blockY * block.height + y, guestWidth, guestHeight);
          switch (layout) {
          case Layout::I8:
            *out++ = pixel.r;
            break;
          case Layout::IA4:
            *out++ = static_cast<uint8_t>((pixel.a & 0xf0) | (pixel.r >> 4));
            break;
          case Layout::IA8:
            *out++ = pixel.a;
            *out++ = pixel.r;
            break;
          case Layout::RGB565: {
            const uint16_t packed =
                static_cast<uint16_t>(((pixel.r >> 3) << 11) | ((pixel.g >> 2) << 5) | (pixel.b >> 3));
            write_be16(out, packed);
            break;
          }
          case Layout::RGB5A3: {
            const uint16_t packed =
                pixel.a >= 224
                    ? static_cast<uint16_t>(0x8000 | ((pixel.r >> 3) << 10) | ((pixel.g >> 3) << 5) | (pixel.b >> 3))
                    : static_cast<uint16_t>(((pixel.a >> 5) << 12) | ((pixel.r >> 4) << 8) | ((pixel.g >> 4) << 4) |
                                            (pixel.b >> 4));
            write_be16(out, packed);
            break;
          }
          default:
            return false;
          }
        }
      }
    }
  }
  return true;
}

} // namespace aurora::gfx::efb_ram
