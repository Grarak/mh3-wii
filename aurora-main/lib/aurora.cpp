#include <aurora/aurora.h>
#include <aurora/native_frame_gate.hpp>
#include <aurora/native_draw_memory.hpp>
#ifdef AURORA_GXM
#include "gxm/draw_memory.hpp"
#include "gxm/device.hpp"
#endif

#ifdef AURORA_ENABLE_GX
#include "gfx/common.hpp"
#include "gfx/efb_ram_copy.hpp"
#include "gx/fifo.hpp"
#include "gx/shader_info.hpp"
#include "imgui.hpp"
#include "gfx/depth_peek.hpp"
#include "gl/device.hpp"
#include "gl/gl.hpp"
#include "dolphin/vi/vi_internal.hpp"
#endif

#include "input.hpp"
#include "internal.hpp"
#include "window.hpp"

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_thread.h>
#include <magic_enum.hpp>

#include "system_info.hpp"
#include "tracy/Tracy.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <atomic>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#ifdef AURORA_ENABLE_GX
namespace aurora::gx {
// Producer pacing feedback for the adaptive slot count, defined in lib/gx/shader_info.cpp;
// declared here so the C entry point at the bottom of this file can forward to it.
void report_producer_paced(bool paced) noexcept;
} // namespace aurora::gx
#endif

namespace aurora {
AuroraConfig g_config;
uint32_t g_sdlCustomEventsStart;
char g_gameName[4];
std::atomic<AuroraFrameWorkerWaitCallback> g_frameWorkerWaitCallback{nullptr};
// Presentation schedule for the frame being sealed, set by the producer. Jobs carry absolute
// deadlines derived from it, so the presenter cannot drift. Zero means present when ready.
std::atomic<uint64_t> g_presentScheduleBaseNanos{0};
std::atomic<uint64_t> g_presentScheduleIntervalNanos{0};

namespace {
Module Log("aurora");
NativeFrameGate g_nativeFrame;
bool g_nativeFrameskip = false;

std::atomic<uint32_t> g_captureFrame{UINT32_MAX};
std::string g_captureOutputPath;

using PresentClock = std::chrono::steady_clock;

struct PresentTimingSample {
  PresentClock::time_point presentedAt{};
  std::chrono::nanoseconds interval{};
  // Slot carried a copy of the native image instead of replayed interpolation, so the present
  // counts toward the rate but shows no new motion.
  bool duplicated = false;
};

std::mutex g_presentTimingMutex;
std::array<PresentTimingSample, 512> g_presentTimingSamples{};
size_t g_presentTimingWriteIndex = 0;
size_t g_presentTimingSampleCount = 0;
std::optional<PresentClock::time_point> g_lastSuccessfulPresent;
uint64_t g_totalPresentCount = 0;

void record_successful_present(bool, uint32_t,
                               std::chrono::nanoseconds,
                               std::chrono::nanoseconds,
                               std::chrono::nanoseconds,
                               std::chrono::nanoseconds,
                               std::chrono::nanoseconds,
                               std::chrono::nanoseconds, bool duplicated,
                               uint32_t, std::chrono::nanoseconds) noexcept {
  const auto now = PresentClock::now();
  std::chrono::nanoseconds interval{};
  {
    std::lock_guard lock(g_presentTimingMutex);
    if (g_lastSuccessfulPresent) {
      interval = std::chrono::duration_cast<std::chrono::nanoseconds>(now - *g_lastSuccessfulPresent);
    }
    g_lastSuccessfulPresent = now;
    g_presentTimingSamples[g_presentTimingWriteIndex] = {
        .presentedAt = now,
        .interval = interval,
        .duplicated = duplicated,
    };
    g_presentTimingWriteIndex = (g_presentTimingWriteIndex + 1) % g_presentTimingSamples.size();
    g_presentTimingSampleCount = std::min(g_presentTimingSampleCount + 1, g_presentTimingSamples.size());
    ++g_totalPresentCount;
  }
}

AuroraPresentTiming snapshot_present_timing() noexcept {
  constexpr auto kWindow = std::chrono::seconds(1);
  const auto cutoff = PresentClock::now() - kWindow;
  std::vector<double> milliseconds;
  uint64_t totalPresentCount = 0;
  size_t newMotionSamples = 0;
  {
    std::lock_guard lock(g_presentTimingMutex);
    totalPresentCount = g_totalPresentCount;
    milliseconds.reserve(g_presentTimingSampleCount);
    for (size_t i = 0; i < g_presentTimingSampleCount; ++i) {
      const auto& sample = g_presentTimingSamples[i];
      if (sample.presentedAt >= cutoff && sample.interval.count() > 0) {
        milliseconds.push_back(
            std::chrono::duration<double, std::milli>(sample.interval).count());
        if (!sample.duplicated) {
          ++newMotionSamples;
        }
      }
    }
  }

  AuroraPresentTiming result{
      .totalPresentCount = totalPresentCount,
      .sampleCount = static_cast<uint32_t>(milliseconds.size()),
  };
  if (milliseconds.empty()) {
    return result;
  }

  double totalMilliseconds = 0.0;
  for (const double value : milliseconds) {
    totalMilliseconds += value;
  }
  result.averageFrameTimeMs = totalMilliseconds / static_cast<double>(milliseconds.size());
  result.framesPerSecond = result.averageFrameTimeMs > 0.0 ? 1000.0 / result.averageFrameTimeMs : 0.0;
  // Duplicated presentation slots keep the presented cadence but carry no new
  // motion; scale them out so this reads as the rate the eye actually sees.
  result.effectiveFramesPerSecond =
      result.framesPerSecond *
      (static_cast<double>(newMotionSamples) / static_cast<double>(milliseconds.size()));
  for (const double value : milliseconds) {
    const double difference = value - result.averageFrameTimeMs;
    result.jitterMs += difference * difference;
  }
  result.jitterMs = std::sqrt(result.jitterMs / static_cast<double>(milliseconds.size()));
  std::sort(milliseconds.begin(), milliseconds.end());
  result.p95FrameTimeMs =
      milliseconds[static_cast<size_t>(std::floor(static_cast<double>(milliseconds.size() - 1) * 0.95))];
  return result;
}

void append_u16(std::ofstream& out, uint16_t value) {
  const char bytes[] = {static_cast<char>(value), static_cast<char>(value >> 8)};
  out.write(bytes, sizeof(bytes));
}

void append_u32(std::ofstream& out, uint32_t value) {
  const char bytes[] = {static_cast<char>(value), static_cast<char>(value >> 8),
                        static_cast<char>(value >> 16), static_cast<char>(value >> 24)};
  out.write(bytes, sizeof(bytes));
}

bool write_bmp(const char* path, const uint8_t* pixels, uint32_t width, uint32_t height,
               uint32_t bytesPerRow, bool bgra) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  constexpr uint32_t pixelOffset = 14 + 40;
  const uint32_t imageSize = width * height * 4;
  out.write("BM", 2);
  append_u32(out, pixelOffset + imageSize);
  append_u16(out, 0);
  append_u16(out, 0);
  append_u32(out, pixelOffset);
  append_u32(out, 40);
  append_u32(out, width);
  append_u32(out, height);
  append_u16(out, 1);
  append_u16(out, 32);
  append_u32(out, 0);
  append_u32(out, imageSize);
  append_u32(out, 2835);
  append_u32(out, 2835);
  append_u32(out, 0);
  append_u32(out, 0);
  std::vector<uint8_t> row(width * 4);
  for (uint32_t y = height; y-- > 0;) {
    const auto* source = pixels + static_cast<size_t>(y) * bytesPerRow;
    if (bgra) {
      out.write(reinterpret_cast<const char*>(source), row.size());
    } else {
      for (uint32_t x = 0; x < width; ++x) {
        row[x * 4 + 0] = source[x * 4 + 2];
        row[x * 4 + 1] = source[x * 4 + 1];
        row[x * 4 + 2] = source[x * 4 + 0];
        row[x * 4 + 3] = source[x * 4 + 3];
      }
      out.write(reinterpret_cast<const char*>(row.data()), row.size());
    }
  }
  return out.good();
}

// AuroraBackend is an anonymous C typedef enum, which magic_enum cannot
// reflect under this toolchain -- name it by hand for diagnostics.
constexpr const char* backend_name(AuroraBackend backend) noexcept {
  switch (backend) {
  case BACKEND_AUTO:
    return "Auto";
  case BACKEND_D3D11:
    return "D3D11";
  case BACKEND_D3D12:
    return "D3D12";
  case BACKEND_METAL:
    return "Metal";
  case BACKEND_VULKAN:
    return "Vulkan";
  case BACKEND_OPENGL:
    return "OpenGL";
  case BACKEND_OPENGLES:
    return "OpenGLES";
  case BACKEND_GXM:
    return "GXM";
  case BACKEND_WEBGPU:
    return "WebGPU";
  case BACKEND_NULL:
    return "Null";
  }
  return "Unknown";
}

bool g_initialFrame = false;


AuroraInfo initialize(int argc, char* argv[], const AuroraConfig& config) noexcept {
  g_config = config;
  Log.info("Aurora initializing");
  log_system_information();
  if (g_config.appName == nullptr) {
    g_config.appName = "Aurora";
  } else {
    g_config.appName = strdup(g_config.appName);
  }
  if (g_config.userPath == nullptr) {
    g_config.userPath = SDL_GetPrefPath(nullptr, g_config.appName);
  } else {
    g_config.userPath = strdup(g_config.userPath);
  }
  if (g_config.cachePath == nullptr) {
    g_config.cachePath = SDL_GetPrefPath(nullptr, g_config.appName);
  } else {
    g_config.cachePath = strdup(g_config.cachePath);
  }
  if (g_config.resourcesPath == nullptr) {
    g_config.resourcesPath = SDL_GetBasePath();
  } else {
    g_config.resourcesPath = strdup(g_config.resourcesPath);
  }
  if (g_config.pipelineCachePath == nullptr) {
    g_config.pipelineCachePath = g_config.cachePath;
  } else {
    g_config.pipelineCachePath = strdup(g_config.pipelineCachePath);
  }
  if (g_config.msaa == 0) {
    g_config.msaa = 1;
  }
  if (g_config.maxTextureAnisotropy == 0) {
    g_config.maxTextureAnisotropy = 16;
  }
  ASSERT(window::initialize(), "Error initializing window");

  g_sdlCustomEventsStart = SDL_RegisterEvents(2);
  ASSERT(g_sdlCustomEventsStart, "Failed to allocate user events: {}", SDL_GetError());
  ASSERT(window::initialize_event_watch(), "Error initializing SDL event watch");

#ifdef AURORA_ENABLE_GX
#ifdef AURORA_GXM
  const AuroraBackend selectedBackend = BACKEND_GXM;
#else
  const AuroraBackend selectedBackend = BACKEND_OPENGLES;
#endif
  if (config.desiredBackend != BACKEND_AUTO && config.desiredBackend != selectedBackend) {
    Log.warn("Requested graphics backend {} is unavailable; using {}", backend_name(config.desiredBackend),
             backend_name(selectedBackend));
  }
  if (!window::create_window(selectedBackend) || !gpu::initialize(window::get_sdl_window())) {
    std::string error = SDL_GetError();
    if (error.empty()) {
      error = std::string(backend_name(selectedBackend)) + " is unavailable";
    }
    window::destroy_window();
    SDL_SetError("%s", error.c_str());
    Log.error("Graphics initialization failed: {}", error);
    return {
        .initializationStatus = AURORA_INITIALIZATION_GRAPHICS_UNAVAILABLE,
        .initializationError = SDL_GetError(),
    };
  }
#else
  AuroraBackend selectedBackend = BACKEND_NULL;
  ASSERT(window::create_window(BACKEND_NULL), "Error creating window: {}", SDL_GetError());
  ASSERT(window::create_renderer(), "Failed to initialize SDL renderer: {}", SDL_GetError());
#endif

  window::show_window();

#ifdef AURORA_ENABLE_GX
  gfx::initialize();

  imgui::create_context();
#endif
  const auto size = window::get_window_size();
  Log.info("Using framebuffer size {}x{} scale {}", size.fb_width, size.fb_height, size.scale);
#ifdef AURORA_ENABLE_GX
  if (g_config.imGuiInitCallback != nullptr) {
    g_config.imGuiInitCallback(&size);
  }
  imgui::initialize();
#endif

  g_initialFrame = true;
  g_config.desiredBackend = selectedBackend;
  return {
      .backend = selectedBackend,
      .userPath = g_config.userPath,
      .cachePath = g_config.cachePath,
      .window = window::get_sdl_window(),
      .windowSize = size,
  };
}


#ifdef AURORA_ENABLE_GX
std::recursive_mutex g_rendererGpuMutex;

struct FrameCapture {
  std::string path;
  uint32_t frame = 0;
};

// Producer side: claim the capture request once the frame it names has been sealed.
std::optional<FrameCapture> take_frame_capture(bool allowLaterFrame = false) {
  const uint32_t requestedFrame = g_captureFrame.load(std::memory_order_acquire);
  const uint32_t currentFrame = gfx::current_frame();
  if (requestedFrame == UINT32_MAX || currentFrame < requestedFrame) return std::nullopt;
  g_captureFrame.store(UINT32_MAX, std::memory_order_release);
  if (currentFrame != requestedFrame && !allowLaterFrame) {
    Log.error("Missed requested frame capture {} (current frame {})", requestedFrame, currentFrame);
    return std::nullopt;
  }
  return FrameCapture{.path = g_captureOutputPath, .frame = currentFrame};
}

// Render thread.
void write_frame_capture(const FrameCapture& capture, const gpu::PresentSource& source) {
  if (!source.texture || source.width == 0 || source.height == 0) {
    Log.error("Frame {} capture has no present-source texture", capture.frame);
    return;
  }
  std::vector<uint8_t> pixels(static_cast<size_t>(source.width) * source.height * 4);
  gl::read_pixels(source.texture->gl(), 0, 0, source.width, source.height, pixels.data());
  if (write_bmp(capture.path.c_str(), pixels.data(), source.width, source.height, source.width * 4, false)) {
    Log.info("Captured rendered frame to '{}' ({}x{})", capture.path, source.width, source.height);
  } else {
    Log.error("Failed to write rendered frame capture to '{}'", capture.path);
  }
}

std::atomic<uint64_t> g_presentedFrames{0};

// Render thread: letterbox the present source into the window and swap.
void present_frame(const gpu::PresentSource& source) {
  ZoneScopedN("Present");
  const auto [width, height] = gl::drawable_size();
  auto viewport = gpu::calculate_present_viewport(width, height, source.width, source.height);
  float presentAspect = 0.f;
  if (window::get_present_aspect_ratio(presentAspect)) {
    viewport = gpu::calculate_present_viewport_for_aspect(width, height, presentAspect);
  }
  gl::present(source.texture, static_cast<int32_t>(viewport.left), static_cast<int32_t>(viewport.top),
              static_cast<int32_t>(viewport.width), static_cast<int32_t>(viewport.height));
  record_successful_present(false, 0, {}, {}, {}, {}, {}, {}, false, 0, {});
  g_presentedFrames.fetch_add(1, std::memory_order_relaxed);
}

void record_frame_telemetry() {
  TracyPlot("aurora: drawCallCount", static_cast<int64_t>(gfx::g_stats.drawCallCount));
  TracyPlot("aurora: mergedDrawCallCount", static_cast<int64_t>(gfx::g_stats.mergedDrawCallCount));
  TracyPlot("aurora: lastVertSize", static_cast<int64_t>(gfx::g_stats.lastVertSize));
  TracyPlot("aurora: lastUniformSize", static_cast<int64_t>(gfx::g_stats.lastUniformSize));
  TracyPlot("aurora: lastIndexSize", static_cast<int64_t>(gfx::g_stats.lastIndexSize));
  TracyPlot("aurora: lastStorageSize", static_cast<int64_t>(gfx::g_stats.lastStorageSize));
  TracyPlot("aurora: frameIndex", static_cast<int64_t>(gfx::current_frame()));
  FrameMarkNamed("Aurora frame");
}
#endif

void shutdown() noexcept {
  g_nativeFrame.end();
#ifdef AURORA_ENABLE_GX
  imgui::shutdown();
  gfx::shutdown();
  gpu::shutdown();
#endif
  input::shutdown();
  window::shutdown();
}

const AuroraEvent* update() noexcept {
  ZoneScoped;
  if (g_initialFrame) {
    g_initialFrame = false;
    input::initialize();
  }
  return window::poll_events();
}

bool begin_frame() noexcept {
  ZoneScoped;
#ifdef AURORA_ENABLE_GX
  if (g_nativeFrame.active()) Log.fatal("Cannot mix native and recorded frames");
  window::pump_events();
  if (!window::is_presentable() || window::is_paused()) {
    return false;
  }
  std::lock_guard gpuLock(g_rendererGpuMutex);
  if (window::native_resize_pending() ||
      !window::native_window_size_matches(gpu::g_graphicsConfig.width, gpu::g_graphicsConfig.height)) {
    window::sync_frame_buffer_size();
  }
  // Blocks while every staging set is in flight on the render thread; that is the
  // producer's backpressure.
  const bool began = gfx::begin_frame();
  // The FIFO bridge may decide frameskip before preparing its first draw.
  // Protect a requested capture before that decision can drop geometry.
  if (began && g_captureFrame.load(std::memory_order_acquire) == gfx::current_frame() + 1)
    gfx::require_frame_render();
  return began;
#else
  return true;
#endif
}

bool begin_native_frame() noexcept {
#ifdef AURORA_ENABLE_GX
  if (!gl::on_render_thread()) Log.fatal("Native frame begun outside the game thread");
  if (g_nativeFrame.active()) Log.fatal("Native frame begun before the previous frame ended");
  window::pump_events();
#ifdef AURORA_GXM
  // CDRAM lease and queue admission are mandatory for the native GXM path.
  // Disabling optional frameskip cannot authorize overwriting a busy slot.
  constexpr bool skipBusy = true;
#else
  const bool skipBusy = g_nativeFrameskip;
#endif
  const bool draw = g_nativeFrame.begin(skipBusy,
      window::is_presentable() && !window::is_paused(), [] { return gl::gpu_frame_pending(); });
  gfx::g_stats.presentedFrameCount = 0;
  gfx::g_stats.interpolatedFrameCount = 0;
  if (!draw) return false;
#ifdef AURORA_GXM
  const auto memory = gfx::native_draw_memory(gl::native_frame_slot());
  gxm::bind_draw_memory(memory.uniforms.data(), memory.uniforms.size(), memory.indices.data(), memory.indices.size(),
                        memory.vertices.data(), memory.vertices.size(), memory.attributes.data(), memory.attributes.size());
#endif
  // A skipped frame must not recreate surfaces, rotate staging or touch the
  // previous frame's CDRAM. Resize only once its notification has arrived.
  if (window::native_resize_pending() ||
      !window::native_window_size_matches(gpu::g_graphicsConfig.width, gpu::g_graphicsConfig.height))
    window::sync_frame_buffer_size();
  return true;
#else
  return g_nativeFrame.begin(g_nativeFrameskip, true, [] { return false; });
#endif
}

void end_native_frame() {
  if (!g_nativeFrame.active()) Log.fatal("Native frame ended without a matching begin");
#ifdef AURORA_ENABLE_GX
  if (!gl::on_render_thread()) Log.fatal("Native frame ended outside the game thread");
  const bool draw = g_nativeFrame.should_render();
  gfx::advance_native_frame();
  if (draw) {
    const auto source = gpu::current_present_source();
    // Explicit development capture can read back. A busy frame never forces
    // rendering or a GPU wait; retain its request for the next rendered frame.
    if (auto capture = take_frame_capture(true)) write_frame_capture(*capture, source);
    present_frame(source);
    gfx::g_stats.presentedFrameCount = 1;
  } else {
    gfx::count_skipped_frame();
  }
  record_frame_telemetry();
#endif
  g_nativeFrame.end();
}

// Seals the frame on the producer and hands it to the render thread, which renders,
// resolves the frame-latent EFB readbacks, and presents.
void end_frame() {
  ZoneScoped;
#ifdef AURORA_ENABLE_GX
  window::pump_events();
  std::lock_guard gpuLock(g_rendererGpuMutex);
  // Probe-sized CPU-consumed copies read back after the frame renders. Their downscale blits push
  // uniforms, so prepare them while this frame's staging set is still being recorded.
  auto asyncDownloads = gfx::efb_ram::seal_async_downloads();
  gfx::end_frame();
  auto capture = take_frame_capture();
  if (capture.has_value()) {
    gfx::require_frame_render();
  }
  const bool skip = gfx::frame_skipped();
  // Latched before the next gfx::begin_frame() clears the display-copy override.
  auto presentSource = gpu::current_present_source();
  gfx::efb_ram::cancel();
  auto frame = std::make_shared<gfx::SealedFrame>();
  gfx::seal_frame(*frame);
  gfx::expire_bind_group_cache();
  if (skip) {
    // Frameskip: the render thread only runs the frame's palette conversions.
    gfx::count_skipped_frame();
    gl::post([frame = std::move(frame)]() mutable {
      gfx::render(*frame, -1, true);
      gfx::after_submit();
    });
    record_frame_telemetry();
    return;
  }
  gfx::frame_render_posted();
  gl::post([frame = std::move(frame), asyncDownloads = std::move(asyncDownloads),
            capture = std::move(capture), presentSource = std::move(presentSource)]() mutable {
    gfx::render(*frame, -1, true);
    if (asyncDownloads) {
      std::move(asyncDownloads)();
    }
    if (capture.has_value()) {
      write_frame_capture(*capture, presentSource);
    }
    present_frame(presentSource);
    gfx::after_submit();
    gfx::frame_render_finished();
  });
  gfx::g_stats.presentedFrameCount = 1;
  gfx::g_stats.interpolatedFrameCount = 0;
  record_frame_telemetry();
#endif
}

#ifdef AURORA_ENABLE_GX
// Submits everything recorded so far, runs `readback` on the render thread after it, and
// resumes the same frame without clearing the EFB.
bool flush_and_resume(void* dest, bool download) {
  gx::mark_frame_interpolation_replay_unsafe();
  std::lock_guard gpuLock(g_rendererGpuMutex);
  if (download && !gfx::efb_ram::prepare_downloads(dest)) return false;
  gfx::flush_recording();
  const bool copied = !download || gfx::efb_ram::download(dest);
  const bool resumed = gfx::resume_frame();
  return copied && resumed;
}
#endif
} // namespace

// Frames are sealed synchronously on the producer, so there is never a worker to join.
bool native_frame_drawing_allowed() noexcept { return g_nativeFrame.drawing_allowed(); }
NativeDrawMemory native_draw_memory() noexcept {
#ifdef AURORA_ENABLE_GX
  if (g_nativeFrame.should_render()) return gfx::native_draw_memory(gl::native_frame_slot());
#endif
  return {};
}
void wait_for_frame_worker() noexcept {}
std::chrono::nanoseconds wait_for_frame_worker_sealed() noexcept { return std::chrono::nanoseconds::zero(); }
bool wait_for_frame_worker_for(std::chrono::microseconds) noexcept { return true; }
#ifdef AURORA_ENABLE_GX
std::recursive_mutex& renderer_gpu_mutex() noexcept { return g_rendererGpuMutex; }
#endif
} // namespace aurora

// C API bindings
AuroraInfo aurora_initialize(int argc, char* argv[], const AuroraConfig* config) {
  return aurora::initialize(argc, argv, *config);
}
void aurora_shutdown() { aurora::shutdown(); }
void aurora_set_common_dialog_active(bool active) {
#ifdef AURORA_GXM
  if (!aurora::gl::on_render_thread())
    aurora::Log.fatal("Common dialog changed outside the game thread");
  aurora::gxm::set_common_dialog_active(active);
#else
  (void)active;
#endif
}
const AuroraEvent* aurora_update() { return aurora::update(); }
bool aurora_begin_frame() { return aurora::begin_frame(); }
void aurora_end_frame() { aurora::end_frame(); }
bool aurora_configure_native_target(uint32_t width, uint32_t height) {
#ifdef AURORA_ENABLE_GX
  if (aurora::g_nativeFrame.active()) aurora::Log.fatal("Native target changed during a frame");
  if (aurora::gl::initialized() && !aurora::gl::on_render_thread())
    aurora::Log.fatal("Native target configured outside the game thread");
  return aurora::vi::configure_native(width, height);
#else
  return width && height;
#endif
}
bool aurora_begin_native_frame() { return aurora::begin_native_frame(); }
bool aurora_native_frame_should_render() { return aurora::g_nativeFrame.should_render(); }
uint32_t aurora_native_frame_slot() {
#ifdef AURORA_ENABLE_GX
  return aurora::gl::native_frame_slot();
#else
  return 0;
#endif
}
void aurora_end_native_frame() { aurora::end_native_frame(); }
void aurora_set_frame_worker_wait_callback(AuroraFrameWorkerWaitCallback callback) {
  aurora::g_frameWorkerWaitCallback.store(callback, std::memory_order_release);
}
void aurora_set_frameskip(bool enabled) {
  aurora::g_nativeFrameskip = enabled;
#ifdef AURORA_ENABLE_GX
  aurora::gfx::set_frameskip(enabled);
#endif
}
uint64_t aurora_skipped_frame_count() {
#ifdef AURORA_ENABLE_GX
  return aurora::gfx::skipped_frame_count();
#else
  return 0;
#endif
}
uint64_t aurora_presented_frame_count() {
  return aurora::g_presentedFrames.load(std::memory_order_relaxed);
}
void aurora_wait_for_gpu_idle() {
#ifdef AURORA_ENABLE_GX
  aurora::gl::finish();
#endif
}
void aurora_wait_for_frame_worker() { aurora::wait_for_frame_worker(); }
bool aurora_wait_for_frame_worker_for(uint32_t timeoutMicros) {
  return aurora::wait_for_frame_worker_for(std::chrono::microseconds(timeoutMicros));
}
void aurora_set_present_schedule(uint64_t baseNanos, uint64_t intervalNanos) {
  aurora::g_presentScheduleBaseNanos.store(baseNanos, std::memory_order_release);
  aurora::g_presentScheduleIntervalNanos.store(intervalNanos, std::memory_order_release);
}
void aurora_report_producer_paced(bool paced) {
#ifdef AURORA_ENABLE_GX
  aurora::gx::report_producer_paced(paced);
#else
  (void)paced;
#endif
}
void aurora_get_frame_interpolation_diagnostics(AuroraFrameInterpolationDiagnostics* diagnostics) {
  if (diagnostics != nullptr) {
    aurora::gx::get_frame_interpolation_diagnostics(*diagnostics);
  }
}

void aurora_get_present_timing(AuroraPresentTiming* timing) {
  if (timing != nullptr) {
    *timing = aurora::snapshot_present_timing();
  }
}
void aurora_set_frame_interpolation_fps(uint32_t targetFps) {
#ifdef AURORA_ENABLE_GX
  aurora::gx::set_frame_interpolation_fps(targetFps);
#else
  (void)targetFps;
#endif
}
uint32_t aurora_get_frame_interpolation_fps() {
#ifdef AURORA_ENABLE_GX
  return aurora::gx::frame_interpolation_fps();
#else
  return 0;
#endif
}
void aurora_request_frame_capture(uint32_t frame, const char* outputPath) {
  aurora::g_captureOutputPath = outputPath != nullptr ? outputPath : "frame_capture.bmp";
  aurora::g_captureFrame.store(frame, std::memory_order_release);
}
bool aurora_flush_efb_copies_to_ram() {
#ifdef AURORA_ENABLE_GX
  if (!aurora::gfx::efb_ram::has_pending()) {
    return true;
  }
  return aurora::flush_and_resume(nullptr, true);
#else
  return true;
#endif
}
bool aurora_flush_efb_copy_to_ram(void* dest) {
#ifdef AURORA_ENABLE_GX
  if (dest == nullptr || !aurora::gfx::efb_ram::has_pending(dest)) {
    return false;
  }
  return aurora::flush_and_resume(dest, true);
#else
  (void)dest;
  return true;
#endif
}
bool aurora_flush_draws() {
#ifdef AURORA_ENABLE_GX
  return aurora::flush_and_resume(nullptr, false);
#else
  return false;
#endif
}
AuroraBackend aurora_get_backend() { return aurora::g_config.desiredBackend; }
const AuroraBackend* aurora_get_available_backends(size_t* count) {
#ifdef AURORA_GXM
  static constexpr AuroraBackend Backends[] = {BACKEND_GXM};
#else
  static constexpr AuroraBackend Backends[] = {BACKEND_OPENGLES};
#endif
  if (count != nullptr) {
    *count = 1;
  }
  return Backends;
}
void aurora_set_log_level(AuroraLogLevel level) { aurora::g_config.logLevel = level; }
void aurora_set_pause_on_focus_lost(bool value) { aurora::g_config.pauseOnFocusLost = value; }
void aurora_set_disable_copy_filter(bool disabled) { aurora::g_config.disableCopyFilter = disabled; }
bool aurora_get_disable_copy_filter() { return aurora::g_config.disableCopyFilter; }
void aurora_set_background_input(bool value) {
  aurora::g_config.allowJoystickBackgroundEvents = value;
  aurora::window::set_background_input(value);
}
void aurora_set_display_mode(AuroraDisplayMode mode) { aurora::window::set_display_mode(mode); }
AuroraDisplayMode aurora_get_display_mode() { return aurora::window::get_display_mode(); }
