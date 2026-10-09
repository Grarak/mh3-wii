#pragma once

// Render targets shared by the GX renderer: the EFB color/depth textures, the
// presentation source and the letterboxing math.

#include "gl.hpp"

#include <aurora/aurora.h>

#include <cstdint>

namespace aurora::gpu {
struct GraphicsConfig {
  // Window drawable size in pixels.
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t msaaSamples = 1;
  uint16_t textureAnisotropy = 1;
  uint32_t maxTextureDimension2D = 2048;
};

struct RenderTarget {
  gl::TexturePtr texture;
  uint32_t width = 0;
  uint32_t height = 0;

  explicit operator bool() const noexcept { return static_cast<bool>(texture); }
};

struct PresentSource {
  gl::TexturePtr texture;
  uint32_t width = 0;
  uint32_t height = 0;
};

struct Viewport {
  float left;
  float top;
  float width;
  float height;
  float znear;
  float zfar;

  bool operator==(const Viewport& rhs) const { return left == rhs.left && top == rhs.top && width == rhs.width && height == rhs.height && znear == rhs.znear && zfar == rhs.zfar; }
  bool operator!=(const Viewport& rhs) const { return !(*this == rhs); }
};

extern GraphicsConfig g_graphicsConfig;
// EFB color (RGBA8) and depth (Depth32F), top-down rows.
extern RenderTarget g_frameBuffer;
extern RenderTarget g_depthBuffer;

bool initialize(SDL_Window* window);
void shutdown();
void resize_swapchain(uint32_t width, uint32_t height, uint32_t native_width, uint32_t native_height, bool force = false);
const RenderTarget& present_source() noexcept;
PresentSource current_present_source() noexcept;
void set_present_source_override(gl::TexturePtr texture, uint32_t width, uint32_t height) noexcept;
void clear_present_source_override() noexcept;
Viewport calculate_present_viewport(uint32_t surface_width, uint32_t surface_height, uint32_t content_width, uint32_t content_height) noexcept;
Viewport calculate_present_viewport_for_aspect(uint32_t surface_width, uint32_t surface_height, float content_aspect) noexcept;
} // namespace aurora::gpu
