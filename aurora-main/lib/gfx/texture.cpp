#include "common.hpp"

#include "../internal.hpp"
#include "aurora/aurora.h"
#include "texture.hpp"
#include "texture_convert.hpp"
#include "../gx/gx_fmt.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <utility>

#include <fmt/format.h>

namespace aurora::gfx {
namespace {
Module Log("aurora::gfx");

uint32_t clamp_mip_count(uint32_t width, uint32_t height, uint32_t mips, const char* label) noexcept {
  const uint32_t maxMips = max_texture_mip_count(width, height);
  const uint32_t safeMips = std::clamp(mips, 1u, maxMips);

  if (safeMips != mips) {
    Log.warn("{}: clamping mip count {} to {} for {}x{} texture", label, mips, safeMips, width, height);
  }

  return safeMips;
}

// Uploads every mip of `data` (tightly packed, mip after mip). Returns false if the data ran out.
bool upload_mips(const TextureRef& ref, ArrayRef<uint8_t> data, const char* label) noexcept {
  const auto bytesPerPixel = gl::format_info(ref.format).bytesPerPixel;
  size_t offset = 0;

  for (uint32_t mip = 0; mip < ref.mipCount; ++mip) {
    const uint32_t mipWidth = std::max(ref.size.width >> mip, 1u);
    const uint32_t mipHeight = std::max(ref.size.height >> mip, 1u);
    const uint32_t bytesPerRow = mipWidth * bytesPerPixel;
    const size_t dataSize = static_cast<size_t>(bytesPerRow) * mipHeight;

    if (offset > data.size() || dataSize > data.size() - offset) {
      Log.warn("{}: stopping at mip {}: expected at least {} bytes, got {}", label, mip, offset + dataSize, data.size());
      return false;
    }

    gl::write_texture(ref.texture, mip, 0, 0, mipWidth, mipHeight, data.data() + offset, bytesPerRow);
    offset += dataSize;
  }

  if (data.size() != UINT32_MAX && offset < data.size()) {
    Log.warn("{}: texture used {} bytes, but given {} bytes", label, offset, data.size());
  }

  return true;
}
} // namespace

uint64_t calc_texture_size(gl::Format format, u32 width, u32 height, u32 mips) noexcept {
  const auto bytesPerPixel = gl::format_info(format).bytesPerPixel;
  uint64_t total = 0;

  for (uint32_t mip = 0; mip < mips; ++mip) {
    if (format == gl::Format::BC1 || format == gl::Format::BC3)
      total += uint64_t(std::max((width >> mip) / 4, 1u)) * std::max((height >> mip) / 4, 1u) * (format == gl::Format::BC1 ? 8 : 16);
    else
      total += static_cast<uint64_t>(std::max(width >> mip, 1u)) * std::max(height >> mip, 1u) * bytesPerPixel;
  }

  return total;
}

TextureHandle new_static_texture_2d(uint32_t width, uint32_t height, uint32_t mips, u32 format, ArrayRef<uint8_t> data, bool tlut, const char* label) noexcept {

  mips = clamp_mip_count(width, height, mips, label);
  auto handle = new_dynamic_texture_2d(width, height, mips, format, label);
  auto& ref = *handle;

  ConvertedTexture converted;

  if (ref.gxFormat != InvalidTextureFormat) {
    if (tlut) {
      CHECK(ref.size.height == 1, "new_static_texture_2d[{}]: expected tlut height 1, got {}", label, ref.size.height);
      CHECK(ref.mipCount == 1, "new_static_texture_2d[{}]: expected tlut mipCount 1, got {}", label, ref.mipCount);
      converted = convert_tlut(ref.gxFormat, ref.size.width, data);
    } else {
      converted = convert_texture(ref.gxFormat, ref.size.width, ref.size.height, ref.mipCount, data);
    }

    if (!converted.data.empty()) {
      data = converted.data;
      ref.hasArbitraryMips = converted.hasArbitraryMips;
    }
  }

  upload_mips(ref, data, label);
  return handle;
}

TextureHandle new_dynamic_texture_2d(uint32_t width, uint32_t height, uint32_t mips, u32 gxFormat, const char* label) noexcept {
  mips = clamp_mip_count(width, height, mips, label);
  const auto format = to_gl_format(gxFormat);
  return std::make_shared<TextureRef>(gl::create_texture(width, height, mips, format, label), Extent{width, height, 1}, format, mips, gxFormat);
}

TextureHandle new_render_texture(uint32_t width, uint32_t height, u32 gxFormat, const char* label) noexcept {
  auto handle = std::make_shared<TextureRef>(gl::create_texture(width, height, 1, gl::Format::RGBA8, label), Extent{width, height, 1}, gl::Format::RGBA8, 1, gxFormat);
  handle->isRenderTarget = true;
  return handle;
}

TextureHandle new_conv_texture(uint32_t width, uint32_t height, u32 gxFormat, const char* label) noexcept {
  const auto format = to_gl_format(gxFormat);
  auto handle = std::make_shared<TextureRef>(gl::create_texture(width, height, 1, format, label), Extent{width, height, 1}, format, 1, gxFormat);
  handle->isRenderTarget = true;
  return handle;
}

void write_texture(TextureRef& ref, ArrayRef<uint8_t> data) noexcept {

  ConvertedTexture converted;

  if (ref.gxFormat != InvalidTextureFormat) {
    converted = convert_texture(ref.gxFormat, ref.size.width, ref.size.height, ref.mipCount, data);
    ref.hasArbitraryMips = converted.hasArbitraryMips;

    if (!converted.data.empty()) {
      data = converted.data;
    }
  }

  upload_mips(ref, data, "write_texture");
}
} // namespace aurora::gfx
