#pragma once
#include "device.hpp"
#include "../gl/gl.hpp"

namespace aurora::gxm {
SceGxmBlendInfo blend_info(const gl::RasterState& state, const std::array<float, 4>& blendColor);
void apply_raster(const gl::RasterState& state, SceGxmContext* context);
void apply_viewport(SceGxmContext* context, int32_t x, int32_t y, int32_t width, int32_t height, float minDepth, float maxDepth);

// apply_raster and apply_viewport skip the GXM calls that would repeat the
// state they last applied (most draws repeat it). Every scene begin forgets
// it, so a scene starts from explicit calls whatever GXM keeps across scenes.
void forget_raster_state() noexcept;

// Region clipping rounds to hardware tiles. This mask pass clips at pixels,
// bypasses the depth test, and retains the depth and color contents.
// It changes native vertex/fragment/stencil/cull/viewport state; callers must
// bind their draw programs and apply raster/viewport state afterwards.
class PixelClip {
public:
  explicit PixelClip(Device& device);
  ~PixelClip();
  void apply(uint32_t targetWidth, uint32_t targetHeight, bool enabled, int32_t x, int32_t y, int32_t width, int32_t height);

private:
  void rectangle(uint32_t targetWidth, uint32_t targetHeight, int32_t x, int32_t y, int32_t width, int32_t height, SceGxmStencilFunc func);
  Device& m_device;
  PatchedProgram m_vertex;
  MappedMemory m_quad;
  SceGxmFragmentProgram* m_mask = nullptr;
};
} // namespace aurora::gxm
