#pragma once

// The native AX mixer: owned by the audio thread except for the command ring's
// producer side, the produced-tick counter and the guest memory bases, which
// the game thread sets before the audio thread starts.

#include "native_audio.h"

#include <array>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace NativeAudio {

class Reverb;

struct PcmSample;

struct Voice {
    uint16_t pb[kPbHalfwords];
    PcmSample* pcm;   // the decoded copy it plays from, if any (Mixer::PlayDecoded)
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
    uint32_t DecodeFromMemory(Voice& voice, int16_t* out, uint32_t count);
    uint32_t PlayDecoded(Voice& voice, int16_t* out, uint32_t count);
    PcmSample* FindDecoded(const Voice& voice);
    void ForgetDecoded(uint32_t end);
    uint8_t ByteAt(uint32_t address) const;
    const uint8_t* BytesAt(uint32_t address, uint32_t bytes) const;

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

    // Bank samples' ADPCM decoded to PCM16 as voices play them, by end
    // address, and the end addresses whose data changed under a voice (a
    // stream's ring), which always decode from memory.
    std::unordered_map<uint32_t, std::unique_ptr<PcmSample>> decoded_;
    std::unordered_set<uint32_t> rewritten_;
    size_t decodedBytes_ = 0;

    uint32_t rendered_ = 0;  // ticks mixed so far
    alignas(16) int16_t tick_[kTickSamples * 2];
    uint32_t tickPosition_ = kTickSamples;
};

// A bank sample decoded to PCM16 from the first frame a voice played on,
// frame by frame as far as voices got (the ADPCM history carries on from the
// last decoded frame).
struct PcmSample {
    uint32_t base = 0, end = 0;  // nibble addresses: the first frame, the last sample
    uint32_t check = 0;          // SampleCheck of the ADPCM bytes and coefficients
    uint32_t frames = 0;         // frames decoded
    int32_t yn1 = 0, yn2 = 0;    // the history after them
    uint32_t lastUse = 0;        // the tick a voice last played it
    const uint8_t* data = nullptr;  // the ADPCM bytes from `base` on
    uint16_t coefs[16] = {};
    std::vector<int16_t> pcm;    // every sample to `end`; the first frames * 14 decoded

    uint32_t Index(uint32_t address) const { return ((address - base) >> 4) * 14 + (address & 0xF) - 2; }
    uint32_t Address(uint32_t index) const { return base + index / 14 * 16 + index % 14 + 2; }
};

// The native 4-tap ReverbHi (AXFX ReverbHiExp) on native delay lines.
class Reverb {
  public:
    explicit Reverb(const ReverbConfig& config);
    void Process(int32_t* left, int32_t* right, int32_t* surround);
    void Prefetch() const;
    void SetGains(const float gains[4]);

  private:
    void ProcessSamples(int32_t* left, int32_t* right, int32_t* surround);
    void Clear();

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
    bool heard_[3] = {};  // the channel had input since the reverb started
    bool idle_ = false;   // skipped at an output gain of 0

    // The longest block of a tick that runs stage by stage: no line is
    // shorter and no early tap reads what the block wrote (Reverb::Reverb).
    uint32_t block_ = 0;
};

} // namespace NativeAudio
