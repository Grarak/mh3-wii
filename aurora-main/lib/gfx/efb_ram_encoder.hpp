#pragma once

#include <dolphin/gx/GXEnum.h>

#include <cstddef>
#include <cstdint>

namespace aurora::gfx::efb_ram {

enum class HostPixelOrder : uint8_t {
  RGBA,
  BGRA,
};

bool supports_format(GXTexFmt format) noexcept;
size_t encoded_size(GXTexFmt format, uint32_t width, uint32_t height) noexcept;

// Encodes a decoded host texture back into the tiled layout a GX EFB copy writes. Host dimensions
// may be internally scaled; guest dimensions are always the native copy dimensions.
bool encode(void* dst, size_t dstSize, GXTexFmt format, uint32_t guestWidth, uint32_t guestHeight,
            const uint8_t* hostPixels, uint32_t hostWidth, uint32_t hostHeight, uint32_t hostBytesPerRow,
            HostPixelOrder order) noexcept;

} // namespace aurora::gfx::efb_ram
