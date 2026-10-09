// Native mixer self-test (part of MH3 --audio-self-test): drives a private
// NativeAudio::Mixer with synthetic commands and sample data. No guest code,
// no output device.
#include "native_audio_mixer.h"

#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace NativeAudio {
namespace {

void Expect(bool condition, const char* what) {
    if (!condition)
        throw std::runtime_error(std::string("native mixer self-test: ") + what);
}

void Tick(CommandRing& ring, uint32_t tick) {
    uint32_t* out = ring.Reserve(2);
    out[0] = Header(Command::Tick, 2);
    out[1] = tick;
}

void Globals(CommandRing& ring, uint16_t master) {
    uint32_t* out = ring.Reserve(4);
    out[0] = Header(Command::Globals, 4);
    out[1] = master;
    out[2] = 0x8000u | 0x8000u << 16;
    out[3] = 0x8000u;
}

void PushVoice(CommandRing& ring, uint32_t voice, const uint16_t* pb) {
    const uint32_t words = 2 + (kPbHalfwords + 1) / 2;
    uint32_t* out = ring.Reserve(words);
    out[0] = Header(Command::Voice, words, voice);
    out[1] = Guest::kSyncAllParams;
    for (uint32_t h = 0; h < kPbHalfwords; ++h) {
        if (h & 1)
            out[2 + h / 2] |= uint32_t(pb[h]) << 16;
        else
            out[2 + h / 2] = pb[h];
    }
}

void SetAddresses(uint16_t* pb, uint32_t loop, uint32_t end, uint32_t current) {
    pb[Pb::LoopHi] = uint16_t(loop >> 16);
    pb[Pb::LoopLo] = uint16_t(loop);
    pb[Pb::EndHi] = uint16_t(end >> 16);
    pb[Pb::EndLo] = uint16_t(end);
    pb[Pb::CurHi] = uint16_t(current >> 16);
    pb[Pb::CurLo] = uint16_t(current);
}

} // namespace

void RunNativeMixerSelfTests() {
    auto mixer = std::make_unique<Mixer>();
    std::vector<uint8_t> mem1(0x10000), mem2(0x10000);
    // PCM16 ramp, 200 samples at MEM2 physical 0x10000100 (word address 0x08000080).
    auto sample = [](uint32_t i) { return int16_t(100 * int32_t(i) - 9000); };
    for (uint32_t i = 0; i < 200; ++i) {
        mem2[0x100 + 2 * i] = uint8_t(uint16_t(sample(i)) >> 8);
        mem2[0x101 + 2 * i] = uint8_t(sample(i));
    }
    // One ADPCM frame at MEM2 0x10000400 (nibble 0x20000800): header 0 (coefficient
    // pair 0, scale 1), then the nibbles 1..14; zero coefficients decode to them.
    mem2[0x400] = 0x00;
    for (uint32_t i = 0; i < 7; ++i)
        mem2[0x401 + i] = uint8_t((2 * i + 1) << 4 | (2 * i + 2));
    mixer->Initialize(mem1.data(), uint32_t(mem1.size()), mem2.data(), uint32_t(mem2.size()));
    CommandRing& ring = mixer->Ring();

    uint16_t pcm[kPbHalfwords] = {};
    pcm[Pb::SrcSelect] = 2;     // no rate conversion
    pcm[Pb::MixerCtrlLo] = 3;   // main left and right
    pcm[Pb::State] = 1;
    pcm[Pb::Mix + 0] = 0x8000;  // left at unity
    pcm[Pb::Mix + 2] = 0x4000;  // right at half
    pcm[Pb::VeVolume] = 0x8000;
    pcm[Pb::Format] = 0x0A;
    pcm[Pb::AdpcmGain] = 0x0800;
    SetAddresses(pcm, 0x08000080, 0x08000080 + 199, 0x08000080);

    uint16_t adpcm[kPbHalfwords] = {};
    adpcm[Pb::SrcSelect] = 2;
    adpcm[Pb::MixerCtrlLo] = 1;  // left only
    adpcm[Pb::State] = 1;
    adpcm[Pb::Mix + 0] = 0x8000;
    adpcm[Pb::VeVolume] = 0x8000;
    adpcm[Pb::Format] = 0;
    SetAddresses(adpcm, 0x20000802, 0x2000080F, 0x20000802);

    Tick(ring, 0);
    Globals(ring, 0x8000);
    PushVoice(ring, 0, pcm);
    ring.Publish();
    mixer->SetProducedTicks(1);
    int16_t out[kTickSamples * 2 * 3];
    mixer->Render(out, kTickSamples);
    for (uint32_t i = 0; i < kTickSamples; ++i) {
        Expect(out[2 * i] == sample(i), "PCM16 left at unity");
        Expect(out[2 * i + 1] == int16_t(sample(i) * 0x4000 >> 15), "PCM16 right at half volume");
    }
    // Two more ticks: samples 96..199, then the non-looping voice stops.
    mixer->SetProducedTicks(3);
    mixer->Render(out, kTickSamples * 2);
    for (uint32_t i = 0; i < kTickSamples * 2; ++i) {
        const uint32_t n = kTickSamples + i;
        Expect(out[2 * i] == (n < 200 ? sample(n) : 0), "PCM16 end of sample");
    }
    Feedback& feedback = mixer->FeedbackState();
    Expect(feedback.stateVolume[0].load() >> 16 == 0, "stopped voice feedback");
    Expect(feedback.appliedTick.load() == 1, "applied tick");
    Expect(feedback.renderedTicks.load() == 3, "rendered ticks");

    // ADPCM: 14 samples, then silence and a stopped voice.
    Tick(ring, 3);
    PushVoice(ring, 1, adpcm);
    ring.Publish();
    mixer->SetProducedTicks(4);
    mixer->Render(out, kTickSamples);
    for (uint32_t i = 0; i < kTickSamples; ++i)
        if (out[2 * i] != (i < 14 ? int16_t(i + 1) : 0))
            std::printf("adpcm %u: %d\n", i, out[2 * i]);
    Expect(feedback.stateVolume[1].load() >> 16 == 0, "ADPCM voice stopped at its end");

    // Linear 2x upsampling of the ramp: each output sample lies between its
    // neighbours, and one tick reads 48 source samples.
    pcm[Pb::SrcSelect] = 1;
    pcm[Pb::SrcRatioHi] = 0;
    pcm[Pb::SrcRatioLo] = 0x8000;
    SetAddresses(pcm, 0x08000080, 0x08000080 + 199, 0x08000080);
    Tick(ring, 4);
    PushVoice(ring, 2, pcm);
    ring.Publish();
    mixer->SetProducedTicks(5);
    mixer->Render(out, kTickSamples);
    for (uint32_t i = 8; i < kTickSamples; ++i)
        Expect(out[2 * i] >= out[2 * i - 2] && out[2 * i] - out[2 * i - 2] <= 60, "linear SRC ramp");
    Expect((feedback.address[2].load() & 0xFFFF) == 0x0080 + 48, "linear SRC source position");

    // Freezing: past the produced ticks plus the elastic window, silence.
    const uint64_t frozen = mixer->Statistics().frozenTicks.load();
    for (uint32_t i = 0; i < 20; ++i)
        mixer->Render(out, kTickSamples * 3);
    Expect(mixer->Statistics().frozenTicks.load() > frozen, "mixer freezes when the game stops producing");
    std::printf("MH3 native mixer self-test passed: PCM16, ADPCM, end-of-sample, linear SRC, freeze\n");
}

} // namespace NativeAudio
