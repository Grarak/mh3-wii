#pragma once
#include <cstdint>

// Native Vita sink counters. Frames are interleaved stereo sample frames.
// Underflow/padding is counted when preparing a block, not hardware completion;
// authored zero PCM is not counted as padding. Getter never services playback.
struct NativeAudioOutputStatistics {
    uint64_t queuedFrames = 0, outputCalls = 0, underflowBuffers = 0, paddedFrames = 0;
    uint64_t droppedBlocks = 0, droppedFrames = 0, outputErrors = 0, rateChanges = 0;
    uint32_t rateHz = 0, currentRingFrames = 0, ringHighWaterFrames = 0;
    bool available = false;
};

// Owner calls only; the live Vita instance snapshots its existing ring mutex.
// Other backends report unavailable. Counters are cumulative per sink lifetime.
NativeAudioOutputStatistics GetNativeAudioOutputStatistics();
