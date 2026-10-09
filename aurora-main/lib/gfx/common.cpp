#include "common.hpp"
#include "../gx/shader_info.hpp"

#include "clear.hpp"
#include "depth_peek.hpp"
#include "efb_ram_copy.hpp"
#include "../internal.hpp"
#include "../gx/pipeline.hpp"
#include "pipeline_cache.hpp"
#include "tex_copy_conv.hpp"
#include "tex_palette_conv.hpp"
#include "texture_replacement.hpp"
#include "texture.hpp"
#include "port_capture.hpp"
#include "../window.hpp"
#ifdef AURORA_GXM
#include "../gxm/memory.hpp"
#include "../gxm/draw_memory.hpp"
#endif

#include <algorithm>
#include <atomic>
#include <bit>
#include <condition_variable>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>

#include <absl/container/flat_hash_map.h>
#include <magic_enum.hpp>

#include "tracy/Tracy.hpp"

namespace aurora::gfx {
static Module Log("aurora::gfx");

struct DebugFrameData {
#ifdef AURORA_GFX_DEBUG_GROUPS
  std::vector<std::string> groups;
  std::vector<std::string> markers;
#endif
};
DebugFrameData g_debugFrame;

struct ShaderDrawCommand {
  ShaderType type;
  union {
    clear::DrawData clear;
    gx::DrawData gx;
  };
};
enum class CommandType {
  SetViewport,
  SetScissor,
  Draw,
  DebugMarker,
};
struct Command {
  CommandType type;
#ifdef AURORA_GFX_DEBUG_GROUPS
  std::vector<std::string> debugGroupStack;
#endif
  union Data {
    Viewport setViewport;
    ClipRect setScissor;
    ShaderDrawCommand draw;
    size_t debugMarkerIndex;
  } data;
};
} // namespace aurora::gfx

void aurora_set_guest_write_hooks(AuroraGuestWriteGenerationCallback generation,
                                  AuroraGuestWriteNotifyCallback notify) {
  aurora::g_guestWriteGenerationHook = generation;
  aurora::g_guestWriteNotifyHook = notify;
}

namespace aurora::gfx {
namespace {
struct CachedBindGroup {
  std::shared_ptr<const TextureBindGroup> bindGroup;
  uint32_t lastUsedFrame = 0;
};

constexpr uint32_t BindGroupCacheRetainFrames = 32;
constexpr uint32_t BindGroupCacheSweepPeriod = 16;

// Word buffers are R32UI textures of this many 32-bit words per row.
constexpr uint32_t WordBufferRowWords = 2048;
constexpr uint32_t WordBufferRowBytes = WordBufferRowWords * 4;
constexpr uint32_t word_buffer_rows(uint64_t bytes) noexcept {
  return static_cast<uint32_t>((bytes + WordBufferRowBytes - 1) / WordBufferRowBytes);
}
} // namespace

static absl::flat_hash_map<BindGroupRef, CachedBindGroup> g_cachedBindGroups;
// Bind groups dropped from the cache while recorded draws may still reference them. They travel
// with the next submitted frame and are released once the render thread has executed it.
static std::vector<std::shared_ptr<const TextureBindGroup>> g_retiredBindGroups;

// Recording buffers of one staging batch. A small pool bounds how far the producer can run ahead
// of the render thread, which returns each set once it has uploaded it.
struct StagingSet {
#ifdef AURORA_GXM
  // These owners precede the borrowed ByteBuffer views so they are freed last.
  std::array<gxm::MappedMemory, 4> memory;
#endif
  ByteBuffer verts;
  ByteBuffer uniforms;
  ByteBuffer indices;
  ByteBuffer storage;
};
constexpr size_t FrameSlotCount = 3;
static std::mutex g_stagingPoolMutex;
static std::condition_variable g_stagingPoolCv;
static std::vector<std::unique_ptr<StagingSet>> g_stagingPool;
static std::unique_ptr<StagingSet> g_staging;
static ByteBuffer& g_verts() noexcept { return g_staging->verts; }
static ByteBuffer& g_uniforms() noexcept { return g_staging->uniforms; }
static ByteBuffer& g_indices() noexcept { return g_staging->indices; }
static ByteBuffer& g_storage() noexcept { return g_staging->storage; }
static size_t currentStagingBuffer = 0;
static uint32_t g_uniformAlignment = 256;
constexpr uint32_t StorageAlignment = 16;
// Advanced once per logical frame in the seal prologue, under the renderer GPU mutex and with the
// producer blocked, so every later reader sees a value that no longer moves.
static uint32_t g_frameIndex = UINT32_MAX;

// Render thread: the GPU copies of the most recently uploaded batch. The word buffers rotate so a
// new batch never overwrites a texture that queued draws still read.
struct GpuBuffers {
  GLuint uniform = 0;
  GLuint index = 0;
  std::array<gl::TexturePtr, FrameSlotCount> verts;
  std::array<gl::TexturePtr, FrameSlotCount> storage;
  size_t slot = 0;
#ifdef AURORA_GXM
  std::unique_ptr<StagingSet> active;
#endif
};
static GpuBuffers g_gpu;

// for imgui debug
AuroraStats g_stats{};
uint32_t g_drawCallCount = 0;
uint32_t g_mergedDrawCallCount = 0;

using CommandList = std::vector<Command>;
struct RenderPass {
  gl::TexturePtr color;
  gl::TexturePtr depth;
  Extent targetSize;
  uint32_t msaaSamples = 1;

  TextureHandle resolveTarget;
  TextureHandle resolveSourceSnapshot;
  GXTexFmt resolveFormat = GX_TF_RGBA8;
  ClipRect resolveRect;
  ClipRect resolveSnapshotRect;
  Vec4<float> resolveSourceRect;
  Range resolveUniformRange;
  std::array<u32, 3> resolveCopyFilterCoefficients{0, 64, 0};
  Vec4<float> clearColorValue{0.f, 0.f, 0.f, 0.f};
  // 1.f is the forward-Z "farthest" clear value; under UseReversedZ farthest is 0.f instead (see
  // gx::clear_depth_value(), which the main render pass explicitly overrides this default with -
  // any OTHER pass that keeps this default, e.g. an offscreen render-to-texture pass composited
  // later, needs the same reversed-Z-aware value or its depth buffer starts "already nearest",
  // failing every subsequent depth test and making whatever's drawn into it vanish).
  float clearDepthValue = gx::UseReversedZ ? 0.f : 1.f;
  CommandList commands;
  bool clearColor = true;
  bool clearDepth = true;
  // The resolve destination outlives the frame with no re-issue path (one-shot
  // bake), so this pass may not drop draws even in skip-unready-pipelines mode.
  bool requireReadyPipelines = false;
  bool resolveHalfScale = false;
  bool resolveCopyFilterActive = false;
  bool resolveForceOpaqueAlpha = false;
  bool resolveNeedsConversion = false;
  bool resolveNeedsShaderSampling = false;
  bool resolveLinearSampling = false;
  bool snapshotColorResolveSource = false;
  std::vector<tex_palette_conv::ConvRequest> paletteConvs;
};
static std::vector<RenderPass> g_renderPasses;
static u32 g_currentRenderPass = UINT32_MAX;

// Recycle command storage: discarding passes used to free their command lists too, so each frame
// rebuilt hundreds of KB from zero capacity. The passes themselves are cheap to recreate.
using CommandListPool = std::vector<CommandList>;
static CommandListPool g_commandListPool;
static constexpr size_t MaxPooledCommandLists = 32;
// The pool is the only recording storage both sides touch, a few pointer moves per frame, so a
// plain mutex is cheaper than the alternatives and keeps the vector's invariants.
static std::mutex g_commandListPoolMutex;

static CommandList acquire_command_list() noexcept {
  std::lock_guard lock{g_commandListPoolMutex};
  if (g_commandListPool.empty()) {
    return {};
  }
  CommandList list = std::move(g_commandListPool.back());
  g_commandListPool.pop_back();
  list.clear();
  return list;
}

static void release_command_list(CommandList&& list) noexcept {
  if (list.capacity() == 0) {
    return;
  }
  std::lock_guard lock{g_commandListPoolMutex};
  if (g_commandListPool.size() >= MaxPooledCommandLists) {
    return;
  }
  list.clear();
  g_commandListPool.emplace_back(std::move(list));
}

static void release_render_pass(RenderPass& pass) noexcept { release_command_list(std::move(pass.commands)); }

static void recycle_render_passes(std::vector<RenderPass>& passes) noexcept {
  for (auto& pass : passes) {
    release_render_pass(pass);
  }
  passes.clear();
}

struct SealedFrameData {
  depth_peek::FrameMapping depthMapping;
  bool skipDraws = false;
  DebugFrameData debug;
  std::vector<RenderPass> passes;
  // Released after the render thread has executed `passes`.
  std::vector<std::shared_ptr<const TextureBindGroup>> retiredBindGroups;
};

SealedFrame::SealedFrame() : m_data(std::make_unique<SealedFrameData>()) {}
SealedFrame::~SealedFrame() = default;
SealedFrame::SealedFrame(SealedFrame&&) noexcept = default;
SealedFrame& SealedFrame::operator=(SealedFrame&&) noexcept = default;

static RenderPass& push_render_pass(RenderPass&& pass) {
  auto& out = g_renderPasses.emplace_back(std::move(pass));
  if (out.commands.capacity() == 0) {
    out.commands = acquire_command_list();
  }
  return out;
}
// The producer may build the next frame's GX stream while the worker prepares the target, and
// this is the only renderer-owned property that path queries, so publish it explicitly.
static std::atomic_bool g_inOffscreen{false};
static std::optional<RenderPass> g_suspendedEfbPass;
static Viewport g_suspendedEfbViewport;
static ClipRect g_suspendedEfbScissor;
// Prefix referenced by a suspended EFB pass. Preserve its offsets across an
// offscreen split, without rendering it before the bake it may sample finishes.
static StagingSizes g_suspendedEfbBytes{};
static constexpr StagingSizes PhysicalStagingCapacity{
    VertexBufferSize, UniformBufferSize, IndexBufferSize, StorageBufferSize};
static StagingSizes g_stagingCapacity = PhysicalStagingCapacity;
static uint64_t g_stagingEpoch = 0;
static uint64_t g_stagingSplitCount = 0;
static StagingSizes g_stagingHighWater{};

StagingSizes staging_usage() noexcept {
  if (!g_staging) {
    return {};
  }
  return {g_verts().size(), g_uniforms().size(), g_indices().size(), g_storage().size()};
}
StagingSizes staging_high_water() noexcept { return g_stagingHighWater; }
uint64_t staging_epoch() noexcept { return g_stagingEpoch; }
uint64_t staging_split_count() noexcept { return g_stagingSplitCount; }
uint64_t staging_uniform_bytes(uint64_t bytes) { return staging_padded(bytes, g_uniformAlignment); }
uint64_t staging_storage_bytes(uint64_t bytes) { return staging_padded(bytes, StorageAlignment); }
void set_staging_capacity_limits_for_testing(const StagingSizes& limits) {
  for (unsigned i = 0; i < limits.size(); ++i) {
    if (limits[i] > PhysicalStagingCapacity[i])
      throw StagingCapacityError("Test staging capacity exceeds physical buffer");
  }
  g_stagingCapacity = limits;
  g_stagingHighWater = {};
}
bool staging_has_space(const StagingSizes& demand) {
  // Async readback preparation runs in the worker's noexcept seal prologue.
  // Reserve all 32 slots plus the uniform binding's 3840-byte trailing window.
  const StagingSizes tail{0, gx::MaxUniformSize + efb_ram::MaxAsyncReadbackSlots * staging_uniform_bytes(48), 0, 0};
  const StagingSizes retained = g_suspendedEfbPass ? g_suspendedEfbBytes : StagingSizes{};
  if (!staging_fits(retained, demand, tail, g_stagingCapacity))
    throw StagingCapacityError("GPU operation exceeds staging capacity including retained EFB data");
  return staging_fits(staging_usage(), demand, tail, g_stagingCapacity);
}
void ensure_staging_space(const StagingSizes& demand) {
  if (staging_has_space(demand)) return;
  split_staging_batch();
  if (!staging_has_space(demand))
    throw StagingCapacityError("GPU operation still exceeds staging capacity after submission");
}

static void discard_suspended_efb_pass() noexcept {
  if (g_suspendedEfbPass) {
    release_render_pass(*g_suspendedEfbPass);
    g_suspendedEfbPass.reset();
  }
}

static bool has_current_render_pass() noexcept { return g_currentRenderPass < g_renderPasses.size(); }
static gl::TexturePtr g_offscreenColor;
static gl::TexturePtr g_offscreenDepth;

struct ResolveSourceSnapshotPool {
  TextureHandle entry;
};

// Snapshot resources live in the same safe frame slots as the mapped staging buffers, and copies
// within a frame are ordered, so one grow-only snapshot serves every resolve in that slot.
static std::array<ResolveSourceSnapshotPool, FrameSlotCount> g_resolveSourceSnapshotPools;
static size_t g_recordingSnapshotSlot = 0;

static TextureHandle new_resolve_source_snapshot(Extent size) noexcept {
  return std::make_shared<TextureRef>(
      gl::create_texture(size.width, size.height, 1, gl::Format::RGBA8, "GX Copy Source Snapshot"), size,
      gl::Format::RGBA8, 1, GX_TF_RGBA8);
}

static TextureHandle acquire_resolve_source_snapshot(uint32_t width, uint32_t height) noexcept {
  auto& pool = g_resolveSourceSnapshotPools[g_recordingSnapshotSlot];
  if (pool.entry && pool.entry->size.width >= width && pool.entry->size.height >= height) {
    return pool.entry;
  }
  const Extent size{
      std::max(width, pool.entry ? pool.entry->size.width : 0u),
      std::max(height, pool.entry ? pool.entry->size.height : 0u),
      1,
  };
  pool.entry = new_resolve_source_snapshot(size);
  return pool.entry;
}

struct ResolveSamplingPlan {
  bool needsConversion = false;
  bool needsShaderSampling = false;
  bool usesLinearSampling = false;
};

static ResolveSamplingPlan make_resolve_sampling_plan(const TextureHandle& target, GXTexFmt format,
                                                       const ClipRect& resolveRect, const Vec4<float>& sourceRect,
                                                       bool halfScale, bool copyFilterActive,
                                                       bool forceOpaqueAlpha) noexcept {
  const auto differs = [](float lhs, float rhs) { return std::abs(lhs - rhs) > 0.01f; };
  const auto differsFromDst = [&](uint32_t dst, float src) {
    return differs(static_cast<float>(dst), src);
  };
  const bool sourceMatchesIntegerRect =
      !differs(sourceRect.x(), static_cast<float>(resolveRect.x)) &&
      !differs(sourceRect.y(), static_cast<float>(resolveRect.y)) &&
      !differs(sourceRect.z(), static_cast<float>(resolveRect.width)) &&
      !differs(sourceRect.w(), static_cast<float>(resolveRect.height));
  const bool dstMatchesSource = target && !differsFromDst(target->size.width, sourceRect.z()) &&
                                !differsFromDst(target->size.height, sourceRect.w());
  const bool needsShaderSampling =
      halfScale || copyFilterActive || forceOpaqueAlpha || !sourceMatchesIntegerRect || !dstMatchesSource;
  return {
      .needsConversion = tex_copy_conv::needs_conversion(format),
      .needsShaderSampling = needsShaderSampling,
      .usesLinearSampling = halfScale || (needsShaderSampling && !copyFilterActive),
  };
}

static ClipRect calculate_resolve_snapshot_rect(const Extent& targetSize,
                                                const Vec4<float>& sourceRect,
                                                const ResolveSamplingPlan& samplingPlan,
                                                bool copyFilterActive) noexcept {
  const ClipRect fullTarget{
      .x = 0,
      .y = 0,
      .width = static_cast<int32_t>(targetSize.width),
      .height = static_cast<int32_t>(targetSize.height),
  };
  // Shader-sampled and converted copies can feed effects that read outside the copy rectangle
  // (MKW's DOF chain), so keep those full-target. Exact copies have a closed, crop-safe footprint.
  if (samplingPlan.needsConversion ||
      samplingPlan.needsShaderSampling ||
      targetSize.width == 0 || targetSize.height == 0 ||
      !std::isfinite(sourceRect.x()) || !std::isfinite(sourceRect.y()) ||
      !std::isfinite(sourceRect.z()) || !std::isfinite(sourceRect.w()) ||
      sourceRect.z() <= 0.0f || sourceRect.w() <= 0.0f) {
    return fullTarget;
  }

  // Linear sampling can touch one neighbor on every edge. The vertical GX
  // copy filter explicitly samples the previous and next rows.
  const int32_t haloX = samplingPlan.usesLinearSampling ? 1 : 0;
  const int32_t haloY = (samplingPlan.usesLinearSampling || copyFilterActive) ? 1 : 0;
  const int32_t targetWidth = static_cast<int32_t>(targetSize.width);
  const int32_t targetHeight = static_cast<int32_t>(targetSize.height);
  const int32_t left = std::clamp(static_cast<int32_t>(std::floor(sourceRect.x())) - haloX, 0, targetWidth - 1);
  const int32_t top = std::clamp(static_cast<int32_t>(std::floor(sourceRect.y())) - haloY, 0, targetHeight - 1);
  const int32_t right = std::clamp(static_cast<int32_t>(std::ceil(sourceRect.x() + sourceRect.z())) + haloX,
                                   left + 1, targetWidth);
  const int32_t bottom = std::clamp(static_cast<int32_t>(std::ceil(sourceRect.y() + sourceRect.w())) + haloY,
                                    top + 1, targetHeight);
  return {
      .x = left,
      .y = top,
      .width = right - left,
      .height = bottom - top,
  };
}

static void set_efb_targets(RenderPass& pass) {
  pass.color = gpu::g_frameBuffer.texture;
  pass.depth = gpu::g_depthBuffer.texture;
  pass.targetSize = {gpu::g_frameBuffer.width, gpu::g_frameBuffer.height, 1};
  pass.msaaSamples = 1;
}

struct OffscreenCacheKey {
  uint32_t width;
  uint32_t height;

  bool operator==(const OffscreenCacheKey& rhs) const { return width == rhs.width && height == rhs.height; }
  template <typename H>
  friend H AbslHashValue(H h, const OffscreenCacheKey& key) {
    return H::combine(std::move(h), key.width, key.height);
  }
};
struct OffscreenCacheEntry {
  gl::TexturePtr color;
  gl::TexturePtr depth;
};
static absl::flat_hash_map<OffscreenCacheKey, OffscreenCacheEntry> g_offscreenCache;

static inline void push_command(CommandType type, const Command::Data& data) {
  if (!has_current_render_pass())
    UNLIKELY {
      Log.warn("Dropping command {}", magic_enum::enum_name(type));
      return;
    }
  g_renderPasses[g_currentRenderPass].commands.push_back({
      .type = type,
#ifdef AURORA_GFX_DEBUG_GROUPS
      .debugGroupStack = g_debugFrame.groups,
#endif
      .data = data,
  });
}

template <>
gx::DrawData* get_last_draw_command() {
  if (g_currentRenderPass >= g_renderPasses.size()) {
    return nullptr;
  }
  auto& last = g_renderPasses[g_currentRenderPass].commands.back();
  if (last.type != CommandType::Draw || last.data.draw.type != ShaderType::GX) {
    return nullptr;
  }
  return &last.data.draw.gx;
}

static void push_draw_command(ShaderDrawCommand data) {
  push_command(CommandType::Draw, Command::Data{.draw = data});
  ++g_drawCallCount;
}

Vec2<uint32_t> get_frame_buffer_size() noexcept {
  if (gpu::g_frameBuffer.width != 0 && gpu::g_frameBuffer.height != 0) {
    return {gpu::g_frameBuffer.width, gpu::g_frameBuffer.height};
  }
  const auto windowSize = window::get_window_size();
  return {windowSize.fb_width, windowSize.fb_height};
}

// Render-thread only: the frame worker recycles g_renderPasses in end_frame, so an off-thread
// read is a use-after-free rather than a stale value. Use get_frame_buffer_size() instead.
Vec2<uint32_t> get_render_target_size() noexcept {
  if (g_currentRenderPass < g_renderPasses.size()) {
    const auto& size = g_renderPasses[g_currentRenderPass].targetSize;
    return {size.width, size.height};
  }
  return get_frame_buffer_size();
}

static Viewport g_cachedViewport;
void set_viewport(const Viewport& cmd) noexcept {
  if (cmd != g_cachedViewport) {
    push_command(CommandType::SetViewport, Command::Data{.setViewport = cmd});
    g_cachedViewport = cmd;
  }
}

static ClipRect g_cachedScissor;
void set_scissor(const ClipRect& cmd) noexcept {
  if (cmd != g_cachedScissor) {
    push_command(CommandType::SetScissor, Command::Data{.setScissor = cmd});
    g_cachedScissor = cmd;
  }
}

template <>
void push_draw_command(clear::DrawData data) {
  push_draw_command(ShaderDrawCommand{.type = ShaderType::Clear, .clear = data});
}

void resolve_pass(TextureHandle texture, ClipRect rect, bool clearColor, bool clearAlpha, bool clearDepth,
                  Vec4<float> clearColorValue, float clearDepthValue, GXTexFmt resolveFormat,
                  const Vec4<float>* sourceRectPixels, bool halfScale,
                  const std::array<u32, 3>* copyFilterCoefficients, bool forceOpaqueAlpha,
                  float copyFilterRowStride, bool clampTop, bool clampBottom, bool persistentCopy) {
  // Resolve current render pass
  if (!has_current_render_pass()) {
    Log.warn("Dropping resolve pass without an active render pass");
    return;
  }
  ensure_staging_space({0, 2 * staging_uniform_bytes(48), 0, 0});
  auto& prevPass = g_renderPasses[g_currentRenderPass];
  const auto targetWidth = static_cast<int32_t>(prevPass.targetSize.width);
  const auto targetHeight = static_cast<int32_t>(prevPass.targetSize.height);
  if (targetWidth <= 0 || targetHeight <= 0) {
    Log.warn("Dropping resolve pass with invalid target size {}x{}", targetWidth, targetHeight);
    return;
  }
  Vec4<float> sourceRect = sourceRectPixels != nullptr
                               ? *sourceRectPixels
                               : Vec4<float>{static_cast<float>(rect.x), static_cast<float>(rect.y),
                                             static_cast<float>(rect.width), static_cast<float>(rect.height)};
  if (targetWidth > 0 && targetHeight > 0) {
    const int32_t left = std::clamp(rect.x, 0, targetWidth - 1);
    const int32_t top = std::clamp(rect.y, 0, targetHeight - 1);
    const int32_t right = std::clamp(rect.x + rect.width, left + 1, targetWidth);
    const int32_t bottom = std::clamp(rect.y + rect.height, top + 1, targetHeight);
    rect = {
        .x = left,
        .y = top,
        .width = right - left,
        .height = bottom - top,
    };

    const float srcW = static_cast<float>(targetWidth);
    const float srcH = static_cast<float>(targetHeight);
    const float srcLeft = std::clamp(sourceRect.x(), 0.0f, srcW);
    const float srcTop = std::clamp(sourceRect.y(), 0.0f, srcH);
    const float srcRight = std::clamp(sourceRect.x() + sourceRect.z(), srcLeft, srcW);
    const float srcBottom = std::clamp(sourceRect.y() + sourceRect.w(), srcTop, srcH);
    sourceRect = {srcLeft, srcTop, std::max(srcRight - srcLeft, 1.0f), std::max(srcBottom - srcTop, 1.0f)};
  }
  prevPass.resolveTarget = std::move(texture);
  prevPass.requireReadyPipelines |= persistentCopy;
  prevPass.resolveRect = rect;
  prevPass.resolveSourceRect = sourceRect;
  prevPass.resolveFormat = resolveFormat;
  prevPass.resolveHalfScale = halfScale;
  prevPass.resolveForceOpaqueAlpha = forceOpaqueAlpha;
  prevPass.resolveCopyFilterCoefficients =
      copyFilterCoefficients != nullptr ? *copyFilterCoefficients : std::array<u32, 3>{0, 64, 0};
  prevPass.resolveCopyFilterActive = prevPass.resolveCopyFilterCoefficients[0] != 0 ||
                                     prevPass.resolveCopyFilterCoefficients[1] != 64 ||
                                     prevPass.resolveCopyFilterCoefficients[2] != 0;
  const auto samplingPlan = make_resolve_sampling_plan(
      prevPass.resolveTarget, resolveFormat, rect, sourceRect, halfScale,
      prevPass.resolveCopyFilterActive, forceOpaqueAlpha);
  prevPass.resolveNeedsConversion = samplingPlan.needsConversion;
  prevPass.resolveNeedsShaderSampling = samplingPlan.needsShaderSampling;
  prevPass.resolveLinearSampling = samplingPlan.usesLinearSampling;
  prevPass.snapshotColorResolveSource =
      !gx::is_depth_format(resolveFormat) && (clearColor || clearAlpha || clearDepth);
  Vec4<float> uniformSourceRect = sourceRect;
  float srcW = static_cast<float>(prevPass.targetSize.width);
  float srcH = static_cast<float>(prevPass.targetSize.height);
  if (prevPass.snapshotColorResolveSource) {
    prevPass.resolveSnapshotRect = calculate_resolve_snapshot_rect(
        prevPass.targetSize, sourceRect, samplingPlan, prevPass.resolveCopyFilterActive);
    prevPass.resolveSourceSnapshot = acquire_resolve_source_snapshot(
        static_cast<uint32_t>(prevPass.resolveSnapshotRect.width),
        static_cast<uint32_t>(prevPass.resolveSnapshotRect.height));
    uniformSourceRect = {
        sourceRect.x() - static_cast<float>(prevPass.resolveSnapshotRect.x),
        sourceRect.y() - static_cast<float>(prevPass.resolveSnapshotRect.y),
        sourceRect.z(), sourceRect.w()};
    srcW = static_cast<float>(prevPass.resolveSourceSnapshot->size.width);
    srcH = static_cast<float>(prevPass.resolveSourceSnapshot->size.height);
  }
  // GX's copy-clamp bits pin every vertical filter tap to the first or last source texel; without
  // it a filtered copy at internal resolution samples an unrelated EFB row as a visible border.
  const float clampTopPixels = clampTop ? uniformSourceRect.y() : 0.0f;
  const float clampBottomPixels =
      clampBottom ? uniformSourceRect.y() + uniformSourceRect.w() : srcH;
  const float clampTopUv = (clampTopPixels + 0.5f) / srcH;
  const float clampBottomUv = (clampBottomPixels - 0.5f) / srcH;
  // Push UV transform uniform for tex_copy_conv (crop region in UV space)
  const std::array resolveUniform{
      uniformSourceRect.x() / srcW,
      uniformSourceRect.y() / srcH,
      uniformSourceRect.z() / srcW,
      uniformSourceRect.w() / srcH,
      static_cast<float>(prevPass.resolveCopyFilterCoefficients[0]),
      static_cast<float>(prevPass.resolveCopyFilterCoefficients[1]),
      static_cast<float>(prevPass.resolveCopyFilterCoefficients[2]),
      prevPass.resolveCopyFilterActive ? 1.0f : 0.0f,
      prevPass.resolveForceOpaqueAlpha ? 1.0f : 0.0f,
      std::max(copyFilterRowStride, 1.0f),
      clampTopUv,
      clampBottomUv,
  };
  prevPass.resolveUniformRange = push_uniform(resolveUniform);
  const bool clearFullTarget = rect.x <= 0 && rect.y <= 0 &&
                               rect.width >= static_cast<int32_t>(prevPass.targetSize.width) &&
                               rect.height >= static_cast<int32_t>(prevPass.targetSize.height);
  const bool useAttachmentColorClear = clearFullTarget && clearColor && clearAlpha;
  const bool useAttachmentDepthClear = clearFullTarget && clearDepth;

  // Populate new render pass from previous
  const auto msaaSamples = prevPass.msaaSamples;
  RenderPass newPass{
      .color = prevPass.color,
      .depth = prevPass.depth,
      .targetSize = prevPass.targetSize,
      .msaaSamples = msaaSamples,
      .clearColorValue = clearColorValue,
      .clearDepthValue = clearDepthValue,
      .clearColor = useAttachmentColorClear,
      .clearDepth = useAttachmentDepthClear,
  };
  push_render_pass(std::move(newPass));
  ++g_currentRenderPass;

  if ((!useAttachmentColorClear && (clearColor || clearAlpha)) || (!useAttachmentDepthClear && clearDepth)) {
    // GX copy clears cover the copied EFB rectangle, not always the whole target, so use a scissored
    // clear draw unless the load op can clear all of it.
    push_draw_command(clear::DrawData{
        .color = clearColorValue,
        .depth = clearDepthValue,
        .clearColor = clearColor,
        .clearAlpha = clearAlpha,
        .clearDepth = clearDepth,
        .useScissor = !clearFullTarget,
        .scissor = rect,
    });
  }
  push_command(CommandType::SetViewport, Command::Data{.setViewport = g_cachedViewport});
  push_command(CommandType::SetScissor, Command::Data{.setScissor = g_cachedScissor});
}

void queue_palette_conv(tex_palette_conv::ConvRequest req) {
  if (!has_current_render_pass()) {
    Log.warn("Dropping palette conversion without an active render pass");
    return;
  }
  g_renderPasses[g_currentRenderPass].paletteConvs.push_back(std::move(req));
}

bool is_offscreen() noexcept { return g_inOffscreen; }

uint32_t get_sample_count() noexcept {
  if (!has_current_render_pass()) {
    return 1;
  }
  return g_renderPasses[g_currentRenderPass].msaaSamples;
}

void clear_caches() noexcept {
  g_offscreenCache.clear();
  // Retire rather than free; see g_retiredBindGroups.
  g_retiredBindGroups.reserve(g_retiredBindGroups.size() + g_cachedBindGroups.size());
  for (auto& entry : g_cachedBindGroups) {
    g_retiredBindGroups.emplace_back(std::move(entry.second.bindGroup));
  }
  g_cachedBindGroups.clear();
}

static OffscreenCacheEntry get_offscreen_textures(uint32_t width, uint32_t height) {
  OffscreenCacheKey key{width, height};
  if (const auto it = g_offscreenCache.find(key); it != g_offscreenCache.end()) {
    return it->second;
  }
  OffscreenCacheEntry entry{
      .color = gl::create_texture(width, height, 1, gl::Format::RGBA8, "Offscreen Color"),
      .depth = gl::create_texture(width, height, 1, gl::Format::Depth32F, "Offscreen Depth"),
  };
  auto [insertIt, _] = g_offscreenCache.emplace(key, std::move(entry));
  return insertIt->second;
}

void begin_offscreen(uint32_t width, uint32_t height) {
  ZoneScoped;
  CHECK(g_currentRenderPass != UINT32_MAX, "begin_offscreen called outside of a frame");

  // If the current EFB pass has no resolve target, its output is unobservable.
  // Suspend it so that we can resume it after the offscreen pass.
  if (!g_inOffscreen) {
    auto& currentPass = g_renderPasses[g_currentRenderPass];
    if (!currentPass.resolveTarget) {
      g_suspendedEfbBytes = staging_usage();
      g_suspendedEfbPass = std::move(currentPass);
      g_renderPasses.pop_back();
      --g_currentRenderPass;
    }
    g_suspendedEfbViewport = g_cachedViewport;
    g_suspendedEfbScissor = g_cachedScissor;
  }

  // Create offscreen textures
  auto offscreenEntry = get_offscreen_textures(width, height);
  g_offscreenColor = std::move(offscreenEntry.color);
  g_offscreenDepth = std::move(offscreenEntry.depth);

  // Start a new pass with offscreen targets
  RenderPass newPass{
      .color = g_offscreenColor,
      .depth = g_offscreenDepth,
      .targetSize = {width, height, 1},
      .msaaSamples = 1,
      .clearColorValue = {0.f, 0.f, 0.f, 0.f},
      // See the RenderPass::clearDepthValue default's comment: this offscreen pass gets its own
      // depth buffer, and the farthest clear value is 0.f, not 1.f, under UseReversedZ.
      .clearDepthValue = gx::UseReversedZ ? 0.f : 1.f,
      .clearColor = true,
      .clearDepth = true,
  };
  push_render_pass(std::move(newPass));
  ++g_currentRenderPass;

  g_inOffscreen = true;

  g_cachedViewport = {0.f, 0.f, static_cast<float>(width), static_cast<float>(height), 0.f, 1.f};
  g_cachedScissor = {0, 0, static_cast<int32_t>(width), static_cast<int32_t>(height)};
  push_command(CommandType::SetViewport, Command::Data{.setViewport = g_cachedViewport});
  push_command(CommandType::SetScissor, Command::Data{.setScissor = g_cachedScissor});
}

void end_offscreen() {
  ZoneScoped;
  CHECK(g_inOffscreen, "end_offscreen called without begin_offscreen");

  g_inOffscreen = false;
  g_offscreenColor.reset();
  g_offscreenDepth.reset();

  // Resume suspended EFB pass, or start a new one (load existing content)
  if (g_suspendedEfbPass) {
    // Keeps its own recorded commands.
    g_renderPasses.emplace_back(std::move(*g_suspendedEfbPass));
    g_suspendedEfbPass.reset();
  } else {
    auto& pass = push_render_pass(RenderPass{});
    pass.clearColor = false;
    pass.clearDepth = false;
  }
  ++g_currentRenderPass;
  set_efb_targets(g_renderPasses[g_currentRenderPass]);

  g_cachedViewport = g_suspendedEfbViewport;
  g_cachedScissor = g_suspendedEfbScissor;
  push_command(CommandType::SetViewport, Command::Data{.setViewport = g_cachedViewport});
  push_command(CommandType::SetScissor, Command::Data{.setScissor = g_cachedScissor});
}

template <>
void push_draw_command(gx::DrawData data) {
  push_draw_command(ShaderDrawCommand{.type = ShaderType::GX, .gx = data});
}

template <>
PipelineRef pipeline_ref(const gx::PipelineConfig& config) {
  return find_pipeline(ShaderType::GX, config, [=] { return create_pipeline(config); });
}

static std::unique_ptr<StagingSet> new_staging_set() {
  auto set = std::make_unique<StagingSet>();
#ifdef AURORA_GXM
  size_t allocation = 0;
  const auto reserve = [&](ByteBuffer& buffer, uint64_t size) {
    auto& memory = set->memory[allocation++];
    memory = gxm::MappedMemory(size, gxm::MappedMemory::Kind::Draw, "Aurora direct draw slot");
    buffer = ByteBuffer{static_cast<uint8_t*>(memory.data()), static_cast<size_t>(size)};
  };
#else
  const auto reserve = [](ByteBuffer& buffer, uint64_t size) {
    buffer = ByteBuffer{static_cast<size_t>(size)};
    buffer.clear();
  };
#endif
  reserve(set->verts, VertexBufferSize + WordBufferRowBytes);
  // The trailing uniform window pads the batch; see end_batch_impl.
  reserve(set->uniforms, UniformBufferSize + gx::MaxUniformSize);
  reserve(set->indices, IndexBufferSize);
  // Word buffers upload whole rows.
  reserve(set->storage, StorageBufferSize + WordBufferRowBytes);
  return set;
}

static void release_staging_set(std::unique_ptr<StagingSet> set) noexcept {
  set->verts.clear();
  set->uniforms.clear();
  set->indices.clear();
  set->storage.clear();
  {
    std::lock_guard lock(g_stagingPoolMutex);
    g_stagingPool.push_back(std::move(set));
  }
  g_stagingPoolCv.notify_one();
}

// Blocks until the render thread has uploaded an older batch and returned its set.
static void acquire_staging_set() {
  if (g_staging) {
    return;
  }
  ZoneScopedN("Wait for staging set");
  std::unique_lock lock(g_stagingPoolMutex);
  g_stagingPoolCv.wait(lock, [] { return !g_stagingPool.empty(); });
  g_staging = std::move(g_stagingPool.back());
  g_stagingPool.pop_back();
}

// Render thread: copy a finished batch into the GPU buffers the following draws read.
static void upload_batch(StagingSet& set) {
  ZoneScopedN("Upload staging batch");
#ifdef AURORA_GXM
  // No upload or word textures: native Cg reads explicit uniform buffers.
  gxm::bind_draw_memory(set.uniforms.data(), set.uniforms.size(), set.indices.data(), set.indices.size(),
                        set.verts.data(), set.verts.size(), set.storage.data(), set.storage.size());
#else
  auto& gpu = g_gpu;
  gpu.slot = (gpu.slot + 1) % FrameSlotCount;
  if (!set.uniforms.empty()) {
    // Orphan the store so draws still queued on the previous contents keep them.
    // Size it to this batch (already padded by MaxUniformSize): orphaning the full
    // 24 MB made drivers such as llvmpipe allocate and zero-fault it every batch.
    gl::write_buffer(gpu.uniform, false, set.uniforms.data(), set.uniforms.size());
  }
  if (!set.indices.empty()) {
    gl::bind_draw_vao(gpu.index);
    gl::write_buffer(gpu.index, true, set.indices.data(), set.indices.size());
  }
  const auto uploadWords = [](const gl::TexturePtr& texture, ByteBuffer& buffer) {
    if (buffer.empty()) {
      return;
    }
    const uint32_t rows = word_buffer_rows(buffer.size());
    const size_t padded = static_cast<size_t>(rows) * WordBufferRowBytes;
    if (padded > buffer.size()) {
      buffer.append_zeroes(padded - buffer.size());
    }
    gl::upload_texture(*texture, 0, 0, 0, WordBufferRowWords, rows, buffer.data(), WordBufferRowBytes);
  };
  uploadWords(gpu.verts[gpu.slot], set.verts);
  uploadWords(gpu.storage[gpu.slot], set.storage);
#endif
}

void initialize() {
  g_frameIndex = 0;
  g_uniformAlignment = static_cast<uint32_t>(std::max(gl::caps().uniformOffsetAlignment, 256));
  depth_peek::initialize();
  tex_copy_conv::initialize();
  tex_palette_conv::initialize();
  texture_replacement::initialize();

  {
    std::lock_guard lock(g_stagingPoolMutex);
    g_stagingPool.clear();
    for (size_t i = 0; i < FrameSlotCount; ++i) {
      g_stagingPool.push_back(new_staging_set());
    }
  }
  g_staging.reset();
  currentStagingBuffer = 0;
#ifndef AURORA_GXM
  for (size_t i = 0; i < FrameSlotCount; ++i) {
    g_gpu.verts[i] = gl::create_texture(WordBufferRowWords, word_buffer_rows(VertexBufferSize) + 1, 1,
                                        gl::Format::R32UI, "Vertex word buffer");
    g_gpu.storage[i] = gl::create_texture(WordBufferRowWords, word_buffer_rows(StorageBufferSize) + 1, 1,
                                          gl::Format::R32UI, "Storage word buffer");
  }
#endif
  gl::post([] {
    g_gpu.uniform = gl::create_buffer();
    g_gpu.index = gl::create_buffer();
    gl::bind_draw_vao(g_gpu.index);
  });

  gx::initialize();
  initialize_pipeline_cache();
}

void shutdown() {
  shutdown_pipeline_cache();
  gx::clear_shader_module_cache();
  efb_ram::shutdown();
  depth_peek::shutdown();
  tex_copy_conv::shutdown();
  tex_palette_conv::shutdown();
  texture_replacement::shutdown();
  gx::shutdown();

  gl::finish();
#ifdef AURORA_GXM
  gl::call([] { g_gpu.active.reset(); });
#endif
  g_cachedBindGroups.clear();
  g_retiredBindGroups.clear();
  g_staging.reset();
  {
    std::lock_guard lock(g_stagingPoolMutex);
    g_stagingPool.clear();
  }
  for (auto& pool : g_resolveSourceSnapshotPools) {
    pool.entry.reset();
  }
  discard_suspended_efb_pass();
  g_renderPasses.clear();
  g_commandListPool.clear();
  g_currentRenderPass = UINT32_MAX;
  g_offscreenCache.clear();
  g_offscreenColor.reset();
  g_offscreenDepth.reset();
  g_inOffscreen = false;
  g_frameIndex = UINT32_MAX;
  currentStagingBuffer = 0;
  gl::call([] {
    gl::destroy_buffer(g_gpu.uniform);
    gl::destroy_buffer(g_gpu.index);
    g_gpu = {};
  });
}

static bool g_preserveNextEfb = false;
void preserve_next_efb_frame() noexcept { g_preserveNextEfb = true; }

static bool begin_frame_impl(bool clearEfb, bool capacityResume = false) {
  ZoneScoped;
  acquire_staging_set();
  ++g_stagingEpoch;
  g_recordingSnapshotSlot = currentStagingBuffer;

  g_drawCallCount = 0;
  g_mergedDrawCallCount = 0;
  if (clearEfb) {
    gx::begin_frame_interpolation();
  }
  discard_suspended_efb_pass();
  if (!capacityResume) gpu::clear_present_source_override();

  push_render_pass(RenderPass{});
  set_efb_targets(g_renderPasses[0]);
  g_renderPasses[0].clearColorValue = gx::g_gxState.clearColor;
  g_renderPasses[0].clearDepthValue = gx::clear_depth_value();
  g_renderPasses[0].clearColor = clearEfb;
  g_renderPasses[0].clearDepth = clearEfb;
  g_currentRenderPass = 0;
  // Refresh paired render viewport/scissor from logical state in case the FB size changed.
  const auto mappedRenderState = gx::map_logical_render_state();
  const bool renderStateChanged = gx::g_gxState.renderViewport != mappedRenderState.viewport ||
                                  gx::g_gxState.renderScissor != mappedRenderState.scissor;
  gx::g_gxState.renderViewport = mappedRenderState.viewport;
  gx::g_gxState.renderScissor = mappedRenderState.scissor;
  gx::g_gxState.stateDirty = gx::g_gxState.stateDirty || renderStateChanged;
  g_cachedViewport = mappedRenderState.viewport;
  g_cachedScissor = mappedRenderState.scissor;
  push_command(CommandType::SetViewport, Command::Data{.setViewport = g_cachedViewport});
  push_command(CommandType::SetScissor, Command::Data{.setScissor = g_cachedScissor});
  begin_pipeline_frame();
  return true;
}

// A GX display copy's clear flag only affects the next frame's EFB; MH3 relies on
// GXDrawDone keeping the EFB intact, so the embedder can request one preserved frame.
static bool g_frameskip = false;
static bool g_mustRender = false;
static int g_skipDecision = -1; // -1: not decided yet for this frame
static std::atomic<uint32_t> g_framesInFlight{0};
static std::atomic<uint64_t> g_skippedFrames{0};

void set_frameskip(bool enabled) noexcept { g_frameskip = enabled; }
void require_frame_render() noexcept { g_mustRender = true; }
bool frame_skipped() noexcept {
  if (g_skipDecision < 0) {
    g_skipDecision = g_frameskip && !g_mustRender && g_framesInFlight.load(std::memory_order_acquire) > 0;
  }
  return g_skipDecision == 1;
}
void count_skipped_frame() noexcept { g_skippedFrames.fetch_add(1, std::memory_order_relaxed); }
void frame_render_posted() noexcept { g_framesInFlight.fetch_add(1, std::memory_order_release); }
void frame_render_finished() noexcept { g_framesInFlight.fetch_sub(1, std::memory_order_release); }
uint64_t skipped_frame_count() noexcept { return g_skippedFrames.load(std::memory_order_relaxed); }

bool begin_frame() {
  // A new guest frame: the previous one was sealed, so reset the skip decision.
  g_skipDecision = -1;
  g_mustRender = false;
  const bool clear = !g_preserveNextEfb;
  g_preserveNextEfb = false;
  return begin_frame_impl(clear);
}

bool resume_frame() { return begin_frame_impl(false); }

void abort_frame() noexcept {
  efb_ram::cancel();
  if (g_staging) {
    // Pending interpolation tasks hold raw pointers into the staging buffers.
    gx::drop_pending_frame_interpolation_uniforms();
    release_staging_set(std::move(g_staging));
    currentStagingBuffer = (currentStagingBuffer + 1) % FrameSlotCount;
  }
  recycle_render_passes(g_renderPasses);
  g_currentRenderPass = UINT32_MAX;
  discard_suspended_efb_pass();
  g_inOffscreen = false;
  for (auto& array : gx::g_gxState.arrays) {
    array.cachedRange = {};
  }
  gpu::clear_present_source_override();
  end_pipeline_frame();
}

static void end_batch_impl(bool advanceFrame) {
  ZoneScoped;
  ASSERT(!advanceFrame || !g_inOffscreen, "end_frame called while offscreen rendering is active");
  if (advanceFrame) {
    gx::finalize_frame_interpolation();
  } else {
    // Mid-frame batch split: the staging buffers are about to be handed off, so pending
    // interpolation tasks pointing into them would dangle.
    gx::drop_pending_frame_interpolation_uniforms();
  }
  if (g_staging) {
    g_uniforms().append_zeroes(gx::MaxUniformSize); // Pad the end of the buffer
    const auto used = staging_usage();
    for (unsigned i = 0; i < used.size(); ++i) g_stagingHighWater[i] = std::max(g_stagingHighWater[i], used[i]);
    g_stats.drawCallCount = g_drawCallCount;
    g_stats.mergedDrawCallCount = g_mergedDrawCallCount;
    g_stats.lastVertSize = g_verts().size();
    g_stats.lastUniformSize = g_uniforms().size();
    g_stats.lastIndexSize = g_indices().size();
    g_stats.lastStorageSize = g_storage().size();
    port_capture::frame(g_frameIndex, g_drawCallCount, g_mergedDrawCallCount,
                        static_cast<uint32_t>(g_renderPasses.size()), g_verts().size(), g_uniforms().size(),
                        g_indices().size(), g_storage().size());
    gl::post([set = std::move(g_staging)]() mutable {
#ifdef AURORA_GXM
      // A queued scene still reads the previous slot. Synchronize before
      // recycling, then retain this slot across all interpolation replays.
      gl::finish();
      if (g_gpu.active) release_staging_set(std::move(g_gpu.active));
      upload_batch(*set);
      g_gpu.active = std::move(set);
#else
      upload_batch(*set);
      release_staging_set(std::move(set));
#endif
    });
  }
  currentStagingBuffer = (currentStagingBuffer + 1) % FrameSlotCount;
  g_currentRenderPass = UINT32_MAX;
  for (auto& array : gx::g_gxState.arrays) {
    array.cachedRange = {};
  }
  end_pipeline_frame();
  if (advanceFrame) {
    ++g_frameIndex;
  }
}

void end_frame() { end_batch_impl(true); }

void end_batch() { end_batch_impl(false); }

// Hands the passes recorded so far to the render thread, after the batch holding their data.
static void submit_recorded_passes() {
  auto frame = std::make_shared<SealedFrame>();
  auto& data = frame->data();
  data.depthMapping = depth_peek::capture_frame_mapping();
  data.skipDraws = frame_skipped();
  data.passes.swap(g_renderPasses);
  data.retiredBindGroups.swap(g_retiredBindGroups);
#ifdef AURORA_GFX_DEBUG_GROUPS
  data.debug.groups = g_debugFrame.groups;
  data.debug.markers.swap(g_debugFrame.markers);
#endif
  gl::post([frame = std::move(frame)] {
    render(*frame, -1, true);
    after_submit();
  });
  g_currentRenderPass = UINT32_MAX;
  expire_bind_group_cache();
}

void flush_recording() {
  end_batch();
  submit_recorded_passes();
}

void split_staging_batch() {
  // Never called under the decoder's renderer lock. FIFO admission yields its unconsumed command first.
  std::lock_guard gpuLock(aurora::renderer_gpu_mutex());
  if (!has_current_render_pass())
    throw StagingCapacityError("Cannot split staging outside an active render pass");
  gx::mark_frame_interpolation_replay_unsafe();
  const bool offscreen = g_inOffscreen;
  const auto viewport = g_cachedViewport;
  const auto scissor = g_cachedScissor;
  const auto renderViewport = gx::g_gxState.renderViewport;
  const auto renderScissor = gx::g_gxState.renderScissor;
  const auto& active = g_renderPasses[g_currentRenderPass];
  RenderPass continuation{
      .color = active.color,
      .depth = active.depth,
      .targetSize = active.targetSize,
      .msaaSamples = active.msaaSamples,
      .clearColor = false,
      .clearDepth = false,
      .requireReadyPipelines = active.requireReadyPipelines || offscreen,
  };
  auto suspended = std::move(g_suspendedEfbPass);
  g_suspendedEfbPass.reset();
  std::array<std::vector<uint8_t>, 4> retained;
  std::array<ByteBuffer*, 4> buffers{&g_verts(), &g_uniforms(), &g_indices(), &g_storage()};
  if (suspended) {
    for (unsigned i = 0; i < buffers.size(); ++i) {
      if (g_suspendedEfbBytes[i])
        retained[i].assign(buffers[i]->data(), buffers[i]->data() + g_suspendedEfbBytes[i]);
    }
  }
  // GXCopyTex can capture the ordinary EFB as well as an explicit offscreen
  // target. Its command may arrive in the next batch, after this prefix has
  // already been submitted. Preserve every prefix; target kind cannot tell
  // us whether the guest will later retain these pixels in a texture.
  for (auto& pass : g_renderPasses) pass.requireReadyPipelines = true;
  flush_recording();
  if (!begin_frame_impl(false, true))
    throw StagingCapacityError("Staging remap failed after capacity submission");
  recycle_render_passes(g_renderPasses);
  push_render_pass(std::move(continuation));
  g_currentRenderPass = 0;
  g_suspendedEfbPass = std::move(suspended);
  std::array<ByteBuffer*, 4> fresh{&g_verts(), &g_uniforms(), &g_indices(), &g_storage()};
  for (unsigned i = 0; i < fresh.size(); ++i) {
    if (!retained[i].empty()) fresh[i]->append(retained[i].data(), retained[i].size());
  }
  g_inOffscreen = offscreen;
  g_cachedViewport = viewport;
  g_cachedScissor = scissor;
  gx::g_gxState.renderViewport = renderViewport;
  gx::g_gxState.renderScissor = renderScissor;
  gx::g_gxState.stateDirty = true;
  push_command(CommandType::SetViewport, Command::Data{.setViewport = viewport});
  push_command(CommandType::SetScissor, Command::Data{.setScissor = scissor});
  ++g_stagingSplitCount;
}

uint32_t current_frame() noexcept { return g_frameIndex; }

// The only place that erases from g_cachedBindGroups, whose handles the frame being encoded still
// holds, so it runs in the seal prologue with the renderer mutex held and the producer excluded.
void expire_bind_group_cache() noexcept {
  if (g_cachedBindGroups.empty() || g_frameIndex == UINT32_MAX || g_frameIndex % BindGroupCacheSweepPeriod != 0) {
    return;
  }

  ZoneScoped;
  for (auto it = g_cachedBindGroups.begin(); it != g_cachedBindGroups.end();) {
    if (g_frameIndex - it->second.lastUsedFrame > BindGroupCacheRetainFrames) {
      g_retiredBindGroups.emplace_back(std::move(it->second.bindGroup));
      g_cachedBindGroups.erase(it++);
    } else {
      ++it;
    }
  }
}

// Debug labels for the render passes. Formatting them per pass per slot costs a heap-allocating
// fmt::format for a name nothing reads outside a capture.
static const char* render_pass_label(u32 index) noexcept {
  static constexpr std::array<const char*, 16> kRenderPassLabels{
      "Render pass 0",  "Render pass 1",  "Render pass 2",  "Render pass 3",
      "Render pass 4",  "Render pass 5",  "Render pass 6",  "Render pass 7",
      "Render pass 8",  "Render pass 9",  "Render pass 10", "Render pass 11",
      "Render pass 12", "Render pass 13", "Render pass 14", "Render pass 15",
  };
  return index < kRenderPassLabels.size() ? kRenderPassLabels[index] : "Render pass";
}

static void render_pass_impl(const std::vector<RenderPass>& passes, u32 idx, int32_t interpolatedFrame,
                             DebugFrameData& debugFrame);

// Render thread: copy a rectangle of `source` (level 0) into `dest` at the given offset.
static void copy_texture_region(GLuint source, GLuint dest, int32_t srcX, int32_t srcY, int32_t dstX, int32_t dstY,
                                int32_t width, int32_t height) {
  gl::copy_texture_region(source, dest, srcX, srcY, dstX, dstY, width, height);
}

static void render_impl(std::vector<RenderPass>& renderPasses, int32_t interpolatedFrame, bool finalize,
                        DebugFrameData& debugFrame, const depth_peek::FrameMapping& depthMapping,
                        bool skipDraws = false) {
  ZoneScoped;
  // Palette conversions and EFB copies depend on sealed frame state, not on the interpolation
  // weight, so run them on the native render only.
  const bool encodeTextureBakes = interpolatedFrame < 0;
  for (u32 i = 0; i < renderPasses.size(); ++i) {
    const auto& passInfo = renderPasses[i];
    if (encodeTextureBakes) {
      for (const auto& conv : passInfo.paletteConvs) {
        tex_palette_conv::run(conv);
      }
    }
    if (skipDraws) {
      // Skipped frame: keep the palette conversions above, drop draws and resolves.
      continue;
    }
    const bool hasRenderWork = passInfo.clearColor || passInfo.clearDepth || !passInfo.commands.empty();
    if (i == renderPasses.size() - 1) {
      ASSERT(!passInfo.resolveTarget, "Final render pass must not have resolve target");
    } else if (!(passInfo.resolveTarget && encodeTextureBakes) && !hasRenderWork) {
      // Skip only empty intermediate passes: offscreen and scratch passes with resolves still have to
      // run for later samplers.
      continue;
    }
    if (!passInfo.color) {
      continue;
    }

    gl::bind_framebuffer(passInfo.color->gl(), passInfo.depth ? passInfo.depth->gl() : 0);
    if (passInfo.clearColor || passInfo.clearDepth) {
      gl::set_scissor_enabled(false);
      if (passInfo.clearColor) {
        gl::set_color_mask(true, true, true, true);
        const std::array color{passInfo.clearColorValue.x(), passInfo.clearColorValue.y(),
                               passInfo.clearColorValue.z(), passInfo.clearColorValue.w()};
        gl::clear_color(color.data());
      }
      if (passInfo.clearDepth && passInfo.depth) {
        gl::set_depth_write(true);
        gl::clear_depth(passInfo.clearDepthValue);
      }
    }
    render_pass_impl(renderPasses, i, interpolatedFrame, debugFrame);

    if (finalize && i == renderPasses.size() - 1) {
      depth_peek::encode_frame_snapshot(passInfo.depth, passInfo.targetSize.width, passInfo.targetSize.height,
                                        depthMapping);
    }

    if (passInfo.resolveTarget) {
      const bool isDepth = gx::is_depth_format(passInfo.resolveFormat);
      gl::TexturePtr resolveSource = isDepth ? passInfo.depth : passInfo.color;
      int32_t sourceOffsetX = 0;
      int32_t sourceOffsetY = 0;
      if (passInfo.snapshotColorResolveSource && passInfo.resolveSourceSnapshot) {
        copy_texture_region(passInfo.color->gl(), passInfo.resolveSourceSnapshot->texture->gl(),
                            passInfo.resolveSnapshotRect.x, passInfo.resolveSnapshotRect.y, 0, 0,
                            passInfo.resolveSnapshotRect.width, passInfo.resolveSnapshotRect.height);
        resolveSource = passInfo.resolveSourceSnapshot->texture;
        sourceOffsetX = passInfo.resolveSnapshotRect.x;
        sourceOffsetY = passInfo.resolveSnapshotRect.y;
      }
      const tex_copy_conv::ConvRequest convReq{
          .fmt = passInfo.resolveFormat,
          .src = resolveSource,
          .uniformRange = passInfo.resolveUniformRange,
          .dst = passInfo.resolveTarget,
          .sampleFilter = passInfo.resolveLinearSampling ? tex_copy_conv::SampleFilter::Linear
                                                         : tex_copy_conv::SampleFilter::Nearest,
          .forceOpaqueAlpha = passInfo.resolveForceOpaqueAlpha,
      };
      if (passInfo.resolveNeedsConversion) {
        tex_copy_conv::run(convReq);
      } else if (passInfo.resolveNeedsShaderSampling || isDepth) {
        tex_copy_conv::blit(convReq);
      } else {
        copy_texture_region(resolveSource->gl(), passInfo.resolveTarget->texture->gl(),
                            passInfo.resolveRect.x - sourceOffsetX, passInfo.resolveRect.y - sourceOffsetY, 0, 0,
                            passInfo.resolveRect.width, passInfo.resolveRect.height);
      }
    }
  }
  if (finalize) {
    recycle_render_passes(renderPasses);
  }

#if defined(AURORA_GFX_DEBUG_GROUPS)
  if (finalize && !debugFrame.groups.empty()) {
    for (auto& it : std::ranges::reverse_view(debugFrame.groups)) {
      Log.warn("Debug group was not popped at end of frame: {}", it);
    }
    debugFrame.groups.clear();
  }

  if (finalize && debugFrame.markers.size() > 0) {
    debugFrame.markers.clear();
  }
#endif
}

// Experimental CPU scheduling at the detached-command boundary. Do not cross
// viewport/scissor, clear, copy/pass, blend, or depth-state dependencies. This
// groups pass types, not textures: coplanar order between categories still needs
// visual validation before enabling it by default.
static void group_cutout_draws(std::vector<RenderPass>& passes) {
  static const bool enabled = [] {
    const char* value = std::getenv("AURORA_GROUP_CUTOUTS");
    return value && std::strcmp(value, "1") == 0;
  }();
  if (!enabled) return;
  ZoneScopedN("CPU cutout grouping");
  const auto pipeline = [](const Command& command) -> const Pipeline* {
    if (command.type != CommandType::Draw || command.data.draw.type != ShaderType::GX) return nullptr;
    const auto* p = find_pipeline_object(command.data.draw.gx.pipeline);
    if (!p || !p->program || p->program->replaces_depth()) return nullptr;
    const auto& state = p->raster;
    if (state.blend || !state.depthTest || !state.depthWrite || !state.colorMask[0] ||
        !state.colorMask[1] || !state.colorMask[2]) return nullptr;
    if (state.depthFunc != GL_LESS && state.depthFunc != GL_LEQUAL &&
        state.depthFunc != GL_GREATER && state.depthFunc != GL_GEQUAL) return nullptr;
    return p;
  };
  uint32_t regions = 0, draws = 0, transitionsBefore = 0, transitionsAfter = 0;
  for (auto& pass : passes) {
    auto& commands = pass.commands;
    for (size_t begin = 0; begin < commands.size();) {
      const auto* first = pipeline(commands[begin]);
      if (!first) { ++begin; continue; }
      size_t end = begin + 1;
      bool lastDiscard = first->program->uses_discard();
      bool anyDiscard = lastDiscard, anyOpaque = !lastDiscard;
      uint32_t transitions = 0;
      while (end < commands.size()) {
        const auto* next = pipeline(commands[end]);
        if (!next || next->raster.depthFunc != first->raster.depthFunc ||
            next->raster.colorMask != first->raster.colorMask) break;
        const bool discard = next->program->uses_discard();
        transitions += discard != lastDiscard;
        lastDiscard = discard;
        anyDiscard |= discard;
        anyOpaque |= !discard;
        ++end;
      }
      if (anyDiscard && anyOpaque) {
        // Command's tagged union is constructible but not assignable. Copy only
        // this eligible region, retaining original order within each category.
        CommandList ordered;
        ordered.reserve(end - begin);
        for (const bool discard : {false, true}) {
          for (size_t i = begin; i < end; ++i) {
            if (pipeline(commands[i])->program->uses_discard() == discard)
              ordered.push_back(commands[i]);
          }
        }
        for (size_t i = begin; i < end; ++i) {
          std::destroy_at(&commands[i]);
          std::construct_at(&commands[i], std::move(ordered[i - begin]));
        }
        ++regions;
        draws += end - begin;
        transitionsBefore += transitions;
        ++transitionsAfter;
      }
      begin = end;
    }
  }
  port_capture::schedule(g_frameIndex, regions, draws, transitionsBefore, transitionsAfter);
}

void seal_frame(SealedFrame& out) noexcept {
  out.data().depthMapping = depth_peek::capture_frame_mapping();
  out.data().skipDraws = frame_skipped();
  ZoneScoped;
  auto& passes = out.data().passes;
  recycle_render_passes(passes);
  passes.swap(g_renderPasses);
  if (!out.data().skipDraws) group_cutout_draws(passes);
  if (!out.data().skipDraws && port_capture::enabled()) {
    for (u32 i = 0; i < passes.size(); ++i) {
      const auto& pass = passes[i];
      port_capture::pass(g_frameIndex, i, pass.color.get(), pass.depth.get(),
          pass.resolveTarget ? pass.resolveTarget->texture.get() : nullptr,
          pass.resolveSourceSnapshot ? pass.resolveSourceSnapshot->texture.get() : nullptr,
          pass.targetSize.width, pass.targetSize.height, pass.commands.size());
    }
  }
  out.data().retiredBindGroups.clear();
  out.data().retiredBindGroups.swap(g_retiredBindGroups);
#ifdef AURORA_GFX_DEBUG_GROUPS
  // Marker indices and unmatched-group warnings belong to these detached passes.
  // The next producer frame must not modify strings still read by the render thread.
  auto& debug = out.data().debug;
  debug.groups.clear();
  debug.markers.clear();
  debug.groups.swap(g_debugFrame.groups);
  debug.markers.swap(g_debugFrame.markers);
#endif
  g_currentRenderPass = UINT32_MAX;
}

void render(SealedFrame& frame, int32_t interpolatedFrame, bool finalize) {
  render_impl(frame.data().passes, interpolatedFrame, finalize, frame.data().debug, frame.data().depthMapping,
              frame.data().skipDraws);
  if (finalize) {
    frame.data().retiredBindGroups.clear();
  }
}

void after_submit() noexcept { depth_peek::after_submit(); }

static void bind_word_buffers() {
#ifndef AURORA_GXM
  const GLuint sampler = gl::sampler(gl::NearestClamp);
  gl::bind_texture(VertexBufferUnit, g_gpu.verts[g_gpu.slot]->gl(), sampler);
  gl::bind_texture(StorageBufferUnit, g_gpu.storage[g_gpu.slot]->gl(), sampler);
#endif
}

GLuint uniform_buffer() noexcept { return g_gpu.uniform; }

static void render_pass_impl(const std::vector<RenderPass>& renderPasses, u32 idx, int32_t interpolatedFrame,
                             DebugFrameData& debugFrame) {
  (void)debugFrame;
  // Per-invocation, not per-process: bindings do not carry across passes.
  gx::DrawEncodeState encodeState{};
  const auto& pass = renderPasses[idx];
  const auto& size = pass.targetSize;
  ClipRect scissor{0, 0, static_cast<int32_t>(size.width), static_cast<int32_t>(size.height)};
  gl::set_scissor_enabled(true);
  gl::set_scissor(scissor.x, scissor.y, scissor.width, scissor.height);
  gl::set_viewport(0, 0, static_cast<int32_t>(size.width), static_cast<int32_t>(size.height), 0.f, 1.f);
  bind_word_buffers();
  gl::bind_draw_vao(g_gpu.index);

  for (const auto& cmd : pass.commands) {
    switch (cmd.type) {
    case CommandType::SetViewport: {
      const auto& vp = cmd.data.setViewport;
      // vp.znear/vp.zfar are in GX's own distance terms (0 = near); under UseReversedZ the host
      // depth-buffer storage direction is flipped, so the range is remapped through 1-x the same
      // way the projection matrix, depth compare function, and clear value all are.
      const float minDepth = gx::UseReversedZ ? 1.0f - vp.zfar : vp.znear;
      const float maxDepth = gx::UseReversedZ ? 1.0f - vp.znear : vp.zfar;
      // Every pass renders vertically flipped, so the top-left origin maps to GL rows directly.
      gl::set_viewport(static_cast<int32_t>(std::lround(vp.left)), static_cast<int32_t>(std::lround(vp.top)),
                       static_cast<int32_t>(std::lround(vp.width)), static_cast<int32_t>(std::lround(vp.height)),
                       std::clamp(minDepth, 0.f, 1.f), std::clamp(maxDepth, 0.f, 1.f));
    } break;
    case CommandType::SetScissor: {
      const auto& sc = cmd.data.setScissor;
      const auto left = std::clamp(sc.x, 0, static_cast<int32_t>(size.width));
      const auto top = std::clamp(sc.y, 0, static_cast<int32_t>(size.height));
      const auto right = std::clamp(sc.x + sc.width, left, static_cast<int32_t>(size.width));
      const auto bottom = std::clamp(sc.y + sc.height, top, static_cast<int32_t>(size.height));
      scissor = {left, top, right - left, bottom - top};
      gl::set_scissor(scissor.x, scissor.y, scissor.width, scissor.height);
    } break;
    case CommandType::Draw: {
      const auto& draw = cmd.data.draw;
      switch (draw.type) {
      case ShaderType::GX: {
        const gfx::Range* uniformOverride = nullptr;
        if (interpolatedFrame >= 0 &&
            static_cast<size_t>(interpolatedFrame) < draw.gx.interpolatedUniformRanges.size() &&
            draw.gx.interpolatedUniformRanges[interpolatedFrame].size != 0) {
          uniformOverride = &draw.gx.interpolatedUniformRanges[interpolatedFrame];
        }
        gx::render(draw.gx, encodeState, pass.requireReadyPipelines, uniformOverride);
      } break;
      case ShaderType::Clear:
        clear::render(draw.clear, size.width, size.height, scissor);
        break;
      }
    } break;
    case CommandType::DebugMarker:
      break;
    }
  }
}

bool bind_pipeline(PipelineRef ref, PipelineRef& currentPipeline, bool requireReady) {
  (void)requireReady;
  if (ref == currentPipeline) {
    return true;
  }
  const Pipeline* pipeline = find_pipeline_object(ref);
  if (pipeline == nullptr || !pipeline->program) {
    return false;
  }
  const GLuint program = pipeline->program->get();
  if (program == 0) {
    return false;
  }
  gl::use_program(program);
  gl::apply_raster_state(pipeline->raster);
  currentPipeline = ref;
  return true;
}

static inline Range push(ByteBuffer& target, const uint8_t* data, size_t length, size_t alignment) {
  size_t padding = 0;
  if (alignment != 0) {
    const size_t remainder = length % alignment;
    if (remainder != 0) {
      padding = alignment - remainder;
    }
  }
  auto begin = target.size();
  if (length == 0) {
    length = alignment;
    target.append_zeroes(alignment);
  } else {
    target.append(data, length);
    if (padding > 0) {
      target.append_zeroes(padding);
    }
  }
  return {static_cast<uint32_t>(begin), static_cast<uint32_t>(length + padding)};
}
static inline Range map(ByteBuffer& target, size_t length, size_t alignment) {
  size_t padding = 0;
  if (alignment != 0) {
    const size_t remainder = length % alignment;
    if (remainder != 0) {
      padding = alignment - remainder;
    }
  }
  auto begin = target.size();
  if (length == 0) {
    length = alignment;
    target.append_zeroes(length);
  } else {
    // The caller fills [0, length); only the alignment padding needs clearing,
    // otherwise it hands the next frame's readers whatever was there before.
    target.append_uninitialized(length);
    target.append_zeroes(padding);
  }
  return {static_cast<uint32_t>(begin), static_cast<uint32_t>(length + padding)};
}
Range push_verts(const uint8_t* data, size_t length) { return push(g_verts(), data, length, 0); }
Range push_indices(const uint8_t* data, size_t length) { return push(g_indices(), data, length, 0); }
Range push_uniform(const uint8_t* data, size_t length) { return push(g_uniforms(), data, length, g_uniformAlignment); }
Range push_storage(const uint8_t* data, size_t length) { return push(g_storage(), data, length, StorageAlignment); }
std::pair<ByteBuffer, Range> map_verts(size_t length) {
  const auto range = map(g_verts(), length, 4);
  return {ByteBuffer{g_verts().data() + range.offset, range.size}, range};
}
std::pair<ByteBuffer, Range> map_indices(size_t length) {
  const auto range = map(g_indices(), length, 4);
  return {ByteBuffer{g_indices().data() + range.offset, range.size}, range};
}
std::pair<ByteBuffer, Range> map_uniform(size_t length) {
  const auto range = map(g_uniforms(), length, g_uniformAlignment);
  return {ByteBuffer{g_uniforms().data() + range.offset, range.size}, range};
}
std::pair<ByteBuffer, Range> copy_uniform(Range source) {
  const auto destination = map(g_uniforms(), source.size, g_uniformAlignment);
  std::memcpy(g_uniforms().data() + destination.offset, g_uniforms().data() + source.offset, source.size);
  return {ByteBuffer{g_uniforms().data() + destination.offset, destination.size}, destination};
}
std::pair<ByteBuffer, Range> map_storage(size_t length) {
  const auto range = map(g_storage(), length, StorageAlignment);
  return {ByteBuffer{g_storage().data() + range.offset, range.size}, range};
}

static HashType hash_bind_group(const TextureBindGroup& group) noexcept {
  struct Entry {
    uintptr_t texture;
    GLenum wrapS, wrapT, minFilter, magFilter;
    uint32_t minLod, maxLod, maxAnisotropy;
  };
  std::array<Entry, MaxBoundTextures> entries{};
  for (size_t i = 0; i < MaxBoundTextures; ++i) {
    const auto& binding = group.textures[i];
    const auto& sampler = binding.sampler;
    entries[i] = {
        .texture = reinterpret_cast<uintptr_t>(binding.ref.get()),
        .wrapS = sampler.wrapS,
        .wrapT = sampler.wrapT,
        .minFilter = sampler.minFilter,
        .magFilter = sampler.magFilter,
        .minLod = std::bit_cast<uint32_t>(sampler.minLod),
        .maxLod = std::bit_cast<uint32_t>(sampler.maxLod),
        .maxAnisotropy = sampler.maxAnisotropy,
    };
  }
  return xxh3_hash_s(entries.data(), sizeof(entries));
}

BindGroupRef bind_group_ref(TextureBindGroup&& group) {
  const auto id = hash_bind_group(group);
  const auto it = g_cachedBindGroups.find(id);
  if (it == g_cachedBindGroups.end()) {
    g_cachedBindGroups.emplace(id, CachedBindGroup{
                                       .bindGroup = std::make_shared<const TextureBindGroup>(std::move(group)),
                                       .lastUsedFrame = g_frameIndex,
                                   });
  } else {
    it->second.lastUsedFrame = g_frameIndex;
  }
  return id;
}

const TextureBindGroup* find_bind_group(BindGroupRef id) {
  const auto it = g_cachedBindGroups.find(id);
  CHECK(it != g_cachedBindGroups.end(), "find_bind_group: failed to locate {:x}", id);
  return it->second.bindGroup.get();
}

uint32_t align_uniform(uint32_t value) { return AURORA_ALIGN(value, g_uniformAlignment); }

void insert_debug_marker(std::string label) {
#if defined(AURORA_GFX_DEBUG_GROUPS)
  auto idx = g_debugFrame.markers.size();
  g_debugFrame.markers.emplace_back(std::move(label));
  push_command(CommandType::DebugMarker, {.debugMarkerIndex = idx});
#endif
}

} // namespace aurora::gfx

void aurora::gfx::push_debug_group(std::string label) {
#if defined(AURORA_GFX_DEBUG_GROUPS)
  g_debugFrame.groups.push_back(std::move(label));
#endif
}
void aurora_push_debug_group(const char* label) {
#ifdef AURORA_GFX_DEBUG_GROUPS
  aurora::gfx::g_debugFrame.groups.emplace_back(label);
#endif
}
void aurora_pop_debug_group() {
#ifdef AURORA_GFX_DEBUG_GROUPS
  if (aurora::gfx::g_debugFrame.groups.empty()) {
    aurora::gfx::Log.error("Debug group stack underflowed!");
    return;
  }

  aurora::gfx::g_debugFrame.groups.pop_back();
#endif
}

const AuroraStats* aurora_get_stats() { return &aurora::gfx::g_stats; }
