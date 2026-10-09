#include "device.hpp"

#include "../internal.hpp"
#include "../window.hpp"
#include "../dolphin/vi/vi_internal.hpp"

#include <aurora/render_size_limits.hpp>

#include <algorithm>
#include <cmath>

namespace aurora::gx {
void clear_display_copy_cache() noexcept;
} // namespace aurora::gx

namespace aurora::gfx {
void clear_caches() noexcept;
} // namespace aurora::gfx

namespace aurora::gpu {
static Module Log("aurora::gpu");

GraphicsConfig g_graphicsConfig;
RenderTarget g_frameBuffer;
RenderTarget g_depthBuffer;

static bool g_presentSourceOverrideActive = false;
static PresentSource g_presentSourceOverride;

namespace {
struct RenderTargetSize {
  uint32_t width;
  uint32_t height;
};

RenderTargetSize clamp_frame_buffer_size(uint32_t width, uint32_t height) noexcept {
  const uint32_t maxDimension = g_graphicsConfig.maxTextureDimension2D;

  if (maxDimension != 0 && (width > maxDimension || height > maxDimension)) {
    if (width > maxDimension) {
      height = std::max(1u, static_cast<uint32_t>((static_cast<uint64_t>(height) * maxDimension) / width));
      width = maxDimension;
    }

    if (height > maxDimension) {
      width = std::max(1u, static_cast<uint32_t>((static_cast<uint64_t>(width) * maxDimension) / height));
      height = maxDimension;
    }
  }

  const auto budgeted = render_size_limits::fit_framebuffer_to_budget(width, height, maxDimension);
  return {budgeted.width, budgeted.height};
}

} // namespace

bool initialize(SDL_Window* window) {
  // The guest paces itself; presentation never enables VSync.
  if (!gl::initialize(window)) {
    return false;
  }

  const auto& caps = gl::caps();
  const auto size = window::get_window_size();
  g_graphicsConfig = GraphicsConfig{
      .width = size.native_fb_width,
      .height = size.native_fb_height,
      .msaaSamples = (g_config.msaa == 2 || g_config.msaa == 4) && caps.maxRenderTargetSamples >= g_config.msaa ? uint32_t(g_config.msaa) : 1u,
      .textureAnisotropy = static_cast<uint16_t>(std::clamp<int32_t>(g_config.maxTextureAnisotropy, 1, std::max(caps.maxAnisotropy, 1))),
      .maxTextureDimension2D = static_cast<uint32_t>(std::max(caps.maxTextureSize, 1)),
  };

  if (g_config.msaa > 1 && g_graphicsConfig.msaaSamples == 1) {
    Log.info("MSAA {}x is not supported by this renderer; rendering single-sampled", g_config.msaa);
  }

  resize_swapchain(size.fb_width, size.fb_height, size.native_fb_width, size.native_fb_height, true);
  return true;
}

void shutdown() {
  clear_present_source_override();
  g_frameBuffer = {};
  g_depthBuffer = {};
  gl::shutdown();
}

const RenderTarget& present_source() noexcept {
  return g_frameBuffer;
}

PresentSource current_present_source() noexcept {
  if (g_presentSourceOverrideActive && g_presentSourceOverride.texture) {
    return g_presentSourceOverride;
  }

  return {
      .texture = g_frameBuffer.texture,
      .width = g_frameBuffer.width,
      .height = g_frameBuffer.height,
  };
}

void set_present_source_override(gl::TexturePtr texture, uint32_t width, uint32_t height) noexcept {
  g_presentSourceOverride = {
      .texture = std::move(texture),
      .width = width,
      .height = height,
  };

  g_presentSourceOverrideActive = true;
}

void clear_present_source_override() noexcept {
  g_presentSourceOverrideActive = false;
  g_presentSourceOverride = {};
}

Viewport calculate_present_viewport_for_aspect(uint32_t surface_width, uint32_t surface_height, float content_aspect) noexcept {
  if (surface_width == 0 || surface_height == 0 || !(content_aspect > 0.f)) {
    return {};
  }

  uint32_t viewport_width = surface_width;
  uint32_t viewport_height =
      std::min<uint32_t>(surface_height, std::max<uint32_t>(1u, static_cast<uint32_t>(std::lround(static_cast<double>(viewport_width) * static_cast<double>(1.f / content_aspect)))));
  if (viewport_height == surface_height) {
    viewport_width = std::min<uint32_t>(surface_width, std::max<uint32_t>(1u, static_cast<uint32_t>(std::lround(static_cast<double>(viewport_height) * static_cast<double>(content_aspect)))));
  }

  return {
      .left = static_cast<float>((surface_width - viewport_width) / 2),
      .top = static_cast<float>((surface_height - viewport_height) / 2),
      .width = static_cast<float>(viewport_width),
      .height = static_cast<float>(viewport_height),
      .znear = 0.f,
      .zfar = 1.f,
  };
}

Viewport calculate_present_viewport(uint32_t surface_width, uint32_t surface_height, uint32_t content_width, uint32_t content_height) noexcept {
  if (content_width == 0 || content_height == 0) {
    return {};
  }

  return calculate_present_viewport_for_aspect(surface_width, surface_height, static_cast<float>(content_width) / static_cast<float>(content_height));
}

void resize_swapchain(uint32_t width, uint32_t height, uint32_t native_width, uint32_t native_height, bool force) {
  if (!gl::initialized() || width == 0 || height == 0 || native_height == 0 || native_width == 0) {
    return;
  }

  uint32_t render_width = width;
  uint32_t render_height = height;
  const auto efbSize = vi::configured_fb_size();

  if (efbSize.x != 0 && efbSize.y != 0) {
    render_width = std::max(render_width, efbSize.x);
    render_height = std::max(render_height, efbSize.y);
  }

  const auto clamped = clamp_frame_buffer_size(render_width, render_height);

  if (clamped.width != render_width || clamped.height != render_height) {
    Log.warn("Render target {}x{} exceeds the framebuffer budget; clamping to {}x{}", render_width, render_height, clamped.width, clamped.height);
  }

  render_width = clamped.width;
  render_height = clamped.height;

  const bool sizeChanged = g_frameBuffer.width != render_width || g_frameBuffer.height != render_height;
  g_graphicsConfig.width = native_width;
  g_graphicsConfig.height = native_height;

  if (!force && !sizeChanged) {
    return;
  }

  if (!sizeChanged && g_frameBuffer) {
    return;
  }

  gx::clear_display_copy_cache();
  gfx::clear_caches();
  clear_present_source_override();
  g_frameBuffer = {
      .texture = gl::create_render_target(render_width, render_height, gl::Format::RGBA8, g_graphicsConfig.msaaSamples, "EFB color"),
      .width = render_width,
      .height = render_height,
  };

  g_depthBuffer = {
      .texture = gl::create_render_target(render_width, render_height, gl::Format::Depth32F, g_graphicsConfig.msaaSamples, "EFB depth"),
      .width = render_width,
      .height = render_height,
  };

  Log.info("EFB render target {}x{}, {} sample(s) per pixel", render_width, render_height, g_graphicsConfig.msaaSamples);
}
} // namespace aurora::gpu
