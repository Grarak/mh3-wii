#pragma once

// The voice operations of the game's sound engine (sound_engine.cpp,
// sound_stream.cpp): the AX calls it makes and the AXVPB fields it writes
// and reads directly. `voice` is the AXVPB the engine keeps in its records.
// Implemented in native_audio_api.cpp.
#include <cstdint>

namespace SoundVoice {

// AXPBADDR: loop flag, format (0 ADPCM, 10 PCM16, 0x19 PCM8) and the loop,
// end and current addresses (in samples or nibbles).
struct Addr {
    uint16_t loopFlag, format;
    uint32_t loop, end, current;
};
// AXPBADPCM: 8 coefficient pairs, gain and the decoder state.
struct Adpcm {
    uint16_t coefs[16];
    uint16_t gain, predScale, yn1, yn2;
};

void Free(uint32_t voice);                                // AXFreeVoice
void SetState(uint32_t voice, uint32_t state);           // AXSetVoiceState
void SetPriority(uint32_t voice, uint32_t priority);     // AXSetVoicePriority
void SetSrcType(uint32_t voice, uint32_t type);          // AXSetVoiceSrcType
void SetSrcRatio(uint32_t voice, float ratio);           // AXSetVoiceSrcRatio
void SetCurrentAddr(uint32_t voice, uint32_t address);   // AXSetVoiceCurrentAddr
void SetAdpcmLoop(uint32_t voice, uint16_t predScale, uint16_t yn1, uint16_t yn2);  // AXSetVoiceAdpcmLoop
void SetLpf(uint32_t voice, uint16_t on, uint16_t yn1, uint16_t a0, uint16_t b0);   // AXSetVoiceLpf
void LpfCoefs(uint16_t frequency, uint16_t& a0, uint16_t& b0);                       // AXGetLpfCoefs

// The engine's inlined AXSetVoiceAddr/Adpcm/Src/Type writes. SetFormat and
// SetLoop are the two halves of AXPBADDR the stream setup writes separately.
void SetAddr(uint32_t voice, const Addr& addr);
void SetFormat(uint32_t voice, uint16_t loopFlag, uint16_t format);
void SetLoop(uint32_t voice, uint16_t loopFlag, uint32_t loop, uint32_t end, uint32_t current);
void SetAdpcm(uint32_t voice, const Adpcm& adpcm);
void SetSrc(uint32_t voice, uint32_t ratio);             // 16.16, history cleared
void SetType(uint32_t voice, uint16_t type);

uint32_t State(uint32_t voice);          // 0 stopped, 1 running
uint32_t LoopFlag(uint32_t voice);
uint32_t CurrentAddr(uint32_t voice);

} // namespace SoundVoice
