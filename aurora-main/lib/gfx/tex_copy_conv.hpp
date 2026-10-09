#pragma once

#include "common.hpp"

#include <dolphin/gx/GXEnum.h>

namespace aurora::gfx::tex_copy_conv {

enum class SampleFilter : uint8_t {
  Nearest,
  Linear,
};

struct ConvRequest {
  GXTexFmt fmt;
  gl::TexturePtr src; // Resolved EFB / offscreen color or depth texture
  Range uniformRange; // UV transform uniform (offset + scale)
  TextureHandle dst;  // Destination texture
  SampleFilter sampleFilter = SampleFilter::Nearest;
  bool forceOpaqueAlpha = false;
};

bool needs_conversion(GXTexFmt fmt);

void initialize();
void shutdown();

// Render thread. `uniforms` is the batch uniform buffer holding req.uniformRange.
void run(const ConvRequest& req);
void blit(const ConvRequest& req);

} // namespace aurora::gfx::tex_copy_conv
