// Native voices, RMHP08: the game-thread voice table the sound engine, MIX
// and the native frame driver use (SoundVoice, sound_voice.h), and the RVL
// SDK AX entry points guest code still calls, as thin wrappers over it.
//
// There is no AX layer underneath: a voice is a host record holding the
// mixer's voice image (the AXPB fields the mixer reads, host-endian), the
// sync bits of the groups written since the last tick, its priority, its
// drop callback and owner. Once per 3 ms tick the frame driver sends the
// changed groups to the mixer (FlushVoices, __AXSyncPB's precedence rules)
// and takes the mixer's state, envelope volume and position back.
//
// Guest code identifies a voice by the address of what was its AXVPB
// (kVpbStorage + index * kVpbSize, never dereferenced now): the engine's
// records, SYN and the AX callbacks keep these handles. Allocation keeps the
// SDK's rules: a free voice, else the oldest voice of the lowest priority
// below the request is stolen and its drop callback called (the engine's two
// callbacks natively).
#include "native_audio.h"
#include "sound_engine.h"
#include "sound_voice.h"

#include "abi_bridge.h"
#include "guest_call.h"
#include "memory_access.h"

#include <cmath>
#include <cstdio>
#include <cstring>

using namespace NativeAudio;
using namespace NativeAudio::Guest;
using MemoryInline::FlatRead16;
using MemoryInline::FlatRead32;
using MemoryInline::FlatWrite16;
using MemoryInline::FlatWrite32;

namespace NativeAudio {
void StartNativeAudio(CpuContext* cpu);
void StopNativeAudio();
void NoteAuxCallback(uint32_t bus, uint32_t callback, uint32_t context);
} // namespace NativeAudio

namespace {

constexpr uint32_t kOSRegisterVersion = 0x804CBA00;
// The engine's voice drop callbacks (AXAcquireVoice's r4).
constexpr uint32_t kSeVoiceCallback = 0x800E5738, kStreamVoiceCallback = 0x800E9EB8;

struct Voice {
    uint16_t pb[kPbHalfwords];
    uint32_t sync;        // AXVPB.sync: groups written since the last tick
    uint32_t priority;    // 0: free
    uint32_t callback, context;
    int16_t prev, next;   // priority stack links (-1: none)
    bool depop;           // stopped while running: fade out
    // Frame driver: the tick of the state/volume/address the game last set
    // (feedback waits until the mixer applied it) and whether the mixer was
    // told the voice runs.
    uint32_t stateTick, volumeTick, addressTick;
    bool running;
};

struct Voices {
    Voice voice[kMaxVoices];
    int16_t head[32], tail[32];  // priority stacks, newest first; [0] the free stack
    uint32_t dirty[kMaxVoices / 32];  // voices the next flush visits
    const Feedback* feedback;        // the mixer's, once the first tick is flushed
    bool initialized;
};
Voices v;

// Marks `bits` changed; the next flush sends them.
void Touch(Voice& x, uint32_t bits) {
    x.sync |= bits;
    const uint32_t i = uint32_t(&x - v.voice);
    v.dirty[i >> 5] |= 1u << (i & 31);
}

// The mixer's state, envelope volume and position of a voice the game has
// not set since (the mixer applied its last write), read when asked.
void Refresh(Voice& x) {
    if (!v.feedback)
        return;
    const uint32_t i = uint32_t(&x - v.voice);
    const uint32_t applied = v.feedback->appliedTick.load(std::memory_order_acquire);
    if (!(x.sync & kSyncState) && int32_t(applied - x.stateTick) > 0)
        x.pb[Pb::State] = uint16_t(v.feedback->stateVolume[i].load(std::memory_order_relaxed) >> 16);
    if (!(x.sync & (kSyncAddr | kSyncCurrAddr)) && int32_t(applied - x.addressTick) > 0) {
        const uint32_t address = v.feedback->address[i].load(std::memory_order_relaxed);
        x.pb[Pb::CurHi] = uint16_t(address >> 16);
        x.pb[Pb::CurLo] = uint16_t(address);
    }
}

uint32_t Handle(uint32_t index) {
    return kVpbStorage + index * kVpbSize;
}

// The handle's voice; out of range handles (0, stale pointers) get none.
Voice* At(uint32_t handle, uint32_t* index = nullptr) {
    const uint32_t offset = handle - kVpbStorage;
    const uint32_t i = offset / kVpbSize;
    if (i >= kMaxVoices || offset % kVpbSize)
        return nullptr;
    if (index)
        *index = i;
    return &v.voice[i];
}

// __AXRemoveFromStack.
void RemoveFromStack(uint32_t i) {
    Voice& x = v.voice[i];
    const uint32_t p = x.priority;
    if (v.head[p] == v.tail[p]) {
        v.head[p] = v.tail[p] = -1;
    } else if (v.head[p] == int16_t(i)) {
        v.head[p] = x.next;
        v.voice[x.next].prev = -1;
    } else if (v.tail[p] == int16_t(i)) {
        v.tail[p] = x.prev;
        v.voice[x.prev].next = -1;
    } else {
        v.voice[x.prev].next = x.next;
        v.voice[x.next].prev = x.prev;
    }
}

// Links voice `i` in as the newest of `priority`.
void PushToStack(uint32_t i, uint32_t priority) {
    Voice& x = v.voice[i];
    x.next = v.head[priority];
    x.prev = -1;
    if (v.head[priority] < 0)
        v.tail[priority] = int16_t(i);
    else
        v.voice[v.head[priority]].prev = int16_t(i);
    v.head[priority] = int16_t(i);
}

// __AXPushFreeStack.
void PushFreeStack(uint32_t i) {
    v.voice[i].next = v.head[0];
    v.head[0] = int16_t(i);
    v.voice[i].priority = 0;
}

// __AXSetPBDefault: stopped, ITD/LPF/biquad off.
void SetPBDefault(Voice& x) {
    x.pb[Pb::State] = 0;
    x.pb[Pb::Itd] = 0;
    x.pb[Pb::LpfOn] = 0;
    x.pb[Pb::BiquadOn] = 0;
    x.sync = 0;
    Touch(x, kSyncPbDefault);
}

void InitVoices() {
    std::memset(&v, 0, sizeof(v));
    for (uint32_t p = 0; p < 32; ++p)
        v.head[p] = v.tail[p] = -1;
    for (uint32_t i = 0; i < kMaxVoices; ++i) {
        SetPBDefault(v.voice[i]);
        v.voice[i].prev = v.voice[i].next = -1;
        PushFreeStack(i);
    }
    v.initialized = true;
}

// A stolen voice's drop callback (r3 the handle).
void Dropped(CpuContext* cpu, uint32_t i) {
    const Voice& x = v.voice[i];
    const uint32_t handle = Handle(i);
    if (x.callback == kSeVoiceCallback) {
        SoundEngine::SeVoiceDropped(handle, x.context);
    } else if (x.callback == kStreamVoiceCallback) {
        SoundEngine::Stream::VoiceDropped(cpu, handle, x.context);
    } else if (x.callback) {
        // A guest callback reads its context from the AXVPB's user context.
        FlatWrite32(handle + kVpbUserContext, x.context);
        GuestFrame frame(cpu, 0x30);
        cpu->gpr[3] = handle;
        InvokeIndirectCpu(x.callback, cpu);
    }
}

void InitEx(CpuContext* cpu, uint32_t outputMode) {
    if (FlatRead32(kAxInitialized) != 0)
        return;
    {
        GuestFrame frame(cpu, 0x10);
        GuestCall{cpu}(kOSRegisterVersion, {FlatRead32(kSdkVersion)});
    }
    InitVoices();
    // The SDK state guest code reads: the voice count (MIX/SYN init), the
    // master and aux return volumes and the aux callbacks.
    FlatWrite32(kVoiceCount, kMaxVoices);
    FlatWrite32(kVpbs, kVpbStorage);
    for (const uint32_t at : {kAuxACallback, kAuxBCallback, kAuxCCallback, kAuxAContext, kAuxBContext, kAuxCContext})
        FlatWrite32(at, 0);
    FlatWrite32(kMode, 0);
    FlatWrite32(kCompressor, 1);
    FlatWrite16(kMasterVolume, 0x8000);
    FlatWrite16(kAuxAReturn, 0x8000);
    FlatWrite16(kAuxBReturn, 0x8000);
    FlatWrite16(kAuxCReturn, 0x8000);
    FlatWrite32(kOutputBufferMode, outputMode);
    FlatWrite32(kUserCallback, 0);
    StartNativeAudio(cpu);
    FlatWrite32(kAxInitialized, 1);
}

uint32_t ToUnsigned(double value) {
    if (!(value > 0.0))
        return 0;
    if (value >= 4294967296.0)
        return 0xFFFFFFFFu;
    return uint32_t(value);
}

// __AXSyncPB for one changed voice at `tick`: the groups the game changed go
// to the mixer; the fields it did not set get the mixer's state, envelope
// volume and position back once the mixer applied the game's last write.
void SyncVoice(CommandRing& ring, const Feedback& feedback, uint32_t applied, Voice& x, uint32_t index, uint32_t tick, uint32_t& records) {
    const uint32_t sync = x.sync;
    bool backState = false, backVolume = false, backAddress = false;
    uint32_t groups = 0;
    if (sync == 0) {
        backState = backVolume = backAddress = true;
    } else if (sync & kSyncAllParams) {
        groups = kSyncAllParams;
        x.stateTick = x.volumeTick = x.addressTick = tick;
        x.running = x.pb[Pb::State] == 1;
    } else {
        groups = sync & (kSyncSrcSelect | kSyncMixerCtrl | kSyncType | kSyncMix | kSyncDpop | kSyncAdpcm | kSyncAdpcmLoop);
        if (sync & kSyncState) {
            groups |= kSyncState;
            x.stateTick = tick;
            x.running = x.pb[Pb::State] == 1;
        } else {
            backState = true;
        }
        if (sync & kSyncItdTarget)
            groups |= kSyncItdTarget;
        else if (sync & kSyncItd)
            groups |= kSyncItd;
        if (sync & kSyncVeDelta) {
            groups |= kSyncVeDelta;
            backVolume = true;
        } else if (sync & kSyncVe) {
            groups |= kSyncVe;
            x.volumeTick = tick;
        }
        if ((sync & (kSyncLoop | kSyncLoopAddr | kSyncEndAddr | kSyncCurrAddr)) == 0) {
            if (sync & kSyncAddr) {
                groups |= kSyncAddr;
                x.addressTick = tick;
            } else {
                backAddress = true;
            }
        } else {
            groups |= sync & (kSyncLoop | kSyncLoopAddr | kSyncEndAddr);
            if (sync & kSyncCurrAddr) {
                groups |= kSyncCurrAddr;
                x.addressTick = tick;
            } else {
                backAddress = true;
            }
        }
        if (sync & kSyncSrcRatio)
            groups |= kSyncSrcRatio;
        else if (sync & kSyncSrc)
            groups |= kSyncSrc;
        if (sync & kSyncLpfCoef)
            groups |= kSyncLpfCoef;
        else if (sync & kSyncLpf)
            groups |= kSyncLpf;
        if (sync & kSyncBiquadCoef)
            groups |= kSyncBiquadCoef;
        else if (sync & kSyncBiquad)
            groups |= kSyncBiquad;
    }
    if (groups) {
        uint32_t halfwords = 0;
        for (uint32_t bits = groups; bits; bits &= bits - 1)
            halfwords += kSyncGroups[__builtin_ctz(bits)].count;
        const uint32_t words = 2 + (halfwords + 1) / 2;
        uint32_t* out = ring.Reserve(words);
        out[0] = Header(Command::Voice, words, index, x.depop ? kFlagDepop : 0);
        out[1] = groups;
        uint32_t* data = out + 2;
        uint32_t half = 0;
        for (uint32_t bits = groups; bits; bits &= bits - 1) {
            const SyncGroup group = kSyncGroups[__builtin_ctz(bits)];
            for (uint32_t i = 0; i < group.count; ++i, ++half) {
                const uint32_t value = x.pb[group.offset + i];
                if (half & 1)
                    data[half >> 1] |= value << 16;
                else
                    data[half >> 1] = value;
            }
        }
        ++records;
    }
    if (backState && int32_t(applied - x.stateTick) > 0)
        x.pb[Pb::State] = uint16_t(feedback.stateVolume[index].load(std::memory_order_relaxed) >> 16);
    if (backVolume && int32_t(applied - x.volumeTick) > 0)
        x.pb[Pb::VeVolume] = uint16_t(feedback.stateVolume[index].load(std::memory_order_relaxed));
    if (backAddress && int32_t(applied - x.addressTick) > 0) {
        const uint32_t address = feedback.address[index].load(std::memory_order_relaxed);
        x.pb[Pb::CurHi] = uint16_t(address >> 16);
        x.pb[Pb::CurLo] = uint16_t(address);
    }
    x.sync = 0;
    x.depop = false;
}

} // namespace

namespace NativeAudio {

uint32_t FlushVoices(CommandRing& ring, const Feedback& feedback, uint32_t tick) {
    if (!v.initialized)
        return 0;
    v.feedback = &feedback;
    const uint32_t applied = feedback.appliedTick.load(std::memory_order_acquire);
    uint32_t records = 0;
    for (uint32_t w = 0; w < kMaxVoices / 32; ++w) {
        for (uint32_t bits = v.dirty[w]; bits; bits &= bits - 1) {
            const uint32_t i = w * 32 + uint32_t(__builtin_ctz(bits));
            Voice& x = v.voice[i];
            if (x.priority) {
                SyncVoice(ring, feedback, applied, x, i, tick, records);
                continue;
            }
            // A freed voice: stop it in the mixer once.
            if (x.running) {
                ring.Reserve(1)[0] = Header(Command::Stop, 1, i, x.depop ? kFlagDepop : 0);
                x.running = false;
                x.stateTick = tick;
            }
            x.depop = false;
        }
        v.dirty[w] = 0;
    }
    return records;
}

void ForgetMixerVoices() {
    for (Voice& x : v.voice)
        x.running = false;
}

} // namespace NativeAudio

namespace SoundVoice {

uint32_t Index(uint32_t voice) {
    uint32_t index = 0;
    At(voice, &index);
    return index;
}

uint32_t Context(uint32_t voice) {
    const Voice* x = At(voice);
    return x ? x->context : 0;
}

uint32_t Acquire(CpuContext* cpu, uint32_t priority, uint32_t callback, uint32_t context) {
    int32_t i = v.head[0];
    if (i >= 0) {
        v.head[0] = v.voice[i].next;
    } else {
        // Steal the oldest voice of the lowest priority below the request.
        for (uint32_t p = 1; p < priority && p < 32; ++p) {
            if (v.head[p] < 0)
                continue;
            i = v.tail[p];
            if (v.head[p] == v.tail[p]) {
                v.head[p] = v.tail[p] = -1;
            } else {
                v.tail[p] = v.voice[i].prev;
                v.voice[v.tail[p]].next = -1;
            }
            Refresh(v.voice[i]);
            if (v.voice[i].pb[Pb::State] == 1)
                v.voice[i].depop = true;
            Dropped(cpu, uint32_t(i));
            break;
        }
    }
    if (i < 0)
        return 0;
    Voice& x = v.voice[i];
    PushToStack(uint32_t(i), priority);
    x.priority = priority;
    x.callback = callback;
    x.context = context;
    SetPBDefault(x);
    return Handle(uint32_t(i));
}

void Free(uint32_t voice) {
    uint32_t i;
    if (!At(voice, &i) || v.voice[i].priority == 0)
        return;
    Voice& x = v.voice[i];
    RemoveFromStack(i);
    Refresh(x);
    if (x.pb[Pb::State] == 1)
        x.depop = true;
    SetPBDefault(x);
    PushFreeStack(i);
}

void SetState(uint32_t voice, uint32_t state) {
    Voice* x = At(voice);
    if (!x)
        return;
    Refresh(*x);
    if (x->pb[Pb::State] == state)
        return;
    x->pb[Pb::State] = uint16_t(state);
    Touch(*x, kSyncState);
    if (state == 0)
        x->depop = true;
}

void SetPriority(uint32_t voice, uint32_t priority) {
    uint32_t i;
    if (!At(voice, &i) || v.voice[i].priority == 0)
        return;
    RemoveFromStack(i);
    PushToStack(i, priority);
    v.voice[i].priority = priority;
}

// 0 none, 1 linear, 2-4 polyphase (coefficient set 0-2).
void SetSrcType(uint32_t voice, uint32_t type) {
    Voice* x = At(voice);
    if (!x)
        return;
    if (type == 0) {
        x->pb[Pb::SrcSelect] = 2;
    } else if (type == 1) {
        x->pb[Pb::SrcSelect] = 1;
    } else if (type <= 4) {
        x->pb[Pb::SrcSelect] = 0;
        x->pb[Pb::CoefSelect] = uint16_t(type - 2);
    }
    Touch(*x, kSyncSrcSelect);
}

// Stored as 16.16 (fmuls by 65536.0f).
void SetSrcRatio(uint32_t voice, float ratio) {
    Voice* x = At(voice);
    if (!x)
        return;
    const uint32_t fixed = ToUnsigned(double(float(65536.0f * ratio)));
    x->pb[Pb::SrcRatioHi] = uint16_t(fixed >> 16);
    x->pb[Pb::SrcRatioLo] = uint16_t(fixed);
    Touch(*x, kSyncSrcRatio);
}

void SetCurrentAddr(uint32_t voice, uint32_t address) {
    Voice* x = At(voice);
    if (!x)
        return;
    x->pb[Pb::CurHi] = uint16_t(address >> 16);
    x->pb[Pb::CurLo] = uint16_t(address);
    Touch(*x, kSyncCurrAddr);
}

void SetAdpcmLoop(uint32_t voice, uint16_t predScale, uint16_t yn1, uint16_t yn2) {
    Voice* x = At(voice);
    if (!x)
        return;
    x->pb[Pb::LoopPredScale] = predScale;
    x->pb[Pb::LoopYn1] = yn1;
    x->pb[Pb::LoopYn2] = yn2;
    Touch(*x, kSyncAdpcmLoop);
}

void SetLpf(uint32_t voice, uint16_t on, uint16_t yn1, uint16_t a0, uint16_t b0) {
    Voice* x = At(voice);
    if (!x)
        return;
    x->pb[Pb::LpfOn] = on;
    x->pb[Pb::LpfYn1] = yn1;
    x->pb[Pb::LpfA0] = a0;
    x->pb[Pb::LpfB0] = b0;
    Touch(*x, kSyncLpf);
}

void SetLpfCoefs(uint32_t voice, uint16_t a0, uint16_t b0) {
    Voice* x = At(voice);
    if (!x)
        return;
    x->pb[Pb::LpfA0] = a0;
    x->pb[Pb::LpfB0] = b0;
    Touch(*x, kSyncLpfCoef);
}

// c = 2 - cos(2 pi f / 32000); b0 = 32768 (c - sqrt(c^2 - 1)); a0 = 0x7FFF - b0,
// in the original's single-precision steps.
void LpfCoefs(uint16_t frequency, uint16_t& a0, uint16_t& b0) {
    const float w = float(6.2831854820251465f * float(frequency)) / 32000.0f;
    const float c = 2.0f - float(std::cos(double(w)));
    const float root = float(std::sqrt(double(c * c - 1.0f)));
    const float scaled = 32768.0f * -(root - c);
    b0 = uint16_t(int32_t(scaled));
    a0 = uint16_t(0x7FFF - b0);
}

void SetFormat(uint32_t voice, uint16_t loopFlag, uint16_t format) {
    Voice* x = At(voice);
    if (!x)
        return;
    x->pb[Pb::LoopFlag] = loopFlag;
    x->pb[Pb::Format] = format;
    Touch(*x, kSyncAddr);
}

void SetLoop(uint32_t voice, uint16_t loopFlag, uint32_t loop, uint32_t end, uint32_t current) {
    Voice* x = At(voice);
    if (!x)
        return;
    x->pb[Pb::LoopFlag] = loopFlag;
    x->pb[Pb::LoopHi] = uint16_t(loop >> 16);
    x->pb[Pb::LoopLo] = uint16_t(loop);
    x->pb[Pb::EndHi] = uint16_t(end >> 16);
    x->pb[Pb::EndLo] = uint16_t(end);
    x->pb[Pb::CurHi] = uint16_t(current >> 16);
    x->pb[Pb::CurLo] = uint16_t(current);
    Touch(*x, kSyncAddr);
}

void SetAddr(uint32_t voice, const Addr& addr) {
    SetFormat(voice, addr.loopFlag, addr.format);
    SetLoop(voice, addr.loopFlag, addr.loop, addr.end, addr.current);
}

void SetAdpcm(uint32_t voice, const Adpcm& adpcm) {
    Voice* x = At(voice);
    if (!x)
        return;
    std::memcpy(&x->pb[Pb::AdpcmCoefs], adpcm.coefs, sizeof(adpcm.coefs));
    x->pb[Pb::AdpcmGain] = adpcm.gain;
    x->pb[Pb::AdpcmPredScale] = adpcm.predScale;
    x->pb[Pb::AdpcmYn1] = adpcm.yn1;
    x->pb[Pb::AdpcmYn2] = adpcm.yn2;
    Touch(*x, kSyncAdpcm);
}

void SetSrc(uint32_t voice, uint32_t ratio) {
    Voice* x = At(voice);
    if (!x)
        return;
    x->pb[Pb::SrcRatioHi] = uint16_t(ratio >> 16);
    x->pb[Pb::SrcRatioLo] = uint16_t(ratio);
    x->pb[Pb::SrcFrac] = 0;
    for (uint32_t i = 0; i < 4; ++i)
        x->pb[Pb::SrcLast + i] = 0;
    Touch(*x, kSyncSrc);
}

void SetType(uint32_t voice, uint16_t type) {
    Voice* x = At(voice);
    if (!x)
        return;
    x->pb[Pb::Type] = type;
    Touch(*x, kSyncType);
}

void SetMix(uint32_t voice, const uint16_t (&mix)[24], uint32_t mixerCtrl) {
    Voice* x = At(voice);
    if (!x)
        return;
    std::memcpy(&x->pb[Pb::Mix], mix, sizeof(mix));
    x->pb[Pb::MixerCtrlHi] = uint16_t(mixerCtrl >> 16);
    x->pb[Pb::MixerCtrlLo] = uint16_t(mixerCtrl);
    Touch(*x, kSyncMix | kSyncMixerCtrl);
}

void SetVe(uint32_t voice, uint16_t volume, uint16_t delta) {
    Voice* x = At(voice);
    if (!x)
        return;
    x->pb[Pb::VeVolume] = volume;
    x->pb[Pb::VeDelta] = delta;
    Touch(*x, kSyncVe);
}

uint32_t State(uint32_t voice) {
    Voice* x = At(voice);
    if (!x)
        return 0;
    Refresh(*x);
    return x->pb[Pb::State];
}

uint32_t LoopFlag(uint32_t voice) {
    const Voice* x = At(voice);
    return x ? x->pb[Pb::LoopFlag] : 0;
}

uint32_t CurrentAddr(uint32_t voice) {
    Voice* x = At(voice);
    if (!x)
        return 0;
    Refresh(*x);
    return uint32_t(x->pb[Pb::CurHi]) << 16 | x->pb[Pb::CurLo];
}

} // namespace SoundVoice

extern "C" {

// AXInit, 0x8046EA50, and AXInitEx, 0x8046EAB0 (r3 the output buffer mode).
void MH3_AXInit(CpuContext* cpu) {
    InitEx(cpu, 0);
}
void MH3_AXInitEx(CpuContext* cpu) {
    InitEx(cpu, cpu->gpr[3]);
}

// AXQuit, 0x8046EB10.
void MH3_AXQuit(CpuContext*) {
    if (FlatRead32(kAxInitialized) == 0)
        return;
    FlatWrite32(kUserCallback, 0);
    StopNativeAudio();
    InitVoices();
    FlatWrite32(kVpbs, 0);
    FlatWrite32(kVoiceCount, 0);
    for (const uint32_t at : {kAuxACallback, kAuxBCallback, kAuxCCallback})
        FlatWrite32(at, 0);
    FlatWrite32(kAxInitialized, 0);
}

// AXIsInit, 0x8046EB60.
void MH3_AXIsInit(CpuContext* cpu) {
    cpu->gpr[3] = FlatRead32(kAxInitialized);
}

// AXFreeVoice, 0x8046EE60.
void MH3_AXFreeVoice(CpuContext* cpu) {
    SoundVoice::Free(cpu->gpr[3]);
}

// AXAcquireVoice, 0x8046EEE0: r3 priority, r4 drop callback, r5 user context.
void MH3_AXAcquireVoice(CpuContext* cpu) {
    const uint32_t priority = cpu->gpr[3], callback = cpu->gpr[4], context = cpu->gpr[5];
    uint32_t voice;
    {
        GuestFrame frame(cpu, 0x30);
        voice = SoundVoice::Acquire(cpu, priority, callback, context);
    }
    cpu->gpr[3] = voice;
}

// AXSetVoicePriority, 0x8046F070.
void MH3_AXSetVoicePriority(CpuContext* cpu) {
    SoundVoice::SetPriority(cpu->gpr[3], cpu->gpr[4]);
}

// AXRegisterAuxACallback/B/C, 0x8046F820/0x8046F890/0x8046F900: r3
// callback, r4 context. The mixer runs a known effect natively.
static void RegisterAux(uint32_t bus, uint32_t callback, uint32_t context) {
    static constexpr uint32_t kCallback[3] = {kAuxACallback, kAuxBCallback, kAuxCCallback};
    static constexpr uint32_t kContext[3] = {kAuxAContext, kAuxBContext, kAuxCContext};
    FlatWrite32(kCallback[bus], callback);
    FlatWrite32(kContext[bus], context);
    NoteAuxCallback(bus, callback, context);
}
void MH3_AXRegisterAuxACallback(CpuContext* cpu) {
    RegisterAux(0, cpu->gpr[3], cpu->gpr[4]);
}
void MH3_AXRegisterAuxBCallback(CpuContext* cpu) {
    RegisterAux(1, cpu->gpr[3], cpu->gpr[4]);
}
void MH3_AXRegisterAuxCCallback(CpuContext* cpu) {
    RegisterAux(2, cpu->gpr[3], cpu->gpr[4]);
}

// AXGetAuxACallback, 0x8046F970.
void MH3_AXGetAuxACallback(CpuContext* cpu) {
    FlatWrite32(cpu->gpr[3], FlatRead32(kAuxACallback));
    FlatWrite32(cpu->gpr[4], FlatRead32(kAuxAContext));
}

// AXSetMode 0x804703F0, AXGetMode 0x80470400, AXSetCompressor 0x80470410.
void MH3_AXSetMode(CpuContext* cpu) {
    if (FlatRead32(kMode) != cpu->gpr[3])
        std::fprintf(stderr, "MH3 audio: AX mode %u\n", unsigned(cpu->gpr[3]));
    FlatWrite32(kMode, cpu->gpr[3]);
}
void MH3_AXGetMode(CpuContext* cpu) {
    cpu->gpr[3] = FlatRead32(kMode);
}
void MH3_AXSetCompressor(CpuContext* cpu) {
    FlatWrite32(kCompressor, cpu->gpr[3]);
}

// AXGet/SetAux{A,B,C}ReturnVolume, 0x80470420..0x80470490; AXSetMasterVolume
// 0x80470450 (clamped to 0x8000).
void MH3_AXGetAuxAReturnVolume(CpuContext* cpu) {
    cpu->gpr[3] = FlatRead16(kAuxAReturn);
}
void MH3_AXGetAuxBReturnVolume(CpuContext* cpu) {
    cpu->gpr[3] = FlatRead16(kAuxBReturn);
}
void MH3_AXGetAuxCReturnVolume(CpuContext* cpu) {
    cpu->gpr[3] = FlatRead16(kAuxCReturn);
}
void MH3_AXSetMasterVolume(CpuContext* cpu) {
    FlatWrite16(kMasterVolume, uint16_t(cpu->gpr[3] > 0x8000 ? 0x8000 : cpu->gpr[3]));
}
void MH3_AXSetAuxAReturnVolume(CpuContext* cpu) {
    FlatWrite16(kAuxAReturn, uint16_t(cpu->gpr[3]));
}
void MH3_AXSetAuxBReturnVolume(CpuContext* cpu) {
    FlatWrite16(kAuxBReturn, uint16_t(cpu->gpr[3]));
}
void MH3_AXSetAuxCReturnVolume(CpuContext* cpu) {
    FlatWrite16(kAuxCReturn, uint16_t(cpu->gpr[3]));
}

// AXRegisterCallback, 0x80470BA0: the per-frame user callback; returns the old one.
void MH3_AXRegisterCallback(CpuContext* cpu) {
    const uint32_t old = FlatRead32(kUserCallback);
    FlatWrite32(kUserCallback, cpu->gpr[3]);
    cpu->gpr[3] = old;
}

// AXSetVoiceSrcType 0x80471BD0, AXSetVoiceState 0x80471C90,
// AXSetVoiceCurrentAddr 0x80471D00, AXSetVoiceSrcRatio 0x80471D60 (f1),
// AXSetVoiceAdpcmLoop 0x80471DE0 (r4 -> {pred_scale, yn1, yn2}),
// AXSetVoiceLpf 0x80471E40 (r4 -> {on, yn1, a0, b0}), AXSetVoiceLpfCoefs
// 0x80471EB0 (r4 a0, r5 b0), AXGetLpfCoefs 0x80471F10 (r3 cutoff, r4 -> a0,
// r5 -> b0), AXGetMaxVoices 0x80471FD0.
void MH3_AXSetVoiceSrcType(CpuContext* cpu) {
    SoundVoice::SetSrcType(cpu->gpr[3], cpu->gpr[4]);
}
void MH3_AXSetVoiceState(CpuContext* cpu) {
    SoundVoice::SetState(cpu->gpr[3], cpu->gpr[4]);
}
void MH3_AXSetVoiceCurrentAddr(CpuContext* cpu) {
    SoundVoice::SetCurrentAddr(cpu->gpr[3], cpu->gpr[4]);
}
void MH3_AXSetVoiceSrcRatio(CpuContext* cpu) {
    SoundVoice::SetSrcRatio(cpu->gpr[3], float(cpu->fpr[1].d));
}
void MH3_AXSetVoiceAdpcmLoop(CpuContext* cpu) {
    const uint32_t loop = cpu->gpr[4];
    SoundVoice::SetAdpcmLoop(cpu->gpr[3], FlatRead16(loop), FlatRead16(loop + 2), FlatRead16(loop + 4));
}
void MH3_AXSetVoiceLpf(CpuContext* cpu) {
    const uint32_t lpf = cpu->gpr[4];
    SoundVoice::SetLpf(cpu->gpr[3], FlatRead16(lpf), FlatRead16(lpf + 2), FlatRead16(lpf + 4), FlatRead16(lpf + 6));
}
void MH3_AXSetVoiceLpfCoefs(CpuContext* cpu) {
    SoundVoice::SetLpfCoefs(cpu->gpr[3], uint16_t(cpu->gpr[4]), uint16_t(cpu->gpr[5]));
}
void MH3_AXGetLpfCoefs(CpuContext* cpu) {
    uint16_t a0, b0;
    SoundVoice::LpfCoefs(uint16_t(cpu->gpr[3]), a0, b0);
    FlatWrite16(cpu->gpr[5], b0);
    FlatWrite16(cpu->gpr[4], a0);
}
void MH3_AXGetMaxVoices(CpuContext* cpu) {
    cpu->gpr[3] = FlatRead32(kVoiceCount);
}

} // extern "C"

REGISTER_NATIVE_FUNCTION_AS(0x8046EA50, MH3_AXInit, "AXInit");
REGISTER_NATIVE_FUNCTION_AS(0x8046EAB0, MH3_AXInitEx, "AXInitEx");
REGISTER_NATIVE_FUNCTION_AS(0x8046EB10, MH3_AXQuit, "AXQuit");
REGISTER_NATIVE_FUNCTION_AS(0x8046EB60, MH3_AXIsInit, "AXIsInit");
REGISTER_NATIVE_FUNCTION_AS(0x8046EE60, MH3_AXFreeVoice, "AXFreeVoice");
REGISTER_NATIVE_FUNCTION_AS(0x8046EEE0, MH3_AXAcquireVoice, "AXAcquireVoice");
REGISTER_NATIVE_FUNCTION_AS(0x8046F070, MH3_AXSetVoicePriority, "AXSetVoicePriority");
REGISTER_NATIVE_FUNCTION_AS(0x8046F820, MH3_AXRegisterAuxACallback, "AXRegisterAuxACallback");
REGISTER_NATIVE_FUNCTION_AS(0x8046F890, MH3_AXRegisterAuxBCallback, "AXRegisterAuxBCallback");
REGISTER_NATIVE_FUNCTION_AS(0x8046F900, MH3_AXRegisterAuxCCallback, "AXRegisterAuxCCallback");
REGISTER_NATIVE_FUNCTION_AS(0x8046F970, MH3_AXGetAuxACallback, "AXGetAuxACallback");
REGISTER_NATIVE_FUNCTION_AS(0x804703F0, MH3_AXSetMode, "AXSetMode");
REGISTER_NATIVE_FUNCTION_AS(0x80470400, MH3_AXGetMode, "AXGetMode");
REGISTER_NATIVE_FUNCTION_AS(0x80470410, MH3_AXSetCompressor, "AXSetCompressor");
REGISTER_NATIVE_FUNCTION_AS(0x80470420, MH3_AXGetAuxAReturnVolume, "AXGetAuxAReturnVolume");
REGISTER_NATIVE_FUNCTION_AS(0x80470430, MH3_AXGetAuxBReturnVolume, "AXGetAuxBReturnVolume");
REGISTER_NATIVE_FUNCTION_AS(0x80470440, MH3_AXGetAuxCReturnVolume, "AXGetAuxCReturnVolume");
REGISTER_NATIVE_FUNCTION_AS(0x80470450, MH3_AXSetMasterVolume, "AXSetMasterVolume");
REGISTER_NATIVE_FUNCTION_AS(0x80470470, MH3_AXSetAuxAReturnVolume, "AXSetAuxAReturnVolume");
REGISTER_NATIVE_FUNCTION_AS(0x80470480, MH3_AXSetAuxBReturnVolume, "AXSetAuxBReturnVolume");
REGISTER_NATIVE_FUNCTION_AS(0x80470490, MH3_AXSetAuxCReturnVolume, "AXSetAuxCReturnVolume");
REGISTER_NATIVE_FUNCTION_AS(0x80470BA0, MH3_AXRegisterCallback, "AXRegisterCallback");
REGISTER_NATIVE_FUNCTION_AS(0x80471BD0, MH3_AXSetVoiceSrcType, "AXSetVoiceSrcType");
REGISTER_NATIVE_FUNCTION_AS(0x80471C90, MH3_AXSetVoiceState, "AXSetVoiceState");
REGISTER_NATIVE_FUNCTION_AS(0x80471D00, MH3_AXSetVoiceCurrentAddr, "AXSetVoiceCurrentAddr");
REGISTER_NATIVE_FUNCTION_AS(0x80471D60, MH3_AXSetVoiceSrcRatio, "AXSetVoiceSrcRatio");
REGISTER_NATIVE_FUNCTION_AS(0x80471DE0, MH3_AXSetVoiceAdpcmLoop, "AXSetVoiceAdpcmLoop");
REGISTER_NATIVE_FUNCTION_AS(0x80471E40, MH3_AXSetVoiceLpf, "AXSetVoiceLpf");
REGISTER_NATIVE_FUNCTION_AS(0x80471EB0, MH3_AXSetVoiceLpfCoefs, "AXSetVoiceLpfCoefs");
REGISTER_NATIVE_FUNCTION_AS(0x80471F10, MH3_AXGetLpfCoefs, "AXGetLpfCoefs");
REGISTER_NATIVE_FUNCTION_AS(0x80471FD0, MH3_AXGetMaxVoices, "AXGetMaxVoices");
