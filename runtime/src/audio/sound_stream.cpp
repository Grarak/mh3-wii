// The game's sound engine, RMHP08: the stream manager (sound_engine.h). A
// stream plays one or two voices (left/right) looping over a ring of three
// chunks per channel in a manager buffer, refilled from the disc by chained
// DVD reads (left chunk, then right chunk, each optionally split at the
// stream's loop point). Every tick it follows the voices' play position,
// re-points them when they would run into a chunk that is not loaded yet,
// issues the next read, and applies fades and volume ramps.
#include "sound_engine.h"

#include "guest_call.h"
#include "port_check.h"
#include "sdk_mix.h"
#include "sound_voice.h"

#include <cmath>

using namespace SoundEngine;

extern "C" {
// Translated originals (port checks).
void func_800EADE4(CpuContext*);
void func_800EAFAC(CpuContext*);
void func_800EAF10(CpuContext*);
void func_800ED07C(CpuContext*);
void func_800ED120(CpuContext*);
void func_800ED198(CpuContext*);
void func_800EB128(CpuContext*);
void func_800EB378(CpuContext*);
void func_800EABBC(CpuContext*);
void func_800EA4A8(CpuContext*);
void func_800EA5EC(CpuContext*);
void func_800ECDE0(CpuContext*);
void func_800ECEBC(CpuContext*);
void func_800EE5EC(CpuContext*);
void func_800EE7E4(CpuContext*);
void func_800EA6E4(CpuContext*);
void func_800EA848(CpuContext*);
void func_800EA9C8(CpuContext*);
void func_800EAAB8(CpuContext*);
void func_800EAB7C(CpuContext*);
}

namespace {

// Stream manager.
enum : uint32_t {
    kBufferCount = 0x08,
    kBuffersUsed = 0x0C,  // bit per buffer
    kStreamCount = 0x10,  // playing streams
    kBuffers = 0x14,      // buffer addresses
};

// Stream record (object + 0x1C).
enum : uint32_t {
    kInfo = 0x00,         // stream info*
    kLeft = 0x04,         // AXVPB*
    kRight = 0x08,        // AXVPB*, 0 for mono
    kFile = 0x0C,         // DVDFileInfo (its command block's user data +0x2C is the object)
    kCancelObject = 0x38,
    kSize = 0x40,         // u32 bytes, both channels
    kEnvelope = 0x48,     // envelope (sound_engine.h offsets); +0 the stream level
    kVolumeOut = 0x4E,    // s16
    kPitch = 0x58,        // s16 cents
    kPan = 0x5A,          // s16
    kAuxA = 0x5C,         // s16 levels
    kAuxB = 0x5E,
    kAuxC = 0x60,
    kLifetime = 0x68,     // u32 ticks until release, ~0 forever
    kCountdown = 0x6C,
    kFadeLength = 0x70,   // u32 ticks
    kFadePosition = 0x74,
    kPlayChunk = 0x89,    // u8 0..2: the chunk the voices play
    kFader = 0x8A,        // s16 level
    kVolume = 0x8C,       // s16 0..127, the ramp's target
    kBuffer = 0x90,       // manager buffer
    kMonoChunkBytes = 0x94,
    kFadeGain = 0x98,     // float
    kRampTarget = 0x9C,   // s16 (percent for kRampScale)
    kRampStart = 0x9E,    // s16
    kRampPosition = 0xA0, // u32
    kReadChunk = 0xA9,    // u8 0..2: the chunk being filled
    kLastRead = 0xAA,     // u8: the chunk the last read was issued for
    kCancelled = 0xAB,    // u8: the DVD reads were cancelled
    kReadOffset = 0xAC,   // u32 disc offset of the next left read
    kReadOffset2 = 0xB0,  // u32 disc offset of the next right read
    kRampLength = 0xBC,   // u32 ticks
    kPosition = 0xC0,     // u32 buffer byte position of the left voice
    kPositionOld = 0xC4,
    kPlayed = 0xC8,       // u32 bytes played (within the loop)
    kLoopEnd = 0xCC,      // u32 bytes, 0 when the stream ends
    kSplitRead = 0xD0,    // u8: the read stops at the loop end, the rest follows from the loop start
    kLastChunk = 0xD1,    // u8: the end of the data was read
    kChannels = 0xD3,     // s8
    kGainCategory = 0xD4, // u16: the slot (PlayStream, GetStreamStatus); its low byte selects the stream gain
    kReadPending = 0xD6,  // u8
    kLooping = 0xD8,      // u8
    kLoops = 0xE0,        // u32
};

// Stream info.
enum : uint32_t {
    kInfoChannels = 0x00,    // u32, 0 stereo (two voices, the left one panned hard left), 1-2 mono
    kInfoFormat = 0x04,      // 0/1 ADPCM, 2/3 PCM16, 4/5 PCM8
    kInfoRate = 0x08,
    kInfoLoopStart = 0x10,   // samples
    kInfoLoopEnd = 0x14,
    kInfoLeft = 0x10,        // per channel: ADPCM at +0x10, its loop context at +0x38
    kInfoRight = 0x50,
};

// Guest functions.
constexpr uint32_t kDvdReadAsyncPrio = 0x804A69A0;  // (file, buffer, length, offset, callback, priority)
constexpr uint32_t kDvdCancelAsync = 0x804AAE90;    // (block, callback)
constexpr uint32_t kDvdClose = 0x804A67A0;          // (file)
constexpr uint32_t kDvdGetCommandBlockStatus = 0x804AACC0;

// The DVD callbacks below (their guest addresses are passed to the reads).
constexpr uint32_t kReadLeftDone = 0x800EA6E4, kReadRightDone = 0x800EA848, kReadLeftRestDone = 0x800EA9C8, kReadRightRestDone = 0x800EAAB8;
constexpr uint32_t kCancelDone = 0x800EAB7C;

uint32_t ChannelBytes() {
    return FlatRead32(kStreamChannelBytes);
}

uint32_t ChunkBytes() {
    return FlatRead32(kStreamChunkBytes);
}

uint32_t ChannelSize(uint32_t record) {
    return FlatRead32(record + kSize) / uint32_t(int32_t(int8_t(FlatRead8(record + kChannels))));
}

uint32_t NextChunk(uint32_t chunk) {
    return chunk + 1 > 2 ? 0 : chunk + 1;
}

void AdvanceReadChunk(uint32_t record) {
    FlatWrite8(record + kReadChunk, uint8_t(NextChunk(FlatRead8(record + kReadChunk))));
}

// Address conversions by format (0x800EE8CC bytes -> voice address,
// 0x800EE918 voice address -> bytes, 0x800EE964 samples -> bytes,
// 0x800EE9DC samples -> voice address). ADPCM packs 14 samples in 8 bytes.
uint32_t Format(uint32_t info) {
    return FlatRead32(info + kInfoFormat);
}

uint32_t BytesToAddress(uint32_t info, uint32_t v) {
    const uint32_t f = Format(info);
    return f < 2 ? v << 1 : f < 4 ? v >> 1 : f < 6 ? v : 0;
}

uint32_t AddressToBytes(uint32_t info, uint32_t v) {
    const uint32_t f = Format(info);
    return f < 2 ? v >> 1 : f < 4 ? v << 1 : f < 6 ? v : 0;
}

uint32_t SamplesToBytes(uint32_t info, uint32_t v) {
    const uint32_t f = Format(info);
    return f < 2 ? (v % 14 + v / 14 * 16) >> 1 : f < 4 ? v << 1 : f < 6 ? v : 0;
}

uint32_t SamplesToAddress(uint32_t info, uint32_t v) {
    const uint32_t f = Format(info);
    return f < 2 ? v % 14 + v / 14 * 16 : f < 4 ? v << 1 : f < 6 ? v : 0;
}

// 0x800EA81C: n rounded up to a multiple of `align`.
uint32_t RoundUp(uint32_t n, uint32_t align) {
    return align ? align * ((n + align - 1) / align) : 0;
}

// The voice rate (16.16) of a sample rate shifted by `cents`, as the
// engine's voice setups compute it.
uint32_t SrcRatio(uint32_t rate, int16_t cents) {
    const double shift = std::pow(2.0, double(cents) / 1200.0);
    const float r = float(rate) * float(shift);
    const double fixed = 65536.0 * double(r / 32000.0f);
    return fixed > 0.0 ? (fixed >= 4294967296.0 ? 0xFFFFFFFFu : uint32_t(fixed)) : 0;
}

// 0x800EE5EC: a stream voice's format, ADPCM state (from `channel`, the
// info's per-channel block) and rate; always looping, type 1 (stream).
void SetupVoice(uint32_t info, uint32_t voice, uint32_t channel, uint32_t ratio) {
    SoundVoice::SetSrc(voice, ratio);
    const uint32_t format = Format(info);

    if (format > 5)
        return;

    static constexpr uint16_t kFormats[3] = {0, 10, 0x19};
    static constexpr uint16_t kGains[3] = {0, 0x800, 0x100};
    SoundVoice::SetFormat(voice, 1, kFormats[format / 2]);
    SoundVoice::Adpcm adpcm{};

    if (format < 2) {
        const uint32_t from = channel + 0x10;

        for (uint32_t i = 0; i < 16; ++i)
            adpcm.coefs[i] = FlatRead16(from + i * 2);

        adpcm.gain = FlatRead16(from + 0x20);
        adpcm.predScale = FlatRead16(from + 0x22);
        adpcm.yn1 = FlatRead16(from + 0x24);
        adpcm.yn2 = FlatRead16(from + 0x26);
    } else {
        adpcm.gain = kGains[format / 2];
    }

    SoundVoice::SetAdpcm(voice, adpcm);

    if (format < 2)
        SoundVoice::SetAdpcmLoop(voice, FlatRead16(channel + 0x38), FlatRead16(channel + 0x3A), FlatRead16(channel + 0x3C));

    SoundVoice::SetType(voice, 1);
}

// 0x800EE7E4.
void SetupVoices(uint32_t info, uint32_t left, uint32_t right, int16_t cents) {
    const uint32_t ratio = SrcRatio(FlatRead32(info + kInfoRate), cents);

    if (left)
        SetupVoice(info, left, info + kInfoLeft, ratio);

    if (right)
        SetupVoice(info, right, info + kInfoRight, ratio);
}

// 0x800EA4A8: a voice's loop over its channel's ring (`address` the ring),
// or over the whole data when it fits, with the stream's own loop points.
void SetVoiceRing(uint32_t record, uint32_t voice, uint32_t address) {
    if (!address)
        return;

    const uint32_t info = FlatRead32(record + kInfo);
    const uint32_t start = BytesToAddress(info, address - 0x80000000u);
    uint32_t loop = start, end;
    uint16_t loopFlag;

    if (ChannelSize(record) > ChannelBytes()) {
        loopFlag = 1;
        end = start + BytesToAddress(info, ChannelBytes()) - 1;
    } else if (FlatRead32(record + kLoopEnd) == 0) {
        loopFlag = 0;
        end = start + BytesToAddress(info, ChannelSize(record)) - 1;
    } else {
        loopFlag = 1;
        loop = start + SamplesToAddress(info, FlatRead32(info + kInfoLoopStart));
        end = start + SamplesToAddress(info, FlatRead32(info + kInfoLoopEnd)) - 1;
    }

    if (Format(info) < 2)
        loop += 2;

    SoundVoice::SetLoop(voice, loopFlag, loop, end, start);
}

// 0x800EA5EC: points the voices at the start of the next chunk.
void Seek(uint32_t record) {
    const uint32_t buffer = FlatRead32(record + kBuffer);

    if (!buffer)
        return;

    const uint32_t info = FlatRead32(record + kInfo), left = FlatRead32(record + kLeft), right = FlatRead32(record + kRight);
    const uint32_t chunk = NextChunk(FlatRead8(record + kPlayChunk));

    if (!right) {
        const uint32_t a = BytesToAddress(info, buffer + chunk * FlatRead32(record + kMonoChunkBytes) - 0x80000000u);

        if (left)
            SoundVoice::SetCurrentAddr(left, a);

        return;
    }

    const uint32_t l = buffer + chunk * ChunkBytes(), r = l + ChannelBytes();
    const uint32_t a = BytesToAddress(info, l - 0x80000000u), b = BytesToAddress(info, r - 0x80000000u);

    if (left)
        SoundVoice::SetCurrentAddr(left, a);

    if (right)
        SoundVoice::SetCurrentAddr(right, b);
}

// 0x800EBAAC: takes a free manager buffer (0 if none).
uint32_t TakeBuffer(uint32_t manager) {
    const uint32_t used = FlatRead32(manager + kBuffersUsed);
    uint32_t n = 0;

    for (uint32_t left = FlatRead32(manager + kBufferCount);; ++n, --left) {
        if (left == 0)
            return 0;

        if (!(used & (1u << n)))
            break;
    }

    FlatWrite32(manager + kBuffersUsed, used | 1u << n);
    return FlatRead32(manager + kBuffers + n * 4);
}

// 0x800EBB04.
void GiveBuffer(uint32_t manager, uint32_t buffer) {
    const uint32_t count = FlatRead32(manager + kBufferCount);

    for (uint32_t i = 0; i < count; ++i)
        if (FlatRead32(manager + kBuffers + i * 4) == buffer)
            FlatWrite32(manager + kBuffersUsed, FlatRead32(manager + kBuffersUsed) & ~(1u << i));
}

void Read(CpuContext* cpu, uint32_t record, uint32_t buffer, uint32_t length, uint32_t offset, uint32_t callback) {
    GuestFrame frame(cpu, 0x20);
    GuestCall{cpu}.Call<kDvdReadAsyncPrio>({record + kFile, buffer, length, offset, callback, 0});
}

void CancelReads(CpuContext* cpu, uint32_t object) {
    const uint32_t record = Record(object);
    FlatWrite32(record + kCancelObject, object);
    GuestFrame frame(cpu, 0x20);
    GuestCall{cpu}.Call<kDvdCancelAsync>({record + kFile, kCancelDone});
}

// 0x800ECEBC: reads the next chunk's left half (the DVD callbacks chain the
// rest) when the read chunk is free and data remains.
void Refill(CpuContext* cpu, uint32_t object) {
    const uint32_t record = Record(object);
    const uint32_t chunk = FlatRead8(record + kReadChunk);

    if (FlatRead8(record + kLastRead) == chunk || chunk == FlatRead8(record + kPlayChunk))
        return;

    const uint32_t size = FlatRead32(record + kSize);

    if (size == 0)
        return;

    if (FlatRead32(record + kLoopEnd) != 0 && !(size > FlatRead32(record + kReadOffset2)))
        return;

    const uint32_t chunkBytes = ChunkBytes();
    const uint32_t buffer = FlatRead32(record + kBuffer) + chunk * chunkBytes;
    FlatWrite32(record + kCancelObject, object);
    const uint32_t channelSize = ChannelSize(record);
    uint32_t length = chunkBytes;

    if (channelSize > ChannelBytes()) {
        const uint32_t loopEnd = FlatRead32(record + kLoopEnd);

        if (loopEnd != 0) {
            const uint32_t offset = FlatRead32(record + kReadOffset);

            if (offset + chunkBytes > loopEnd) {
                length = loopEnd - offset;
                FlatWrite8(record + kSplitRead, 1);
                FlatWrite32(record + kLoops, FlatRead32(record + kLoops) + 1);
            }
        } else {
            const uint32_t end = FlatRead32(record + kReadOffset2) + chunkBytes;

            if (end > size)
                length = end - size;

            if (FlatRead8(record + kLastChunk) == 0 && size - FlatRead32(kStreamEndMargin) < end)
                FlatWrite8(record + kLastChunk, 1);
        }
    } else {
        length = channelSize;

        if (FlatRead32(record + kLoopEnd) == 0)
            FlatWrite8(record + kLastChunk, 1);
    }

    const uint32_t offset = FlatRead32(record + kReadOffset);

    if (offset + length > ChannelSize(record))
        length = ChannelSize(record) - offset;

    const uint32_t bytes = RoundUp(length, 32);
    Set(object, kChunkLoading << chunk);
    Clear(object, kChunkLoaded << chunk);
    FlatWrite8(record + kLastRead, uint8_t(chunk));
    Read(cpu, record, buffer, bytes, offset, kReadLeftDone);
}

// 0x800ECDE0: pauses the stream while the drive cannot read (error, cover
// open, ...), resumes it after, and refills when idle.
void ServiceDisc(CpuContext* cpu, uint32_t manager, uint32_t object) {
    int32_t status;
    {
        GuestFrame frame(cpu, 0x20);
        status = int32_t(GuestCall{cpu}.Call<kDvdGetCommandBlockStatus>({Record(object) + kFile}));
    }

    if (uint32_t(status - 6) < 2 || status == -1 || status == 4) {
        Set(object, kDvdPaused);
        ManagerPause(cpu, manager, object);
        return;
    }

    if (status == 0)
        Refill(cpu, object);

    if (Has(object, kDvdPaused)) {
        ManagerResume(cpu, manager, object);
        Clear(object, kDvdPaused);
    }
}

// 0x800EABBC: follows the left voice through the ring; at a chunk boundary
// re-points the voices to the next chunk, or skips it while it is loaded.
// Stops the stream at the end of its data.
void Follow(CpuContext* cpu, uint32_t manager, uint32_t object) {
    const uint32_t record = Record(object);
    const uint32_t buffer = FlatRead32(record + kBuffer), left = FlatRead32(record + kLeft);

    if (!buffer || !left)
        return;

    const uint32_t info = FlatRead32(record + kInfo);
    const uint32_t base = buffer - 0x80000000u, chunk = ChunkBytes();
    const uint32_t first = base + chunk, second = base + chunk * 2;
    const uint32_t position = AddressToBytes(info, SoundVoice::CurrentAddr(left));
    FlatWrite32(record + kPosition, position);
    const uint32_t old = FlatRead32(record + kPositionOld);
    uint32_t moved = position - old;
    bool boundary = false;

    if (old < first && first < position)
        boundary = true;
    else if (old < second && second < position)
        boundary = true;
    else if (position < old) {
        boundary = true;
        moved += ChannelBytes();
    }

    const uint32_t played = FlatRead32(record + kPlayed), loopEnd = FlatRead32(record + kLoopEnd);

    if (loopEnd == 0 || FlatRead8(record + kLooping) == 1 || played + moved < loopEnd)
        FlatWrite32(record + kPlayed, played + moved);
    else
        FlatWrite32(record + kPlayed, played + moved - loopEnd + SamplesToBytes(info, FlatRead32(info + kInfoLoopStart)));

    if (FlatRead8(record + kLastChunk) == 1 && ChannelSize(record) - FlatRead32(kStreamEndMargin) < FlatRead32(record + kPlayed)) {
        ManagerStop(cpu, manager, object);
        return;
    }

    FlatWrite32(record + kPositionOld, FlatRead32(record + kPosition));

    if (boundary && ChannelBytes() < ChannelSize(record)) {
        if (!Has(object, kChunkLoaded << NextChunk(FlatRead8(record + kPlayChunk))))
            Seek(record);
        else
            FlatWrite8(record + kPlayChunk, uint8_t(NextChunk(FlatRead8(record + kPlayChunk))));
    }
}

int16_t Clamp127(int32_t v) {
    return int16_t(v > 127 ? 127 : v < 0 ? 0 : v);
}

// A ramp step of the output volume toward kVolume (in the envelope's scale).
void RampStep(uint32_t record, float envelope, uint32_t position) {
    const int16_t start = S16(record + kRampStart);
    const float t = float(position);
    const float delta = float(S16(record + kVolume) - start);
    const float length = 1.0f + float(FlatRead32(record + kRampLength));
    const float step = t * (delta / length);
    const float v = float(start) + step;
    FlatWrite16(record + kVolumeOut, uint16_t(int32_t(envelope * v)));
}

// The end of a ramp: the output volume is the target's.
void RampEnd(uint32_t record, float envelope) {
    FlatWrite16(record + kVolumeOut, uint16_t(int32_t(float(S16(record + kVolume)) * envelope)));
    FlatWrite16(record + kRampStart, FlatRead16(record + kVolumeOut));
}

// 0x800EB378: fades and volume ramps, then the voices' input level.
void UpdateVolume(uint32_t object) {
    const uint32_t record = Record(object);
    float envelope = 1.0f;
    bool changed = false;
    const uint8_t state = FlatRead8(record + kEnvelope + kEnvState);

    if (state != kEnvStart && state != kEnvHolding) {
        envelope = float(S16(record + kEnvelope + kEnvLevel)) / 127.0f;
        changed = true;
    }

    if (Has(object, kFadeIn)) {
        const uint32_t position = FlatRead32(record + kFadePosition) + 1, length = FlatRead32(record + kFadeLength);
        FlatWrite32(record + kFadePosition, position);

        if (position < length) {
            FlatWriteFloat32(record + kFadeGain, float(position) / float(length + 1));
        } else {
            FlatWriteFloat32(record + kFadeGain, 1.0f);
            Clear(object, kFadeIn);
        }

        FlatWrite16(record + kVolumeOut, FlatRead16(record + kRampStart));
        changed = true;
    }

    if (Has(object, kFadeOut)) {
        const uint32_t position = FlatRead32(record + kFadePosition) + 1, length = FlatRead32(record + kFadeLength);
        FlatWrite32(record + kFadePosition, position);

        if (position < length) {
            const float t = float(position) / float(length + 1);
            FlatWriteFloat32(record + kFadeGain, 1.0f - t);
            FlatWrite16(record + kVolumeOut, FlatRead16(record + kRampStart));
        } else {
            FlatWriteFloat32(record + kFadeGain, 0.0f);
            Clear(object, kFadeOut);
        }

        changed = true;
    }

    // The three ramps differ only in their target: kRampTarget itself, the
    // start plus it, or the stream level scaled by it (percent).
    const uint32_t ramps[3] = {kRampTo, kRampBy, kRampScale};

    for (uint32_t kind = 0; kind < 3; ++kind) {
        if (!Has(object, ramps[kind]))
            continue;

        const uint32_t position = FlatRead32(record + kRampPosition) + 1;
        FlatWrite32(record + kRampPosition, position);
        const bool running = position < FlatRead32(record + kRampLength);
        int32_t target;

        if (kind == 0) {
            target = S16(record + kRampTarget);
        } else if (kind == 1) {
            target = int16_t(S16(record + kRampStart) + S16(record + kRampTarget));
        } else {
            const float level = float(S16(record + kEnvelope));
            const float percent = float(S16(record + kRampTarget));
            target = int16_t(int32_t(running ? level * (percent / 100.0f) : level * (0.01f * percent)));
        }

        FlatWrite16(record + kVolume, uint16_t(Clamp127(int16_t(target))));

        if (running) {
            RampStep(record, envelope, position);
        } else {
            RampEnd(record, envelope);
            Clear(object, ramps[kind]);
        }

        changed = true;
    }

    if (changed) {
        const int32_t v = int32_t(float(S16(record + kVolumeOut)) * FlatReadFloat32(record + kFadeGain));
        FlatWrite16(record + kVolumeOut, uint16_t(Clamp127(int16_t(v))));
    }

    // The category gain applies every tick (0x800E4940 is constant 1).
    const uint16_t volume = FlatRead16(record + kVolumeOut);
    const uint32_t category = FlatRead16(record + kGainCategory) & 0xFF;
    const float gain = FlatReadFloat32(FlatRead8(kStreamGainSelect + category) ? kStreamGainB : kStreamGainA);
    uint32_t level = uint32_t(int32_t(float(volume) * gain));

    if ((level & 0xFFFF) > 127)
        level = 127;

    for (const uint32_t at : {kLeft, kRight})
        if (const uint32_t voice = FlatRead32(record + at))
            SdkMix::SetInput(voice, Level(int16_t(level)));
}

// Releases the voices, the buffer and the file of a finished stream.
uint32_t Finish(CpuContext* cpu, uint32_t manager, uint32_t object) {
    const uint32_t record = Record(object);

    for (const uint32_t at : {kLeft, kRight}) {
        if (const uint32_t voice = FlatRead32(record + at)) {
            SoundVoice::Free(voice);
            FlatWrite32(record + at, 0);
        }
    }

    GiveBuffer(manager, FlatRead32(record + kBuffer));
    {
        GuestFrame frame(cpu, 0x20);
        GuestCall{cpu}.Call<kDvdClose>({record + kFile});
    }

    const uint32_t next = FreeObject(object);
    FlatWrite32(manager + kStreamCount, FlatRead32(manager + kStreamCount) - 1);
    return next;
}

// ---------------------------------------------------------------------------
// DVD callbacks: r3 the bytes read (or -1..-3), r4 the command block.
// ---------------------------------------------------------------------------

bool ReadFailed(int32_t result) {
    return result == -1 || result == -2 || result == -3;
}

// 0x800EA6E4: the left half arrived; read the right half (or the rest of
// the chunk for mono data).
void ReadLeftDone(CpuContext* cpu, int32_t result, uint32_t block) {
    if (ReadFailed(result))
        return;

    const uint32_t object = FlatRead32(block + 0x2C), record = Record(object);
    const uint32_t buffer = FlatRead32(record + kBuffer);

    if (!buffer)
        return;

    const uint32_t chunk = FlatRead8(record + kReadChunk);

    if (chunk == 0 && Format(FlatRead32(record + kInfo)) <= 1)
        if (const uint32_t left = FlatRead32(record + kLeft))
            SoundVoice::SetAdpcmLoop(left, FlatRead8(buffer), 0, 0);
    uint32_t length = ChunkBytes();

    if (FlatRead8(record + kSplitRead) == 1 || FlatRead8(record + kLastChunk) == 1 || !(ChannelSize(record) > ChannelBytes()))
        length = uint32_t(result);

    FlatWrite32(record + kReadOffset, FlatRead32(record + kReadOffset) + uint32_t(result));
    const uint32_t offset2 = FlatRead32(record + kReadOffset2);

    if (offset2 + length > FlatRead32(record + kSize))
        length = FlatRead32(record + kSize) - offset2;

    const uint32_t bytes = RoundUp(length, 32);
    Read(cpu, record, chunk * ChunkBytes() + buffer + ChannelBytes(), bytes, offset2, kReadRightDone);
}

// 0x800EA848: the right half arrived: the chunk is loaded, or (split at the
// loop end) the left rest is read from the loop start.
void ReadRightDone(CpuContext* cpu, int32_t result, uint32_t block) {
    if (ReadFailed(result))
        return;

    const uint32_t object = FlatRead32(block + 0x2C), record = Record(object);
    const uint32_t buffer = FlatRead32(record + kBuffer);

    if (!buffer)
        return;

    const uint32_t chunk = FlatRead8(record + kReadChunk);

    if (chunk == 0)
        if (const uint32_t right = FlatRead32(record + kRight))
            SoundVoice::SetAdpcmLoop(right, FlatRead8(buffer + ChannelBytes()), 0, 0);
    if (FlatRead8(record + kSplitRead) == 1) {
        const uint32_t rest = ChunkBytes() - uint32_t(result);
        const uint32_t info = FlatRead32(record + kInfo), loopStart = FlatRead32(info + kInfoLoopStart);
        const uint32_t offset = int8_t(FlatRead8(record + kChannels)) == 1 ? AddressToBytes(info, loopStart) : SamplesToBytes(info, loopStart);
        FlatWrite32(record + kReadOffset, offset & ~7u);
        Read(cpu, record, chunk * ChunkBytes() + (uint32_t(result) + buffer), rest, FlatRead32(record + kReadOffset), kReadLeftRestDone);
        return;
    }

    FlatWrite32(record + kReadOffset2, FlatRead32(record + kReadOffset2) + uint32_t(result));
    Set(object, kChunkLoaded << chunk);
    Clear(object, kChunkLoading << chunk);

    if (FlatRead32(record + kSize) > ChannelBytes()) {
        AdvanceReadChunk(record);
        FlatWrite8(record + kReadPending, 0);
    }
}

// 0x800EA9C8: the left rest arrived; read the right rest from the loop start.
void ReadLeftRestDone(CpuContext* cpu, int32_t result, uint32_t block) {
    if (ReadFailed(result))
        return;

    const uint32_t object = FlatRead32(block + 0x2C), record = Record(object);
    const uint32_t buffer = FlatRead32(record + kBuffer);

    if (!buffer)
        return;

    const uint32_t chunkBytes = ChunkBytes();
    const uint32_t to = (chunkBytes - uint32_t(result)) + (FlatRead8(record + kReadChunk) * chunkBytes + (buffer + ChannelBytes()));
    const uint32_t info = FlatRead32(record + kInfo);
    const uint32_t loopStart = SamplesToBytes(info, FlatRead32(info + kInfoLoopStart));
    FlatWrite32(record + kReadOffset, FlatRead32(record + kReadOffset) + uint32_t(result));

    if (int8_t(FlatRead8(record + kChannels)) == 1)
        FlatWrite32(record + kReadOffset2, 0);
    else
        FlatWrite32(record + kReadOffset2, (loopStart & ~7u) + (FlatRead32(record + kSize) >> 1));

    Read(cpu, record, to, uint32_t(result), FlatRead32(record + kReadOffset2), kReadRightRestDone);
}

// 0x800EAAB8: the right rest arrived: the chunk is loaded.
void ReadRightRestDone(int32_t result, uint32_t block) {
    if (ReadFailed(result))
        return;

    const uint32_t object = FlatRead32(block + 0x2C), record = Record(object);
    FlatWrite32(record + kReadOffset2, FlatRead32(record + kReadOffset2) + uint32_t(result));
    FlatWrite8(record + kSplitRead, 0);
    const uint32_t chunk = FlatRead8(record + kReadChunk);
    Set(object, kChunkLoaded << chunk);
    Clear(object, kChunkLoading << chunk);
    AdvanceReadChunk(record);
    FlatWrite8(record + kReadPending, 0);
}

// 0x800EAB7C: the reads were cancelled (the stream may be freed now).
void CancelDone(uint32_t block) {
    const uint32_t record = Record(FlatRead32(block + 0x2C));
    FlatWrite8(record + kCancelled, 1);
    FlatWrite16(record + kGainCategory, 0xFFFF);
    FlatWrite8(record + kReadPending, 0);
}

} // namespace

// +0x14, 0x800EAF10: AX stole one of the voices: the stream ends.
void SoundEngine::Stream::VoiceDropped(CpuContext* cpu, uint32_t voice, uint32_t object) {
    const uint32_t record = Record(object);

    if (FlatRead32(record + kLeft) == voice)
        FlatWrite32(record + kLeft, 0);

    if (FlatRead32(record + kRight) == voice)
        FlatWrite32(record + kRight, 0);

    Clear(object, kTicking);
    Set(object, kReleased);
    CancelReads(cpu, object);
}

namespace {

// ---------------------------------------------------------------------------
// Entry points.
// ---------------------------------------------------------------------------

void TickPort(CpuContext* cpu) {
    Stream::Tick(cpu, cpu->gpr[3], cpu->gpr[4]);
}

void UpdatePort(CpuContext* cpu) {
    uint32_t object = FlatRead32(cpu->gpr[4]);
    const bool freed = Stream::Update(cpu, cpu->gpr[3], object);

    if (freed)
        FlatWrite32(cpu->gpr[4], object);

    cpu->gpr[3] = freed;
}

void VoiceDroppedPort(CpuContext* cpu) {
    Stream::VoiceDropped(cpu, cpu->gpr[4], SoundVoice::Context(cpu->gpr[4]));
}

// 0x800E9EB8: the stream voices' AX drop callback (r3 the voice).
void VoiceCallbackPort(CpuContext* cpu) {
    Stream::VoiceDropped(cpu, cpu->gpr[3], SoundVoice::Context(cpu->gpr[3]));
}

void StopPort(CpuContext* cpu) {
    Stream::Stop(cpu, cpu->gpr[3], cpu->gpr[4]);
}

void PausePort(CpuContext* cpu) {
    Stream::Pause(cpu->gpr[4]);
}

void ResumePort(CpuContext* cpu) {
    Stream::Resume(cpu->gpr[4]);
}

void StartPort(CpuContext* cpu) {
    Stream::Start(cpu->gpr[3], cpu->gpr[4]);
}

void UpdateVolumePort(CpuContext* cpu) {
    UpdateVolume(cpu->gpr[4]);
}

void FollowPort(CpuContext* cpu) {
    Follow(cpu, cpu->gpr[3], cpu->gpr[4]);
}

void SetVoiceRingPort(CpuContext* cpu) {
    SetVoiceRing(cpu->gpr[3], cpu->gpr[4], cpu->gpr[5]);
}

void SeekPort(CpuContext* cpu) {
    Seek(cpu->gpr[3]);
}

void ServiceDiscPort(CpuContext* cpu) {
    ServiceDisc(cpu, cpu->gpr[3], cpu->gpr[4]);
}

void RefillPort(CpuContext* cpu) {
    Refill(cpu, cpu->gpr[4]);
}

void SetupVoicePort(CpuContext* cpu) {
    SetupVoice(cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6]);
}

void SetupVoicesPort(CpuContext* cpu) {
    SetupVoices(cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], int16_t(cpu->gpr[6]));
}

void ReadLeftDonePort(CpuContext* cpu) {
    ReadLeftDone(cpu, int32_t(cpu->gpr[3]), cpu->gpr[4]);
}

void ReadRightDonePort(CpuContext* cpu) {
    ReadRightDone(cpu, int32_t(cpu->gpr[3]), cpu->gpr[4]);
}

void ReadLeftRestDonePort(CpuContext* cpu) {
    ReadLeftRestDone(cpu, int32_t(cpu->gpr[3]), cpu->gpr[4]);
}

void ReadRightRestDonePort(CpuContext* cpu) {
    ReadRightRestDone(int32_t(cpu->gpr[3]), cpu->gpr[4]);
}

void CancelDonePort(CpuContext* cpu) {
    CancelDone(cpu->gpr[4]);
}

} // namespace

namespace SoundEngine::Stream {

// +0x0C, 0x800EADE4.
void Tick(CpuContext* cpu, uint32_t manager, uint32_t object) {
    const uint32_t record = Record(object);

    if (Has(object, kNeedsBuffer)) {
        if (FlatRead32(manager + kBufferCount) <= FlatRead32(manager + kStreamCount))
            return;

        const uint32_t buffer = TakeBuffer(manager);
        FlatWrite32(record + kBuffer, buffer);

        if (!buffer)
            return;

        Clear(object, kNeedsBuffer);
    }

    if (FlatRead8(record + kEnvelope + kEnvState) != kEnvStart)
        Follow(cpu, manager, object);

    const uint32_t countdown = FlatRead32(record + kCountdown);

    if (countdown != 0xFFFFFFFFu && countdown != 0)
        FlatWrite32(record + kCountdown, countdown - 1);

    if (FlatRead8(record + kEnvelope + kEnvState) != kEnvStart && FlatRead32(record + kLifetime) != 0xFFFFFFFFu) {
        const uint32_t lifetime = FlatRead32(record + kLifetime) - 1;
        FlatWrite32(record + kLifetime, lifetime);

        if (lifetime == 0) {
            FlatWrite8(record + kEnvelope + kEnvState, kEnvReleasing);
            FlatWrite32(record + kLifetime, 0xFFFFFFFFu);
        }
    }

    Envelope(cpu, manager, object, record + kEnvelope);
    ServiceDisc(cpu, manager, object);
    UpdateVolume(object);
}

// +0x10, 0x800EAFAC: frees a stream whose reads were cancelled after a
// stop, or whose (non-looping) voice finished.
bool Update(CpuContext* cpu, uint32_t manager, uint32_t& object) {
    const uint32_t record = Record(object);

    if (Has(object, kReleased)) {
        if (!FlatRead8(record + kCancelled))
            return false;
    } else {
        if (Has(object, kPaused) || !Has(object, kStarted))
            return false;

        const uint32_t left = FlatRead32(record + kLeft);

        if (!left || SoundVoice::State(left) != 0 || SoundVoice::LoopFlag(left) == 1)
            return false;
    }

    object = Finish(cpu, manager, object);
    return true;
}

// +0x18, 0x800ED07C.
void Stop(CpuContext* cpu, uint32_t, uint32_t object) {
    if (!object)
        return;

    Clear(object, kPaused);
    Clear(object, kTicking);
    Set(object, kReleased);
    const uint32_t record = Record(object);

    for (const uint32_t at : {kLeft, kRight})
        if (const uint32_t voice = FlatRead32(record + at))
            SoundVoice::SetState(voice, 0);
    CancelReads(cpu, object);
}

// +0x1C, 0x800ED120.
void Pause(uint32_t object) {
    if (!object)
        return;

    const uint32_t record = Record(object);

    for (const uint32_t at : {kLeft, kRight})
        if (const uint32_t voice = FlatRead32(record + at))
            SoundVoice::SetState(voice, 0);
    Set(object, kPaused);
}

// +0x20, 0x800ED198.
void Resume(uint32_t object) {
    if (!object || !Has(object, kPaused))
        return;

    Clear(object, kPaused);
    const uint32_t record = Record(object);

    for (const uint32_t at : {kLeft, kRight})
        if (const uint32_t voice = FlatRead32(record + at))
            SoundVoice::SetState(voice, 1);
}

// +0x24, 0x800EB128: starts the voices once the first chunk is loaded.
void Start(uint32_t manager, uint32_t object) {
    const uint32_t record = Record(object);

    if (!Has(object, kChunkLoaded << NextChunk(FlatRead8(record + kPlayChunk))))
        return;

    const uint32_t info = FlatRead32(record + kInfo), left = FlatRead32(record + kLeft), right = FlatRead32(record + kRight);
    const uint32_t buffer = FlatRead32(record + kBuffer);
    SetupVoices(info, left, right, S16(record + kPitch));

    if (left)
        SetVoiceRing(record, left, buffer);

    if (right)
        SetVoiceRing(record, right, buffer + ChannelBytes());

    const int32_t pan = FlatRead32(info + kInfoChannels) == 0 ? 0 : S16(record + kPan);
    const auto start = [&](uint32_t voice, int32_t voicePan) {
        const int32_t fader = Level(S16(record + kFader)), auxC = Level(S16(record + kAuxC)), auxB = Level(S16(record + kAuxB));
        const int32_t auxA = Level(S16(record + kAuxA)), input = Level(S16(record + kVolumeOut));
        SdkMix::InitChannel(voice, MixMode(), input, uint32_t(auxA), uint32_t(auxB), uint32_t(auxC), uint32_t(voicePan), 0x7F, uint32_t(fader));
        SoundVoice::SetState(voice, Has(object, kPaused) ? 0 : 1);
    };

    if (left)
        start(left, pan);

    if (right)
        start(right, 0x7F);

    FlatWrite32(manager + kStreamCount, FlatRead32(manager + kStreamCount) + 1);
    Set(object, kStarted);
    FlatWrite8(record + kEnvelope + kEnvState, kEnvAttacking);
}

bool Played(uint32_t slot, uint32_t& object, double& seconds) {
    for (object = FlatRead32(kActive + kNext); object != kActive; object = FlatRead32(object + kNext)) {
        const uint32_t record = Record(object);

        if (FlatRead8(object + kObjType) != kTypeStream || FlatRead16(record + kGainCategory) != slot)
            continue;

        const uint32_t info = FlatRead32(record + kInfo), rate = info ? FlatRead32(info + kInfoRate) : 0;

        if (!FlatRead32(record + kLeft) || !rate)
            return false;

        // ADPCM frames are 8 bytes of 14 samples.
        const uint32_t bytes = FlatRead32(record + kPlayed), f = Format(info);
        const double samples = f < 2 ? bytes * (14.0 / 8.0) : f < 4 ? bytes / 2.0 : bytes;
        seconds = samples / rate;
        return true;
    }

    object = 0;
    return false;
}

} // namespace SoundEngine::Stream

extern "C" {

void MH3_StreamTick(CpuContext* cpu) {
    Entry(cpu, "StreamTick", func_800EADE4, TickPort);
}

void MH3_StreamUpdate(CpuContext* cpu) {
    Entry(cpu, "StreamUpdate", func_800EAFAC, UpdatePort);
}

void MH3_StreamVoiceDropped(CpuContext* cpu) {
    Entry(cpu, "StreamVoiceDropped", func_800EAF10, VoiceDroppedPort);
}

void MH3_StreamVoiceCallback(CpuContext* cpu) {
    VoiceCallbackPort(cpu);
}

void MH3_StreamStop(CpuContext* cpu) {
    Entry(cpu, "StreamStop", func_800ED07C, StopPort);
}

void MH3_StreamPause(CpuContext* cpu) {
    Entry(cpu, "StreamPause", func_800ED120, PausePort);
}

void MH3_StreamResume(CpuContext* cpu) {
    Entry(cpu, "StreamResume", func_800ED198, ResumePort);
}

void MH3_StreamStart(CpuContext* cpu) {
    Entry(cpu, "StreamStart", func_800EB128, StartPort);
}

void MH3_StreamUpdateVolume(CpuContext* cpu) {
    Entry(cpu, "StreamUpdateVolume", func_800EB378, UpdateVolumePort);
}

void MH3_StreamFollow(CpuContext* cpu) {
    Entry(cpu, "StreamFollow", func_800EABBC, FollowPort);
}

void MH3_StreamSetVoiceRing(CpuContext* cpu) {
    Entry(cpu, "StreamSetVoiceRing", func_800EA4A8, SetVoiceRingPort);
}

void MH3_StreamSeek(CpuContext* cpu) {
    Entry(cpu, "StreamSeek", func_800EA5EC, SeekPort);
}

void MH3_StreamServiceDisc(CpuContext* cpu) {
    Entry(cpu, "StreamServiceDisc", func_800ECDE0, ServiceDiscPort);
}

void MH3_StreamRefill(CpuContext* cpu) {
    Entry(cpu, "StreamRefill", func_800ECEBC, RefillPort);
}

void MH3_StreamSetupVoice(CpuContext* cpu) {
    Entry(cpu, "StreamSetupVoice", func_800EE5EC, SetupVoicePort);
}

void MH3_StreamSetupVoices(CpuContext* cpu) {
    Entry(cpu, "StreamSetupVoices", func_800EE7E4, SetupVoicesPort);
}

void MH3_StreamReadLeftDone(CpuContext* cpu) {
    Entry(cpu, "StreamReadLeftDone", func_800EA6E4, ReadLeftDonePort);
}

void MH3_StreamReadRightDone(CpuContext* cpu) {
    Entry(cpu, "StreamReadRightDone", func_800EA848, ReadRightDonePort);
}

void MH3_StreamReadLeftRestDone(CpuContext* cpu) {
    Entry(cpu, "StreamReadLeftRestDone", func_800EA9C8, ReadLeftRestDonePort);
}

void MH3_StreamReadRightRestDone(CpuContext* cpu) {
    Entry(cpu, "StreamReadRightRestDone", func_800EAAB8, ReadRightRestDonePort);
}

void MH3_StreamCancelDone(CpuContext* cpu) {
    Entry(cpu, "StreamCancelDone", func_800EAB7C, CancelDonePort);
}

// Helpers (r3 result).
void MH3_StreamTakeBuffer(CpuContext* cpu) {  // 0x800EBAAC (manager)
    cpu->gpr[3] = TakeBuffer(cpu->gpr[3]);
}

void MH3_StreamGiveBuffer(CpuContext* cpu) {  // 0x800EBB04 (manager, buffer)
    GiveBuffer(cpu->gpr[3], cpu->gpr[4]);
}

void MH3_StreamRoundUp(CpuContext* cpu) {  // 0x800EA81C (n, align)
    cpu->gpr[3] = RoundUp(cpu->gpr[3], cpu->gpr[4]);
}

void MH3_StreamBytesToAddress(CpuContext* cpu) {  // 0x800EE8CC (info, bytes)
    cpu->gpr[3] = BytesToAddress(cpu->gpr[3], cpu->gpr[4]);
}

void MH3_StreamAddressToBytes(CpuContext* cpu) {  // 0x800EE918 (info, address)
    cpu->gpr[3] = AddressToBytes(cpu->gpr[3], cpu->gpr[4]);
}

void MH3_StreamSamplesToBytes(CpuContext* cpu) {  // 0x800EE964 (info, samples)
    cpu->gpr[3] = SamplesToBytes(cpu->gpr[3], cpu->gpr[4]);
}

void MH3_StreamSamplesToAddress(CpuContext* cpu) {  // 0x800EE9DC (info, samples)
    cpu->gpr[3] = SamplesToAddress(cpu->gpr[3], cpu->gpr[4]);
}

} // extern "C"

REGISTER_NATIVE_FUNCTION_AS(0x800EADE4, MH3_StreamTick, "StreamTick");
REGISTER_NATIVE_FUNCTION_AS(0x800EAFAC, MH3_StreamUpdate, "StreamUpdate");
REGISTER_NATIVE_FUNCTION_AS(0x800EAF10, MH3_StreamVoiceDropped, "StreamVoiceDropped");
REGISTER_NATIVE_FUNCTION_AS(0x800E9EB8, MH3_StreamVoiceCallback, "StreamVoiceCallback");
REGISTER_NATIVE_FUNCTION_AS(0x800ED07C, MH3_StreamStop, "StreamStop");
REGISTER_NATIVE_FUNCTION_AS(0x800ED120, MH3_StreamPause, "StreamPause");
REGISTER_NATIVE_FUNCTION_AS(0x800ED198, MH3_StreamResume, "StreamResume");
REGISTER_NATIVE_FUNCTION_AS(0x800EB128, MH3_StreamStart, "StreamStart");
REGISTER_NATIVE_FUNCTION_AS(0x800EB378, MH3_StreamUpdateVolume, "StreamUpdateVolume");
REGISTER_NATIVE_FUNCTION_AS(0x800EABBC, MH3_StreamFollow, "StreamFollow");
REGISTER_NATIVE_FUNCTION_AS(0x800EA4A8, MH3_StreamSetVoiceRing, "StreamSetVoiceRing");
REGISTER_NATIVE_FUNCTION_AS(0x800EA5EC, MH3_StreamSeek, "StreamSeek");
REGISTER_NATIVE_FUNCTION_AS(0x800ECDE0, MH3_StreamServiceDisc, "StreamServiceDisc");
REGISTER_NATIVE_FUNCTION_AS(0x800ECEBC, MH3_StreamRefill, "StreamRefill");
REGISTER_NATIVE_FUNCTION_AS(0x800EE5EC, MH3_StreamSetupVoice, "StreamSetupVoice");
REGISTER_NATIVE_FUNCTION_AS(0x800EE7E4, MH3_StreamSetupVoices, "StreamSetupVoices");
REGISTER_NATIVE_FUNCTION_AS(0x800EA6E4, MH3_StreamReadLeftDone, "StreamReadLeftDone");
REGISTER_NATIVE_FUNCTION_AS(0x800EA848, MH3_StreamReadRightDone, "StreamReadRightDone");
REGISTER_NATIVE_FUNCTION_AS(0x800EA9C8, MH3_StreamReadLeftRestDone, "StreamReadLeftRestDone");
REGISTER_NATIVE_FUNCTION_AS(0x800EAAB8, MH3_StreamReadRightRestDone, "StreamReadRightRestDone");
REGISTER_NATIVE_FUNCTION_AS(0x800EAB7C, MH3_StreamCancelDone, "StreamCancelDone");
REGISTER_NATIVE_FUNCTION_AS(0x800EBAAC, MH3_StreamTakeBuffer, "StreamTakeBuffer");
REGISTER_NATIVE_FUNCTION_AS(0x800EBB04, MH3_StreamGiveBuffer, "StreamGiveBuffer");
REGISTER_NATIVE_FUNCTION_AS(0x800EA81C, MH3_StreamRoundUp, "StreamRoundUp");
REGISTER_NATIVE_FUNCTION_AS(0x800EE8CC, MH3_StreamBytesToAddress, "StreamBytesToAddress");
REGISTER_NATIVE_FUNCTION_AS(0x800EE918, MH3_StreamAddressToBytes, "StreamAddressToBytes");
REGISTER_NATIVE_FUNCTION_AS(0x800EE964, MH3_StreamSamplesToBytes, "StreamSamplesToBytes");
REGISTER_NATIVE_FUNCTION_AS(0x800EE9DC, MH3_StreamSamplesToAddress, "StreamSamplesToAddress");
