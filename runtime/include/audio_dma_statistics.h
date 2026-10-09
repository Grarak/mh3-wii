#pragma once
#include <cstdint>

// Cooperative AI scheduler, read only on its game-owner thread. These counters
// measure successful DMA completions, not AX job counts or audible playback.
struct NativeAudioDmaStatistics {
    uint64_t serviceCalls = 0, enabledServiceCalls = 0;
    uint64_t completedBlocks = 0, completedFrames = 0, completedDurationNs = 0;
    uint64_t capHits = 0, maxServiceGapNs = 0, maxDebtNs = 0;
    uint64_t currentDebtNs = 0, currentPeriodNs = 0;
    uint32_t rateHz = 0, lengthBytes = 0;
    bool enabled = false;
};
// No clock sampling, MMIO access, callback, queue submission or debt advance.
// rateHz is the last observed active AI rate; zero before the first such visit.
// Counters/maxima are cumulative; current debt follows existing start/stop resets.
NativeAudioDmaStatistics GetNativeAudioDmaStatistics();
