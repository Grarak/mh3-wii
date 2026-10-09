// Native AX frame driver, RMHP08: the game-thread half of the native audio
// path. It replaces __AXOutNewFrame (0x804704A0) and its AI DMA/DSP clock.
//
// The audio thread's mixed ticks are the clock. At each device service point
// (the frame pacer's PumpDevices, VIWaitForRetrace, OSSleepTicks) the driver
// produces AX frames ("ticks") until it is `lead` ticks ahead of the mixer,
// where `lead` follows the largest number of ticks the mixer advanced between
// two services in the last second, plus a margin. For each tick, in
// __AXOutNewFrame's order:
//   1. sync: every voice on a priority stack whose AXVPB sync bits are set
//      sends the changed PB groups (__AXSyncPB's precedence rules) to the
//      mixer, tagged with the tick; fields the game did not set get the
//      mixer's state, envelope volume and current address back. Voices on the
//      free stack are stopped.
//   2. the voice callback stack (__AXServiceCallbackStack);
//   3. the user callback the game registered with AXRegisterCallback (MIX,
//      SEQ, SYN and the sound engine; guest code, so it stays on this thread).
// The aux callbacks are not called: the mixer runs the reverb natively.
//
// A field the game changed is not overwritten by mixer feedback until the
// mixer has applied that tick (per field group: state, envelope, address),
// so a just-started voice is not seen as stopped while its start is queued.
#include "native_audio_mixer.h"

#include "abi_bridge.h"
#include "guest_flat_memory.h"
#include "memory_access.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using MemoryInline::FlatRead16;
using MemoryInline::FlatRead32;
using MemoryInline::FlatWrite16;
using MemoryInline::FlatWrite32;

extern "C" void MH3_AXServiceCallbackStack(CpuContext* cpu);

namespace NativeAudio {
namespace {
using namespace Guest;

constexpr uint32_t kMinLead = 2;
constexpr uint32_t kMaxLead = 64;            // 192 ms
constexpr uint32_t kMaxTicksPerService = 96; // bounds one service's burst
constexpr uint32_t kLeadWindowTicks = 333;   // one second of audio
// The largest tick: marker, reset, globals, three effects and every voice
// with every group.
constexpr uint32_t kWorstTickWords = 2 + 1 + 4 + 3 * (2 + kReverbConfigWords) + kMaxVoices * (2 + (kPbHalfwords + 1) / 2);

inline uint16_t Load16(const uint8_t* p) {
    uint16_t v;
    std::memcpy(&v, p, 2);
    return __builtin_bswap16(v);
}
inline uint32_t Load32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return __builtin_bswap32(v);
}
inline void Store16(uint8_t* p, uint16_t v) {
    v = __builtin_bswap16(v);
    std::memcpy(p, &v, 2);
}
inline void Store32(uint8_t* p, uint32_t v) {
    v = __builtin_bswap32(v);
    std::memcpy(p, &v, 4);
}
inline float LoadFloat(uint32_t address) {
    const uint32_t bits = FlatRead32(address);
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

uint64_t NowNs() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

struct PendingAux {
    bool pending = false;
    AuxKind kind = AuxKind::None;
    ReverbConfig config{};
};

struct Driver {
    bool active = false;
    bool servicing = false;
    bool resetPending = false;
    uint32_t produced = 0;      // next tick to produce
    uint32_t lastRendered = 0;  // mixer ticks at the previous service
    uint32_t lead = 8;
    uint32_t windowStart = 0, windowMax = 0;
    uint32_t stateTick[kMaxVoices]{}, volumeTick[kMaxVoices]{}, addressTick[kMaxVoices]{};
    bool running[kMaxVoices]{};
    bool globalsSent = false;
    uint32_t globals[3]{};
    PendingAux aux[3];
    // Statistics (game thread).
    uint64_t syncNs = 0, callbackNs = 0, serviceNs = 0, ticks = 0, voiceRecords = 0, services = 0;
    uint32_t ringHighWater = 0;
};
Driver g;

// Sync of one AXVPB (host pointer `vpb`) for tick `tick`: __AXSyncPB with the
// DSP PB replaced by the mixer's command stream and feedback.
void SyncVoice(CommandRing& ring, const Feedback& feedback, uint32_t applied, uint8_t* vpb, uint32_t index, uint32_t tick) {
    const uint32_t sync = Load32(vpb + kVpbSync);
    const uint32_t depop = Load32(vpb + kVpbDepop);
    uint8_t* pb = vpb + kVpbPb;
    bool backState = false, backVolume = false, backAddress = false;
    uint32_t groups = 0;
    if (sync == 0) {
        backState = backVolume = backAddress = true;
    } else if (sync & kSyncAllParams) {
        groups = kSyncAllParams;
        g.stateTick[index] = g.volumeTick[index] = g.addressTick[index] = tick;
        g.running[index] = Load16(pb + 0x10) == 1;
    } else {
        groups = sync & (kSyncSrcSelect | kSyncMixerCtrl | kSyncType | kSyncMix | kSyncDpop | kSyncAdpcm | kSyncAdpcmLoop);
        if (sync & kSyncState) {
            groups |= kSyncState;
            g.stateTick[index] = tick;
            g.running[index] = Load16(pb + 0x10) == 1;
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
            g.volumeTick[index] = tick;
        }
        if ((sync & (kSyncLoop | kSyncLoopAddr | kSyncEndAddr | kSyncCurrAddr)) == 0) {
            if (sync & kSyncAddr) {
                groups |= kSyncAddr;
                g.addressTick[index] = tick;
            } else {
                backAddress = true;
            }
        } else {
            groups |= sync & (kSyncLoop | kSyncLoopAddr | kSyncEndAddr);
            if (sync & kSyncCurrAddr) {
                groups |= kSyncCurrAddr;
                g.addressTick[index] = tick;
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
        out[0] = Header(Command::Voice, words, index, depop ? kFlagDepop : 0);
        out[1] = groups;
        uint32_t* data = out + 2;
        uint32_t half = 0;
        for (uint32_t bits = groups; bits; bits &= bits - 1) {
            const SyncGroup group = kSyncGroups[__builtin_ctz(bits)];
            for (uint32_t i = 0; i < group.count; ++i, ++half) {
                const uint32_t value = Load16(pb + (group.offset + i) * 2);
                if (half & 1)
                    data[half >> 1] |= value << 16;
                else
                    data[half >> 1] = value;
            }
        }
        ++g.voiceRecords;
    }
    // Feedback: what the mixer did, unless this field still waits in the queue.
    if (backState && int32_t(applied - g.stateTick[index]) > 0)
        Store16(pb + 0x10, uint16_t(feedback.stateVolume[index].load(std::memory_order_relaxed) >> 16));
    if (backVolume && int32_t(applied - g.volumeTick[index]) > 0)
        Store16(pb + 0x6A, uint16_t(feedback.stateVolume[index].load(std::memory_order_relaxed)));
    if (backAddress && int32_t(applied - g.addressTick[index]) > 0)
        Store32(pb + 0x7A, feedback.address[index].load(std::memory_order_relaxed));
    Store32(vpb + kVpbSync, 0);
    Store32(vpb + kVpbDepop, 0);
}

// __AXSyncPBs without the DSP cycle budget.
void SyncVoices(CommandRing& ring, Feedback& feedback, uint32_t tick) {
    const uint32_t count = std::min(FlatRead32(kVoiceCount), kMaxVoices);
    const uint32_t vpbs = FlatRead32(kVpbs);
    if (!count || !vpbs)
        return;
    uint8_t* base = GuestFlat::Host(vpbs);
    const uint32_t applied = feedback.appliedTick.load(std::memory_order_acquire);
    const uint32_t span = count * kVpbSize;
    auto voiceAt = [&](uint32_t address, uint32_t& index) -> uint8_t* {
        const uint32_t offset = address - vpbs;
        if (offset >= span || offset % kVpbSize)
            return nullptr;
        index = offset / kVpbSize;
        return base + offset;
    };
    for (uint32_t priority = 31; priority >= 1; --priority) {
        uint32_t address = FlatRead32(kStackHeads + priority * 4);
        for (uint32_t guard = 0; address && guard < count; ++guard) {
            uint32_t index;
            uint8_t* vpb = voiceAt(address, index);
            if (!vpb)
                break;
            address = Load32(vpb + kVpbNext);
            SyncVoice(ring, feedback, applied, vpb, index, tick);
        }
    }
    uint32_t address = FlatRead32(kStackHeads);
    for (uint32_t guard = 0; address && guard < count; ++guard) {
        uint32_t index;
        uint8_t* vpb = voiceAt(address, index);
        if (!vpb)
            break;
        address = Load32(vpb + kVpbNext);
        if (g.running[index]) {
            uint32_t* out = ring.Reserve(1);
            out[0] = Header(Command::Stop, 1, index, Load32(vpb + kVpbDepop) ? kFlagDepop : 0);
            g.running[index] = false;
            g.stateTick[index] = tick;
        }
        Store32(vpb + kVpbDepop, 0);
    }
}

void EmitGlobals(CommandRing& ring) {
    const uint32_t words[3] = {uint32_t(FlatRead16(kMasterVolume)) | (FlatRead32(kMode) & 0xFFFF) << 16,
                               uint32_t(FlatRead16(kAuxAReturn)) | uint32_t(FlatRead16(kAuxBReturn)) << 16, FlatRead16(kAuxCReturn)};
    if (g.globalsSent && std::memcmp(words, g.globals, sizeof(words)) == 0)
        return;
    uint32_t* out = ring.Reserve(4);
    out[0] = Header(Command::Globals, 4);
    std::memcpy(out + 1, words, sizeof(words));
    std::memcpy(g.globals, words, sizeof(words));
    g.globalsSent = true;
}

void EmitAux(CommandRing& ring) {
    for (uint32_t bus = 0; bus < 3; ++bus) {
        PendingAux& aux = g.aux[bus];
        if (!aux.pending)
            continue;
        const uint32_t words = 2 + (aux.kind == AuxKind::ReverbHi ? kReverbConfigWords : 0);
        uint32_t* out = ring.Reserve(words);
        out[0] = Header(Command::Aux, words, bus);
        out[1] = uint32_t(aux.kind);
        if (aux.kind == AuxKind::ReverbHi)
            std::memcpy(out + 2, &aux.config, sizeof(aux.config));
        aux.pending = false;
    }
}

void RunUserCallback(CpuContext* cpu) {
    const uint32_t callback = FlatRead32(kUserCallback);
    if (!callback)
        return;
    struct Restore {
        CpuContext* cpu;
        CpuContext saved;
        ~Restore() { *cpu = saved; }
    } restore{cpu, *cpu};
    cpu->msr &= ~0x8000u;  // as from the AX interrupt
    InvokeIndirectCpu(callback, cpu);
}

void ProduceTick(CpuContext* cpu, Mixer& mixer) {
    CommandRing& ring = mixer.Ring();
    const uint32_t tick = g.produced;
    const uint64_t start = NowNs();
    uint32_t* marker = ring.Reserve(2);
    marker[0] = Header(Command::Tick, 2);
    marker[1] = tick;
    if (g.resetPending) {
        ring.Reserve(1)[0] = Header(Command::Reset, 1);
        g.resetPending = false;
        g.globalsSent = false;
        std::fill(std::begin(g.running), std::end(g.running), false);
    }
    EmitGlobals(ring);
    EmitAux(ring);
    SyncVoices(ring, mixer.FeedbackState(), tick);
    ring.Publish();
    g.ringHighWater = std::max(g.ringHighWater, ring.Used());
    g.produced = tick + 1;
    mixer.SetProducedTicks(g.produced);
    const uint64_t synced = NowNs();
    {
        // The callback stack holds voices dropped for DSP time; none are
        // dropped now, so this is normally empty.
        if (FlatRead32(kCallbackStack)) {
            const CpuContext saved = *cpu;
            MH3_AXServiceCallbackStack(cpu);
            *cpu = saved;
        }
        RunUserCallback(cpu);
    }
    const uint64_t done = NowNs();
    g.syncNs += synced - start;
    g.callbackNs += done - synced;
    ++g.ticks;
}

ReverbConfig SnapshotReverb(uint32_t state) {
    // AXFX_REVERBHI_EXP fields (see audio_reverb.cpp); the translated
    // AXFXReverbHiInit/Settings filled them before the registration.
    ReverbConfig c{};
    c.earlyLength = FlatRead32(state + 0x18);
    for (uint32_t k = 0; k < 3; ++k) {
        c.earlyTaps[k] = FlatRead32(state + 0x0C + 4 * k);
        c.earlyGains[k] = LoadFloat(state + 0x20 + 4 * k);
        c.combPos[k] = FlatRead32(state + 0x68 + 4 * k);
        c.combLength[k] = FlatRead32(state + 0x74 + 4 * k);
        c.combGains[k] = LoadFloat(state + 0x8C + 4 * k);
        c.lastPos[k] = FlatRead32(state + 0xD4 + 4 * k);
        c.lastLength[k] = FlatRead32(state + 0xE0 + 4 * k);
    }
    c.preDelayPos = FlatRead32(state + 0x38);
    c.preDelayLength = FlatRead32(state + 0x3C);
    for (uint32_t k = 0; k < 2; ++k) {
        c.allpassPos[k] = FlatRead32(state + 0xB0 + 4 * k);
        c.allpassLength[k] = FlatRead32(state + 0xB8 + 4 * k);
    }
    c.allpassGain = LoadFloat(state + 0xF8);
    c.lowpassCoef = LoadFloat(state + 0x108);
    c.crosstalk = LoadFloat(state + 0x12C);
    c.level = LoadFloat(state + 0x134);
    c.outGain = LoadFloat(state + 0x140);
    c.sendGain = LoadFloat(state + 0x144);
    return c;
}

bool ValidReverb(const ReverbConfig& c) {
    constexpr uint32_t kMaxLine = 1u << 17;  // 4 s at 32 kHz
    if (!c.earlyLength || c.earlyLength > kMaxLine || c.preDelayLength > kMaxLine)
        return false;
    for (uint32_t k = 0; k < 3; ++k)
        if (!c.combLength[k] || c.combLength[k] > kMaxLine || !c.lastLength[k] || c.lastLength[k] > kMaxLine)
            return false;
    for (uint32_t k = 0; k < 2; ++k)
        if (!c.allpassLength[k] || c.allpassLength[k] > kMaxLine)
            return false;
    return true;
}

} // namespace

bool UseNativeAudio() {
#ifdef MH3_DEVELOPMENT
    // MH3_NATIVE_AUDIO=0: the previous AX/DSP HLE (comparison only).
    static const bool native = [] {
        const char* v = std::getenv("MH3_NATIVE_AUDIO");
        return !(v && v[0] == '0');
    }();
    return native;
#else
    return true;
#endif
}

bool NativeAudioActive() {
    return g.active;
}

void StartNativeAudio(CpuContext*) {
    if (g.active) {
        g.resetPending = true;
        return;
    }
    Mixer& mixer = GetMixer();
    mixer.Initialize(GuestFlat::Host(Memory::kMem1CachedBase), uint32_t(Memory::kMem1Size), GuestFlat::Host(Memory::kMem2CachedBase), uint32_t(Memory::kMem2Size));
    const uint32_t lead = g.lead;
    g = Driver{};
    g.lead = lead;
    g.active = true;
    StartAudioOutput();
    std::fprintf(stderr, "MH3 audio: native AX mixer (no DSP), %u voices, 32 kHz\n", unsigned(kMaxVoices));
}

void StopNativeAudio() {
    if (g.active)
        g.resetPending = true;
}

void NoteAuxCallback(uint32_t bus, uint32_t callback, uint32_t context) {
    if (!g.active || bus >= 3)
        return;
    PendingAux& aux = g.aux[bus];
    aux.pending = true;
    aux.kind = AuxKind::None;
    if (callback == kAxfxReverbHiCallback && context) {
        aux.config = SnapshotReverb(context);
        if (ValidReverb(aux.config) && (FlatRead32(context + 0x10C) & 1) == 0)
            aux.kind = AuxKind::ReverbHi;
        else
            std::fprintf(stderr, "MH3 audio: aux %c reverb state at %08x not initialized; effect off\n", 'A' + bus, unsigned(context));
    } else if (callback) {
        static bool reported[3];
        if (!reported[bus]) {
            std::fprintf(stderr, "MH3 audio: aux %c callback %08x has no native effect; effect off\n", 'A' + bus, unsigned(callback));
            reported[bus] = true;
        }
    }
}

void ServiceNativeAudio(CpuContext* cpu) {
    if (!g.active || g.servicing)
        return;
    g.servicing = true;
    struct Done {
        ~Done() { g.servicing = false; }
    } done;
    const uint64_t start = NowNs();
    Mixer& mixer = GetMixer();
    const uint32_t rendered = mixer.FeedbackState().renderedTicks.load(std::memory_order_acquire);
    // Lead: the most ticks the mixer advanced between two services over the
    // last second, plus a margin; it grows at once and shrinks per window.
    const uint32_t advanced = rendered - g.lastRendered;
    g.lastRendered = rendered;
    g.windowMax = std::max(g.windowMax, advanced);
    g.lead = std::max(g.lead, std::min(advanced + kMinLead, kMaxLead));
    if (rendered - g.windowStart >= kLeadWindowTicks) {
        g.lead = std::clamp(g.windowMax + kMinLead, kMinLead, kMaxLead);
        g.windowMax = 0;
        g.windowStart = rendered;
    }
    const uint32_t target = rendered + g.lead;
    CommandRing& ring = mixer.Ring();
    for (uint32_t n = 0; int32_t(target - g.produced) > 0 && n < kMaxTicksPerService; ++n) {
        if (ring.Free() < kWorstTickWords)
            break;
        ProduceTick(cpu, mixer);
    }
    ++g.services;
    g.serviceNs += NowNs() - start;
}

void ReportNativeAudioStatistics(double seconds) {
    if (!g.active)
        return;
    static Driver previous;
    static uint64_t previousRenderNs = 0, previousRendered = 0, previousFrozen = 0, previousLate = 0;
    MixerStatistics& s = GetMixer().Statistics();
    const uint64_t renderNs = s.renderNs.load(std::memory_order_relaxed);
    const uint64_t rendered = s.renderedTicks.load(std::memory_order_relaxed);
    const uint64_t frozen = s.frozenTicks.load(std::memory_order_relaxed);
    const uint64_t late = s.lateTicks.load(std::memory_order_relaxed);
    const double mixerMs = double(renderNs - previousRenderNs) / (1e6 * seconds);
    std::fprintf(stderr,
                 "MH3 audio: native mixer %.2f ms/s (%.1f%% of a core), %.1f ticks/s, padded %.2f ms/s (game behind), late %.1f ticks/s, voices %u active/%u peak, "
                 "thread %d core %d affinity %x\n",
                 mixerMs, mixerMs / 10.0, double(rendered - previousRendered) / seconds, double(frozen - previousFrozen) * 3.0 / seconds, double(late - previousLate) / seconds,
                 s.activeVoices.load(std::memory_order_relaxed), s.peakVoices.load(std::memory_order_relaxed), s.thread.load(std::memory_order_relaxed),
                 s.core.load(std::memory_order_relaxed), unsigned(s.affinity.load(std::memory_order_relaxed)));
    std::fprintf(stderr, "MH3 audio: game thread sync %.2f ms/s, AX callback %.2f ms/s, service %.2f ms/s; %.1f ticks/s produced, lead %u ticks, %.1f voice updates/tick, ring %.1f KiB high-water\n",
                 double(g.syncNs - previous.syncNs) / (1e6 * seconds), double(g.callbackNs - previous.callbackNs) / (1e6 * seconds),
                 double(g.serviceNs - previous.serviceNs) / (1e6 * seconds), double(g.ticks - previous.ticks) / seconds, g.lead,
                 g.ticks > previous.ticks ? double(g.voiceRecords - previous.voiceRecords) / double(g.ticks - previous.ticks) : 0.0, g.ringHighWater * 4.0 / 1024.0);
    previous.syncNs = g.syncNs;
    previous.callbackNs = g.callbackNs;
    previous.serviceNs = g.serviceNs;
    previous.ticks = g.ticks;
    previous.voiceRecords = g.voiceRecords;
    previousRenderNs = renderNs;
    previousRendered = rendered;
    previousFrozen = frozen;
    previousLate = late;
}

} // namespace NativeAudio
