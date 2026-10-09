#pragma once

// The WebGPU device of the web build (docs/optimizations/platforms/web-build.md), restored from
// Aurora's Dawn renderer (03132238^:aurora-main/lib/webgpu/gpu.cpp) onto
// Emscripten's emdawnwebgpu. WebGPU objects belong to the JavaScript realm
// that created them, so the device, the canvas surface and every renderer call
// live on the browser's main thread; other threads reach it by proxying.

#include "wgpu.hpp"

#include <cstdint>
#include <exception>
#include <type_traits>
#include <utility>

#include <emscripten/proxying.h>
#include <emscripten/threading.h>

namespace aurora::webgpu {
extern wgpu::Instance g_instance;
extern wgpu::Adapter g_adapter;
extern wgpu::Device g_device;
extern wgpu::Queue g_queue;
extern wgpu::Surface g_surface;
extern wgpu::TextureFormat g_surfaceFormat;
extern wgpu::Limits g_limits;

// Main thread only: whether the surface is configured for this size.
struct SurfaceSize {
  uint32_t width = 0, height = 0;
};

// Requests the adapter and device on the browser's main thread and waits for
// them (any thread but the main one), then creates and configures the canvas
// surface. False when WebGPU is unavailable.
bool initialize() noexcept;
void shutdown() noexcept;

// Main thread only: (re)configures the canvas surface at `width` x `height`.
void configure_surface(uint32_t width, uint32_t height) noexcept;
SurfaceSize surface_size() noexcept;

// The canvas's drawing buffer size in pixels (any thread).
std::pair<uint32_t, uint32_t> canvas_size() noexcept;

// Reports a lost device or an uncaptured validation error: the renderer
// stops, as Aurora's Dawn backend did.
[[noreturn]] void fatal(const char* what) noexcept;

inline bool on_gpu_thread() noexcept { return emscripten_is_main_browser_thread(); }

namespace detail {
template <typename F>
struct Proxied {
  F* function;
#if defined(__cpp_exceptions)
  std::exception_ptr error;
#endif
  static void run(void* self) {
    auto* call = static_cast<Proxied*>(self);
#if defined(__cpp_exceptions)
    try {
      (*call->function)();
    } catch (...) {
      call->error = std::current_exception();
    }
#else
    (*call->function)();
#endif
  }
};
} // namespace detail

// Runs `function` on the browser's main thread and waits for it; inline
// there. Exceptions reach the caller.
template <typename F>
void on_gpu(F&& function) {
  if (on_gpu_thread()) {
    function();
    return;
  }

  detail::Proxied<std::remove_reference_t<F>> call{&function};

  if (!emscripten_proxy_sync(emscripten_proxy_get_system_queue(), emscripten_main_runtime_thread_id(), &decltype(call)::run, &call))
    fatal("Cannot proxy a renderer call to the browser's main thread");
#if defined(__cpp_exceptions)
  if (call.error)
    std::rethrow_exception(call.error);
#endif
}
} // namespace aurora::webgpu
