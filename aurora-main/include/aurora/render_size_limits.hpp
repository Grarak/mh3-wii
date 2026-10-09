#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace aurora::render_size_limits {

struct Size {
  uint32_t width;
  uint32_t height;
};

// The canvas follows the window aspect however wide it gets, since the presenter stretches it and
// a cap would distort the UI. Only the portrait side clamps, where the guest cannot fill taller.
inline constexpr float kMaxDynamicPortraitExpansion = 6.f;
inline constexpr float kMinDynamicAspect = (16.f / 9.f) / kMaxDynamicPortraitExpansion;

// The adapter limit is an API ceiling, not an allocation budget: a 16384-wide RGBA attachment is
// near 100 MiB. 8192 plus an 8K-UHD pixel budget keeps 8x 16:9 while avoiding device removal.
inline constexpr uint32_t kMaxFramebufferDimension = 8192;
inline constexpr uint64_t kMaxFramebufferPixels = 7680ull * 4320ull;

inline float clamp_dynamic_aspect(float aspect) noexcept {
  if (!std::isfinite(aspect) || aspect <= 0.f) {
    return 1.f;
  }
  return std::max(aspect, kMinDynamicAspect);
}

inline Size scale_framebuffer_to_aspect(uint32_t width, uint32_t height, float scale, float requestedAspect) noexcept {
  if (width == 0 || height == 0 || !std::isfinite(scale) || scale <= 0.f) {
    return {std::max(width, 1u), std::max(height, 1u)};
  }

  const auto scaledWidth = std::max(1u, static_cast<uint32_t>(std::lround(static_cast<double>(width) * scale)));
  const auto scaledHeight = std::max(1u, static_cast<uint32_t>(std::lround(static_cast<double>(height) * scale)));
  const float aspect = clamp_dynamic_aspect(requestedAspect);
  if (aspect >= static_cast<float>(width) / static_cast<float>(height)) {
    return {
        std::max(1u, static_cast<uint32_t>(std::lround(static_cast<double>(scaledHeight) * aspect))),
        scaledHeight,
    };
  }
  return {
      scaledWidth,
      std::max(1u, static_cast<uint32_t>(std::lround(static_cast<double>(scaledWidth) / aspect))),
  };
}

inline Size fit_framebuffer_to_budget(uint32_t width, uint32_t height, uint32_t adapterMaxDimension) noexcept {
  if (width == 0 || height == 0) {
    return {width, height};
  }

  uint32_t dimensionLimit = kMaxFramebufferDimension;
  if (adapterMaxDimension != 0 && adapterMaxDimension != std::numeric_limits<uint32_t>::max()) {
    dimensionLimit = std::min(dimensionLimit, adapterMaxDimension);
  }
  dimensionLimit = std::max(dimensionLimit, 1u);

  double fitScale = 1.0;
  fitScale = std::min(fitScale, static_cast<double>(dimensionLimit) / static_cast<double>(width));
  fitScale = std::min(fitScale, static_cast<double>(dimensionLimit) / static_cast<double>(height));

  const double requestedPixels = static_cast<double>(width) * static_cast<double>(height);
  if (requestedPixels > static_cast<double>(kMaxFramebufferPixels)) {
    fitScale = std::min(fitScale, std::sqrt(static_cast<double>(kMaxFramebufferPixels) / requestedPixels));
  }

  if (fitScale >= 1.0) {
    return {width, height};
  }
  return {
      std::max(1u, static_cast<uint32_t>(std::floor(static_cast<double>(width) * fitScale))),
      std::max(1u, static_cast<uint32_t>(std::floor(static_cast<double>(height) * fitScale))),
  };
}

} // namespace aurora::render_size_limits
