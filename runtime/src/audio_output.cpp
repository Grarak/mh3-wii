#include "audio_dma.h"
#include "audio_output_statistics.h"
#include <SDL3/SDL.h>
#include <cstdlib>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>
namespace {
SDL_AudioStream* stream = nullptr;
uint32_t streamRate = 0;
void Shutdown() {
    if (stream)
        SDL_DestroyAudioStream(stream);
    stream = nullptr;
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}
void Check(bool result) {
    if (!result)
        throw std::runtime_error(std::string("SDL audio: ") + SDL_GetError());
}
} // namespace
void QueueHostPcm(const uint8_t* data, uint32_t bytes, uint32_t rate) {
    if (bytes % 4 || (rate != 32000 && rate != 48000))
        throw std::runtime_error("Invalid Wii PCM block");
    if (!stream) {
        Check(SDL_InitSubSystem(SDL_INIT_AUDIO));
        SDL_AudioSpec spec{SDL_AUDIO_S16, 2, int(rate)};
        stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        Check(stream != nullptr);
        Check(SDL_ResumeAudioStreamDevice(stream));
        streamRate = rate;
        std::atexit(Shutdown);
        std::fprintf(stderr, "MH3 audio: SDL playback opened at %u Hz stereo\n", rate);
    }
    if (rate != streamRate) {
        SDL_AudioSpec spec{SDL_AUDIO_S16, 2, int(rate)};
        Check(SDL_SetAudioStreamFormat(stream, &spec, nullptr));
        streamRate = rate;
    }
#ifdef MH3_DEVELOPMENT
    // MH3_AUDIO_DUMP=file: every guest PCM block as raw s16le stereo at the
    // rate printed at open (playback drops below do not apply).
    static FILE* dump = [] {
        const char* path = std::getenv("MH3_AUDIO_DUMP");
        return path ? std::fopen(path, "wb") : nullptr;
    }();
    if (dump) {
        const auto pcm = DecodeWiiPcm(data, bytes);
        std::fwrite(pcm.data(), 1, bytes, dump);
        std::fflush(dump);
    }
#endif
    // Bound host latency after long guest stalls while still delivering the
    // corresponding guest completion callbacks. Report dropped audio explicitly.
    if (SDL_GetAudioStreamQueued(stream) > int(rate)) {
        static bool reported = false;
        if (!reported) {
            std::fprintf(stderr, "MH3 audio: playback backlog; dropping excess PCM blocks\n");
            reported = true;
        }
        return;
    }
    const auto samples = DecodeWiiPcm(data, bytes);
    Check(SDL_PutAudioStreamData(stream, samples.data(), bytes));
}

NativeAudioOutputStatistics GetNativeAudioOutputStatistics() {
    return {};
}
