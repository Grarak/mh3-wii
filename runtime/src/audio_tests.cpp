#include "abi_bridge.h"
#include "fatal.h"
#include "audio_dma.h"
#include <array>
#include <iostream>
#include <stdexcept>

namespace NativeAudio {
void RunNativeMixerSelfTests();
}

void RunAudioSelfTests(CpuContext*) {
    NativeAudio::RunNativeMixerSelfTests();
    AudioDmaState timing;
    timing.enabled = true;
    timing.length = 384;

    if (timing.Period(32000) != 3000000 || timing.Period(48000) != 2000000)
        MH3_THROW(std::runtime_error("AI DMA block durations failed"));

    timing.elapsedNs = 2000000;

    if (timing.Ready(32000) || timing.BytesLeft(32000) != 128 || !timing.Ready(48000))
        MH3_THROW(std::runtime_error("AI DMA partial block timing failed"));

    timing.elapsedNs = 6500000;
    unsigned blocks = 0;

    while (timing.Ready(32000)) {
        timing.elapsedNs -= timing.Period(32000);
        ++blocks;
    }

    if (blocks != 2 || timing.BytesLeft(32000) != 320)
        MH3_THROW(std::runtime_error("AI DMA carry between blocks failed"));

    timing.enabled = false;

    if (timing.Ready(32000) || timing.BytesLeft(32000))
        MH3_THROW(std::runtime_error("Stopped AI DMA remained active"));
}
