#include "clear.hpp"

#include "../webgpu/gpu.hpp"
#include "tracy/Tracy.hpp"

#include <algorithm>
#include <array>

namespace {
wgpu::ColorWriteMask clear_write_mask(bool clearColor, bool clearAlpha) {
  auto writeMask = wgpu::ColorWriteMask::None;
  if (clearColor) {
    writeMask |= wgpu::ColorWriteMask::Red | wgpu::ColorWriteMask::Green | wgpu::ColorWriteMask::Blue;
  }
  if (clearAlpha) {
    writeMask |= wgpu::ColorWriteMask::Alpha;
  }
  return writeMask;
}
} // namespace

namespace aurora::gfx::clear {

using webgpu::g_device;
using webgpu::g_graphicsConfig;

wgpu::RenderPipeline create_pipeline(const PipelineConfig& config) {
  ZoneScoped;
  wgpu::ShaderSourceWGSL sourceDescriptor{};
  sourceDescriptor.code = R"""(
struct VertexOutput {
    @builtin(position) pos: vec4<f32>,
};

struct ClearUniform {
    depth: f32,
};

struct FragmentOutput {
    @location(0) color: vec4<f32>,
    @builtin(frag_depth) depth: f32,
};

@group(1) @binding(0) var<uniform> clear: ClearUniform;

var<private> pos: array<vec2<f32>, 3> = array<vec2<f32>, 3>(
    vec2(-1.0, 1.0),
    vec2(-1.0, -3.0),
    vec2(3.0, 1.0),
);

@vertex
fn vs_main(@builtin(vertex_index) vtxIdx: u32) -> VertexOutput {
    var out: VertexOutput;
    out.pos = vec4<f32>(pos[vtxIdx], 0.0, 1.0);
    return out;
}

@fragment
fn fs_main() -> FragmentOutput {
    var out: FragmentOutput;
    out.color = vec4<f32>(1.0);
    out.depth = clear.depth;
    return out;
}
)""";
  const wgpu::ShaderModuleDescriptor moduleDescriptor{
      .nextInChain = &sourceDescriptor,
      .label = "EFB Clear Module",
  };
  auto module = g_device.CreateShaderModule(&moduleDescriptor);
  const std::array layouts{
      g_staticBindGroupLayout,
      g_uniformBindGroupLayout,
  };
  const wgpu::PipelineLayoutDescriptor layoutDescriptor{
      .label = "EFB Clear Pipeline Layout",
      .bindGroupLayoutCount = layouts.size(),
      .bindGroupLayouts = layouts.data(),
  };
  auto pipelineLayout = g_device.CreatePipelineLayout(&layoutDescriptor);
  constexpr wgpu::BlendState blendState{
      .color =
          wgpu::BlendComponent{
              .operation = wgpu::BlendOperation::Add,
              .srcFactor = wgpu::BlendFactor::Constant,
              .dstFactor = wgpu::BlendFactor::Zero,
          },
      .alpha =
          wgpu::BlendComponent{
              .operation = wgpu::BlendOperation::Add,
              .srcFactor = wgpu::BlendFactor::Constant,
              .dstFactor = wgpu::BlendFactor::Zero,
          },
  };
  const wgpu::ColorTargetState colorTarget{
      .format = g_graphicsConfig.surfaceConfiguration.format,
      .blend = &blendState,
      .writeMask = clear_write_mask(config.clearColor, config.clearAlpha),
  };
  const wgpu::FragmentState fragmentState{
      .module = module,
      .entryPoint = "fs_main",
      .targetCount = 1,
      .targets = &colorTarget,
  };
  const wgpu::DepthStencilState depthStencil{
      .format = g_graphicsConfig.depthFormat,
      .depthWriteEnabled = config.clearDepth,
      .depthCompare = wgpu::CompareFunction::Always,
  };
  const auto label = fmt::format("EFB Clear Pipeline (color {}, alpha {}, depth {})", config.clearColor,
                                 config.clearAlpha, config.clearDepth);
  const wgpu::RenderPipelineDescriptor pipelineDescriptor{
      .label = label.c_str(),
      .layout = pipelineLayout,
      .vertex =
          wgpu::VertexState{
              .module = module,
              .entryPoint = "vs_main",
          },
      .primitive =
          wgpu::PrimitiveState{
              .topology = wgpu::PrimitiveTopology::TriangleList,
          },
      .depthStencil = &depthStencil,
      .multisample =
          wgpu::MultisampleState{
              .count = config.msaaSamples,
          },
      .fragment = &fragmentState,
  };
  return g_device.CreateRenderPipeline(&pipelineDescriptor);
}

void render(const DrawData& data, const wgpu::RenderPassEncoder& pass, const wgpu::Extent3D& targetSize,
            PipelineRef& currentPipeline) {
  if (!bind_pipeline(data.pipeline, pass, currentPipeline)) {
    return;
  }

  const std::array offsets{data.uniformRange.offset};
  pass.SetBindGroup(1, g_uniformBindGroup, offsets.size(), offsets.data());
  pass.SetBlendConstant(&data.color);
  pass.SetViewport(0.f, 0.f, static_cast<float>(targetSize.width), static_cast<float>(targetSize.height), 0.f, 1.f);
  if (data.useScissor) {
    const auto x = static_cast<uint32_t>(std::clamp<int32_t>(data.scissor.x, 0, static_cast<int32_t>(targetSize.width)));
    const auto y = static_cast<uint32_t>(std::clamp<int32_t>(data.scissor.y, 0, static_cast<int32_t>(targetSize.height)));
    const auto right = static_cast<uint32_t>(std::clamp<int32_t>(
        data.scissor.x + data.scissor.width, 0, static_cast<int32_t>(targetSize.width)));
    const auto bottom = static_cast<uint32_t>(std::clamp<int32_t>(
        data.scissor.y + data.scissor.height, 0, static_cast<int32_t>(targetSize.height)));
    if (right <= x || bottom <= y) {
      return;
    }
    pass.SetScissorRect(x, y, right - x, bottom - y);
  } else {
    pass.SetScissorRect(0, 0, targetSize.width, targetSize.height);
  }
  pass.Draw(3);
}
} // namespace aurora::gfx::clear
