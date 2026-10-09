#pragma once

// The native AX mixer: owned by the audio thread except for the command ring's
// producer side, the produced-tick counter and the guest memory bases, which
// the game thread sets before the audio thread starts.

#include "native_audio.h"

#include <array>
#include <cstdint>
#include <memory>

namespace NativeAudio {

class Reverb;

struct Voice {
    uint16_t pb[kPbHalfwords];
    bool fading;      // stopped with depop: one more tick fading out
    bool published;   // feedback needs one more publication after stopping
};

class Mixer {
  public:
    Mixer();
    ~Mixer();

    // Game thread, before the audio thread starts (and with it stopped).
    void Initialize(const uint8_t* mem1, uint32_t mem1Size, const uint8_t* mem2, uint32_t mem2Size);
    CommandRing& Ring() { return ring_; }
    Feedback& FeedbackState() { return feedback_; }
    MixerStatistics& Statistics() { return statistics_; }
    void SetProducedTicks(uint32_t produced) { produced_.store(produced, std::memory_order_release); }

    // Audio thread.
    void Render(int16_t* out, uint32_t frames);

  private:
    void RenderTick(int16_t* out);
    uint32_t ApplyCommands(uint32_t tick);  // returns the last applied tick + 1, or 0
    void ApplyVoice(const uint32_t* record);
    void ApplyAux(uint32_t bus, const uint32_t* record);
    void ResetVoices();
    void ProcessVoice(Voice& voice, bool fade);
    uint32_t Decode(Voice& voice, int16_t* out, uint32_t count);
    uint8_t ByteAt(uint32_t address) const;

    CommandRing ring_;
    Feedback feedback_;
    MixerStatistics statistics_;
    std::atomic<uint32_t> produced_{0};

    const uint8_t* mem1_ = nullptr;
    const uint8_t* mem2_ = nullptr;
    uint32_t mem1Size_ = 0, mem2Size_ = 0;

    Voice voices_[kMaxVoices];

    // Bus order: main L R S, aux A L R S, aux B L R S, aux C L R S.
    alignas(16) int32_t bus_[12][kTickSamples];
    uint16_t masterVolume_ = 0x8000, lastMasterVolume_ = 0x8000;
    uint16_t auxReturn_[3] = {0x8000, 0x8000, 0x8000}, lastAuxReturn_[3] = {0x8000, 0x8000, 0x8000};
    uint16_t mode_ = 0;
    std::unique_ptr<Reverb> aux_[3];

    uint32_t rendered_ = 0;  // ticks mixed so far
    alignas(16) int16_t tick_[kTickSamples * 2];
    uint32_t tickPosition_ = kTickSamples;
};

// The native 4-tap ReverbHi (AXFX ReverbHiExp) on native delay lines.
class Reverb {
  public:
    explicit Reverb(const ReverbConfig& config);
    void Process(int32_t* left, int32_t* right, int32_t* surround);

  private:
    struct Line {
        std::unique_ptr<float[]> samples;
        uint32_t length = 0;
        void Allocate(uint32_t n) {
            length = n;
            samples.reset(n ? new float[n]() : nullptr);
        }
    };

    ReverbConfig config_;
    Line early_[3], preDelay_[3], comb_[3][3], allpass_[3][2], last_[3];
    float lowpass_[3] = {};
};

} // namespace NativeAudio
