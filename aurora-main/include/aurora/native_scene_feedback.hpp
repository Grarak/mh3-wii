#pragma once
#include <aurora/native_texture.hpp>
#include <array>
#include <cstdint>
#include <vector>

namespace aurora {
struct SceneCaptureParameters {
  std::array<float,4> source{}; // logical top-left x/y/width/height
  uint32_t width=0,height=0,format=6;
  bool box_filter=false;
};
struct SceneGlareLevel {uint16_t source_width,source_height,width,height;};
struct SceneGlareParameters {
  std::vector<SceneGlareLevel> levels;
  uint32_t width=0,height=0;
  float low_threshold=0,high_threshold=1,darken=0,brighten=0,shift_scale=1;
};
// Graphics owner only. Each successful publication creates a fresh GPU target;
// identities name guest texture slots, never writable guest pixel arrays.
bool publish_scene_capture(const void* identity,const SceneCaptureParameters& parameters);
bool publish_scene_glare(const void* identity,const SceneGlareParameters& parameters);
bool bind_scene_feedback_texture(uint32_t unit,const void* identity,uint32_t width,uint32_t height,
                                  const NativeTextureSampler& sampling);
bool clone_scene_feedback_texture(const void* destination,const void* source);
uint64_t scene_feedback_texture_version(const void* identity,uint32_t width,uint32_t height);
void clear_scene_feedback_textures();
}
