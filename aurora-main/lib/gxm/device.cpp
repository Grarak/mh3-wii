#include "device.hpp"
#include <aurora/fatal.hpp>
#include "raster.hpp"
#include "razor.hpp"
#include "shader_bank.hpp"
#ifdef MH3_DEVELOPMENT
#include "runtime_shader.hpp"
#endif
#include "native_capacity.hpp"
#include <aurora/gpu_completion.hpp>
#include <aurora/thread_role.hpp>
#include <psp2/display.h>
#include <psp2/common_dialog.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <set>

// These firmware exports are in the SDK stub library but absent from its header.
extern "C" void sceGxmDepthStencilSurfaceSetBackgroundMask(SceGxmDepthStencilSurface*, SceBool);
extern "C" bool sceGxmIsDebugVersion(void);

namespace aurora::gxm {
namespace {
std::atomic<Device*> g_frameDevice{nullptr};
std::atomic<uint32_t> g_displayQueued{0}, g_displayCompleted{0};
std::atomic<uint32_t> g_displayPresented{0}, g_displayFailures{0};
std::atomic<uint64_t> g_displayWaitMicros{0};
std::atomic<uint64_t> g_pacedFlips{0}, g_pacingErrorMicros{0}, g_flipVblanks[4]{};
std::atomic<uint32_t> g_pacingErrorMaxMicros{0};
std::atomic<bool> g_flipTrace{false};
FlipSample g_flips[256];
std::atomic<uint64_t> g_flipCount{0};
std::atomic<int64_t> g_repeatedVblanks{0};

// Flips wait for VBlank (set_vsync).
std::atomic<bool> g_vsync{true};
std::atomic<uint64_t> g_latencyFlips{0}, g_latencyMicros[4]{};
std::atomic<uint32_t> g_latencyMaxMicros{0};
std::atomic<uint64_t> g_finishMicros{0}, g_finishCalls{0};
bool g_commonDialogActive = false;

struct DisplayData {
  void* address;
  uint64_t game_us;                         // game time of the frame, 0 = unknown
  uint64_t input_us, published_us, presented_us; // steady clock; input 0 = unknown
};

uint64_t steady_us() {
  return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

// Display callback only: the flipped frame's input-to-flip time in its parts.
void record_latency(const DisplayData& display, uint64_t gpuDoneUs, uint64_t flipUs) {
  if (!display.input_us)
    return;

  const uint64_t marks[5] = {display.input_us, display.published_us, display.presented_us, gpuDoneUs, flipUs};

  for (unsigned i = 0; i < 4; ++i)
    g_latencyMicros[i].fetch_add(marks[i + 1] - marks[i], std::memory_order_relaxed);

  g_latencyFlips.fetch_add(1, std::memory_order_relaxed);
  const uint32_t total = uint32_t(std::min<uint64_t>(flipUs - display.input_us, UINT32_MAX));

  if (total > g_latencyMaxMicros.load(std::memory_order_relaxed))
    g_latencyMaxMicros.store(total, std::memory_order_relaxed);
}

// Display callback only: compares each flip's interval with the game time its
// frame advanced over the previous flipped frame. Equal intervals are smooth
// motion; the difference is the judder the viewer sees.
void record_pacing(uint64_t gameUs) {
  static uint64_t lastFlip = 0, lastGame = 0;

  if (!gameUs)
    return;

  const uint64_t now = sceKernelGetProcessTimeWide();

  if (lastGame && gameUs > lastGame) {
    const int64_t flip = int64_t(now - lastFlip), game = int64_t(gameUs - lastGame);
    const uint32_t error = uint32_t(std::min<int64_t>(flip > game ? flip - game : game - flip, UINT32_MAX));
    g_pacedFlips.fetch_add(1, std::memory_order_relaxed);
    g_pacingErrorMicros.fetch_add(error, std::memory_order_relaxed);

    if (error > g_pacingErrorMaxMicros.load(std::memory_order_relaxed))
      g_pacingErrorMaxMicros.store(error, std::memory_order_relaxed);

    const int64_t vblanks = (flip + 8341) / 16683;
    g_flipVblanks[std::clamp<int64_t>(vblanks, 1, 4) - 1].fetch_add(1, std::memory_order_relaxed);
  }

  if (g_flipTrace.load(std::memory_order_relaxed)) {
    const uint64_t count = g_flipCount.load(std::memory_order_relaxed);
    g_flips[count % std::size(g_flips)] = {now, gameUs};
    g_flipCount.store(count + 1, std::memory_order_release);
  }

  lastFlip = now;
  lastGame = gameUs;
}

void display_callback(const void* data) {
  aurora::pin_current_thread(aurora::ThreadRole::Worker);
  const auto& display = *static_cast<const DisplayData*>(data);
  const uint64_t gpuDoneUs = steady_us();
  SceDisplayFrameBuf frame{};
  frame.size = sizeof(frame);
  frame.base = display.address;
  frame.pitch = Device::Stride;
  frame.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
  frame.width = Device::Width;
  frame.height = Device::Height;

  // VSync: flip at the next VBlank and wait for it, so the old buffer's sync
  // is released only after scanout has moved on. This wait belongs to the
  // display callback on core 0, never the game or render thread. Without
  // VSync (the game thread limit off, set_vsync) the flip is still set for
  // the next VBlank but the callback does not wait for it, so the display
  // queue no longer paces the GPU and the render thread (the newest frame at
  // each VBlank is shown; a buffer may be reused before scanout leaves it).
  // SCE_DISPLAY_SETBUF_IMMEDIATE fails from the display queue's callback
  // (every flip, devkit 2026-10-06: a black screen); vita2d's no-vsync mode
  // does the same as here.
  const bool vsync = g_vsync.load(std::memory_order_relaxed);
  const int setResult = sceDisplaySetFrameBuf(&frame, SCE_DISPLAY_SETBUF_NEXTFRAME);
  int waitResult = 0;

  if (setResult < 0) {
    static std::atomic<bool> reported{false};

    if (!reported.exchange(true, std::memory_order_relaxed))
      std::printf("Aurora GXM: sceDisplaySetFrameBuf failed: 0x%08x\n", unsigned(setResult));
  }

  if (setResult >= 0 && vsync) {
    const auto started = std::chrono::steady_clock::now();
    waitResult = sceDisplayWaitSetFrameBuf();
    g_displayWaitMicros.fetch_add(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count(), std::memory_order_relaxed);
  }

  if (setResult >= 0 && waitResult >= 0) {
    // The VBlank count of each flip, read after its wait: a read the callback
    // took late counts one VBlank too many there and one too few at the next.
    static uint32_t lastVcount = 0;
    static bool counted = false;
    const uint32_t vcount = uint32_t(sceDisplayGetVcount());

    if (vsync && counted)
      g_repeatedVblanks.fetch_add(int64_t(int32_t(vcount - lastVcount)) - 1, std::memory_order_relaxed);

    lastVcount = vcount;
    counted = vsync;
    g_displayPresented.fetch_add(1, std::memory_order_relaxed);
    record_pacing(display.game_us);
    record_latency(display, gpuDoneUs, steady_us());
  } else
    g_displayFailures.fetch_add(1, std::memory_order_relaxed);

  g_displayCompleted.fetch_add(1, std::memory_order_release);
}

void* host_alloc(void*, unsigned bytes) {
  return std::malloc(bytes);
}

void host_free(void*, void* memory) {
  std::free(memory);
}

// Every program comes from the offline bank (docs/vita-shader-bank.md). A
// miss means the bank and the program sources disagree: development builds
// report it and compile at runtime, release builds (no compiler) stop.
const SceGxmProgram* find_binary(uint64_t hash, ShaderStage stage) {
  if (const auto* program = shader_bank::find(hash, stage))
    return program;
#ifdef MH3_DEVELOPMENT
  return runtime_shader_binary(hash, stage);
#else
  char message[96];
  std::snprintf(message, sizeof(message), "GXM shader bank has no program %016llx_%c", static_cast<unsigned long long>(hash), "vfr"[unsigned(stage)]);
  AURORA_THROW(std::runtime_error(message));
#endif
}
} // namespace

void check(int result, const char* operation) {
  if (result >= 0)
    return;

  char message[160];
  std::snprintf(message, sizeof(message), "GXM %s failed: %08x", operation, unsigned(result));
  AURORA_THROW(std::runtime_error(message));
}

const SceGxmProgram* vertex_binary(uint64_t hash) {
  return find_binary(hash, ShaderStage::Vertex);
}

const SceGxmProgram* fragment_binary(uint64_t hash) {
  return find_binary(hash, ShaderStage::Fragment);
}

const SceGxmProgram* fragment_scalar_binary(uint64_t hash) {
  return find_binary(hash, ShaderStage::ScalarFragment);
}

Device::Device() {
  // Our vertices read CPU-owned streams/uniforms, never a fragment result, so
  // scenes need no vertex/fragment serialization.
  SceGxmInitializeParams init{};

  // Stock sceGxmInitialize accepts only the affinity flags on firmware 3.65.
  // Query remaining CDRAM after its parameter-buffer allocation below.
  init.flags = SCE_GXM_INITIALIZE_FLAG_DEFAULT;

  // Admission allows three pending flips (gpu_display_queue_full). Keep one
  // extra internal queue entry: the callback publishes completion just before
  // libgxm retires its entry, so this headroom prevents even that bookkeeping
  // race from blocking AddEntry. It is a queue entry, not a fourth buffer.
  init.displayQueueMaxPendingCount = aurora::GpuFrameRing::BufferCount + 1;
  init.displayQueueCallback = display_callback;
  init.displayQueueCallbackDataSize = sizeof(DisplayData);
  init.parameterBufferSize = SCE_GXM_DEFAULT_PARAMETER_BUFFER_SIZE;
  razor_initialize();
  check(sceGxmInitialize(&init), "initialize");
  m_initialized = true;

  // The devkit runs the debug libgxm (more state checks per draw): its CPU
  // profiles overstate the library's share against a retail Vita.
  std::printf("Aurora GXM: %s libgxm\n", sceGxmIsDebugVersion() ? "debug" : "release");

  m_notification = sceGxmGetNotificationRegion();

  if (!m_notification)
    AURORA_THROW(std::runtime_error("GXM notification region unavailable"));
  // No scene has been submitted yet. Initialize our owned retirement word
  // even if the GXM notification region was retained from an earlier device.
  *m_notification = m_sceneSerial;
  g_displayQueued.store(0, std::memory_order_relaxed);
  g_displayCompleted.store(0, std::memory_order_relaxed);
  g_displayPresented.store(0, std::memory_order_relaxed);
  g_displayFailures.store(0, std::memory_order_relaxed);
  g_displayWaitMicros.store(0, std::memory_order_relaxed);
  g_frameDevice.store(this, std::memory_order_release);
  m_memory.reserve(7);
  auto allocate = [&](unsigned bytes, MappedMemory::Kind kind, const char* name) -> MappedMemory& {
    m_memory.emplace_back(bytes, kind, name);
    return m_memory.back();
  };

  auto& vdm = allocate(SCE_GXM_DEFAULT_VDM_RING_BUFFER_SIZE, MappedMemory::Kind::Draw, "Aurora VDM ring");
  auto& vertex = allocate(SCE_GXM_DEFAULT_VERTEX_RING_BUFFER_SIZE, MappedMemory::Kind::Draw, "Aurora vertex ring");
  auto& fragment = allocate(SCE_GXM_DEFAULT_FRAGMENT_RING_BUFFER_SIZE, MappedMemory::Kind::Draw, "Aurora fragment ring");
  auto& usse = allocate(SCE_GXM_DEFAULT_FRAGMENT_USSE_RING_BUFFER_SIZE, MappedMemory::Kind::FragmentUsse, "Aurora fragment USSE ring");
  m_host = std::malloc(SCE_GXM_MINIMUM_CONTEXT_HOST_MEM_SIZE);

  if (!m_host)
    AURORA_THROW(std::bad_alloc());

  SceGxmContextParams context{};
  context.hostMem = m_host;
  context.hostMemSize = SCE_GXM_MINIMUM_CONTEXT_HOST_MEM_SIZE;
  context.vdmRingBufferMem = vdm.data();
  context.vdmRingBufferMemSize = vdm.size();
  context.vertexRingBufferMem = vertex.data();
  context.vertexRingBufferMemSize = vertex.size();
  context.fragmentRingBufferMem = fragment.data();
  context.fragmentRingBufferMemSize = fragment.size();
  context.fragmentUsseRingBufferMem = usse.data();
  context.fragmentUsseRingBufferMemSize = usse.size();
  context.fragmentUsseRingBufferOffset = usse.usse_offset();
  check(sceGxmCreateContext(&context, &m_context), "create context");

  auto& buffer = allocate(4u << 20, MappedMemory::Kind::Draw, "Aurora shader patcher data");
  auto& vertexUsse = allocate(2u << 20, MappedMemory::Kind::VertexUsse, "Aurora vertex programs");
  auto& fragmentUsse = allocate(4u << 20, MappedMemory::Kind::FragmentUsse, "Aurora fragment programs");
  SceGxmShaderPatcherParams patcher{};
  patcher.hostAllocCallback = host_alloc;
  patcher.hostFreeCallback = host_free;
  patcher.bufferMem = buffer.data();
  patcher.bufferMemSize = buffer.size();
  patcher.vertexUsseMem = vertexUsse.data();
  patcher.vertexUsseMemSize = vertexUsse.size();
  patcher.vertexUsseOffset = vertexUsse.usse_offset();
  patcher.fragmentUsseMem = fragmentUsse.data();
  patcher.fragmentUsseMemSize = fragmentUsse.size();
  patcher.fragmentUsseOffset = fragmentUsse.usse_offset();
  check(sceGxmShaderPatcherCreate(&patcher, &m_patcher), "create shader patcher");

  SceKernelFreeMemorySizeInfo available{};
  available.size = sizeof(available);
  check(sceKernelGetFreeMemorySize(&available), "query memory partitions");

  // Three native draw slots: 10.5 MiB with unchanged live capacities, plus
  // numeric-ID, clip and utility buffers (0.75 MiB). Reserve these before
  // committing the texture/display arena. Compact models own separately
  // allocated CDRAM blocks: one quest's take 5-8 MiB, so keep 16 MiB for
  // them, the packs' budget on the Vita (MaxGPUBytes in model_assets.cpp;
  // a pack that still finds no CDRAM goes to main memory, renderer.cpp),
  // and leave 8 MiB for the driver.
  // Textures have no spill, and the old 32 MiB left them 31 MiB, which the
  // opening movie after character creation filled.
  constexpr unsigned producerReserve = NativeProducerReserve;
  constexpr unsigned modelReserve = 16u << 20;
  constexpr unsigned driverReserve = 8u << 20;

  if (available.size_cdram <= producerReserve + modelReserve + driverReserve)
    AURORA_THROW(std::runtime_error("Insufficient CDRAM for Aurora, producer slots and resident models"));

  const unsigned budget = (unsigned(available.size_cdram) - producerReserve - modelReserve - driverReserve) & ~0x3ffffu;
  m_arena = std::make_unique<CdramArena>(budget);
  std::fprintf(stderr, "Aurora GXM: CDRAM texture/display arena %.2f MiB, producer reservation %.2f MiB, model reservation %u MiB\n", budget / 1048576.0, producerReserve / 1048576.0, modelReserve >> 20);

  for (unsigned i = 0; i < aurora::GpuFrameRing::BufferCount; ++i) {
    m_displayData[i] = m_arena->allocate(Stride * Height * 4);

    if (!m_displayData[i])
      AURORA_THROW(std::bad_alloc());

    std::memset(m_displayData[i], 0, Stride * Height * 4);
    check(sceGxmColorSurfaceInit(&m_display[i], SCE_GXM_COLOR_FORMAT_U8U8U8U8_ABGR, SCE_GXM_COLOR_SURFACE_LINEAR, SCE_GXM_COLOR_SURFACE_SCALE_NONE, SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT, Width, Height,
                                 Stride, m_displayData[i]),
          "initialize display surface");
    check(sceGxmSyncObjectCreate(&m_sync[i]), "create display sync");
  }

}

Device::~Device() {
  release();
}

void Device::release() noexcept {
  if (m_inScene)
    sceGxmEndScene(m_context, nullptr, nullptr);

  m_inScene = false;

  if (m_context)
    sceGxmFinish(m_context);

  razor_stop();

  if (m_initialized)
    sceGxmDisplayQueueFinish();

  for (auto sync : m_sync)
    if (sync)
      sceGxmSyncObjectDestroy(sync);
  for (auto [size, target] : m_targets)
    sceGxmDestroyRenderTarget(target);

  m_targets.clear();

  if (m_patcher)
    sceGxmShaderPatcherDestroy(m_patcher);

  if (m_context)
    sceGxmDestroyContext(m_context);

  m_patcher = nullptr;
  m_context = nullptr;
  m_arena.reset();
  m_memory.clear();
  std::free(m_host);
  m_host = nullptr;
  g_frameDevice.store(nullptr, std::memory_order_release);

  if (m_initialized)
    sceGxmTerminate();

  razor_terminate();
  m_initialized = false;
#ifdef MH3_DEVELOPMENT
  clear_runtime_shaders();
#endif
}

void Device::begin(uint32_t width, uint32_t height, const SceGxmColorSurface* color, const SceGxmDepthStencilSurface* depth, SceGxmSyncObject* sync,
                   DepthScene depthScene, SceGxmMultisampleMode multisample) {
  end();
  const auto key = std::make_tuple(width, height, multisample);
  auto found = m_targets.find(key);

  if (found == m_targets.end()) {
    SceGxmRenderTargetParams params{};
    params.width = width;
    params.height = height;
    params.scenesPerFrame = 8;
    params.multisampleMode = multisample;
    params.driverMemBlock = -1;
    SceGxmRenderTarget* target = nullptr;
    check(sceGxmCreateRenderTarget(&params, &target), "create render target");
    found = m_targets.emplace(key, target).first;
  }

  SceGxmDepthStencilSurface disabledDepth{}, sceneDepth;

  if (!depth) {
    check(sceGxmDepthStencilSurfaceInitDisabled(&disabledDepth), "initialize disabled depth");
    sceGxmDepthStencilSurfaceSetBackgroundMask(&disabledDepth, SCE_TRUE);
    depth = &disabledDepth;
  } else if (!depthScene.load || !depthScene.store) {
    sceneDepth = *depth;

    if (!depthScene.load) {
      sceGxmDepthStencilSurfaceSetForceLoadMode(&sceneDepth, SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_DISABLED);
      sceGxmDepthStencilSurfaceSetBackgroundDepth(&sceneDepth, depthScene.background);
    }

    if (!depthScene.store)
      sceGxmDepthStencilSurfaceSetForceStoreMode(&sceneDepth, SCE_GXM_DEPTH_STENCIL_FORCE_STORE_DISABLED);

    depth = &sceneDepth;
  }

  // Firmware retains fragment ordering, including render-to-fragment-texture.
  // No fragment-to-vertex resource edge requires an explicit dependency.
  check(sceGxmBeginScene(m_context, 0, found->second, nullptr, nullptr, sync, color, depth), "begin scene");
  forget_raster_state();
  m_inScene = true;
  m_multisample = multisample;
}

void Device::end() {
  if (!m_inScene)
    return;

  const SceGxmNotification completion{m_notification, m_sceneSerial + 1};
  check(sceGxmEndScene(m_context, nullptr, &completion), "end scene");
  ++m_sceneSerial;

  // Offscreen work extends this frame slot's lease too. Other slots remain
  // available while its fragment notification is pending.
  m_frames.record_scene(m_notification, m_sceneSerial);
  m_inScene = false;
}

std::array<uint64_t, 2> finish_wait_counters() noexcept {
  return {g_finishMicros.load(std::memory_order_relaxed), g_finishCalls.load(std::memory_order_relaxed)};
}

void Device::finish() {
  const auto started = std::chrono::steady_clock::now();
  end();
  sceGxmFinish(m_context);
  const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count();
  g_finishMicros.fetch_add(micros, std::memory_order_relaxed);
  g_finishCalls.fetch_add(1, std::memory_order_relaxed);
}

void Device::begin_display() {
  const unsigned back = m_frames.slot();
  begin(Width, Height, &m_display[back], nullptr, m_sync[back]);
}

bool Device::frame_pending() const noexcept {
  return m_frames.pending() || aurora::gpu_display_queue_full(g_displayQueued.load(std::memory_order_acquire), g_displayCompleted.load(std::memory_order_acquire));
}

void set_vsync(bool enabled) noexcept {
  g_vsync.store(enabled, std::memory_order_relaxed);
  std::fprintf(stderr, "Aurora GXM: VSync %s\n", enabled ? "on (flips at VBlank)" : "off (flips at VBlank, not waited for)");
}

bool gpu_frame_pending() noexcept {
  const auto* device = g_frameDevice.load(std::memory_order_acquire);
  return device && device->frame_pending();
}

void set_flip_trace(bool enabled) noexcept {
  g_flipTrace.store(enabled, std::memory_order_relaxed);
}

int64_t repeated_vblanks() noexcept {
  return g_repeatedVblanks.load(std::memory_order_relaxed);
}

size_t take_flips(FlipSample* out, size_t capacity) noexcept {
  static uint64_t taken = 0;
  const uint64_t count = g_flipCount.load(std::memory_order_acquire);
  const uint64_t first = std::max({taken, count - std::min<uint64_t>(count, std::size(g_flips)), count - std::min<uint64_t>(count, capacity)});
  size_t n = 0;

  for (uint64_t i = first; i < count; ++i)
    out[n++] = g_flips[i % std::size(g_flips)];

  taken = count;
  return n;
}

DisplayStatistics display_statistics() noexcept {
  DisplayStatistics result;
  result.retired = g_displayCompleted.load(std::memory_order_acquire);
  result.queued = g_displayQueued.load(std::memory_order_acquire);
  result.presented = g_displayPresented.load(std::memory_order_relaxed);
  result.failures = g_displayFailures.load(std::memory_order_relaxed);
  result.callback_wait_us = g_displayWaitMicros.load(std::memory_order_relaxed);
  result.backlog = result.queued - result.retired;
  result.paced_flips = g_pacedFlips.load(std::memory_order_relaxed);
  result.pacing_error_us = g_pacingErrorMicros.load(std::memory_order_relaxed);
  result.pacing_error_max_us = g_pacingErrorMaxMicros.exchange(0, std::memory_order_relaxed);

  for (unsigned i = 0; i < 4; ++i)
    result.flip_vblanks[i] = g_flipVblanks[i].load(std::memory_order_relaxed);

  result.latency_flips = g_latencyFlips.load(std::memory_order_relaxed);

  for (unsigned i = 0; i < 4; ++i)
    result.latency_us[i] = g_latencyMicros[i].load(std::memory_order_relaxed);

  result.latency_max_us = g_latencyMaxMicros.exchange(0, std::memory_order_relaxed);

  const auto* device = g_frameDevice.load(std::memory_order_acquire);

  if (device)
    result.pending_scenes = device->pending_scenes();

  return result;
}

void Device::flip() {
  end();
  const unsigned back = m_frames.slot();

  if (g_commonDialogActive) {
    SceCommonDialogUpdateParam params{};
    params.renderTarget.colorSurfaceData = m_displayData[back];
    params.renderTarget.surfaceType = SCE_GXM_COLOR_SURFACE_LINEAR;
    params.renderTarget.colorFormat = SCE_GXM_COLOR_FORMAT_A8B8G8R8;
    params.renderTarget.width = Width;
    params.renderTarget.height = Height;
    params.renderTarget.strideInPixels = Stride;
    params.displaySyncObject = m_sync[back];
    check(sceCommonDialogUpdate(&params), "render common dialog");
  }

  check(sceGxmPadHeartbeat(&m_display[back], m_sync[back]), "display heartbeat");
  razor_frame();
  const DisplayData display{m_displayData[back], m_presentGameUs, m_presentInputUs, m_presentPublishedUs, m_presentInputUs ? steady_us() : 0};
  g_displayQueued.fetch_add(1, std::memory_order_release);
  check(sceGxmDisplayQueueAddEntry(m_sync[m_front], m_sync[back], &display), "queue display");
  m_front = back;
  m_frames.advance();
}

void set_common_dialog_active(bool active) noexcept {
  g_commonDialogActive = active;
}

namespace {
// Program creation on the render thread stalls the frame it happens in: report
// the slow ones (the process time is a cheap microsecond counter).
struct SlowProgramReport {
  const char* what;
  uint64_t hash;
  SceUInt64 start = sceKernelGetProcessTimeWide();
  ~SlowProgramReport() {
    if (const SceUInt64 elapsed = sceKernelGetProcessTimeWide() - start; elapsed >= 2000)
      std::fprintf(stderr, "MH3 GXM slow %s %016llx: %.1f ms\n", what, static_cast<unsigned long long>(hash), elapsed / 1000.0);
  }
};
} // namespace

PatchedProgram::PatchedProgram(Device& device, uint64_t vertexHash, uint64_t fragmentHash, SceGxmIndexFormat indices, std::span<const VertexInput> inputs)
: m_device(device), m_vertexBinary((SlowProgramReport{"vertex lookup", vertexHash}, vertex_binary(vertexHash))),
  m_fragmentBinary((SlowProgramReport{"fragment lookup", fragmentHash}, fragment_binary(fragmentHash))), m_fragmentHash(fragmentHash) {
  const SlowProgramReport report{"program patch", vertexHash};
  check(sceGxmProgramCheck(m_vertexBinary), "check vertex binary");
  check(sceGxmProgramCheck(m_fragmentBinary), "check fragment binary");
  check(sceGxmShaderPatcherRegisterProgram(device.patcher(), m_vertexBinary, &m_vertexId), "register vertex binary");
  check(sceGxmShaderPatcherRegisterProgram(device.patcher(), m_fragmentBinary, &m_fragmentId), "register fragment binary");
  SceGxmVertexAttribute attributes[16]{};
  SceGxmVertexStream streams[16]{{4, uint16_t(indices == SCE_GXM_INDEX_FORMAT_U16 ? SCE_GXM_INDEX_SOURCE_INDEX_16BIT : SCE_GXM_INDEX_SOURCE_INDEX_32BIT)},
                                 {4, uint16_t(indices == SCE_GXM_INDEX_FORMAT_U16 ? SCE_GXM_INDEX_SOURCE_INSTANCE_16BIT : SCE_GXM_INDEX_SOURCE_INSTANCE_32BIT)}};
  unsigned count = 0;
  unsigned streamCount = 2;

  if (!inputs.empty()) {
    streamCount = 0;
    std::fill(std::begin(streams), std::end(streams), SceGxmVertexStream{});

    for (const auto& input : inputs) {
      if (input.stream >= 16 || !input.stride || input.offset >= input.stride || !input.components || input.components > 4 || count == 16)
        AURORA_THROW(std::runtime_error("Native vertex input exceeds hardware layout limits"));

      const auto* parameter = sceGxmProgramFindParameterByName(m_vertexBinary, input.name);

      if (!parameter)
        continue; // Compiler removed an unused attribute.
      if (sceGxmProgramParameterGetCategory(parameter) != SCE_GXM_PARAMETER_CATEGORY_ATTRIBUTE)
        AURORA_THROW(std::runtime_error("Native vertex input is not an attribute"));

      auto& stream = streams[input.stream];

      if (stream.stride && stream.stride != input.stride)
        AURORA_THROW(std::runtime_error("Native vertex stream has conflicting strides"));

      stream.stride = input.stride;
      stream.indexSource = indices == SCE_GXM_INDEX_FORMAT_U16 ? SCE_GXM_INDEX_SOURCE_INDEX_16BIT : SCE_GXM_INDEX_SOURCE_INDEX_32BIT;
      streamCount = std::max(streamCount, unsigned(input.stream) + 1);
      auto& attribute = attributes[count++];
      attribute.streamIndex = input.stream;
      attribute.offset = input.offset;
      attribute.format = input.format;
      attribute.componentCount = input.components;
      attribute.regIndex = sceGxmProgramParameterGetResourceIndex(parameter);
    }

    // GXM requires every attribute the program reads to be bound; GL feeds
    // absent ones the constant (0, 0, 0, 1). Instance-indexed, a plain draw
    // reads element 0 of the defaults stream for every vertex.
    for (unsigned i = 0, total = sceGxmProgramGetParameterCount(m_vertexBinary); i < total; ++i) {
      const auto* parameter = sceGxmProgramGetParameter(m_vertexBinary, i);

      if (sceGxmProgramParameterGetCategory(parameter) != SCE_GXM_PARAMETER_CATEGORY_ATTRIBUTE)
        continue;

      const unsigned reg = sceGxmProgramParameterGetResourceIndex(parameter);

      if (std::any_of(attributes, attributes + count, [&](const SceGxmVertexAttribute& bound) { return bound.regIndex == reg; }))
        continue;

      if (count == 16 || streamCount == 16)
        AURORA_THROW(std::runtime_error("Native vertex defaults exceed hardware layout limits"));

      if (m_defaultStream < 0) {
        m_defaultStream = int(streamCount++);
        streams[m_defaultStream] = {16, uint16_t(indices == SCE_GXM_INDEX_FORMAT_U16 ? SCE_GXM_INDEX_SOURCE_INSTANCE_16BIT : SCE_GXM_INDEX_SOURCE_INSTANCE_32BIT)};
      }

      auto& attribute = attributes[count++];
      attribute.streamIndex = uint16_t(m_defaultStream);
      attribute.format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
      attribute.componentCount = uint8_t(std::clamp(sceGxmProgramParameterGetComponentCount(parameter), 1u, 4u));
      attribute.regIndex = uint16_t(reg);
    }
  } else
    for (unsigned stream = 0; stream < 2; ++stream) {
      const auto* parameter = sceGxmProgramFindParameterByName(m_vertexBinary, stream ? "instance_id" : "vertex_id");

      if (!parameter)
        continue;

      auto& attribute = attributes[count++];
      attribute.streamIndex = stream;
      attribute.format = SCE_GXM_ATTRIBUTE_FORMAT_F32;

      // Sony reflects a whole PA register (four components) for this scalar
      // input. The actual producer stream contains one float ID per vertex.
      attribute.componentCount = 1;
      attribute.regIndex = sceGxmProgramParameterGetResourceIndex(parameter);
    }
  if (const int error = sceGxmShaderPatcherCreateVertexProgram(device.patcher(), m_vertexId, attributes, count, streams, streamCount, &m_vertex); error < 0) {
    char text[96];
    std::snprintf(text, sizeof(text), "GXM patch vertex program %016llx failed: %08x;", static_cast<unsigned long long>(vertexHash), unsigned(error));
    std::string layout = text;

    for (unsigned i = 0; i < count; ++i) {
      const auto& a = attributes[i];
      std::snprintf(text, sizeof(text), " [stream %u offset %u format %u x%u reg %u stride %u]", a.streamIndex, a.offset, unsigned(a.format), a.componentCount, a.regIndex,
                    streams[a.streamIndex].stride);
      layout += text;
    }

    AURORA_THROW(std::runtime_error(layout));
  }
}

PatchedProgram::~PatchedProgram() {
  release();
}

void PatchedProgram::release() noexcept {
  for (auto [key, program] : m_fragments)
    sceGxmShaderPatcherReleaseFragmentProgram(m_device.patcher(), program);

  if (m_vertex)
    sceGxmShaderPatcherReleaseVertexProgram(m_device.patcher(), m_vertex);

  if (m_fragmentId)
    sceGxmShaderPatcherUnregisterProgram(m_device.patcher(), m_fragmentId);

  if (m_scalarFragmentId)
    sceGxmShaderPatcherUnregisterProgram(m_device.patcher(), m_scalarFragmentId);

  if (m_vertexId)
    sceGxmShaderPatcherUnregisterProgram(m_device.patcher(), m_vertexId);
}

void PatchedProgram::bind(const SceGxmBlendInfo& blend, SceGxmOutputRegisterFormat output) {
  const bool scalar = output == SCE_GXM_OUTPUT_REGISTER_FORMAT_FLOAT;

  if (scalar && (blend.colorFunc != SCE_GXM_BLEND_FUNC_NONE || !(blend.colorMask & SCE_GXM_COLOR_MASK_R)))
    AURORA_THROW(std::runtime_error("Scalar F32 target requires an unblended red-channel write"));

  if (scalar && !m_scalarFragmentId) {
    m_scalarFragmentBinary = fragment_scalar_binary(m_fragmentHash);
    check(sceGxmProgramCheck(m_scalarFragmentBinary), "check scalar fragment binary");
    check(sceGxmShaderPatcherRegisterProgram(m_device.patcher(), m_scalarFragmentBinary, &m_scalarFragmentId), "register scalar fragment binary");
  }

  m_boundFragmentBinary = scalar ? m_scalarFragmentBinary : m_fragmentBinary;
  uint32_t blendKey;
  std::memcpy(&blendKey, &blend, sizeof(blendKey));
  const SceGxmMultisampleMode multisample = m_device.scene_multisample();
  const uint64_t key = uint64_t(blendKey) | (uint64_t(output) << 32) | (uint64_t(multisample) << 48);

  // Blend state is part of a GXM fragment program: one patched variant per
  // blend/output state, created once. Consecutive draws mostly repeat it.
  if (m_lastFragment && m_lastKey == key) {
    sceGxmSetVertexProgram(m_device.context(), m_vertex);
    sceGxmSetFragmentProgram(m_device.context(), m_lastFragment);
    m_vertexDefault = nullptr;
    m_fragmentDefault = nullptr;
    return;
  }

  auto found = m_fragments.find(key);

  if (found == m_fragments.end()) {
    SceGxmFragmentProgram* fragment = nullptr;

    // Explicit scalar output cannot use the runtime half4 conversion/blend
    // patch. F32 surfaces have only R, so an unblended R write needs no mask.
    const SlowProgramReport report{"fragment patch", m_fragmentHash};
    check(sceGxmShaderPatcherCreateFragmentProgram(m_device.patcher(), scalar ? m_scalarFragmentId : m_fragmentId, scalar ? SCE_GXM_OUTPUT_REGISTER_FORMAT_DECLARED : output,
                                                   multisample,
                                                   scalar ? nullptr : &blend, m_vertexBinary, &fragment),
          "patch fragment program");
    found = m_fragments.emplace(key, fragment).first;
  }

  m_lastKey = key;
  m_lastFragment = found->second;
  sceGxmSetVertexProgram(m_device.context(), m_vertex);
  sceGxmSetFragmentProgram(m_device.context(), found->second);
  m_vertexDefault = nullptr;
  m_fragmentDefault = nullptr;
}

void PatchedProgram::set_vertex_uniform(const char* name, const float* values, unsigned count) {
  const auto* parameter = sceGxmProgramFindParameterByName(m_vertexBinary, name);

  if (!parameter)
    return;

  if (!m_vertexDefault)
    check(sceGxmReserveVertexDefaultUniformBuffer(m_device.context(), &m_vertexDefault), "reserve vertex defaults");

  check(sceGxmSetUniformDataF(m_vertexDefault, parameter, 0, count, values), "set vertex default");
}

void PatchedProgram::set_fragment_uniform(const char* name, const float* values, unsigned count) {
  const auto* parameter = sceGxmProgramFindParameterByName(m_boundFragmentBinary, name);

  if (!parameter)
    return;

  if (!m_fragmentDefault)
    check(sceGxmReserveFragmentDefaultUniformBuffer(m_device.context(), &m_fragmentDefault), "reserve fragment defaults");

  check(sceGxmSetUniformDataF(m_fragmentDefault, parameter, 0, count, values), "set fragment default");
}
} // namespace aurora::gxm
