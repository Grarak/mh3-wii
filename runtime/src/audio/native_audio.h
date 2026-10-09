#pragma once

// Native audio, RMHP08. The game's voices are mixed natively on an audio
// thread (Vita core 0); there is no AX, DSP, command list, mailbox or AI DMA.
//
// Game thread (core 2): the game's sound engine (sound_engine.cpp,
// sound_stream.cpp) and MIX drive host voice records (native_audio_api.cpp,
// SoundVoice). Once per 3 ms frame ("tick") the native frame driver
// (native_audio_frame.cpp) sends the voices' changed fields to the mixer as
// a command stream, takes the mixer's state/volume/position back, and runs
// the engine's frame. The driver keeps the produced ticks a little ahead of
// the mixer, so every change is applied at its own tick; latency adapts to
// the game's frame time.
//
// Audio thread: NativeMixer (native_audio_mixer.cpp) applies each tick's
// commands, decodes ADPCM/PCM16/PCM8, resamples, filters, mixes main and aux
// buses, runs the aux reverb and hands 32 kHz stereo PCM to the platform
// output. Nothing on it touches guest code; it reads sample data directly
// from guest RAM.

#include <atomic>
#include <cstdint>
#include <cstring>

struct CpuContext;

namespace NativeAudio {

constexpr uint32_t kMaxVoices = 96;     // AXInit's voice count
constexpr uint32_t kTickSamples = 96;   // one AX frame, 3 ms at 32 kHz
constexpr uint32_t kSampleRate = 32000;

// ---------------------------------------------------------------------------
// Guest layout (RVL SDK AX, 0x4302_145): the SDK state guest code still
// reads through the AX entry points, and the AXVPB array whose addresses are
// the voice handles. All addresses were taken from the disassembly;
// r13-relative ones were computed with Python from r13 0x8079B2E0.
// ---------------------------------------------------------------------------
namespace Guest {
enum : uint32_t {
    kAxInitialized = 0x80797388,  // r13-16216
    kCallbackStack = 0x80797390,  // r13-16208, voices dropped for DSP time
    kStackHeads = 0x806F9970,     // AXVPB* [32], priority 0 = free voices
    kStackTails = 0x806F99F0,     // AXVPB* [32]
    kAuxACallback = 0x807973D0,   // r13-16144
    kAuxAContext = 0x807973C4,    // r13-16156
    kAuxBCallback = 0x807973CC,   // r13-16148
    kAuxBContext = 0x807973C0,    // r13-16160
    kAuxCCallback = 0x807973C8,   // r13-16152
    kAuxCContext = 0x807973BC,    // r13-16164
    kAuxAClear = 0x807973DC,      // r13-16132, u8 [3]
    kAuxBClear = 0x807973D8,      // r13-16136
    kAuxCClear = 0x807973D4,      // r13-16140
    kAuxIndexWrite = 0x807973A0,  // r13-16192, aux ring slot indices
    kAuxIndexRead = 0x8079739C,   // r13-16196
    kAuxIndexCpu = 0x80797398,    // r13-16200
    kMode = 0x807973F8,           // r13-16104, AXSetMode
    kCompressor = 0x807973F0,     // r13-16112, AXSetCompressor
    kCompressorTable = 0x807973EC,// r13-16116
    kCompressorRelease = 0x807973E8, // r13-16120 (u16)
    kMasterVolume = 0x807973E6,   // r13-16122 (u16)
    kAuxAReturn = 0x807973E4,     // r13-16124 (u16)
    kAuxBReturn = 0x807973E2,     // r13-16126 (u16)
    kAuxCReturn = 0x807973E0,     // r13-16128 (u16)
    kCommandListPtr = 0x807973FC, // r13-16100
    kCommandListIndex = 0x80797400, // r13-16096
    kCommandListCycles = 0x807973F4, // r13-16108
    kUserCallback = 0x80797434,   // r13-16044, AXRegisterCallback
    kOutputBufferMode = 0x80797418,
    kDspPbs = 0x80797498,         // r13-15944, DSP PB array (0x140 per voice)
    kItdBuffers = 0x8079749C,     // r13-15940 (0x40 per voice)
    kVpbs = 0x807974A0,           // r13-15936, AXVPB array (0x168 per voice)
    kVoiceCount = 0x807974A4,     // r13-15932
    kSyncedVoices = 0x807974A8,   // r13-15928
    kDspCycles = 0x807974AC,
    kMaxDspCycles = 0x807974B0,
    kSdkVersion = 0x807961D8,     // r13-20744, "AX" version string pointer
    kSpbGlobals = 0x80797448,     // r13-16024 .. r13-15948, 20 words
    kDspPbStorage = 0x806FDB80,   // __AXVPBInit's arrays
    kItdStorage = 0x80705380,
    kVpbStorage = 0x80706B80,
    kAuxABuffers = 0x806F9A80,    // 3 slots x 0x600
    kAuxBBuffers = 0x806FAC80,    // 3 slots x 0x600
    kAuxCBuffers = 0x806FBE80,    // 3 slots x 0x480
    kAxfxReverbHiCallback = 0x80475440,
    kAxfxReverbHiDpl2Callback = 0x804755A0,
};

// AXVPB (0x168 bytes).
enum : uint32_t {
    kVpbNext = 0x00,
    kVpbPrev = 0x04,
    kVpbNext1 = 0x08,
    kVpbPriority = 0x0C,
    kVpbCallback = 0x10,
    kVpbUserContext = 0x14,
    kVpbIndex = 0x18,
    kVpbSync = 0x1C,
    kVpbDepop = 0x20,
    kVpbItdBuffer = 0x24,
    kVpbPb = 0x28,  // AXPB, 0x140 bytes
    kVpbSize = 0x168,
    kPbSize = 0x140,
};

// AXVPB.sync bits: the PB groups a voice changed since the last tick.
enum : uint32_t {
    kSyncSrcSelect = 1u << 0,
    kSyncMixerCtrl = 1u << 1,
    kSyncState = 1u << 2,
    kSyncType = 1u << 3,
    kSyncMix = 1u << 4,
    kSyncItd = 1u << 5,
    kSyncItdTarget = 1u << 6,
    kSyncDpop = 1u << 7,
    kSyncVe = 1u << 8,
    kSyncVeDelta = 1u << 9,
    kSyncAddr = 1u << 10,
    kSyncLoop = 1u << 11,
    kSyncLoopAddr = 1u << 12,
    kSyncEndAddr = 1u << 13,
    kSyncCurrAddr = 1u << 14,
    kSyncAdpcm = 1u << 15,
    kSyncSrc = 1u << 16,
    kSyncSrcRatio = 1u << 17,
    kSyncAdpcmLoop = 1u << 18,
    kSyncLpf = 1u << 19,
    kSyncLpfCoef = 1u << 20,
    kSyncBiquad = 1u << 21,
    kSyncBiquadCoef = 1u << 22,
    kSyncAllParams = 1u << 31,
    // __AXSetPBDefault: state, ITD, LPF, biquad and the remote filters.
    kSyncPbDefault = 0x18A80024u,
};
} // namespace Guest

// ---------------------------------------------------------------------------
// A voice's state is the AXPB layout through its biquad (0xD6 bytes), as
// host-endian halfwords at the AXPB halfword offsets: a sync copies halfword
// ranges, as __AXSyncPB copied them into the DSP PB.
// ---------------------------------------------------------------------------
constexpr uint32_t kPbHalfwords = 0xD6 / 2;

namespace Pb {
enum : uint32_t {
    SrcSelect = 0x08 / 2,
    CoefSelect = 0x0A / 2,
    MixerCtrlHi = 0x0C / 2,
    MixerCtrlLo = 0x0E / 2,
    State = 0x10 / 2,
    Type = 0x12 / 2,  // 1: stream (keeps its ADPCM history across the loop)
    Mix = 0x14 / 2,   // 12 x (volume, delta): L R AL AR BL BR CL CR S AS BS CS
    Itd = 0x44 / 2,
    ItdTarget = 0x4E / 2,
    Dpop = 0x52 / 2,
    VeVolume = 0x6A / 2,
    VeDelta = 0x6C / 2,
    LoopFlag = 0x6E / 2,
    Format = 0x70 / 2,
    LoopHi = 0x72 / 2,
    LoopLo = 0x74 / 2,
    EndHi = 0x76 / 2,
    EndLo = 0x78 / 2,
    CurHi = 0x7A / 2,
    CurLo = 0x7C / 2,
    AdpcmCoefs = 0x7E / 2,  // 8 x (coef1, coef2)
    AdpcmGain = 0x9E / 2,
    AdpcmPredScale = 0xA0 / 2,
    AdpcmYn1 = 0xA2 / 2,
    AdpcmYn2 = 0xA4 / 2,
    SrcRatioHi = 0xA6 / 2,
    SrcRatioLo = 0xA8 / 2,
    SrcFrac = 0xAA / 2,
    SrcLast = 0xAC / 2,  // 4 samples, oldest first
    LoopPredScale = 0xB4 / 2,
    LoopYn1 = 0xB6 / 2,
    LoopYn2 = 0xB8 / 2,
    LpfOn = 0xBA / 2,
    LpfYn1 = 0xBC / 2,
    LpfA0 = 0xBE / 2,
    LpfB0 = 0xC0 / 2,
    BiquadOn = 0xC2 / 2,
    BiquadXn1 = 0xC4 / 2,
    BiquadXn2 = 0xC6 / 2,
    BiquadYn1 = 0xC8 / 2,
    BiquadYn2 = 0xCA / 2,
    BiquadB0 = 0xCC / 2,
    BiquadB1 = 0xCE / 2,
    BiquadB2 = 0xD0 / 2,
    BiquadA1 = 0xD2 / 2,
    BiquadA2 = 0xD4 / 2,
};
} // namespace Pb

// One halfword range per sync group, indexed by sync bit; bit 31 is the
// whole native PB. Remote-speaker groups (bits 23-30) are not mixed.
struct SyncGroup {
    uint8_t offset, count;
};
constexpr SyncGroup kSyncGroups[32] = {
    {Pb::SrcSelect, 2}, {Pb::MixerCtrlHi, 2}, {Pb::State, 1}, {Pb::Type, 1}, {Pb::Mix, 24}, {Pb::Itd, 7}, {Pb::ItdTarget, 2}, {Pb::Dpop, 12},
    {Pb::VeVolume, 2},  {Pb::VeDelta, 1},     {Pb::LoopFlag, 8}, {Pb::LoopFlag, 1}, {Pb::LoopHi, 2}, {Pb::EndHi, 2}, {Pb::CurHi, 2}, {Pb::AdpcmCoefs, 20},
    {Pb::SrcRatioHi, 7}, {Pb::SrcRatioHi, 2}, {Pb::LoopPredScale, 3}, {Pb::LpfOn, 4}, {Pb::LpfA0, 2}, {Pb::BiquadOn, 10}, {Pb::BiquadB0, 5}, {0, 0},
    {0, 0},             {0, 0},           {0, 0},      {0, 0},     {0, 0},      {0, 0},     {0, 0},     {0, kPbHalfwords},
};

// ---------------------------------------------------------------------------
// Game -> mixer command stream: one producer (the game thread), one consumer
// (the audio thread), 32-bit words. Every tick's records follow a Tick
// marker and are published together, so the consumer never sees half a tick.
// ---------------------------------------------------------------------------
enum class Command : uint8_t {
    Pad = 0,      // filler up to the ring's end
    Tick = 1,     // [hdr][tick]
    Voice = 2,    // [hdr voice flags][groups][halfwords, two per word]
    Stop = 3,     // [hdr voice flags]: a free voice, state 0
    Globals = 4,  // [hdr][master | mode << 16][auxA | auxB << 16][auxC]
    Aux = 5,      // [hdr bus][kind][parameters...]
    Reset = 6,    // [hdr]: stop every voice, drop every effect
};
enum : uint32_t { kFlagDepop = 1 };

inline uint32_t Header(Command type, uint32_t words, uint32_t voice = 0, uint32_t flags = 0) {
    return uint32_t(type) | voice << 8 | flags << 16 | words << 24;
}
inline Command HeaderType(uint32_t header) {
    return Command(header & 0xFF);
}
inline uint32_t HeaderVoice(uint32_t header) {
    return (header >> 8) & 0xFF;
}
inline uint32_t HeaderFlags(uint32_t header) {
    return (header >> 16) & 0xFF;
}
inline uint32_t HeaderWords(uint32_t header) {
    return header >> 24;
}

class CommandRing {
  public:
    static constexpr uint32_t kWords = 1u << 16;  // 256 KiB
    static constexpr uint32_t kMask = kWords - 1;

    // Producer.
    uint32_t Free() const { return kWords - (pending_ - tail_.load(std::memory_order_acquire)); }
    // Contiguous room for `words`; null when the ring is full (the caller
    // checked Free() for its whole tick first, so this does not happen mid-tick).
    uint32_t* Reserve(uint32_t words) {
        uint32_t position = pending_ & kMask;
        const uint32_t toEnd = kWords - position;
        if (words > toEnd) {
            if (Free() < toEnd + words)
                return nullptr;
            data_[position] = Header(Command::Pad, 0) | (toEnd << 8);  // pad length in the voice/flags/words bits
            pending_ += toEnd;
            position = 0;
        } else if (Free() < words) {
            return nullptr;
        }
        pending_ += words;
        return &data_[position];
    }
    void Publish() { head_.store(pending_, std::memory_order_release); }
    uint32_t Used() const { return pending_ - tail_.load(std::memory_order_acquire); }

    // Consumer. Peek returns the next record's first word, skipping pads.
    const uint32_t* Peek() {
        const uint32_t head = head_.load(std::memory_order_acquire);
        uint32_t tail = tail_.load(std::memory_order_relaxed);
        while (tail != head) {
            const uint32_t* record = &data_[tail & kMask];
            if (HeaderType(*record) != Command::Pad)
                return record;
            tail += *record >> 8;
            tail_.store(tail, std::memory_order_release);
        }
        return nullptr;
    }
    void Pop(uint32_t words) { tail_.store(tail_.load(std::memory_order_relaxed) + words, std::memory_order_release); }
    void Clear() {
        pending_ = 0;
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
    }

  private:
    alignas(64) std::atomic<uint32_t> head_{0};
    alignas(64) std::atomic<uint32_t> tail_{0};
    alignas(64) uint32_t pending_ = 0;
    uint32_t data_[kWords];
};

// Native AXFX reverb (ReverbHiExp), configured from the guest effect state
// the translated AXFXReverbHiInit/Settings computed. Delay lines are native.
struct ReverbConfig {
    uint32_t earlyLength, earlyTaps[3];
    float earlyGains[3];
    uint32_t preDelayLength, preDelayPos;
    uint32_t combLength[3], combPos[3];
    float combGains[3];
    uint32_t allpassLength[2], allpassPos[2];
    uint32_t lastLength[3], lastPos[3];
    float allpassGain, lowpassCoef, crosstalk, level, outGain, sendGain;
};
enum class AuxKind : uint32_t { None = 0, ReverbHi = 1 };
constexpr uint32_t kReverbConfigWords = sizeof(ReverbConfig) / 4;

// Mixer -> game: what the "DSP" did to each voice, as __AXSyncPB copied it
// back (state, envelope volume, current address), and how far it got.
struct Feedback {
    std::atomic<uint32_t> stateVolume[kMaxVoices];  // state << 16 | ve volume
    std::atomic<uint32_t> address[kMaxVoices];      // current address
    std::atomic<uint32_t> appliedTick{0};           // last Tick marker applied
    std::atomic<uint32_t> renderedTicks{0};         // ticks mixed (the audio clock)
};

// Counters for MH3_STATS. The audio thread writes its own; the game thread
// reads them once a second.
struct MixerStatistics {
    std::atomic<uint64_t> renderNs{0}, renderedTicks{0}, frozenTicks{0}, lateTicks{0}, outputFrames{0};
    std::atomic<uint32_t> activeVoices{0}, peakVoices{0};
    std::atomic<int> thread{-1}, core{-1};
    std::atomic<uint32_t> affinity{0};
};

class Mixer;
Mixer& GetMixer();

// Audio thread entry: renders `frames` interleaved stereo frames (left,
// right) into `out`, a whole tick at a time internally.
void MixerRender(int16_t* out, uint32_t frames);
// Platform output (runtime/src/audio_output.cpp, runtime/vita/audio_output.cpp).
// Starts the thread that pulls MixerRender at the device rate; idempotent.
void StartAudioOutput();
void StopAudioOutput();
// Records the audio thread's identity (once, from that thread).
void NoteAudioThread();

// Native voices (native_audio_api.cpp), game thread: sends each voice's
// changed groups for `tick`, stops freed voices once and takes the mixer's
// state, envelope volume and position back; returns the voice records sent.
uint32_t FlushVoices(CommandRing& ring, const Feedback& feedback, uint32_t tick);
// After a mixer reset: no voice runs in the mixer.
void ForgetMixerVoices();

// Game-thread side (native_audio_frame.cpp).
void ServiceNativeAudio(CpuContext* cpu);
bool NativeAudioActive();
void ReportNativeAudioStatistics(double seconds);

} // namespace NativeAudio
