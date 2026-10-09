#include "clear.hpp"

#include <algorithm>
#include <array>

namespace aurora::gfx::clear {
void render(const DrawData& data, uint32_t targetWidth, uint32_t targetHeight, const ClipRect& passScissor) {
  const auto width = static_cast<int32_t>(targetWidth);
  const auto height = static_cast<int32_t>(targetHeight);

  // Without an explicit rectangle the clear behaves like a fullscreen draw: bounded by the pass scissor.
  const ClipRect& rect = data.useScissor ? data.scissor : passScissor;
  const int32_t x = std::clamp(rect.x, 0, width);
  const int32_t y = std::clamp(rect.y, 0, height);
  const int32_t right = std::clamp(rect.x + rect.width, 0, width);
  const int32_t bottom = std::clamp(rect.y + rect.height, 0, height);

  if (right <= x || bottom <= y) {
    return;
  }

  gl::set_scissor_enabled(true);
  gl::set_scissor(x, y, right - x, bottom - y);

  if (data.clearColor || data.clearAlpha) {
    gl::set_color_mask(data.clearColor, data.clearColor, data.clearColor, data.clearAlpha);
    const std::array color{data.color.x(), data.color.y(), data.color.z(), data.color.w()};
    gl::clear_color(color.data());
  }

  if (data.clearDepth) {
    gl::set_depth_write(true);
    const float depth = std::clamp(data.depth, 0.f, 1.f);
    gl::clear_depth(depth);
  }

  gl::set_scissor(passScissor.x, passScissor.y, passScissor.width, passScissor.height);
}
} // namespace aurora::gfx::clear
