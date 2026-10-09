#include "pipeline_cache.hpp"

#include "../gx/pipeline.hpp"
#include "../internal.hpp"

#include <absl/container/flat_hash_map.h>

#include <atomic>
#include <mutex>

namespace aurora::gfx {
namespace {
Module Log("aurora::gfx::pipeline_cache");

std::mutex g_pipelineMutex;
absl::flat_hash_map<PipelineRef, PipelinePtr> g_pipelines;
std::atomic_bool g_skipUnreadyPipelines{false};

template <typename Config>
PipelineRef find_pipeline_impl(ShaderType type, const Config& config, NewPipelineCallback&& cb) {
  const PipelineRef hash = xxh3_hash(config, static_cast<HashType>(type));
  {
    std::lock_guard lock{g_pipelineMutex};
    if (g_pipelines.contains(hash)) {
      return hash;
    }
  }
  auto pipeline = cb();
  if (!pipeline) {
    Log.error("Failed to create pipeline {:x}", hash);
    return hash;
  }
  if (pipeline->program) {
    // Compile ahead of the frame that first draws with it.
    gl::post([program = pipeline->program] { program->get(); });
  }
  std::lock_guard lock{g_pipelineMutex};
  g_pipelines.try_emplace(hash, std::move(pipeline));
  ++g_stats.createdPipelines;
  return hash;
}
} // namespace

template <>
PipelineRef find_pipeline(ShaderType type, const gx::PipelineConfig& config, NewPipelineCallback&& cb) {
  return find_pipeline_impl(type, config, std::move(cb));
}

const Pipeline* find_pipeline_object(PipelineRef ref) noexcept {
  std::lock_guard lock{g_pipelineMutex};
  const auto it = g_pipelines.find(ref);
  return it != g_pipelines.end() ? it->second.get() : nullptr;
}

void initialize_pipeline_cache() {}

void shutdown_pipeline_cache() {
  // Programs delete themselves on the render thread.
  std::lock_guard lock{g_pipelineMutex};
  g_pipelines.clear();
}

void begin_pipeline_frame() {}
void end_pipeline_frame() {}

void set_skip_unready_pipelines(bool enabled) noexcept {
  g_skipUnreadyPipelines.store(enabled, std::memory_order_relaxed);
}

bool skip_unready_pipelines() noexcept { return g_skipUnreadyPipelines.load(std::memory_order_relaxed); }

uint32_t queued_pipeline_count() noexcept { return 0; }

} // namespace aurora::gfx

void aurora_set_skip_unready_pipelines(const bool enabled) { aurora::gfx::set_skip_unready_pipelines(enabled); }

bool aurora_get_skip_unready_pipelines() { return aurora::gfx::skip_unready_pipelines(); }

uint32_t aurora_get_queued_pipeline_count() { return aurora::gfx::queued_pipeline_count(); }
