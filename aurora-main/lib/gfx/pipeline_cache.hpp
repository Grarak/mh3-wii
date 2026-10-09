#pragma once

#include "common.hpp"

#include <functional>
#include <memory>

namespace aurora::gx {
struct PipelineConfig;
} // namespace aurora::gx

namespace aurora::gfx {

// A GX pipeline: its program (shared between pipelines with the same shader
// config) and the fixed-function state applied with it.
struct Pipeline {
  gl::ProgramPtr program;
  gl::RasterState raster;
};

using PipelinePtr = std::shared_ptr<Pipeline>;
using NewPipelineCallback = std::function<PipelinePtr()>;

void initialize_pipeline_cache();
void shutdown_pipeline_cache();
void begin_pipeline_frame();
void end_pipeline_frame();
void set_skip_unready_pipelines(bool enabled) noexcept;
bool skip_unready_pipelines() noexcept;
uint32_t queued_pipeline_count() noexcept;

// Producer: the pipeline for `config`, created on first use. Its program
// compiles on the render thread ahead of the draws that use it.
template <typename Config>
PipelineRef find_pipeline(ShaderType type, const Config& config, NewPipelineCallback&& cb);

// Any thread. The pipeline stays alive until shutdown_pipeline_cache().
const Pipeline* find_pipeline_object(PipelineRef ref) noexcept;

} // namespace aurora::gfx
