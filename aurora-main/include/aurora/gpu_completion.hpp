#pragma once

#include <cstdint>
#include <array>

namespace aurora {
// GXM's notification region is uncached GPU-written memory. Scene serials
// wrap at 32 bits; fewer than 2^31 scenes can be outstanding at once.
inline bool gpu_notification_pending(const volatile unsigned* notification, uint32_t submitted) noexcept {
  return notification && uint32_t(*notification - submitted) >= 0x80000000u;
}

// Three independent frame leases. Every scene consuming the current frame's
// mutable data extends that lease; only a submitted present rotates the ring.
class GpuFrameRing {
public:
  static constexpr unsigned BufferCount = 3;
  unsigned slot() const noexcept { return m_slot; }
  bool pending() const noexcept {
    const auto& fence = m_fences[m_slot];
    return gpu_notification_pending(fence.notification, fence.serial);
  }

  void record_scene(volatile unsigned* notification, uint32_t serial) noexcept { m_fences[m_slot] = {notification, serial}; }
  void advance() noexcept { m_slot = (m_slot + 1) % BufferCount; }

private:
  struct Fence {
    volatile unsigned* notification = nullptr;
    uint32_t serial = 0;
  };

  std::array<Fence, BufferCount> m_fences{};
  unsigned m_slot = 0;
};

inline bool gpu_display_queue_full(uint32_t queued, uint32_t completed) noexcept {
  // Full at the queue's capacity (three pending flips): polled before draw
  // preparation, so sceGxmDisplayQueueAddEntry never blocks. A frame begun
  // with two flips pending renders into the buffer still scanned out: its
  // scene waits on the GPU for that buffer's sync object, not the CPU, and
  // the CPU records it meanwhile.
  return uint32_t(queued - completed) >= GpuFrameRing::BufferCount;
}
} // namespace aurora
