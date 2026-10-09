// RVL SDK MIX library, RMHP08, as native C. The game's sound driver and SYN
// call it to give each AX voice an input level, aux sends, pan/surround pan
// and a fader, all in dB x 10; MIXUpdateSettings (every 3 ms AX frame) turns
// changed settings into AXVPB volumes, per-sample ramps and mixerCtrl bits.
//
// The channel records stay in guest memory with the SDK's layout so the ports
// can be checked against the translated originals (MH3_PORT_CHECK). Remote
// speaker sends are computed as the SDK does; the native mixer does not play
// them.
#include "abi_bridge.h"
#include "memory_access.h"
#include "port_check.h"

#include <cstring>

using MemoryInline::FlatRead16;
using MemoryInline::FlatRead32;
using MemoryInline::FlatWrite16;
using MemoryInline::FlatWrite32;

namespace {

// Globals (r13-relative in the original).
constexpr uint32_t kChannels = 0x80797760;    // MIXChannel* (0x70 per voice)
constexpr uint32_t kInitialized = 0x80797764;
constexpr uint32_t kVoiceCount = 0x80797768;
constexpr uint32_t kSoundMode = 0x8079776C;   // 0 mono, 1 stereo, 2 surround, 3 DPL2
constexpr uint32_t kRemotes = 0x80797770;     // per voice remote record (0x44)
constexpr uint32_t kChannelStorage = 0x8074B078, kRemoteStorage = 0x8074DA78;
// AX state read by MIXInit (native_audio.h Guest::kAxInitialized/kVoiceCount).
constexpr uint32_t kAxInitialized = 0x80797388, kAxVoiceCount = 0x807974A4;

// Tables in .data.
constexpr uint32_t kVolumeTable = 0x8061B6C0;  // u16[0x388 + 0x3C]: dB x 10 -> volume
constexpr uint32_t kPanTable = 0x8061BE50;     // s32[128]: pan -> dB x 10
constexpr uint32_t kDpl2Front = 0x8061C050;    // s16[128]
constexpr uint32_t kDpl2Rear = 0x8061C150;     // s16[128]

// MIXChannel.
enum : uint32_t {
    kVoice = 0x00,
    kMode = 0x04,
    kInput = 0x08,
    kAuxA = 0x0C,
    kAuxB = 0x10,
    kAuxC = 0x14,
    kPan = 0x18,
    kSPan = 0x1C,
    kFader = 0x20,
    kL = 0x24,   // pan attenuations, dB x 10
    kR = 0x28,
    kF = 0x2C,
    kB = 0x30,
    kL1 = 0x34,  // DPL2 rear
    kR1 = 0x38,
    kVolume = 0x3C,  // u16 current, u16 target (+2) pairs from here
    kChannelSize = 0x70,
};
// Volume slots (current; target at +2).
enum : uint32_t {
    kV = 0x3C, kVL = 0x40, kVR = 0x44, kVS = 0x48,
    kVAL = 0x4C, kVAR = 0x50, kVAS = 0x54,
    kVBL = 0x58, kVBR = 0x5C, kVBS = 0x60,
    kVCL = 0x64, kVCR = 0x68, kVCS = 0x6C,
};
// MIXChannel.mode.
enum : uint32_t {
    kAuxAPreFader = 1u << 0,
    kAuxBPreFader = 1u << 1,
    kAuxCPreFader = 1u << 2,
    kMute = 1u << 3,
    kInputRamp = 1u << 29,   // copy the input target next frame
    kInputDirty = 1u << 28,
    kMixRamp = 1u << 31,     // copy the bus targets next frame
    kMixDirty = 1u << 30,
};
// AXVPB fields MIX writes (AXVPB + 0x28 is the AXPB).
enum : uint32_t {
    kVpbIndex = 0x18,
    kVpbSync = 0x1C,
    kVpbMixerCtrl = 0x34,
    kVpbMix = 0x3C,         // AXPBMIX: 12 (volume, delta) pairs
    kVpbVeVolume = 0x92,
    kVpbVeDelta = 0x94,
    kVpbRemoteCtrl = 0x100,
    kVpbRemoteMix = 0x102,  // 8 (volume, delta) pairs
};
enum : uint32_t { kSyncMixerCtrl = 0x2, kSyncMix = 0x10, kSyncVe = 0x100, kSyncRemote = 0x3000000 };

// Remote record (0x44): flags, main dB [4], aux dB [4], then 8 (current,
// target) u16 pairs from 0x24.
enum : uint32_t { kRemoteFlags = 0x00, kRemoteMain = 0x04, kRemoteAux = 0x14, kRemoteVolume = 0x24, kRemoteSize = 0x44 };

int32_t S32(uint32_t address) {
    return int32_t(FlatRead32(address));
}

// __MIXGetVolume: dB x 10 to a 1.15 volume.
uint16_t Volume(int32_t db) {
    if (db < -0x387)
        return 0;
    if (db > 0x3B)
        return 0xFF64;
    return FlatRead16(kVolumeTable + uint32_t(db + 0x388) * 2);
}

uint32_t Channel(uint32_t voice) {
    return FlatRead32(kChannels) + FlatRead32(voice + kVpbIndex) * kChannelSize;
}

// __MIXSetPan: the pan and surround pan attenuations.
void SetPan(uint32_t c) {
    const uint32_t pan = FlatRead32(c + kPan), span = FlatRead32(c + kSPan);
    if (FlatRead32(kSoundMode) == 3) {
        auto front = [](uint32_t i) { return uint32_t(int32_t(int16_t(FlatRead16(kDpl2Front + i * 2)))); };
        auto rear = [](uint32_t i) { return uint32_t(int32_t(int16_t(FlatRead16(kDpl2Rear + i * 2)))); };
        FlatWrite32(c + kL, front(pan));
        FlatWrite32(c + kR, front(0x7F - pan));
        FlatWrite32(c + kF, front(0x7F - span));
        FlatWrite32(c + kB, front(span));
        FlatWrite32(c + kL1, rear(0x7F - pan));
        FlatWrite32(c + kR1, rear(pan));
        return;
    }
    FlatWrite32(c + kL, FlatRead32(kPanTable + pan * 4));
    FlatWrite32(c + kR, FlatRead32(kPanTable + (0x7F - pan) * 4));
    FlatWrite32(c + kF, FlatRead32(kPanTable + (0x7F - span) * 4));
    FlatWrite32(c + kB, FlatRead32(kPanTable + span * 4));
    FlatWrite32(c + kL1, 0);
    FlatWrite32(c + kR1, 0);
}

// The bus volumes of the current sound mode, written at slot + `at` (0 for
// the current values, 2 for the targets). False for a mode MIX has no
// equation for (nothing is written).
bool ComputeBuses(uint32_t c, uint32_t at) {
    const uint32_t mode = FlatRead32(kSoundMode), flags = FlatRead32(c + kMode);
    const int32_t fader = S32(c + kFader), l = S32(c + kL), r = S32(c + kR), f = S32(c + kF), b = S32(c + kB);
    const int32_t l1 = S32(c + kL1), r1 = S32(c + kR1);
    auto put = [&](uint32_t slot, int32_t db) { FlatWrite16(c + slot + at, Volume(db)); };
    const uint32_t aux[3] = {kAuxA, kAuxB, kAuxC};
    const uint32_t preFader[3] = {kAuxAPreFader, kAuxBPreFader, kAuxCPreFader};
    if (mode == 1 || mode == 2) {
        put(kVL, l + f + fader);
        put(kVR, r + f + fader);
        put(kVS, fader + b - 30);
        const uint32_t slots[3][3] = {{kVAL, kVAR, kVAS}, {kVBL, kVBR, kVBS}, {kVCL, kVCR, kVCS}};
        for (uint32_t k = 0; k < 3; ++k) {
            const int32_t send = S32(c + aux[k]);
            const int32_t post = (flags & preFader[k]) ? 0 : fader;
            put(slots[k][0], l + f + post + send);
            put(slots[k][1], r + f + post + send);
            put(slots[k][2], post + send + b - 30);
        }
        return true;
    }
    if (mode == 0) {
        put(kVL, fader + f);
        put(kVR, fader + f);
        put(kVS, fader + b - 30);
        const uint32_t slots[3][3] = {{kVAL, kVAR, kVAS}, {kVBL, kVBR, kVBS}, {kVCL, kVCR, kVCS}};
        for (uint32_t k = 0; k < 3; ++k) {
            const int32_t send = S32(c + aux[k]);
            const int32_t post = (flags & preFader[k]) ? 0 : fader;
            put(slots[k][0], send + f + post);
            put(slots[k][1], send + f + post);
            put(slots[k][2], post + send + b - 30);
        }
        return true;
    }
    if (mode == 3) {
        // DPL2: the surround and aux C slots carry the rear left/right pairs.
        put(kVL, l + f + fader);
        put(kVR, r + f + fader);
        put(kVS, l1 + b + fader);
        put(kVCL, r1 + b + fader);
        const uint32_t slots[2][4] = {{kVAL, kVAR, kVAS, kVCR}, {kVBL, kVBR, kVBS, kVCS}};
        for (uint32_t k = 0; k < 2; ++k) {
            const int32_t send = S32(c + aux[k]);
            const int32_t post = (flags & preFader[k]) ? 0 : fader;
            put(slots[k][0], l + f + post + send);
            put(slots[k][1], r + f + post + send);
            put(slots[k][2], l1 + b + post + send);
            put(slots[k][3], r1 + b + post + send);
        }
        return true;
    }
    return false;
}

// AXPBMIX order: L R AL AR BL BR CL CR S AS BS CS; mixerCtrl bits for the
// volume being non-zero and for its ramp.
struct MixSlot {
    uint32_t slot, vpb, on, ramp;
};
constexpr MixSlot kMixSlots[12] = {
    {kVL, 0x00, 0x1, 0x4},           {kVR, 0x04, 0x2, 0x4},
    {kVAL, 0x08, 0x10000, 0x40000},  {kVAR, 0x0C, 0x20000, 0x40000},
    {kVBL, 0x10, 0x200000, 0x800000}, {kVBR, 0x14, 0x400000, 0x800000},
    {kVCL, 0x18, 0x4000000, 0x10000000}, {kVCR, 0x1C, 0x8000000, 0x10000000},
    {kVS, 0x20, 0x8, 0x10},          {kVAS, 0x24, 0x80000, 0x100000},
    {kVBS, 0x28, 0x1000000, 0x2000000}, {kVCS, 0x2C, 0x20000000, 0x40000000},
};

// The per-sample ramp from current to target over `samples` samples.
uint16_t Delta(uint16_t current, uint16_t target, int32_t samples) {
    return uint16_t((int32_t(target) - int32_t(current)) / samples);
}

// __MIXRmtResetChannel (0x804C6230).
void RemoteReset(uint32_t index) {
    const uint32_t r = FlatRead32(kRemotes) + index * kRemoteSize;
    for (uint32_t i = 0; i < 5; ++i)
        FlatWrite32(r + i * 4, 0);
    for (uint32_t i = 5; i < 9; ++i)
        FlatWrite32(r + i * 4, 0xFFFFFC40u);
    for (uint32_t i = 0; i < 8; ++i)
        FlatWrite16(r + kRemoteVolume + i * 4, 0);
}

// __MIXRmtUpdateSettings (0x804C5DF0): the four Wii Remote speakers' main and
// aux sends, ramped over the remote's 18-sample frame.
void RemoteUpdate(uint32_t index, uint32_t voice) {
    const uint32_t r = FlatRead32(kRemotes) + index * kRemoteSize;
    uint32_t flags = FlatRead32(r + kRemoteFlags);
    if ((flags & 0xC0000000u) == 0)
        return;
    if (flags & 0x80000000u) {
        for (uint32_t i = 0; i < 8; ++i)
            FlatWrite16(r + kRemoteVolume + i * 4, FlatRead16(r + kRemoteVolume + i * 4 + 2));
        flags &= 0x7FFFFFFFu;
        FlatWrite32(r + kRemoteFlags, flags);
    }
    if (flags & 0x40000000u) {
        for (uint32_t k = 0; k < 4; ++k) {
            const int32_t main = S32(r + kRemoteMain + k * 4), aux = S32(r + kRemoteAux + k * 4);
            FlatWrite16(r + kRemoteVolume + k * 8 + 2, Volume(main));
            FlatWrite16(r + kRemoteVolume + k * 8 + 6, Volume((flags & (1u << k)) ? aux : main + aux));
        }
        flags = (flags & 0xBFFFFFFFu) | 0x80000000u;
        FlatWrite32(r + kRemoteFlags, flags);
    }
    uint16_t control = 0;
    for (uint32_t i = 0; i < 8; ++i) {
        const uint16_t current = FlatRead16(r + kRemoteVolume + i * 4), target = FlatRead16(r + kRemoteVolume + i * 4 + 2);
        const uint16_t delta = Delta(current, target, 18);
        FlatWrite16(voice + kVpbRemoteMix + i * 4, current);
        FlatWrite16(voice + kVpbRemoteMix + i * 4 + 2, delta);
        if (current)
            control |= uint16_t(1u << (2 * i));
        if (delta)
            control |= uint16_t(2u << (2 * i));
    }
    FlatWrite16(voice + kVpbRemoteCtrl, control);
    FlatWrite32(voice + kVpbSync, FlatRead32(voice + kVpbSync) | kSyncRemote);
}

// MIXInit (0x804C2D60).
void Init() {
    if (FlatRead32(kAxInitialized) == 0 || FlatRead32(kInitialized) != 0)
        return;
    const uint32_t count = FlatRead32(kAxVoiceCount);
    FlatWrite32(kVoiceCount, count);
    FlatWrite32(kChannels, kChannelStorage);
    FlatWrite32(kRemotes, kRemoteStorage);
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t c = kChannelStorage + i * kChannelSize;
        FlatWrite32(c + kVoice, 0);
        FlatWrite32(c + kMode, 0x50000000u);
        FlatWrite32(c + kInput, 0);
        FlatWrite32(c + kAuxA, 0xFFFFFC40u);
        FlatWrite32(c + kAuxB, 0xFFFFFC40u);
        FlatWrite32(c + kAuxC, 0xFFFFFC40u);
        FlatWrite32(c + kFader, 0);
        FlatWrite32(c + kPan, 0x40);
        FlatWrite32(c + kSPan, 0x7F);
        for (uint32_t slot = kV; slot <= kVCS; slot += 4)
            FlatWrite16(c + slot, 0);
        SetPan(c);
        RemoteReset(i);
    }
    FlatWrite32(kSoundMode, 1);
    FlatWrite32(kInitialized, 1);
}

// MIXInitChannel (0x804C2EC0): r3 voice, r4 mode, r5 input, r6..r8 aux A..C,
// r9 pan, r10 surround pan, stack word fader.
void InitChannel(uint32_t voice, uint32_t mode, int32_t input, uint32_t auxA, uint32_t auxB, uint32_t auxC, uint32_t pan, uint32_t span, uint32_t fader) {
    const uint32_t c = Channel(voice);
    FlatWrite32(c + kVoice, voice);
    FlatWrite32(c + kMode, mode & 0xF);
    FlatWrite32(c + kInput, uint32_t(input));
    FlatWrite32(c + kAuxA, auxA);
    FlatWrite32(c + kAuxB, auxB);
    FlatWrite32(c + kAuxC, auxC);
    FlatWrite32(c + kPan, pan);
    FlatWrite32(c + kSPan, span);
    FlatWrite32(c + kFader, fader);
    SetPan(c);
    FlatWrite16(c + kV, (mode & kMute) ? 0 : Volume(input));
    uint32_t control = 0;
    ComputeBuses(c, 0);
    if (FlatRead32(kSoundMode) == 3)
        control = 0x80000000u;
    RemoteReset(FlatRead32(voice + kVpbIndex));
    FlatWrite16(voice + kVpbVeVolume, FlatRead16(c + kV));
    FlatWrite16(voice + kVpbVeDelta, 0);
    for (const MixSlot& m : kMixSlots) {
        const uint16_t v = FlatRead16(c + m.slot);
        FlatWrite16(voice + kVpbMix + m.vpb, v);
        FlatWrite16(voice + kVpbMix + m.vpb + 2, 0);
        if (v)
            control |= m.on;
    }
    FlatWrite32(voice + kVpbMixerCtrl, control);
    FlatWrite32(voice + kVpbSync, FlatRead32(voice + kVpbSync) | kSyncMixerCtrl | kSyncMix | kSyncVe);
    std::memset(GuestFlat::Host(voice + kVpbRemoteMix), 0, 0x20);
    FlatWrite16(voice + kVpbRemoteCtrl, 0);
    FlatWrite32(voice + kVpbSync, FlatRead32(voice + kVpbSync) | kSyncRemote);
}

// MIXUpdateSettings (0x804C4750), once per AX frame.
void UpdateSettings() {
    if (FlatRead32(kInitialized) == 0)
        return;
    const uint32_t count = FlatRead32(kVoiceCount), channels = FlatRead32(kChannels);
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t c = channels + i * kChannelSize;
        const uint32_t voice = FlatRead32(c + kVoice);
        if (!voice)
            continue;
        uint32_t control = 0;
        uint32_t flags = FlatRead32(c + kMode);
        bool input = (flags & kInputRamp) != 0;
        if (input) {
            FlatWrite16(c + kV, FlatRead16(c + kV + 2));
            flags &= ~kInputRamp;
        }
        if (flags & kInputDirty) {
            FlatWrite16(c + kV + 2, (flags & kMute) ? 0 : Volume(S32(c + kInput)));
            input = true;
            flags = (flags & ~kInputDirty) | kInputRamp;
        }
        bool mix = (flags & kMixRamp) != 0;
        if (mix) {
            for (uint32_t slot = kVL; slot <= kVCS; slot += 4)
                FlatWrite16(c + slot, FlatRead16(c + slot + 2));
            flags &= ~kMixRamp;
        }
        FlatWrite32(c + kMode, flags);
        if (flags & kMixDirty) {
            ComputeBuses(c, 2);
            if (FlatRead32(kSoundMode) == 3)
                control = 0x80000000u;
            mix = true;
            flags = (flags & ~kMixDirty) | kMixRamp;
            FlatWrite32(c + kMode, flags);
        }
        if (input) {
            const uint16_t current = FlatRead16(c + kV);
            FlatWrite16(voice + kVpbVeVolume, current);
            FlatWrite16(voice + kVpbVeDelta, Delta(current, FlatRead16(c + kV + 2), 96));
            FlatWrite32(voice + kVpbSync, FlatRead32(voice + kVpbSync) | kSyncVe);
        }
        if (mix) {
            for (const MixSlot& m : kMixSlots) {
                const uint16_t current = FlatRead16(c + m.slot), delta = Delta(current, FlatRead16(c + m.slot + 2), 96);
                FlatWrite16(voice + kVpbMix + m.vpb, current);
                FlatWrite16(voice + kVpbMix + m.vpb + 2, delta);
                if (current)
                    control |= m.on;
                if (delta)
                    control |= m.ramp;
            }
            FlatWrite32(voice + kVpbMixerCtrl, control);
            FlatWrite32(voice + kVpbSync, FlatRead32(voice + kVpbSync) | kSyncMixerCtrl | kSyncMix);
        }
        RemoteUpdate(i, voice);
    }
}

// Setters: r3 voice, r4 value; each marks the channel for the next update.
void SetField(uint32_t voice, uint32_t field, uint32_t value, uint32_t dirty) {
    const uint32_t c = Channel(voice);
    FlatWrite32(c + field, value);
    FlatWrite32(c + kMode, FlatRead32(c + kMode) | dirty);
}

uint32_t ClampPan(int32_t value) {
    return value < 0 ? 0 : value > 0x7F ? 0x7F : uint32_t(value);
}

void InitPort(CpuContext*) {
    Init();
}
void InitChannelPort(CpuContext* cpu) {
    const uint32_t fader = FlatRead32(cpu->gpr[1] + 8);
    InitChannel(cpu->gpr[3], cpu->gpr[4], int32_t(cpu->gpr[5]), cpu->gpr[6], cpu->gpr[7], cpu->gpr[8], cpu->gpr[9], cpu->gpr[10], fader);
}
void UpdateSettingsPort(CpuContext*) {
    UpdateSettings();
}

} // namespace

extern "C" {
void func_804C2D60(CpuContext*);
void func_804C2EC0(CpuContext*);
void func_804C4750(CpuContext*);

void MH3_MIXInit(CpuContext* cpu) {
    PortCheck::Call(cpu, "MIXInit", func_804C2D60, InitPort);
}
void MH3_MIXQuit(CpuContext*) {
    FlatWrite32(kChannels, 0);
    FlatWrite32(kRemotes, 0);
    FlatWrite32(kInitialized, 0);
}
void MH3_MIXSetSoundMode(CpuContext* cpu) {
    FlatWrite32(kSoundMode, cpu->gpr[3]);
}
void MH3_MIXInitChannel(CpuContext* cpu) {
    PortCheck::Call(cpu, "MIXInitChannel", func_804C2EC0, InitChannelPort);
}
void MH3_MIXReleaseChannel(CpuContext* cpu) {
    FlatWrite32(Channel(cpu->gpr[3]) + kVoice, 0);
}
void MH3_MIXSetInput(CpuContext* cpu) {
    SetField(cpu->gpr[3], kInput, cpu->gpr[4], kInputDirty);
}
void MH3_MIXSetAuxA(CpuContext* cpu) {
    SetField(cpu->gpr[3], kAuxA, cpu->gpr[4], kMixDirty);
}
void MH3_MIXSetAuxB(CpuContext* cpu) {
    SetField(cpu->gpr[3], kAuxB, cpu->gpr[4], kMixDirty);
}
void MH3_MIXSetPan(CpuContext* cpu) {
    const uint32_t c = Channel(cpu->gpr[3]);
    FlatWrite32(c + kPan, ClampPan(int32_t(cpu->gpr[4])));
    SetPan(c);
    FlatWrite32(c + kMode, FlatRead32(c + kMode) | kMixDirty);
}
void MH3_MIXSetSPan(CpuContext* cpu) {
    const uint32_t c = Channel(cpu->gpr[3]);
    FlatWrite32(c + kSPan, ClampPan(int32_t(cpu->gpr[4])));
    SetPan(c);
    FlatWrite32(c + kMode, FlatRead32(c + kMode) | kMixDirty);
}
void MH3_MIXSetFader(CpuContext* cpu) {
    SetField(cpu->gpr[3], kFader, cpu->gpr[4], kMixDirty);
}
void MH3_MIXUpdateSettings(CpuContext* cpu) {
    PortCheck::Call(cpu, "MIXUpdateSettings", func_804C4750, UpdateSettingsPort);
}
} // extern "C"

REGISTER_NATIVE_FUNCTION_AS(0x804C2D60, MH3_MIXInit, "MIXInit");
REGISTER_NATIVE_FUNCTION_AS(0x804C2E80, MH3_MIXQuit, "MIXQuit");
REGISTER_NATIVE_FUNCTION_AS(0x804C2EA0, MH3_MIXSetSoundMode, "MIXSetSoundMode");
REGISTER_NATIVE_FUNCTION_AS(0x804C2EC0, MH3_MIXInitChannel, "MIXInitChannel");
REGISTER_NATIVE_FUNCTION_AS(0x804C4590, MH3_MIXReleaseChannel, "MIXReleaseChannel");
REGISTER_NATIVE_FUNCTION_AS(0x804C45B0, MH3_MIXSetInput, "MIXSetInput");
REGISTER_NATIVE_FUNCTION_AS(0x804C45E0, MH3_MIXSetAuxA, "MIXSetAuxA");
REGISTER_NATIVE_FUNCTION_AS(0x804C4610, MH3_MIXSetAuxB, "MIXSetAuxB");
REGISTER_NATIVE_FUNCTION_AS(0x804C4640, MH3_MIXSetPan, "MIXSetPan");
REGISTER_NATIVE_FUNCTION_AS(0x804C46B0, MH3_MIXSetSPan, "MIXSetSPan");
REGISTER_NATIVE_FUNCTION_AS(0x804C4720, MH3_MIXSetFader, "MIXSetFader");
REGISTER_NATIVE_FUNCTION_AS(0x804C4750, MH3_MIXUpdateSettings, "MIXUpdateSettings");
