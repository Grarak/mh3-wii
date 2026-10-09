#pragma once

#include "common.hpp"
#include "texture.hpp"

#include <dolphin/gx/GXEnum.h>

namespace aurora::gfx::efb_ram {

inline constexpr size_t MaxAsyncReadbackSlots = 32;

void schedule(void* dest, uint32_t width, uint32_t height, GXTexFmt format, TextureHandle texture) noexcept;
bool has_pending(void* dest = nullptr) noexcept;

// Producer: pushes the native-resolution blit uniforms into the current batch.
bool prepare_downloads(void* dest = nullptr);

// Producer: reads the prepared copies back on the render thread and writes
// them to guest RAM. The copies' passes must already have been submitted.
bool download(void* dest = nullptr) noexcept;
void cancel() noexcept;

// Frame-latent readbacks for probe-sized CPU-consumed copies. The producer
// seals them with the frame; the returned task runs on the render thread after
// the frame renders and writes guest RAM from there.
gl::Task seal_async_downloads() noexcept;
void shutdown() noexcept;

} // namespace aurora::gfx::efb_ram
