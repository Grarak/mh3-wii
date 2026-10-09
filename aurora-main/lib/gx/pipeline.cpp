#include "pipeline.hpp"

#include "gx_fmt.hpp"
#include "shader_info.hpp"
#include "tracy/Tracy.hpp"

#include <memory>
#include <mutex>

#include <absl/container/flat_hash_map.h>

namespace aurora::gx {
static Module Log("aurora::gx");

namespace {
struct ShaderConfigHash {
  size_t operator()(const ShaderConfig& config) const noexcept { return static_cast<size_t>(xxh3_hash(config)); }
};

std::mutex sProgramCacheMutex;
absl::flat_hash_map<ShaderConfig, gl::ProgramPtr, ShaderConfigHash> sProgramCache;

gl::ProgramPtr cached_program(const ShaderConfig& config) {
  {
    std::lock_guard lock{sProgramCacheMutex};
    if (const auto it = sProgramCache.find(config); it != sProgramCache.end()) {
      return it->second;
    }
  }
  auto program = build_shader(config);
  std::lock_guard lock{sProgramCacheMutex};
  return sProgramCache.try_emplace(config, std::move(program)).first->second;
}
} // namespace

gfx::PipelinePtr create_pipeline(const PipelineConfig& config) {
  ZoneScoped;
  return build_pipeline(config, cached_program(config.shaderConfig));
}

void clear_shader_module_cache() {
  std::lock_guard lock{sProgramCacheMutex};
  sProgramCache.clear();
}

void render(const DrawData& data, DrawEncodeState& state, bool requireReadyPipeline,
            const gfx::Range* uniformRangeOverride) {
  if (!gfx::bind_pipeline(data.pipeline, state.currentPipeline, requireReadyPipeline)) {
    return;
  }

  // An interpolated presentation slot re-executes the identical draw with only this range replaced.
  const gfx::Range& uniformRange = uniformRangeOverride != nullptr ? *uniformRangeOverride : data.uniformRange;
  gl::bind_uniform_range(0, gfx::uniform_buffer(), uniformRange.offset, MaxUniformSize);
  // Resolved when the draw was recorded; see GXBindGroups.
  const auto* group = data.bindGroups.resolvedTextureBindGroup;
  if (group != nullptr && group != state.boundTextureBindGroup) {
    if (!group->samplersResolved) {
      for (u32 i = 0; i < gfx::MaxBoundTextures; ++i) {
        group->glSamplers[i] = gl::sampler(group->textures[i].sampler);
      }
      group->samplersResolved = true;
    }
    for (u32 i = 0; i < gfx::MaxBoundTextures; ++i) {
      const auto& binding = group->textures[i];
      if (binding.ref) {
        gl::bind_texture(i, binding.ref->texture->gl(), group->glSamplers[i]);
      }
    }
    state.boundTextureBindGroup = group;
  }
  if (data.dstAlpha != UINT32_MAX) {
    gl::set_blend_color(0.f, 0.f, 0.f, static_cast<float>(data.dstAlpha) / 255.f);
  }
  gl::draw_indexed(data.indexCount, data.idxRange.offset, data.instanceCount);
  if (gfx::port_capture::enabled()) {
    const auto* pipeline = gfx::find_pipeline_object(data.pipeline);
    const auto& program = *pipeline->program;
    std::array<const void*, 8> textures{};
    if (group) {
      for (u32 i = 0; i < textures.size(); ++i)
        if (group->textures[i].ref) textures[i] = group->textures[i].ref->texture.get();
    }
    gfx::port_capture::draw(data.capture, program.vertex_hash(), program.fragment_hash(),
                           data.vtxCount, data.indexCount, data.instanceCount,
                           program.uses_discard(), program.replaces_depth(), textures);
  }
}
} // namespace aurora::gx
