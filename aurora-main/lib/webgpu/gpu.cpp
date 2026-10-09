#include "gpu.hpp"

#include "../internal.hpp"

#include <emscripten/html5.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace aurora::webgpu {
static Module Log("aurora::gpu");

wgpu::Instance g_instance;
wgpu::Adapter g_adapter;
wgpu::Device g_device;
wgpu::Queue g_queue;
wgpu::Surface g_surface;
wgpu::TextureFormat g_surfaceFormat = wgpu::TextureFormat::Undefined;
wgpu::Limits g_limits;

namespace {
constexpr const char* CanvasSelector = "#canvas";

// The adapter and device requests complete in the main thread's event loop;
// the requesting thread waits on `requestState` (0 pending, 1 ready, 2 failed).
std::atomic<uint32_t> requestState{0};
SurfaceSize surfaceSize;

// Validation errors after initialization are fatal, as in the Dawn backend:
// a WebGPU error leaves the frame undefined. Device loss stops the game too.
std::atomic_bool initialized{false};

void finish_request(bool ok) {
  requestState.store(ok ? 1 : 2, std::memory_order_release);
  requestState.notify_all();
}

void request_device() {
  std::vector<wgpu::FeatureName> requiredFeatures;
  wgpu::SupportedFeatures supportedFeatures;
  g_adapter.GetFeatures(&supportedFeatures);

  for (size_t i = 0; i < supportedFeatures.featureCount; ++i) {
    const auto feature = supportedFeatures.features[i];

    if (feature == wgpu::FeatureName::Float32Filterable)
      requiredFeatures.push_back(feature);
  }

  wgpu::Limits supportedLimits{};
  g_adapter.GetLimits(&supportedLimits);

  // The renderer needs the default limits, a 16 KiB uniform binding per block
  // and the adapter's largest textures.
  wgpu::Limits requiredLimits{};
  requiredLimits.maxTextureDimension2D = supportedLimits.maxTextureDimension2D;
  wgpu::DeviceDescriptor deviceDescriptor;
  deviceDescriptor.label = "Aurora";
  deviceDescriptor.requiredFeatureCount = requiredFeatures.size();
  deviceDescriptor.requiredFeatures = requiredFeatures.data();
  deviceDescriptor.requiredLimits = &requiredLimits;
  deviceDescriptor.SetUncapturedErrorCallback([](const wgpu::Device&, wgpu::ErrorType type, wgpu::StringView message) {
    const std::string_view text{message};

    if (initialized.load(std::memory_order_acquire)) {
      std::fprintf(stderr, "Aurora WebGPU error %u: %.*s\n", unsigned(type), int(text.size()), text.data());
      fatal("WebGPU validation error");
    }

    Log.warn("WebGPU error {}: {}", unsigned(type), text);
  });
  deviceDescriptor.SetDeviceLostCallback(wgpu::CallbackMode::AllowSpontaneous, [](const wgpu::Device&, wgpu::DeviceLostReason reason, wgpu::StringView message) {
    // Shutdown releases the last device reference here too, not a failure.
    if (reason == wgpu::DeviceLostReason::Destroyed)
      return;

    const std::string_view text{message};
    std::fprintf(stderr, "Aurora WebGPU device lost (%u): %.*s\n", unsigned(reason), int(text.size()), text.data());
    fatal("The WebGPU device was lost");
  });
  g_adapter.RequestDevice(&deviceDescriptor, wgpu::CallbackMode::AllowSpontaneous, [](wgpu::RequestDeviceStatus status, wgpu::Device device, wgpu::StringView message) {
    if (status != wgpu::RequestDeviceStatus::Success) {
      Log.error("Device request failed: {}", std::string_view(message));
      finish_request(false);
      return;
    }

    g_device = std::move(device);
    finish_request(true);
  });
}

// Main thread: starts the asynchronous adapter and device requests.
void request_adapter(void*) {
  g_instance = wgpu::CreateInstance(nullptr);

  if (!g_instance) {
    Log.error("Failed to create the WebGPU instance");
    finish_request(false);
    return;
  }

  const wgpu::RequestAdapterOptions options{
      .powerPreference = wgpu::PowerPreference::HighPerformance,
  };

  g_instance.RequestAdapter(&options, wgpu::CallbackMode::AllowSpontaneous, [](wgpu::RequestAdapterStatus status, wgpu::Adapter adapter, wgpu::StringView message) {
    if (status != wgpu::RequestAdapterStatus::Success || !adapter) {
      Log.error("Adapter request failed: {}", std::string_view(message));
      finish_request(false);
      return;
    }

    g_adapter = std::move(adapter);
    request_device();
  });
}

bool create_surface() {
  wgpu::EmscriptenSurfaceSourceCanvasHTMLSelector canvas;
  canvas.selector = CanvasSelector;
  const wgpu::SurfaceDescriptor surfaceDescriptor{
      .nextInChain = &canvas,
      .label = "Surface",
  };

  g_surface = g_instance.CreateSurface(&surfaceDescriptor);

  if (!g_surface) {
    Log.error("Failed to create the canvas surface");
    return false;
  }

  wgpu::SurfaceCapabilities capabilities;

  if (g_surface.GetCapabilities(g_adapter, &capabilities) != wgpu::Status::Success || capabilities.formatCount == 0) {
    Log.error("The canvas surface has no formats");
    return false;
  }

  // The browser's preferred format comes first.
  g_surfaceFormat = capabilities.formats[0];
  return true;
}
} // namespace

bool initialize() noexcept {
  if (on_gpu_thread()) {
    Log.error("WebGPU initialization must wait for the browser's main thread; call it from another thread");
    return false;
  }

  requestState.store(0, std::memory_order_relaxed);
  emscripten_proxy_async(emscripten_proxy_get_system_queue(), emscripten_main_runtime_thread_id(), request_adapter, nullptr);

  for (uint32_t state = requestState.load(std::memory_order_acquire); state == 0; state = requestState.load(std::memory_order_acquire))
    requestState.wait(0, std::memory_order_acquire);

  if (requestState.load(std::memory_order_acquire) != 1)
    return false;

  bool ok = false;
  on_gpu([&] {
    g_queue = g_device.GetQueue();
    g_device.GetLimits(&g_limits);
    wgpu::AdapterInfo info;
    g_adapter.GetInfo(&info);
    Log.info("Graphics adapter information\n  API: WebGPU\n  Vendor: {}\n  Architecture: {}\n  Device: {}\n  Description: {}", std::string_view(info.vendor),
             std::string_view(info.architecture), std::string_view(info.device), std::string_view(info.description));
    Log.info("Using limits: maxTextureDimension2D {}, minUniformBufferOffsetAlignment {}, maxUniformBufferBindingSize {}", g_limits.maxTextureDimension2D,
             g_limits.minUniformBufferOffsetAlignment, g_limits.maxUniformBufferBindingSize);
    ok = create_surface();
    initialized.store(ok, std::memory_order_release);
  });

  return ok;
}

void shutdown() noexcept {
  on_gpu([] {
    initialized.store(false, std::memory_order_release);

    if (g_surface)
      g_surface.Unconfigure();

    g_surface = {};
    surfaceSize = {};
    g_queue = {};
    g_device = {};
    g_adapter = {};
    g_instance = {};
  });
}

void configure_surface(uint32_t width, uint32_t height) noexcept {
  if (!g_surface || !width || !height || (surfaceSize.width == width && surfaceSize.height == height))
    return;

  const wgpu::SurfaceConfiguration configuration{
      .device = g_device,
      .format = g_surfaceFormat,
      .usage = wgpu::TextureUsage::RenderAttachment,
      .width = width,
      .height = height,
      .alphaMode = wgpu::CompositeAlphaMode::Opaque,
      .presentMode = wgpu::PresentMode::Fifo,
  };

  g_surface.Configure(&configuration);
  surfaceSize = {width, height};
}

SurfaceSize surface_size() noexcept {
  return surfaceSize;
}

std::pair<uint32_t, uint32_t> canvas_size() noexcept {
  int width = 0, height = 0;
  emscripten_get_canvas_element_size(CanvasSelector, &width, &height);
  return {uint32_t(std::max(width, 0)), uint32_t(std::max(height, 0))};
}

void fatal(const char* what) noexcept {
  std::fprintf(stderr, "Aurora WebGPU: %s\n", what);
  std::fflush(stderr);
  std::abort();
}
} // namespace aurora::webgpu
