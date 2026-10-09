#pragma once

#include <cstdint>

namespace mh3::statistics {

// Execution time of this owner thread, not process elapsed time. Vita uses
// kernel runClocks (microseconds); Linux uses CLOCK_THREAD_CPUTIME_ID. Affinity
// is Vita's encoded mask or Linux CPU-index bits (low 32 CPUs), respectively.
struct OwnerCpuSample {
    uint64_t running_us = 0;
    int32_t thread = -1;
    int32_t core = -1;
    int32_t affinity = 0;
    bool available = false;
};

struct NativeFrameWindow {
    uint64_t window_ns = 0;
    uint64_t logical_frames = 0;
    uint64_t rendered_frames = 0;
    uint64_t skipped_frames = 0;
    uint64_t measured_frames = 0;
    uint64_t frame_wall_ns = 0;
    uint64_t frame_wall_max_ns = 0;
    uint64_t rendered_measured_frames = 0;
    uint64_t rendered_wall_ns = 0;
    uint64_t rendered_wall_max_ns = 0;
    uint64_t owner_running_us = 0;
    int32_t owner_thread = -1;
    int32_t owner_core = -1;
    int32_t owner_affinity = 0;
    bool owner_running_available = false;
};

// Single game/render owner; callbacks never access this accumulator. Each
// window counts completed logical frames, including skipped rendering. Frame
// wall intervals exclude the pacer when Begin/End surround NativeFrameBegin,
// GameFrame and NativeFrameEnd in their original main-loop order.
class NativeFrameAccumulator {
public:
    void Reset(uint64_t now_ns, OwnerCpuSample owner = {}) noexcept;
    void Begin(uint64_t now_ns) noexcept;
    void End(uint64_t now_ns, bool rendered) noexcept;
    NativeFrameWindow Take(uint64_t now_ns, OwnerCpuSample owner = {}) noexcept;

private:
    NativeFrameWindow m_window{};
    OwnerCpuSample m_owner{};
    uint64_t m_windowStart = 0;
    uint64_t m_frameStart = 0;
    bool m_started = false;
    bool m_active = false;
};

} // namespace mh3::statistics

// The first Begin samples the owner once. Take samples it once per reporting
// interval. Normal frame Begin/End only read the monotonic clock; no allocations,
// worker threads, GPU synchronization or per-frame kernel thread-info queries.
void BeginNativeFrameStatistics() noexcept;
void EndNativeFrameStatistics(bool rendered) noexcept;
mh3::statistics::NativeFrameWindow TakeNativeFrameStatistics() noexcept;
