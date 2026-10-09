#include "efb_ram_copy.hpp"

#include "efb_ram_encoder.hpp"
#include "tex_copy_conv.hpp"
#include "../internal.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#include <aurora/fast_map.h>

namespace aurora::gfx::efb_ram {
namespace {

Module Log("aurora::gfx::efb_ram");

// A CPU-consumed EFB copy this small is the lens-flare occlusion probe (4x4 Z24X8, 64 bytes).
// Resolving it synchronously stalls the producer, so it is read back after the frame renders.
constexpr size_t kAsyncReadbackMaxBytes = 256;
constexpr size_t kMaxAsyncSlots = MaxAsyncReadbackSlots;

struct PendingCopy {
  void* dest = nullptr;
  uint32_t width = 0;
  uint32_t height = 0;
  GXTexFmt format = GX_TF_RGBA8;
  TextureHandle texture;
  TextureHandle nativeTexture;
  Range nativeBlitUniform;
  uint64_t nativeUniformEpoch = 0;
};

std::vector<PendingCopy> g_pending;
std::vector<PendingCopy> g_asyncPending;

// Destinations that take the frame-latent path, and their cached native-resolution textures.
std::mutex g_asyncMutex;
struct AsyncSlot {
  TextureHandle nativeTexture;
};
FastMap<void*, AsyncSlot> g_asyncSlots;

// An internally scaled copy cannot be read back directly, so blit it down to native-resolution
// RGBA8 first. Pushes a uniform into the batch being recorded.
void ensure_native_texture(PendingCopy& pending, TextureHandle* cache = nullptr) noexcept {
  if (pending.texture->size.width == pending.width && pending.texture->size.height == pending.height) {
    return;
  }
  if (pending.nativeTexture && pending.nativeUniformEpoch == staging_epoch())
    return;
  if (pending.nativeTexture) {
    // Keep the texture; its old staging range belongs to a submitted batch.
  } else if (cache != nullptr && *cache && (*cache)->size.width == pending.width && (*cache)->size.height == pending.height) {
    pending.nativeTexture = *cache;
  } else {
    pending.nativeTexture = new_conv_texture(pending.width, pending.height, GX_TF_RGBA8, "GX EFB RAM native readback");
    if (cache != nullptr) {
      *cache = pending.nativeTexture;
    }
  }
  // The shared blit shader clamps Y to flags.z/w; preserve the full source.
  const std::array nativeBlitUniform{
      0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 64.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f,
  };
  pending.nativeBlitUniform = push_uniform(nativeBlitUniform);
  pending.nativeUniformEpoch = staging_epoch();
}

// Render thread: resolve one copy into guest RAM.
bool read_back(const PendingCopy& pending, std::vector<uint8_t>& scratch) noexcept {
  if (pending.nativeTexture) {
    tex_copy_conv::blit({
        .fmt = GX_TF_RGBA8,
        .src = pending.texture->texture,
        .uniformRange = pending.nativeBlitUniform,
        .dst = pending.nativeTexture,
        .sampleFilter = pending.format == GX_TF_Z16 || pending.format == GX_TF_Z24X8 ? tex_copy_conv::SampleFilter::Nearest : tex_copy_conv::SampleFilter::Linear,
    });
  }
  const auto& texture = pending.nativeTexture ? pending.nativeTexture : pending.texture;
  const uint32_t width = texture->size.width;
  const uint32_t height = texture->size.height;
  scratch.resize(static_cast<size_t>(width) * height * 4);
  gl::read_pixels(texture->texture->gl(), 0, 0, width, height, scratch.data());
  const size_t outputSize = encoded_size(pending.format, pending.width, pending.height);
  const bool ok = encode(pending.dest, outputSize, pending.format, pending.width, pending.height, scratch.data(), width, height, width * 4, HostPixelOrder::RGBA);
  if (!ok) {
    Log.error("Failed to encode EFB RAM copy format=0x{:x} size={}x{}", static_cast<unsigned>(pending.format), pending.width, pending.height);
  }
  // Guest RAM written from outside the embedder, so nothing bumps its write generation and a
  // texture cached over this range would keep its digest.
  notify_guest_write(pending.dest, outputSize);
  return ok;
}

} // namespace

void schedule(void* dest, uint32_t width, uint32_t height, GXTexFmt format, TextureHandle texture) noexcept {
  if (dest == nullptr || width == 0 || height == 0 || !texture) {
    return;
  }
  if (!supports_format(format)) {
    Log.fatal("Unsupported CPU-visible EFB copy format 0x{:x}", static_cast<unsigned>(format));
  }

  PendingCopy request{
      .dest = dest,
      .width = width,
      .height = height,
      .format = format,
      .texture = std::move(texture),
  };

  const size_t encodedSize = encoded_size(format, width, height);
  // Offscreen copies resolve on a separate pass list that the sealed frame's
  // native render does not replay, so they stay on the lazy path.
  bool async = !is_offscreen() && encodedSize != 0 && encodedSize <= kAsyncReadbackMaxBytes;
  if (async) {
    std::lock_guard lock{g_asyncMutex};
    const auto it = g_asyncSlots.find(dest);
    if (it == g_asyncSlots.end()) {
      if (g_asyncSlots.size() >= kMaxAsyncSlots) {
        async = false;
      } else {
        g_asyncSlots.try_emplace(dest);
        // No readback has landed here yet. 0xff decodes to far Z, which the probe reads as unobstructed
        // so a new flare fades in; zero-filled RAM would decode as fully occluded.
        std::memset(dest, 0xff, encodedSize);
        notify_guest_write(dest, encodedSize);
      }
    }
  }

  auto& list = async ? g_asyncPending : g_pending;
  if (auto it = std::find_if(list.begin(), list.end(), [dest](const PendingCopy& pending) { return pending.dest == dest; }); it != list.end()) {
    *it = std::move(request);
  } else {
    list.push_back(std::move(request));
  }
}

bool has_pending(void* dest) noexcept {
  if (dest == nullptr)
    return !g_pending.empty();
  return std::any_of(g_pending.begin(), g_pending.end(), [dest](const PendingCopy& pending) { return pending.dest == dest; });
}

bool prepare_downloads(void* dest) {
  uint64_t copies = 0;
  for (const auto& pending : g_pending) {
    if (dest != nullptr && pending.dest != dest)
      continue;
    if (pending.texture->size.width != pending.width || pending.texture->size.height != pending.height)
      ++copies;
  }
  // Reserve all copies, even already-prepared ones: a split retires their ranges.
  ensure_staging_space({0, copies * staging_uniform_bytes(48), 0, 0});
  bool found = false;
  for (auto& pending : g_pending) {
    if (dest != nullptr && pending.dest != dest)
      continue;
    found = true;
    ensure_native_texture(pending);
  }
  return found;
}

bool download(void* dest) noexcept {
  std::vector<PendingCopy> copies;
  for (auto it = g_pending.begin(); it != g_pending.end();) {
    if (dest != nullptr && it->dest != dest) {
      ++it;
      continue;
    }
    copies.push_back(std::move(*it));
    it = g_pending.erase(it);
  }
  if (copies.empty()) {
    return true;
  }
  bool success = true;
  gl::call([&] {
    std::vector<uint8_t> scratch;
    for (const auto& copy : copies) {
      success &= read_back(copy, scratch);
    }
  });
  return success;
}

void cancel() noexcept {
  g_pending.clear();
  g_asyncPending.clear();
}

gl::Task seal_async_downloads() noexcept {
  if (g_asyncPending.empty()) {
    return {};
  }
  {
    std::lock_guard lock{g_asyncMutex};
    for (auto& pending : g_asyncPending) {
      const auto it = g_asyncSlots.find(pending.dest);
      ensure_native_texture(pending, it != g_asyncSlots.end() ? &it->second.nativeTexture : nullptr);
    }
  }
  std::vector<PendingCopy> copies;
  copies.swap(g_asyncPending);
  return [copies = std::move(copies)] {
    std::vector<uint8_t> scratch;
    for (const auto& copy : copies) {
      // Writes guest RAM from the render thread while the guest may be reading it. The only
      // consumer min/maxes depth for a fade factor, so a torn tile just mixes two frames' depths.
      read_back(copy, scratch);
    }
  };
}

void shutdown() noexcept {
  cancel();
  std::lock_guard lock{g_asyncMutex};
  g_asyncSlots.clear();
}

} // namespace aurora::gfx::efb_ram
