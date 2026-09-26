#pragma once

#include <atomic>
#include <cstdint>

namespace RuntimeGameGraphicsOptions {

inline std::atomic<uint32_t>& DisabledPostProcessingPathsState() noexcept {
    static std::atomic<uint32_t> disabledMask{0};
    return disabledMask;
}

inline uint32_t DisabledPostProcessingPaths() noexcept {
    return DisabledPostProcessingPathsState().load(std::memory_order_relaxed);
}

inline void SetDisabledPostProcessingPaths(uint32_t disabledMask) noexcept {
    DisabledPostProcessingPathsState().store(disabledMask, std::memory_order_relaxed);
}

inline uint32_t FilterScnRendererPathMask(uint32_t pathMask) noexcept {
    return pathMask & ~(DisabledPostProcessingPaths() | 0x20u);
}

} // namespace RuntimeGameGraphicsOptions
