#pragma once

#include "common.hpp"

namespace aurora::gfx::clear {
// A scissored EFB clear. GL clears honour the scissor and write masks, so no
// pipeline is involved.
struct DrawData {
  Vec4<float> color;
  float depth = 0.f;
  bool clearColor = true;
  bool clearAlpha = true;
  bool clearDepth = true;
  // False: clear within the pass's current scissor.
  bool useScissor = false;
  ClipRect scissor{};
};

// Render thread. `passScissor` is the pass's current scissor, restored afterwards.
void render(const DrawData& data, uint32_t targetWidth, uint32_t targetHeight, const ClipRect& passScissor);
} // namespace aurora::gfx::clear
