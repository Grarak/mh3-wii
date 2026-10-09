#pragma once

#include "../gfx/common.hpp"
#include "../gfx/pipeline_cache.hpp"
#include "shader_info.hpp"
#include "../gfx/port_capture.hpp"

namespace aurora::gx {
struct DrawData {
  gfx::PipelineRef pipeline;
  gfx::Range vertRange;
  gfx::Range idxRange;
  gfx::Range uniformRange;
  std::array<gfx::Range, MaxInterpolatedFrames> interpolatedUniformRanges;
  uint32_t vtxCount;
  uint32_t indexCount;
  uint32_t instanceCount;
  bool expandedPrimitive;
  GXBindGroups bindGroups;
  uint32_t dstAlpha;
  gfx::port_capture::DrawState capture;
};

constexpr uint32_t GXPipelineConfigVersion = 19;

constexpr GXFogType effective_pipeline_fog_type(GXFogType fogType, GXZTexOp zTextureOp,
                                                bool zCompLocBeforeTex, GXBlendMode blendMode,
                                                GXLogicOp logicOp) noexcept {
  const bool usesLateZTexture = zTextureOp != GX_ZT_DISABLE && !zCompLocBeforeTex;
  const bool usesUnsupportedLogicFog =
      blendMode == GX_BM_LOGIC && logicOp != GX_LO_OR;
  return usesLateZTexture && usesUnsupportedLogicFog ? GX_FOG_NONE : fogType;
}

struct PipelineConfig {
  uint32_t version = GXPipelineConfigVersion;
  uint32_t msaaSamples = 1;
  ShaderConfig shaderConfig;
  GXCompare depthFunc;
  GXCullMode cullMode;
  GXBlendMode blendMode;
  GXBlendFactor blendFacSrc, blendFacDst;
  GXLogicOp blendOp;
  GXPixelFmt pixelFmt;
  uint32_t dstAlpha;
  bool depthCompare, depthUpdate, alphaUpdate, colorUpdate;
};

static_assert(std::has_unique_object_representations_v<PipelineConfig>);

inline bool valid_pipeline_config(const PipelineConfig& config) noexcept {
  const auto in_range = [](auto value, auto maximum) {
    using Value = decltype(value);
    return static_cast<std::underlying_type_t<Value>>(value) >= 0 &&
           static_cast<std::underlying_type_t<Value>>(value) <=
               static_cast<std::underlying_type_t<Value>>(maximum);
  };

  const bool validSamples = config.msaaSamples == 1 || config.msaaSamples == 2 ||
                            config.msaaSamples == 4 || config.msaaSamples == 8;
  return config.version == GXPipelineConfigVersion && validSamples &&
         in_range(config.depthFunc, GX_ALWAYS) && in_range(config.cullMode, GX_CULL_ALL) &&
         in_range(config.blendMode, GX_BM_SUBTRACT) &&
         in_range(config.blendFacSrc, GX_BL_INVDSTALPHA) &&
         in_range(config.blendFacDst, GX_BL_INVDSTALPHA) &&
         in_range(config.blendOp, GX_LO_SET) && in_range(config.pixelFmt, GX_PF_YUV420);
}

gfx::PipelinePtr create_pipeline(const PipelineConfig& config);
void clear_shader_module_cache();

// Per-pass state carried across the draws of one render pass, so the replay loop can skip
// re-binding what is already bound.
struct DrawEncodeState {
  gfx::PipelineRef currentPipeline = UINTPTR_MAX;
  const gfx::TextureBindGroup* boundTextureBindGroup = nullptr;
};

// Render thread.
void render(const DrawData& data, DrawEncodeState& state, bool requireReadyPipeline,
            const gfx::Range* uniformRangeOverride = nullptr);

void queue_surface(const u8* dlStart, uint32_t dlSize, bool bigEndian) noexcept;
} // namespace aurora::gx
