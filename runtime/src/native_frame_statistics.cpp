#include "native_frame_statistics.h"

#include <algorithm>
#include <chrono>
#ifdef __vita__
#include <psp2/kernel/threadmgr.h>
#endif

namespace mh3::statistics {

void NativeFrameAccumulator::Reset(uint64_t now_ns, OwnerCpuSample owner) noexcept {
    m_window = {};
    m_owner = owner;
    m_windowStart = now_ns;
    m_frameStart = 0;
    m_started = true;
    m_active = false;
}

void NativeFrameAccumulator::Begin(uint64_t now_ns) noexcept {
    if (!m_started) Reset(now_ns);
    m_frameStart = now_ns;
    m_active = true;
}

void NativeFrameAccumulator::End(uint64_t now_ns, bool rendered) noexcept {
    ++m_window.logical_frames;
    if (rendered) ++m_window.rendered_frames;
    else ++m_window.skipped_frames;
    if (m_active && now_ns >= m_frameStart) {
        const auto elapsed = now_ns - m_frameStart;
        ++m_window.measured_frames;
        m_window.frame_wall_ns += elapsed;
        m_window.frame_wall_max_ns = std::max(m_window.frame_wall_max_ns, elapsed);
        if (rendered) {
            ++m_window.rendered_measured_frames;
            m_window.rendered_wall_ns += elapsed;
            m_window.rendered_wall_max_ns = std::max(m_window.rendered_wall_max_ns, elapsed);
        }
    }
    m_active = false;
}

NativeFrameWindow NativeFrameAccumulator::Take(uint64_t now_ns, OwnerCpuSample owner) noexcept {
    if (!m_started) {
        Reset(now_ns, owner);
        return {};
    }
    auto result = m_window;
    result.window_ns = now_ns >= m_windowStart ? now_ns - m_windowStart : 0;
    result.owner_thread = owner.thread;
    result.owner_core = owner.core;
    result.owner_affinity = owner.affinity;
    result.owner_running_available = owner.available && m_owner.available &&
        owner.thread == m_owner.thread && owner.running_us >= m_owner.running_us;
    if (result.owner_running_available)
        result.owner_running_us = owner.running_us - m_owner.running_us;
    m_window = {};
    m_windowStart = now_ns;
    m_owner = owner;
    // Preserve any active frame; reporting must not consume its begin marker.
    return result;
}

} // namespace mh3::statistics

namespace {
mh3::statistics::NativeFrameAccumulator measurements;
bool started = false;

uint64_t monotonic_ns() noexcept {
    using Clock = std::chrono::steady_clock;
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}

mh3::statistics::OwnerCpuSample owner_cpu() noexcept {
    mh3::statistics::OwnerCpuSample result;
#ifdef __vita__
    SceKernelThreadInfo info{};
    info.size = sizeof(info);
    const auto thread = sceKernelGetThreadId();
    if (sceKernelGetThreadInfo(thread, &info) >= 0) {
        result.running_us = info.runClocks;
        result.thread = thread;
        result.core = info.currentCpuId;
        result.affinity = info.currentCpuAffinityMask;
        result.available = true;
    }
#endif
    return result;
}
}

void BeginNativeFrameStatistics() noexcept {
    if (!started) {
        const auto owner = owner_cpu();
        measurements.Reset(monotonic_ns(), owner);
        started = true;
    }
    measurements.Begin(monotonic_ns());
}

void EndNativeFrameStatistics(bool rendered) noexcept {
    measurements.End(monotonic_ns(), rendered);
}

mh3::statistics::NativeFrameWindow TakeNativeFrameStatistics() noexcept {
    const auto owner = owner_cpu();
    return measurements.Take(monotonic_ns(), owner);
}
