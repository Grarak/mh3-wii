#include "depth_peek.hpp"

#include "../dolphin/vi/vi_internal.hpp"
#include "../gx/gx.hpp"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <string>

#include <tracy/Tracy.hpp>

namespace aurora::gfx::depth_peek {
namespace {
Module Log("aurora::gfx::depth_peek");

using Clock = std::chrono::steady_clock;

constexpr uint32_t DepthPeekSnapshotHz = 30;
constexpr auto SnapshotInterval = std::chrono::nanoseconds{1'000'000'000 / DepthPeekSnapshotHz};

struct Params {
  float offsetX = 0.f;
  float offsetY = 0.f;
  float scaleX = 1.f;
  float scaleY = 1.f;
};

struct LatestSnapshot {
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint32_t> data;
};

gl::ProgramPtr g_program;
gl::TexturePtr g_target; // Render thread only.
bool g_snapshotRequested = false;
Clock::time_point g_nextSnapshotTime;
LatestSnapshot g_latest;
std::mutex g_mutex;

constexpr std::string_view VertexShader = R"(
const vec2 positions[3] = vec2[3](vec2(-1.0, 1.0), vec2(-1.0, -3.0), vec2(3.0, 1.0));

void main() {
    gl_Position = aur_position(vec4(positions[gl_VertexID], 0.0, 1.0));
}
)"sv;

// Each destination pixel packs its 24-bit GX depth into RGB bytes.
constexpr std::string_view FragmentShader = R"(
uniform highp sampler2D src;
uniform vec4 params; // offset.xy, scale.xy
layout(location = 0) out vec4 frag_color;

uint gx_z24(float depth) {
    return min(uint(clamp(depth, 0.0, 1.0) * 16777215.0 + 0.5), 0x00ffffffu);
}

void main() {
    vec2 dstCenter = floor(gl_FragCoord.xy) + vec2(0.5);
    ivec2 srcSize = textureSize(src, 0);
    ivec2 srcPixel = clamp(ivec2(floor(params.xy + dstCenter * params.zw)), ivec2(0), srcSize - ivec2(1));
    float depth = texelFetch(src, srcPixel, 0).r;
    uint z = gx_z24(DEPTH_EXPR);
    frag_color = vec4(uvec4(z & 0xffu, (z >> 8u) & 0xffu, z >> 16u, 255u)) / 255.0;
}
)"sv;

std::string build_fragment_source() {
  std::string source(FragmentShader);
  const std::string_view token = "DEPTH_EXPR";
  source.replace(source.find(token), token.size(), gx::UseReversedZ ? "1.0 - depth" : "depth");
  return source;
}

Params make_params(uint32_t srcWidth, uint32_t srcHeight, const FrameMapping& mapping) noexcept {
  Params params{};
  if (mapping.viewportPolicy == AURORA_VIEWPORT_NATIVE) {
    return params;
  }

  const auto logicalSize = mapping.logicalSize;
  if (logicalSize.x == 0 || logicalSize.y == 0 || srcWidth == 0 || srcHeight == 0) {
    return params;
  }

  const bool stretch = mapping.viewportPolicy == AURORA_VIEWPORT_STRETCH ||
                       mapping.viewportPolicy == AURORA_VIEWPORT_16_9;
  const float scaleX = static_cast<float>(srcWidth) / static_cast<float>(logicalSize.x);
  const float scaleY = static_cast<float>(srcHeight) / static_cast<float>(logicalSize.y);
  const float scale = std::min(scaleX, scaleY);
  params.scaleX = stretch ? scaleX : scale;
  params.scaleY = stretch ? scaleY : scale;
  params.offsetX = stretch ? 0.f : (static_cast<float>(srcWidth) - static_cast<float>(logicalSize.x) * scale) * 0.5f;
  params.offsetY =
      stretch ? 0.f : (static_cast<float>(srcHeight) - static_cast<float>(logicalSize.y) * scale) * 0.5f;
  return params;
}
} // namespace

void initialize() {
  g_program = std::make_shared<gl::Program>(gl::ProgramDesc{
      .label = "Depth Peek",
      .vertex = gl::vertex_source(VertexShader),
      .fragment = gl::fragment_source(build_fragment_source()),
      .samplers = {{"src", 0}},
  });
}

void shutdown() {
  testing::reset();
  g_program.reset();
  gl::post([] { g_target.reset(); });
}

void request_snapshot() noexcept {
  std::lock_guard lock{g_mutex};
  g_snapshotRequested = true;
}

bool read_latest(uint16_t x, uint16_t y, uint32_t& z) noexcept {
  std::lock_guard lock{g_mutex};
  if (x >= g_latest.width || y >= g_latest.height || g_latest.data.empty()) {
    return false;
  }
  z = g_latest.data[static_cast<size_t>(y) * g_latest.width + x] & 0x00ffffffu;
  return true;
}

void poll() noexcept {}

FrameMapping capture_frame_mapping() noexcept {
  return {vi::configured_fb_size(), gx::g_gxState.viewportPolicy};
}

void encode_frame_snapshot(const gl::TexturePtr& depth, uint32_t width, uint32_t height,
                           const FrameMapping& mapping) noexcept {
  ZoneScoped;
  const auto now = Clock::now();
  {
    std::lock_guard lock{g_mutex};
    if (!g_snapshotRequested || now < g_nextSnapshotTime) {
      return;
    }
    g_snapshotRequested = false;
    g_nextSnapshotTime = now + SnapshotInterval;
  }

  const auto dstSize = mapping.logicalSize;
  if (!depth || !g_program || dstSize.x == 0 || dstSize.y == 0 || width == 0 || height == 0) {
    return;
  }
  const GLuint program = g_program->get();
  if (program == 0) {
    return;
  }
  if (!g_target || g_target->width != dstSize.x || g_target->height != dstSize.y) {
    g_target = gl::create_texture(dstSize.x, dstSize.y, 1, gl::Format::RGBA8, "Depth Peek");
  }

  const Params params = make_params(width, height, mapping);
  gl::bind_framebuffer(g_target->gl(), 0);
  gl::set_scissor_enabled(false);
  gl::set_viewport(0, 0, static_cast<int32_t>(dstSize.x), static_cast<int32_t>(dstSize.y), 0.f, 1.f);
  gl::apply_raster_state({});
  gl::bind_texture(0, depth->gl(), gl::sampler(gl::NearestClamp));
  gl::use_program(program);
  gl::uniform4(program, "params", params.offsetX, params.offsetY, params.scaleX, params.scaleY);
  gl::draw_fullscreen(program);

  // Synchronous, but limited to DepthPeekSnapshotHz and a logical-size target.
  std::vector<uint32_t> data(static_cast<size_t>(dstSize.x) * dstSize.y);
  gl::read_pixels(g_target->gl(), 0, 0, dstSize.x, dstSize.y, data.data());
  for (auto& value : data) {
    value &= 0x00ffffffu; // little-endian RGBA bytes: r = low, g = mid, b = high
  }
  std::lock_guard lock{g_mutex};
  g_latest.width = dstSize.x;
  g_latest.height = dstSize.y;
  g_latest.data = std::move(data);
}

void after_submit() noexcept {}

namespace testing {
void reset() noexcept {
  std::lock_guard lock{g_mutex};
  g_snapshotRequested = false;
  g_nextSnapshotTime = {};
  g_latest = {};
}

bool snapshot_requested() noexcept {
  std::lock_guard lock{g_mutex};
  return g_snapshotRequested;
}

void set_latest(uint32_t width, uint32_t height, const std::vector<uint32_t>& data) {
  std::lock_guard lock{g_mutex};
  g_latest.width = width;
  g_latest.height = height;
  g_latest.data = data;
}
} // namespace testing

} // namespace aurora::gfx::depth_peek
