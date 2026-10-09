// RVL SDK SEQ and SYN entry points, RMHP08, as native C. The game's AX frame
// callback runs SEQRunAudioFrame and SYNRunAudioFrame every 3 ms, but MH3
// never adds a MIDI sequence or initializes a synth: the sequence list
// (0x8079792C) and the synth list (0x80797950) are written only by these
// init/quit functions, and a SYN voice gets a synth only from a synth's
// note-on. So both frames have nothing to do; reaching a sequence, synth or
// synth voice is reported as unsupported instead of running the MIDI path.
#include "abi_bridge.h"
#include "memory_access.h"

#include <stdexcept>

using MemoryInline::FlatRead32;
using MemoryInline::FlatWrite32;

namespace {

constexpr uint32_t kSeqInitialized = 0x80797928, kSeqList = 0x8079792C;
constexpr uint32_t kSynVoices = 0x80797948, kSynInitialized = 0x8079794C, kSynList = 0x80797950, kSynVoiceCount = 0x80797954;
constexpr uint32_t kSynVoiceStorage = 0x80759CE8, kSynVoiceSize = 0x94, kSynVoiceSynth = 0x08;  // SYNInit
constexpr uint32_t kAxInitialized = 0x80797388, kAxVoiceCount = 0x807974A4;

} // namespace

extern "C" {

// SEQInit (0x804DDAB0).
void MH3_SEQInit(CpuContext*) {
    if (FlatRead32(kSeqInitialized))
        return;

    FlatWrite32(kSeqList, 0);
    FlatWrite32(kSeqInitialized, 1);
}

// SEQQuit (0x804DDAD0).
void MH3_SEQQuit(CpuContext*) {
    FlatWrite32(kSeqList, 0);
    FlatWrite32(kSeqInitialized, 0);
}

// SEQRunAudioFrame (0x804DDAE0).
void MH3_SEQRunAudioFrame(CpuContext*) {
    if (FlatRead32(kSeqInitialized) && FlatRead32(kSeqList))
        throw std::runtime_error("SEQ sequence playback is not ported (MH3 adds none)");
}

// SYNInit (0x804DF720).
void MH3_SYNInit(CpuContext*) {
    if (FlatRead32(kAxInitialized) == 0 || FlatRead32(kSynInitialized) != 0)
        return;

    const uint32_t count = FlatRead32(kAxVoiceCount);
    FlatWrite32(kSynVoiceCount, count);
    FlatWrite32(kSynVoices, kSynVoiceStorage);

    for (uint32_t i = 0; i < count; ++i)
        FlatWrite32(kSynVoiceStorage + i * kSynVoiceSize + kSynVoiceSynth, 0);

    FlatWrite32(kSynList, 0);
    FlatWrite32(kSynInitialized, 1);
}

// SYNQuit (0x804DF7B0).
void MH3_SYNQuit(CpuContext*) {
    FlatWrite32(kSynVoices, 0);
    FlatWrite32(kSynInitialized, 0);
}

// SYNRunAudioFrame (0x804DF7C0): services every synth voice, then runs each
// synth's queued MIDI input. A voice gets a synth only from a synth's
// note-on, so with no synth listed there is nothing to do; the voices are
// not scanned every frame.
void MH3_SYNRunAudioFrame(CpuContext*) {
    if (FlatRead32(kSynInitialized) && FlatRead32(kSynList))
        throw std::runtime_error("SYN synths are not ported (MH3 creates none)");
}

} // extern "C"

REGISTER_NATIVE_FUNCTION_AS(0x804DDAB0, MH3_SEQInit, "SEQInit");
REGISTER_NATIVE_FUNCTION_AS(0x804DDAD0, MH3_SEQQuit, "SEQQuit");
REGISTER_NATIVE_FUNCTION_AS(0x804DDAE0, MH3_SEQRunAudioFrame, "SEQRunAudioFrame");
REGISTER_NATIVE_FUNCTION_AS(0x804DF720, MH3_SYNInit, "SYNInit");
REGISTER_NATIVE_FUNCTION_AS(0x804DF7B0, MH3_SYNQuit, "SYNQuit");
REGISTER_NATIVE_FUNCTION_AS(0x804DF7C0, MH3_SYNRunAudioFrame, "SYNRunAudioFrame");
