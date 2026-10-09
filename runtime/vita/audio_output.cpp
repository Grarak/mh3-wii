#include "audio_dma.h"
#include "audio_output_statistics.h"
#include <aurora/thread_role.hpp>
#include <psp2/audioout.h>
#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {
// A quarter second bounds latency and memory even when guest DMA catches up
// after a stall. The game copies PCM into this ring and never calls SceAudio.
class AudioOutput {
public:
    AudioOutput() : thread([this] { Run(); }) {}
    ~AudioOutput() {
        {
            std::lock_guard lock(mutex);
            stopping = true;
        }
        wake.notify_one();
        thread.join();
    }

    void Queue(const uint8_t* data, uint32_t bytes, uint32_t rate) {
        const size_t frames = bytes / 4;
        {
            std::lock_guard lock(mutex);
            if (failed) throw std::runtime_error("Vita audio output failed");
            if (rate != queueRate) {
                // Old-rate PCM cannot be combined with the new-rate stream.
                if (queueRate) ++statistics.rateChanges;
                queueRate = rate;
                read = count = 0;
            }
            const size_t capacity = rate / 4;
            if (frames > capacity - count) {
                ++statistics.droppedBlocks;statistics.droppedFrames+=frames;
                if (!reportedDrop) {
                    std::fprintf(stderr, "MH3 audio: playback backlog; dropping excess PCM blocks\n");
                    reportedDrop = true;
                }
                return;
            }
            for (size_t i = 0; i < frames; ++i) {
                const size_t target = ((read + count + i) % RingFrames) * 2;
                // Wii DMA is big-endian right/left; SceAudio is native left/right.
                ring[target] = int16_t(uint16_t(data[i * 4 + 2]) << 8 | data[i * 4 + 3]);
                ring[target + 1] = int16_t(uint16_t(data[i * 4]) << 8 | data[i * 4 + 1]);
            }
            count += frames;
            statistics.queuedFrames+=frames;
            statistics.ringHighWaterFrames=std::max(statistics.ringHighWaterFrames,uint32_t(count));
        }
        wake.notify_one();
    }

    NativeAudioOutputStatistics Statistics() {
        std::lock_guard lock(mutex);auto result=statistics;
        result.available=true;result.rateHz=queueRate;result.currentRingFrames=uint32_t(count);
        return result;
    }

private:
    static constexpr size_t OutputFrames = 1024;
    static constexpr size_t RingFrames = 48000 / 4;

    void Run() noexcept {
        aurora::pin_current_thread(aurora::ThreadRole::Worker);
        int port = -1;
        uint32_t outputRate = 0;
        unsigned side=0;
        for (;;) {
            uint32_t rate;
            auto& buffer=output[side];
            {
                std::unique_lock lock(mutex);
                // Before opening the port, wait for the first complete buffer.
                // Once playing, the blocking output call supplies the clock.
                if (port < 0) wake.wait(lock, [&] { return stopping || count >= OutputFrames; });
                if (stopping) break;
                rate = queueRate;
                const size_t available = std::min(count, OutputFrames);
                if (available<OutputFrames) {
                    ++statistics.underflowBuffers;statistics.paddedFrames+=OutputFrames-available;
                }
                for (size_t i = 0; i < OutputFrames; ++i) {
                    const size_t source = ((read + i) % RingFrames) * 2;
                    buffer[i * 2] = i < available ? ring[source] : 0;
                    buffer[i * 2 + 1] = i < available ? ring[source + 1] : 0;
                }
                read = (read + available) % RingFrames;
                count -= available;
            }
            int result = 0;
            if (port < 0) {
                port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, OutputFrames,
                                          rate, SCE_AUDIO_OUT_MODE_STEREO);
                result = port;
                if (port >= 0) std::fprintf(stderr, "MH3 audio: Vita playback on core 0 at %u Hz stereo\n", rate);
            } else if (outputRate != rate) {
                result = sceAudioOutOutput(port, nullptr);
                if (result >= 0) result = sceAudioOutSetConfig(port, OutputFrames, rate, SCE_AUDIO_OUT_MODE_STEREO);
            }
            outputRate = rate;
            if (result >= 0) result = sceAudioOutOutput(port, buffer.data());
            if (result < 0) {
                std::fprintf(stderr, "MH3 audio: native output failed: 0x%08x\n", unsigned(result));
                std::lock_guard lock(mutex);
                ++statistics.outputErrors;
                failed = true;
                break;
            }
            {
                std::lock_guard lock(mutex);++statistics.outputCalls;
            }
            // Return means this buffer STARTED playing, not that its data was
            // copied or consumed. Prepare the other buffer; its earlier use
            // finished before this submission was accepted by the driver.
            side^=1;
        }
        if (port >= 0) {
            sceAudioOutOutput(port, nullptr);
            sceAudioOutReleasePort(port);
        }
    }

    std::mutex mutex;
    std::condition_variable wake;
    std::array<int16_t, RingFrames * 2> ring{};
    std::array<std::array<int16_t, OutputFrames * 2>,2> output{};
    NativeAudioOutputStatistics statistics{};
    size_t read = 0, count = 0;
    uint32_t queueRate = 0;
    bool stopping = false, failed = false, reportedDrop = false;
    // Initialized last: the worker can access every field as soon as it starts.
    std::thread thread;
};

std::unique_ptr<AudioOutput> audio;
void Shutdown() { audio.reset(); }
}

NativeAudioOutputStatistics GetNativeAudioOutputStatistics() {
    return audio ? audio->Statistics() : NativeAudioOutputStatistics{};
}

void QueueHostPcm(const uint8_t* data, uint32_t bytes, uint32_t rate) {
    if (bytes % 4 || (rate != 32000 && rate != 48000))
        throw std::runtime_error("Invalid Wii PCM block");
    if (!audio) {
        audio = std::make_unique<AudioOutput>();
        std::atexit(Shutdown);
    }
    audio->Queue(data, bytes, rate);
}
