#pragma once
#include <aurora/native_texture.hpp>
#include <array>
#include <cstdint>
#include <vector>

namespace aurora {
struct SceneCaptureParameters {
  std::array<float, 4> source{}; // logical top-left x/y/width/height
  uint32_t width = 0, height = 0, format = 6;
  bool box_filter = false;
};

struct SceneGlareLevel {
  uint16_t source_width, source_height, width, height;
};

struct SceneGlareParameters {
  std::vector<SceneGlareLevel> levels;
  uint32_t width = 0, height = 0;
  float low_threshold = 0, high_threshold = 1, darken = 0, brighten = 0, shift_scale = 1;
};

struct SceneFeedbackTargetStats {
  uint32_t width = 0, height = 0;
  uint64_t allocations = 0, reuses = 0, pool_bytes = 0;
  uint32_t pool_targets = 0;
};

struct SceneFeedbackStats {
  static constexpr uint64_t byte_limit = 8u << 20;
  static constexpr uint32_t target_limit = 32;
  uint64_t allocations = 0, allocation_bytes = 0, reuses = 0, evictions = 0, capacity_cancellations = 0;
  uint64_t capture_calls = 0, glare_calls = 0, clone_calls = 0, pool_bytes = 0, peak_pool_bytes = 0;
  uint32_t pool_targets = 0, peak_pool_targets = 0, retained_targets = 0, available_targets = 0, size_count = 0;
  uint64_t untracked_sizes = 0, last_call_bytes = 0;
  uint32_t last_call_targets = 0, last_call_allocations = 0, last_call_reuses = 0;
  std::array<SceneFeedbackTargetStats, target_limit> sizes{};
};

// Graphics owner only. Published backing is immutable while identities,
// clones or bindings retain it. Idle unowned targets may be reused. Capacity
// rejection cancels presentation without publishing the failed operation;
// earlier successful operations retain their revisions and submitted pixels.
bool publish_scene_capture(const void* identity, const SceneCaptureParameters& parameters);
bool publish_scene_glare(const void* identity, const SceneGlareParameters& parameters);
bool bind_scene_feedback_texture(uint32_t unit, const void* identity, uint32_t width, uint32_t height, const NativeTextureSampler& sampling);
bool clone_scene_feedback_texture(const void* destination, const void* source);
uint64_t scene_feedback_texture_version(const void* identity, uint32_t width, uint32_t height);
void clear_scene_feedback_textures();
SceneFeedbackStats scene_feedback_stats();
} // namespace aurora
