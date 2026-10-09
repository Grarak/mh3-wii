#pragma once

#include <aurora/gpu_completion.hpp>
#include <cstddef>

namespace aurora::gxm {
// Native vertices, uniforms and U16 indices share one CDRAM block per
// notification-protected frame slot. No recorded GX word buffers are needed.
inline constexpr size_t CdramBlockAlignment = 256u << 10;
inline constexpr size_t NativeVertexBytes = 1u << 20;
inline constexpr size_t NativeUniformBytes = 2u << 20;
inline constexpr size_t NativeIndexBytes = 512u << 10;
inline constexpr size_t NativeVertexOffset = 0;
inline constexpr size_t NativeUniformOffset = NativeVertexBytes;
inline constexpr size_t NativeIndexOffset = NativeUniformOffset + NativeUniformBytes;
inline constexpr size_t NativeSlotBytes = NativeIndexOffset + NativeIndexBytes;

// DrawMemory's immutable IDs, PixelClip's rectangle and the renderer's
// utility rectangle each own one rounded block. Keep headroom rounded to MiB.
inline constexpr size_t NativeAuxiliaryBytes = 3 * CdramBlockAlignment;
inline constexpr size_t NativeProducerBytes = aurora::GpuFrameRing::BufferCount * NativeSlotBytes + NativeAuxiliaryBytes;
inline constexpr size_t NativeProducerReserve = (NativeProducerBytes + (1u << 20) - 1) & ~size_t((1u << 20) - 1);

static_assert(NativeSlotBytes % CdramBlockAlignment == 0);
static_assert(NativeUniformOffset % 256 == 0 && NativeIndexOffset % 16 == 0);
static_assert(NativeProducerReserve >= NativeProducerBytes);
} // namespace aurora::gxm
