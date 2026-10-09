#include <aurora/native_scene_feedback.hpp>
#include <aurora/aurora.h>
#include "../gl/device.hpp"
#include "../gl/gl.hpp"
#include "../dolphin/vi/vi_internal.hpp"
#include "native_texture_layout.hpp"
#include "native_scene_feedback_shaders.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>

namespace aurora {
namespace {
struct Image {
  gl::TexturePtr texture;
  uint64_t revision;
};

std::map<const void*, Image> images;

// Keep published textures alive while a native unit names their revision.
// Replacing a producer identity must not invalidate the previous binding.
std::array<gl::TexturePtr, 16> sampledImages;
uint64_t revision = 0;
std::array<gl::ProgramPtr, gfx::scene_feedback::shaders::CopyModes> copyPrograms;
gl::ProgramPtr brightProgram;
struct PoolTarget {
  gl::TexturePtr texture;
  uint64_t bytes, acquired;
};

std::vector<PoolTarget> targets;
SceneFeedbackStats stats;
uint64_t acquisition = 0;
void owner() {
  if (!gl::on_render_thread())
    throw std::runtime_error("Scene feedback changed outside graphics owner");
}

void dimensions(uint32_t w, uint32_t h) {
  if (!w || !h || w > uint32_t(gl::caps().maxTextureSize) || h > uint32_t(gl::caps().maxTextureSize))
    throw std::runtime_error("Scene feedback target exceeds renderer dimensions");
}

uint64_t target_bytes(uint32_t w, uint32_t h) {
  // The GXM linear RGBA8 pitch is eight pixels; the CDRAM arena allocates
  // complete 64-byte units. Host accounting keeps this same conservative bound.
  return gfx::texture_align(gfx::native_texture_layout(w, h, 1, gl::Format::RGBA8).total_bytes);
}

SceneFeedbackTargetStats* size_stats(uint32_t w, uint32_t h) {
  for (uint32_t i = 0; i < stats.size_count; i++)
    if (stats.sizes[i].width == w && stats.sizes[i].height == h)
      return &stats.sizes[i];
  if (stats.size_count == stats.sizes.size()) {
    ++stats.untracked_sizes;
    return nullptr;
  }

  auto& size = stats.sizes[stats.size_count++];
  size.width = w;
  size.height = h;
  return &size;
}

bool available(const PoolTarget& target) {
  return target.texture.use_count() == 1 && gl::render_target_reusable(*target.texture);
}

bool saved_binding(const PoolTarget& target, const gl::OffscreenState& saved) {
  const auto name = target.texture->gl();
  return saved.color == name || saved.depth == name || std::find(saved.textures.begin(), saved.textures.end(), name) != saved.textures.end();
}

gl::TexturePtr target(uint32_t w, uint32_t h, const gl::OffscreenState& saved) {
  for (auto& entry : targets)
    if (entry.texture->width == w && entry.texture->height == h && !saved_binding(entry, saved) && available(entry)) {
      entry.acquired = ++acquisition;
      ++stats.reuses;
      ++stats.last_call_reuses;

      if (auto* size = size_stats(w, h))
        ++size->reuses;

      return entry.texture;
    }
  const auto bytes = target_bytes(w, h);

  if (bytes > SceneFeedbackStats::byte_limit)
    return {};
  while (targets.size() >= SceneFeedbackStats::target_limit || stats.pool_bytes + bytes > SceneFeedbackStats::byte_limit || !gl::render_target_allocation_available(size_t(bytes))) {
    auto oldest = targets.end();

    for (auto it = targets.begin(); it != targets.end(); ++it)
      if (!saved_binding(*it, saved) && available(*it) && (oldest == targets.end() || it->acquired < oldest->acquired))
        oldest = it;
    if (oldest == targets.end())
      return {};

    stats.pool_bytes -= oldest->bytes;
    targets.erase(oldest);
    ++stats.evictions;
  }

  if (targets.capacity() < SceneFeedbackStats::target_limit)
    targets.reserve(SceneFeedbackStats::target_limit);

  auto texture = gl::create_texture(w, h, 1, gl::Format::RGBA8, "Native scene feedback");
  targets.push_back({texture, bytes, ++acquisition});
  stats.pool_bytes += bytes;
  ++stats.allocations;
  stats.allocation_bytes += bytes;
  ++stats.last_call_allocations;
  stats.peak_pool_bytes = std::max(stats.peak_pool_bytes, stats.pool_bytes);
  stats.peak_pool_targets = std::max(stats.peak_pool_targets, uint32_t(targets.size()));

  if (auto* size = size_stats(w, h))
    ++size->allocations;

  return texture;
}

void call_targets(uint32_t count, uint64_t bytes) {
  stats.last_call_targets = count;
  stats.last_call_bytes = bytes;
  stats.last_call_allocations = stats.last_call_reuses = 0;
}

bool reject_capacity() {
  ++stats.capacity_cancellations;

  // Seal previously submitted work; retain its successful publications. No
  // target pass of the rejected operation has been submitted or published.
  aurora_cancel_native_frame();
  return false;
}

void programs() {
  namespace shaders = gfx::scene_feedback::shaders;

  for (unsigned mode = 0; mode < shaders::CopyModes; mode++)
    if (!copyPrograms[mode])
      copyPrograms[mode] = std::make_shared<gl::Program>(gl::ProgramDesc{.label = shaders::copy_label(mode),
                                                                         .vertex = gl::vertex_source(shaders::copy_vertex(mode)),
                                                                         .fragment = gl::fragment_source(shaders::copy_fragment(mode)),
                                                                         .samplers = {{"tex0", 0}}});
  if (!brightProgram)
    brightProgram = std::make_shared<gl::Program>(gl::ProgramDesc{.label = std::string(shaders::BrightLabel),
                                                                  .vertex = gl::vertex_source(shaders::Vertex),
                                                                  .fragment = gl::fragment_source(shaders::Bright),
                                                                  .samplers = {{"tex0", 0}}});
}

struct Restore {
  gl::OffscreenState state = gl::save_offscreen_state();
  ~Restore() { gl::restore_offscreen_state(state); }
};

void publish(const void* id, gl::TexturePtr texture) {
  if (!id)
    throw std::runtime_error("Scene feedback has no producer identity");

  if (++revision == 0)
    throw std::runtime_error("Scene feedback revision overflow");

  images.insert_or_assign(id, Image{std::move(texture), revision});
}

void copy_target(const gl::TexturePtr& source, const gl::TexturePtr& destination, const std::array<float, 4>& rect, bool box, bool rgb565) {
  gl::bind_framebuffer(destination->gl(), 0);
  gl::set_viewport(0, 0, destination->width, destination->height, 0, 1);
  gl::set_scissor_enabled(false);
  gl::apply_raster_state({});
  auto program = copyPrograms[(box ? 2 : 0) | (rgb565 ? 1 : 0)]->get();
  gl::uniform4(program, "source_rect", rect[0], rect[1], rect[2], rect[3]);

  if (box)
    gl::uniform4(program, "options", 0.5f / float(source->width), 0.5f / float(source->height), 0, 0);

  gl::bind_texture(0, source->gl(), gl::sampler(gl::LinearClamp));
  gl::draw_fullscreen(program);
}
} // namespace

bool publish_scene_capture(const void* id, const SceneCaptureParameters& p) {
  owner();

  if (!aurora_native_frame_should_render())
    return false;

  if (!id)
    throw std::runtime_error("Scene feedback has no producer identity");

  dimensions(p.width, p.height);

  if (p.format != 4 && p.format != 6)
    throw std::runtime_error("Unsupported scene feedback color format");

  const auto source = gpu::g_frameBuffer.texture;

  if (!source)
    throw std::runtime_error("Scene feedback has no current color target");

  for (float value : p.source)
    if (!std::isfinite(value))
      throw std::runtime_error("Nonfinite scene capture rectangle");
  if (p.source[0] < 0 || p.source[1] < 0 || p.source[2] <= 0 || p.source[3] <= 0)
    throw std::runtime_error("Invalid scene capture rectangle");

  const auto config = vi::configured_fb_size();
  const float logicalW = float(config.x), logicalH = float(config.y);

  if (logicalW <= 0 || logicalH <= 0 || p.source[0] + p.source[2] > logicalW || p.source[1] + p.source[3] > logicalH)
    throw std::runtime_error("Scene capture rectangle exceeds logical color target");

  ++stats.capture_calls;
  call_targets(1, target_bytes(p.width, p.height));
  Restore restore;
  auto output = target(p.width, p.height, restore.state);

  if (!output)
    return reject_capacity();

  programs();
  const std::array<float, 4> rect = {p.source[0] / logicalW, p.source[1] / logicalH, p.source[2] / logicalW, p.source[3] / logicalH};
  copy_target(source, output, rect, p.box_filter, p.format == 4);
  publish(id, std::move(output));
  return true;
}

bool publish_scene_glare(const void* id, const SceneGlareParameters& p) {
  owner();

  if (!aurora_native_frame_should_render())
    return false;

  dimensions(p.width, p.height);

  if (!id)
    throw std::runtime_error("Scene feedback has no producer identity");

  if (!std::isfinite(p.low_threshold) || !std::isfinite(p.high_threshold) || !std::isfinite(p.darken) || !std::isfinite(p.brighten) || !std::isfinite(p.shift_scale))
    throw std::runtime_error("Nonfinite glare parameters");

  auto source = gpu::g_frameBuffer.texture;

  if (!source)
    throw std::runtime_error("Glare has no current scene");

  const auto config = vi::configured_fb_size();
  float sourceLogicalW = float(config.x), sourceLogicalH = float(config.y);
  uint32_t sourceW = source->width, sourceH = source->height;
  uint64_t bytes = target_bytes(p.width, p.height);

  for (const auto& level : p.levels) {
    if (!level.source_width || !level.source_height || level.source_width > sourceLogicalW || level.source_height > sourceLogicalH)
      throw std::runtime_error("Glare downsample exceeds preceding source");

    dimensions(level.width, level.height);
    bytes += target_bytes(level.width, level.height);
    sourceW = level.width;
    sourceH = level.height;
    sourceLogicalW = level.width;
    sourceLogicalH = level.height;
  }

  const bool crop = sourceW != p.width || sourceH != p.height;

  if (crop)
    bytes += target_bytes(p.width, p.height);

  ++stats.glare_calls;
  call_targets(uint32_t(p.levels.size()) + unsigned(crop) + 1, bytes);

  if (p.levels.size() + unsigned(crop) + 1 > SceneFeedbackStats::target_limit || bytes > SceneFeedbackStats::byte_limit)
    return reject_capacity();

  Restore restore;

  // Reserve the complete chain first. Its references keep every destination
  // distinct, even when successive roles have identical descriptors.
  std::array<gl::TexturePtr, SceneFeedbackStats::target_limit> reserved;
  uint32_t count = 0;

  for (const auto& level : p.levels) {
    reserved[count] = target(level.width, level.height, restore.state);

    if (!reserved[count++])
      return reject_capacity();
  }

  if (crop) {
    reserved[count] = target(p.width, p.height, restore.state);

    if (!reserved[count++])
      return reject_capacity();
  }

  reserved[count] = target(p.width, p.height, restore.state);

  if (!reserved[count])
    return reject_capacity();

  programs();
  count = 0;
  sourceLogicalW = float(config.x);
  sourceLogicalH = float(config.y);

  for (const auto& level : p.levels) {
    auto next = reserved[count++];
    const std::array<float, 4> rect = {0, 0, float(level.source_width) / sourceLogicalW, float(level.source_height) / sourceLogicalH};
    copy_target(source, next, rect, true, false);
    source = std::move(next);
    sourceLogicalW = level.width;
    sourceLogicalH = level.height;
  }

  if (crop) {
    auto cropped = reserved[count++];
    const std::array<float, 4> rect = {0, 0, float(p.width) / sourceLogicalW, float(p.height) / sourceLogicalH};
    copy_target(source, cropped, rect, false, false);
    source = std::move(cropped);
  }

  auto output = reserved[count];
  gl::bind_framebuffer(output->gl(), 0);
  gl::set_viewport(0, 0, p.width, p.height, 0, 1);
  gl::set_scissor_enabled(false);
  gl::apply_raster_state({});
  auto program = brightProgram->get();
  gl::uniform4(program, "thresholds", p.low_threshold, p.high_threshold, p.darken, p.brighten);
  gl::uniform4(program, "scale", p.shift_scale, 0, 0, 0);
  gl::bind_texture(0, source->gl(), gl::sampler(gl::NearestClamp));
  gl::draw_fullscreen(program);
  publish(id, std::move(output));
  return true;
}

uint64_t scene_feedback_texture_version(const void* id, uint32_t w, uint32_t h) {
  auto it = images.find(id);
  return it != images.end() && it->second.texture->width == w && it->second.texture->height == h ? it->second.revision : 0;
}

bool bind_scene_feedback_texture(uint32_t unit, const void* id, uint32_t w, uint32_t h, const NativeTextureSampler& s) {
  owner();
  auto it = images.find(id);

  if (it == images.end() || it->second.texture->width != w || it->second.texture->height != h)
    return false;

  if (s.wrap_s || s.wrap_t || s.min_filter > 1 || s.mag_filter > 1 || s.lod_bias != 0)
    throw std::runtime_error("Unsupported scene feedback sampler");

  if (unit >= sampledImages.size())
    throw std::runtime_error("Scene feedback texture unit out of range");

  gl::SamplerDesc sampler = s.min_filter || s.mag_filter ? gl::LinearClamp : gl::NearestClamp;
  sampler.minFilter = s.min_filter ? GL_LINEAR : GL_NEAREST;
  sampler.magFilter = s.mag_filter ? GL_LINEAR : GL_NEAREST;
  sampledImages[unit] = it->second.texture;
  gl::bind_texture(unit, it->second.texture->gl(), gl::sampler(sampler));
  return true;
}

bool clone_scene_feedback_texture(const void* destination, const void* source) {
  owner();

  if (!aurora_native_frame_should_render())
    return false;

  auto it = images.find(source);

  if (it == images.end())
    return false;

  publish(destination, it->second.texture);
  ++stats.clone_calls;
  return true;
}

void clear_scene_feedback_textures() {
  owner();
  sampledImages.fill({});
  images.clear();
  targets.clear();
  stats.pool_bytes = 0;
  copyPrograms.fill({});
  brightProgram.reset();
}

SceneFeedbackStats scene_feedback_stats() {
  owner();
  auto result = stats;
  result.pool_targets = uint32_t(targets.size());

  for (auto& size : result.sizes) {
    size.pool_bytes = 0;
    size.pool_targets = 0;
  }

  for (const auto& entry : targets) {
    result.retained_targets += entry.texture.use_count() != 1;
    result.available_targets += available(entry);

    for (uint32_t i = 0; i < result.size_count; i++)
      if (result.sizes[i].width == entry.texture->width && result.sizes[i].height == entry.texture->height) {
        result.sizes[i].pool_bytes += entry.bytes;
        ++result.sizes[i].pool_targets;
        break;
      }
  }

  return result;
}
} // namespace aurora
