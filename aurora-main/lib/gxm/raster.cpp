#include "raster.hpp"
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace aurora::gxm {
namespace {
SceGxmBlendFunc blend_equation(GLenum value) {
  switch (value) {
  case GL_FUNC_ADD:
    return SCE_GXM_BLEND_FUNC_ADD;
  case GL_FUNC_REVERSE_SUBTRACT:
    return SCE_GXM_BLEND_FUNC_REVERSE_SUBTRACT;
  case GL_MAX:
    return SCE_GXM_BLEND_FUNC_MAX;
  default:
    throw std::runtime_error("Unsupported native blend equation");
  }
}
SceGxmBlendFactor blend_factor(GLenum value, float constantAlpha) {
  switch (value) {
  case GL_ZERO:
    return SCE_GXM_BLEND_FACTOR_ZERO;
  case GL_ONE:
    return SCE_GXM_BLEND_FACTOR_ONE;
  case GL_SRC_COLOR:
    return SCE_GXM_BLEND_FACTOR_SRC_COLOR;
  case GL_ONE_MINUS_SRC_COLOR:
    return SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
  case GL_DST_COLOR:
    return SCE_GXM_BLEND_FACTOR_DST_COLOR;
  case GL_ONE_MINUS_DST_COLOR:
    return SCE_GXM_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
  case GL_SRC_ALPHA:
    return SCE_GXM_BLEND_FACTOR_SRC_ALPHA;
  case GL_ONE_MINUS_SRC_ALPHA:
    return SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  case GL_DST_ALPHA:
    return SCE_GXM_BLEND_FACTOR_DST_ALPHA;
  case GL_ONE_MINUS_DST_ALPHA:
    return SCE_GXM_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
  case GL_CONSTANT_ALPHA:
    if (constantAlpha == 0.f)
      return SCE_GXM_BLEND_FACTOR_ZERO;
    if (constantAlpha == 1.f)
      return SCE_GXM_BLEND_FACTOR_ONE;
    throw std::runtime_error("Fractional constant alpha requires offline programmable blending");
  default:
    throw std::runtime_error("Unsupported native blend factor");
  }
}
SceGxmDepthFunc depth_function(GLenum value) {
  switch (value) {
  case GL_NEVER:
    return SCE_GXM_DEPTH_FUNC_NEVER;
  case GL_LESS:
    return SCE_GXM_DEPTH_FUNC_LESS;
  case GL_EQUAL:
    return SCE_GXM_DEPTH_FUNC_EQUAL;
  case GL_LEQUAL:
    return SCE_GXM_DEPTH_FUNC_LESS_EQUAL;
  case GL_GREATER:
    return SCE_GXM_DEPTH_FUNC_GREATER;
  case GL_NOTEQUAL:
    return SCE_GXM_DEPTH_FUNC_NOT_EQUAL;
  case GL_GEQUAL:
    return SCE_GXM_DEPTH_FUNC_GREATER_EQUAL;
  case GL_ALWAYS:
    return SCE_GXM_DEPTH_FUNC_ALWAYS;
  default:
    throw std::runtime_error("Unsupported native depth function");
  }
}
} // namespace
SceGxmBlendInfo blend_info(const gl::RasterState& state, const std::array<float, 4>& blendColor) {
  SceGxmBlendInfo result{};
  constexpr unsigned masks[4]{SCE_GXM_COLOR_MASK_R, SCE_GXM_COLOR_MASK_G, SCE_GXM_COLOR_MASK_B, SCE_GXM_COLOR_MASK_A};
  for (unsigned i = 0; i < 4; ++i)
    if (state.colorMask[i])
      result.colorMask |= masks[i];
  if (state.blend) {
    if (state.colorMask[0] || state.colorMask[1] || state.colorMask[2]) {
      result.colorFunc = blend_equation(state.colorEquation);
      // MIN/MAX ignore blend factors; canonicalizing avoids irrelevant
      // unsupported factors and duplicate patched fragment programs.
      result.colorSrc = state.colorEquation == GL_MAX ? SCE_GXM_BLEND_FACTOR_ONE : blend_factor(state.srcColor, blendColor[3]);
      result.colorDst = state.colorEquation == GL_MAX ? SCE_GXM_BLEND_FACTOR_ONE : blend_factor(state.dstColor, blendColor[3]);
    }
    if (state.colorMask[3]) {
      result.alphaFunc = blend_equation(state.alphaEquation);
      result.alphaSrc = state.alphaEquation == GL_MAX ? SCE_GXM_BLEND_FACTOR_ONE : blend_factor(state.srcAlpha, blendColor[3]);
      result.alphaDst = state.alphaEquation == GL_MAX ? SCE_GXM_BLEND_FACTOR_ONE : blend_factor(state.dstAlpha, blendColor[3]);
    }
  }
  return result;
}
void apply_raster(const gl::RasterState& state, SceGxmContext* context) {
  // The GLES prelude flips Y and uses CCW fronts; native programs retain Y
  // and use GXM's CW fronts. Both expose the same guest-facing cull state.
  if (state.cull && state.cullFace != GL_BACK && state.cullFace != GL_FRONT)
    throw std::runtime_error("Unsupported native cull face");
  sceGxmSetCullMode(context, !state.cull ? SCE_GXM_CULL_NONE : state.cullFace == GL_BACK ? SCE_GXM_CULL_CCW : SCE_GXM_CULL_CW);
  sceGxmSetTwoSidedEnable(context, SCE_GXM_TWO_SIDED_DISABLED);
  sceGxmSetFrontDepthFunc(context, state.depthTest ? depth_function(state.depthFunc) : SCE_GXM_DEPTH_FUNC_ALWAYS);
  sceGxmSetFrontDepthWriteEnable(context, state.depthTest && state.depthWrite ? SCE_GXM_DEPTH_WRITE_ENABLED : SCE_GXM_DEPTH_WRITE_DISABLED);
  sceGxmSetFrontFragmentProgramEnable(context, SCE_GXM_FRAGMENT_PROGRAM_ENABLED);
  sceGxmSetBackFragmentProgramEnable(context, SCE_GXM_FRAGMENT_PROGRAM_ENABLED);
  sceGxmSetFrontStencilFunc(context, SCE_GXM_STENCIL_FUNC_ALWAYS, SCE_GXM_STENCIL_OP_KEEP, SCE_GXM_STENCIL_OP_KEEP, SCE_GXM_STENCIL_OP_KEEP, 0xff, 0xff);
  sceGxmSetBackStencilFunc(context, SCE_GXM_STENCIL_FUNC_ALWAYS, SCE_GXM_STENCIL_OP_KEEP, SCE_GXM_STENCIL_OP_KEEP, SCE_GXM_STENCIL_OP_KEEP, 0xff, 0xff);
}
void apply_viewport(SceGxmContext* context, int32_t x, int32_t y, int32_t width, int32_t height, float minDepth, float maxDepth) {
  if (width < 0 || height < 0)
    throw std::runtime_error("Negative native viewport dimensions");
  sceGxmSetViewport(context, x + width * 0.5f, width * 0.5f, y + height * 0.5f, -height * 0.5f, (minDepth + maxDepth) * 0.5f, (maxDepth - minDepth) * 0.5f);
}
PixelClip::PixelClip(Device& device) : m_device(device), m_vertex(device, 1, 1), m_quad(4096, MappedMemory::Kind::Draw, "Aurora pixel clip") {
  const float ids[4]{0, 1, 2, 3};
  const uint16_t indices[6]{0, 1, 2, 2, 1, 3};
  std::memcpy(m_quad.data(), ids, sizeof(ids));
  std::memcpy(static_cast<char*>(m_quad.data()) + 64, indices, sizeof(indices));
  check(sceGxmShaderPatcherCreateMaskUpdateFragmentProgram(device.patcher(), &m_mask), "create pixel clip mask program");
}
PixelClip::~PixelClip() {
  m_device.finish();
  if (m_mask)
    sceGxmShaderPatcherReleaseFragmentProgram(m_device.patcher(), m_mask);
}
void PixelClip::rectangle(uint32_t targetWidth, uint32_t targetHeight, int32_t x, int32_t y, int32_t width, int32_t height, SceGxmStencilFunc func) {
  if (width <= 0 || height <= 0)
    return;
  // Each draw needs a fresh default-uniform reservation: changing the
  // previous reservation would also change the earlier rectangle's region.
  SceGxmBlendInfo unused{};
  unused.colorMask = SCE_GXM_COLOR_MASK_ALL;
  m_vertex.bind(unused);
  sceGxmSetFragmentProgram(m_device.context(), m_mask);
  const float region[4]{-1.f + 2.f * x / targetWidth, 1.f - 2.f * y / targetHeight, 2.f * width / targetWidth, -2.f * height / targetHeight};
  const float depth = 0.f;
  m_vertex.set_vertex_uniform("region", region, 4);
  m_vertex.set_vertex_uniform("depth", &depth, 1);
  auto* context = m_device.context();
  sceGxmSetFrontStencilFunc(context, func, SCE_GXM_STENCIL_OP_KEEP, SCE_GXM_STENCIL_OP_KEEP, SCE_GXM_STENCIL_OP_KEEP, 0xff, 0xff);
  sceGxmSetBackStencilFunc(context, func, SCE_GXM_STENCIL_OP_KEEP, SCE_GXM_STENCIL_OP_KEEP, SCE_GXM_STENCIL_OP_KEEP, 0xff, 0xff);
  check(sceGxmDraw(context, SCE_GXM_PRIMITIVE_TRIANGLES, SCE_GXM_INDEX_FORMAT_U16, static_cast<char*>(m_quad.data()) + 64, 6), "draw exact pixel clip mask");
}
void PixelClip::apply(uint32_t targetWidth, uint32_t targetHeight, bool enabled, int32_t x, int32_t y, int32_t width, int32_t height) {
  if (!targetWidth || !targetHeight)
    throw std::runtime_error("Empty native pixel clip target");
  auto* context = m_device.context();
  sceGxmSetCullMode(context, SCE_GXM_CULL_NONE);
  sceGxmSetTwoSidedEnable(context, SCE_GXM_TWO_SIDED_DISABLED);
  sceGxmSetFrontFragmentProgramEnable(context, SCE_GXM_FRAGMENT_PROGRAM_ENABLED);
  sceGxmSetBackFragmentProgramEnable(context, SCE_GXM_FRAGMENT_PROGRAM_ENABLED);
  sceGxmSetRegionClip(context, SCE_GXM_REGION_CLIP_NONE, 0, 0, 0, 0);
  apply_viewport(context, 0, 0, targetWidth, targetHeight, 0.f, 1.f);
  check(sceGxmSetVertexStream(context, 0, m_quad.data()), "bind pixel clip vertex IDs");
  check(sceGxmSetVertexStream(context, 1, m_quad.data()), "bind pixel clip instance IDs");
  if (!enabled) {
    rectangle(targetWidth, targetHeight, 0, 0, targetWidth, targetHeight, SCE_GXM_STENCIL_FUNC_ALWAYS);
    return;
  }
  rectangle(targetWidth, targetHeight, 0, 0, targetWidth, targetHeight, SCE_GXM_STENCIL_FUNC_NEVER);
  const int32_t left = std::clamp(x, 0, int32_t(targetWidth)), top = std::clamp(y, 0, int32_t(targetHeight));
  const int32_t right = std::clamp(int64_t(x) + std::max(width, 0), int64_t(0), int64_t(targetWidth));
  const int32_t bottom = std::clamp(int64_t(y) + std::max(height, 0), int64_t(0), int64_t(targetHeight));
  rectangle(targetWidth, targetHeight, left, top, right - left, bottom - top, SCE_GXM_STENCIL_FUNC_ALWAYS);
}
} // namespace aurora::gxm
