// Native AX library entry points, RMHP08 (RVL SDK AX 0x4302_145). C ports of
// the functions decompiled from the disassembly (Ghidra plus llvm-objdump)
// that the game, MIX, SEQ, SYN and the effect code call. They keep the guest
// AXVPBs and voice stacks exactly as the SDK does, because the game also
// writes AXVPB fields directly and sets their sync bits; the native frame
// driver (native_audio_frame.cpp) syncs those into the mixer once per tick.
// Interrupt masking is dropped: every caller, including the AX frame
// callback, runs on the game thread.
//
// What changed against the SDK: __AXOutInit starts the native mixer instead
// of the DSP task and AI DMA, __AXOutQuit stops it, and registering an aux
// callback hands a known effect (AXFX ReverbHi) to the mixer.
#include "native_audio.h"

#include "abi_bridge.h"
#include "guest_call.h"
#include "memory_access.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace NativeAudio;
using namespace NativeAudio::Guest;
using MemoryInline::FlatRead16;
using MemoryInline::FlatRead32;
using MemoryInline::FlatWrite16;
using MemoryInline::FlatWrite32;

namespace NativeAudio {
// native_audio_frame.cpp
void StartNativeAudio(CpuContext* cpu);
void StopNativeAudio();
void NoteAuxCallback(uint32_t bus, uint32_t callback, uint32_t context);
bool UseNativeAudio();
} // namespace NativeAudio

extern "C" {
void func_80470930(CpuContext* cpu);  // __AXOutInit (translated original: DSP task, AI DMA)
void func_80470B30(CpuContext* cpu);  // __AXOutQuit
}

namespace {

constexpr uint32_t kOSRegisterVersion = 0x804CBA00;

uint32_t StackHead(uint32_t priority) {
    return FlatRead32(kStackHeads + priority * 4);
}

// __AXRemoveFromStack, 0x8046EDD0.
void RemoveFromStack(uint32_t voice) {
    const uint32_t priority = FlatRead32(voice + kVpbPriority);
    const uint32_t headSlot = kStackHeads + priority * 4, tailSlot = kStackTails + priority * 4;
    const uint32_t head = FlatRead32(headSlot), tail = FlatRead32(tailSlot);
    if (head == tail) {
        FlatWrite32(tailSlot, 0);
        FlatWrite32(headSlot, 0);
    } else if (voice == head) {
        const uint32_t next = FlatRead32(voice + kVpbNext);
        FlatWrite32(headSlot, next);
        FlatWrite32(next + kVpbPrev, 0);
    } else if (voice == tail) {
        const uint32_t prev = FlatRead32(voice + kVpbPrev);
        FlatWrite32(tailSlot, prev);
        FlatWrite32(prev + kVpbNext, 0);
    } else {
        const uint32_t prev = FlatRead32(voice + kVpbPrev), next = FlatRead32(voice + kVpbNext);
        FlatWrite32(prev + kVpbNext, next);
        FlatWrite32(next + kVpbPrev, prev);
    }
}

// Links `voice` in as the newest voice of `priority`.
void PushToStack(uint32_t voice, uint32_t priority) {
    const uint32_t headSlot = kStackHeads + priority * 4;
    const uint32_t head = FlatRead32(headSlot);
    FlatWrite32(voice + kVpbNext, head);
    FlatWrite32(voice + kVpbPrev, 0);
    if (head == 0) {
        FlatWrite32(headSlot, voice);
        FlatWrite32(kStackTails + priority * 4, voice);
    } else {
        FlatWrite32(head + kVpbPrev, voice);
        FlatWrite32(headSlot, voice);
    }
}

// __AXPushFreeStack, 0x8046EDA0.
void PushFreeStack(uint32_t voice) {
    FlatWrite32(voice + kVpbNext, FlatRead32(kStackHeads));
    FlatWrite32(kStackHeads, voice);
    FlatWrite32(voice + kVpbPriority, 0);
}

// __AXSetPBDefault, 0x804718B0.
void SetPBDefault(uint32_t voice) {
    FlatWrite16(voice + 0x38, 0);   // pb.state
    FlatWrite16(voice + 0x6C, 0);   // pb.itd.flag
    FlatWrite32(voice + kVpbSync, kSyncPbDefault);
    FlatWrite16(voice + 0xE2, 0);   // pb.lpf.on
    FlatWrite16(voice + 0xEA, 0);   // pb.biquad.on
    FlatWrite16(voice + 0xFE, 0);   // pb.remote
    FlatWrite16(voice + 0x13C, 0);  // pb.rmtIIR.on
    for (uint32_t o = 0x132; o <= 0x13A; o += 2)
        FlatWrite16(voice + o, 0);  // pb.rmtSrc
}

void AllocInit() {
    FlatWrite32(kCallbackStack, 0);
    for (uint32_t p = 0; p < 32; ++p) {
        FlatWrite32(kStackHeads + p * 4, 0);
        FlatWrite32(kStackTails + p * 4, 0);
    }
}

void Zero(uint32_t address, uint32_t bytes) {
    if (bytes)
        std::memset(GuestFlat::Host(address), 0, bytes);
}

// __AXPBInit, 0x80471920.
void PBInit() {
    const uint32_t count = FlatRead32(kVoiceCount);
    const uint32_t dspPbs = FlatRead32(kDspPbs), itds = FlatRead32(kItdBuffers), vpbs = FlatRead32(kVpbs);
    FlatWrite32(kDspCycles, 0);
    FlatWrite32(kMaxDspCycles, FlatRead32(0x800000F8) / 0x29B);  // bus clock / 667
    Zero(dspPbs, count * kPbSize);
    Zero(itds, count * 0x40);
    Zero(vpbs, count * kVpbSize);
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t vpb = vpbs + i * kVpbSize, dsp = dspPbs + i * kPbSize, itd = itds + i * 0x40;
        FlatWrite32(vpb + kVpbIndex, i);
        FlatWrite32(vpb + kVpbItdBuffer, itd);
        SetPBDefault(vpb);
        if (i == count - 1) {
            FlatWrite32(dsp + 0x00, 0);
            FlatWrite32(vpb + kVpbPb + 0x00, 0);
        } else {
            FlatWrite32(vpb + kVpbPb + 0x00, dsp + kPbSize);
            FlatWrite32(dsp + 0x00, dsp + kPbSize);
        }
        FlatWrite32(vpb + kVpbPb + 0x04, dsp);
        FlatWrite32(dsp + 0x04, dsp);
        FlatWrite32(vpb + kVpbPb + 0x46, itd);
        FlatWrite32(dsp + 0x46, itd);
        FlatWrite32(vpb + kVpbPriority, 1);
        PushFreeStack(vpb);
    }
}

void VPBInit() {
    FlatWrite32(kVoiceCount, kMaxVoices);
    FlatWrite32(kDspPbs, kDspPbStorage);
    FlatWrite32(kItdBuffers, kItdStorage);
    FlatWrite32(kVpbs, kVpbStorage);
    PBInit();
}

void VPBQuit() {
    FlatWrite32(kDspPbs, 0);
    FlatWrite32(kItdBuffers, 0);
    FlatWrite32(kVpbs, 0);
    FlatWrite32(kVoiceCount, 0);
}

void SPBInit() {
    for (uint32_t i = 0; i < 20; ++i)
        FlatWrite32(kSpbGlobals + i * 4, 0);
}

void AuxInit() {
    FlatWrite32(kAuxACallback, 0);
    FlatWrite32(kAuxBCallback, 0);
    FlatWrite32(kAuxCCallback, 0);
    FlatWrite32(kAuxAContext, 0);
    FlatWrite32(kAuxBContext, 0);
    FlatWrite32(kAuxCContext, 0);
    FlatWrite32(kAuxIndexWrite, 0);
    FlatWrite32(kAuxIndexRead, 1);
    FlatWrite32(kAuxIndexCpu, 2);
    Zero(kAuxABuffers, 0x480);
    Zero(kAuxBBuffers, 0x480);
    Zero(kAuxCBuffers, 0x480);
    Zero(kAuxAClear, 3);
    Zero(kAuxBClear, 3);
    Zero(kAuxCClear, 3);
}

void AuxQuit() {
    FlatWrite32(kAuxACallback, 0);
    FlatWrite32(kAuxBCallback, 0);
    FlatWrite32(kAuxCCallback, 0);
}

void CLInit() {
    FlatWrite32(kMode, 0);
    FlatWrite32(kCommandListIndex, 0);
    FlatWrite32(kCommandListPtr, 0x806FCC00);
    FlatWrite32(kCompressor, 1);
    FlatWrite32(kCompressorTable, 0x80610280);
    FlatWrite16(kCompressorRelease, 10);
    FlatWrite16(kMasterVolume, 0x8000);
    FlatWrite16(kAuxAReturn, 0x8000);
    FlatWrite16(kAuxBReturn, 0x8000);
    FlatWrite16(kAuxCReturn, 0x8000);
}

void OutInit(CpuContext* cpu, uint32_t outputMode) {
    if (!UseNativeAudio()) {
        cpu->gpr[3] = outputMode;
        func_80470930(cpu);
        return;
    }
    FlatWrite32(kOutputBufferMode, outputMode);
    FlatWrite32(kUserCallback, 0);
    StartNativeAudio(cpu);
}

void OutQuit(CpuContext* cpu) {
    if (!UseNativeAudio()) {
        func_80470B30(cpu);
        return;
    }
    FlatWrite32(kUserCallback, 0);
    StopNativeAudio();
}

void InitEx(CpuContext* cpu, uint32_t outputMode) {
    if (FlatRead32(kAxInitialized) != 0)
        return;
    {
        GuestFrame frame(cpu, 0x10);
        GuestCall{cpu}(kOSRegisterVersion, {FlatRead32(kSdkVersion)});
    }
    AllocInit();
    VPBInit();
    SPBInit();
    AuxInit();
    CLInit();
    OutInit(cpu, outputMode);
    FlatWrite32(kAxInitialized, 1);
}

uint32_t ToUnsigned(double value) {
    if (!(value > 0.0))
        return 0;
    if (value >= 4294967296.0)
        return 0xFFFFFFFFu;
    return uint32_t(value);
}

} // namespace

extern "C" {

// AXInit, 0x8046EA50.
void MH3_AXInit(CpuContext* cpu) {
    InitEx(cpu, 0);
}

// AXInitEx, 0x8046EAB0: r3 = output buffer mode (MH3 passes 1, triple).
void MH3_AXInitEx(CpuContext* cpu) {
    InitEx(cpu, cpu->gpr[3]);
}

// AXQuit, 0x8046EB10.
void MH3_AXQuit(CpuContext* cpu) {
    if (FlatRead32(kAxInitialized) == 0)
        return;
    OutQuit(cpu);
    AllocInit();  // __AXAllocQuit clears the same state
    VPBQuit();
    AuxQuit();
    FlatWrite32(kAxInitialized, 0);
}

// AXIsInit, 0x8046EB60.
void MH3_AXIsInit(CpuContext* cpu) {
    cpu->gpr[3] = FlatRead32(kAxInitialized);
}

// __AXGetStackHead, 0x8046EB70.
void MH3_AXGetStackHead(CpuContext* cpu) {
    cpu->gpr[3] = StackHead(cpu->gpr[3]);
}

// __AXServiceCallbackStack, 0x8046EB90: voices dropped for DSP time get
// their callback and go back to the free stack. Nothing drops voices now
// (the native mixer has no DSP cycle budget), so this normally does nothing.
void MH3_AXServiceCallbackStack(CpuContext* cpu) {
    GuestFrame frame(cpu, 0x20);
    for (uint32_t voice = FlatRead32(kCallbackStack); voice; voice = FlatRead32(kCallbackStack)) {
        FlatWrite32(kCallbackStack, FlatRead32(voice + kVpbNext1));
        if (FlatRead32(voice + kVpbPriority) == 0)
            continue;
        if (const uint32_t callback = FlatRead32(voice + kVpbCallback)) {
            cpu->gpr[3] = voice;
            InvokeIndirectCpu(callback, cpu);
        }
        RemoveFromStack(voice);
        FlatWrite32(voice + kVpbNext, FlatRead32(kStackHeads));
        FlatWrite32(kStackHeads, voice);
        FlatWrite32(voice + kVpbPriority, 0);
    }
}

// __AXAllocInit, 0x8046EC40, and __AXAllocQuit, 0x8046ECF0.
void MH3_AXAllocInit(CpuContext*) {
    AllocInit();
}
void MH3_AXAllocQuit(CpuContext*) {
    AllocInit();
}

// __AXPushFreeStack, 0x8046EDA0.
void MH3_AXPushFreeStack(CpuContext* cpu) {
    PushFreeStack(cpu->gpr[3]);
}

// __AXPushCallbackStack, 0x8046EDC0.
void MH3_AXPushCallbackStack(CpuContext* cpu) {
    const uint32_t voice = cpu->gpr[3];
    FlatWrite32(voice + kVpbNext1, FlatRead32(kCallbackStack));
    FlatWrite32(kCallbackStack, voice);
}

// __AXRemoveFromStack, 0x8046EDD0.
void MH3_AXRemoveFromStack(CpuContext* cpu) {
    RemoveFromStack(cpu->gpr[3]);
}

// AXFreeVoice, 0x8046EE60.
void MH3_AXFreeVoice(CpuContext* cpu) {
    const uint32_t voice = cpu->gpr[3];
    RemoveFromStack(voice);
    if (FlatRead16(voice + 0x38) == 1)
        FlatWrite32(voice + kVpbDepop, 1);
    SetPBDefault(voice);
    PushFreeStack(voice);
}

// AXAcquireVoice, 0x8046EEE0: r3 priority, r4 callback, r5 user context.
// Takes a free voice, else steals the oldest voice of the lowest lower
// priority (calling its callback). Returns the AXVPB or 0.
void MH3_AXAcquireVoice(CpuContext* cpu) {
    const uint32_t priority = cpu->gpr[3], callback = cpu->gpr[4], context = cpu->gpr[5];
    uint32_t voice = FlatRead32(kStackHeads);
    {
        GuestFrame frame(cpu, 0x30);
        if (voice) {
            FlatWrite32(kStackHeads, FlatRead32(voice + kVpbNext));
        } else {
            for (uint32_t p = 1; p < priority && p < 32; ++p) {
                const uint32_t headSlot = kStackHeads + p * 4, tailSlot = kStackTails + p * 4;
                const uint32_t head = FlatRead32(headSlot);
                if (!head)
                    continue;
                const uint32_t tail = FlatRead32(tailSlot);
                if (head == tail) {
                    FlatWrite32(tailSlot, 0);
                    FlatWrite32(headSlot, 0);
                    voice = head;
                } else if (tail) {
                    const uint32_t prev = FlatRead32(tail + kVpbPrev);
                    FlatWrite32(tailSlot, prev);
                    FlatWrite32(prev + kVpbNext, 0);
                    voice = tail;
                }
                if (voice) {
                    if (FlatRead16(voice + 0x38) == 1)
                        FlatWrite32(voice + kVpbDepop, 1);
                    if (const uint32_t stolen = FlatRead32(voice + kVpbCallback)) {
                        cpu->gpr[3] = voice;
                        InvokeIndirectCpu(stolen, cpu);
                    }
                    break;
                }
            }
        }
        if (voice) {
            PushToStack(voice, priority);
            FlatWrite32(voice + kVpbPriority, priority);
            FlatWrite32(voice + kVpbCallback, callback);
            FlatWrite32(voice + kVpbUserContext, context);
            SetPBDefault(voice);
        }
    }
    cpu->gpr[3] = voice;
}

// AXSetVoicePriority, 0x8046F070.
void MH3_AXSetVoicePriority(CpuContext* cpu) {
    const uint32_t voice = cpu->gpr[3], priority = cpu->gpr[4];
    RemoveFromStack(voice);
    PushToStack(voice, priority);
    FlatWrite32(voice + kVpbPriority, priority);
}

// __AXAuxInit, 0x8046F110, and __AXAuxQuit, 0x8046F230.
void MH3_AXAuxInit(CpuContext*) {
    AuxInit();
}
void MH3_AXAuxQuit(CpuContext*) {
    AuxQuit();
}

// AXRegisterAuxACallback/B/C, 0x8046F820/0x8046F890/0x8046F900: r3
// callback, r4 context. The mixer runs a known effect natively.
static void RegisterAux(uint32_t bus, uint32_t callback, uint32_t context) {
    static constexpr uint32_t kCallback[3] = {kAuxACallback, kAuxBCallback, kAuxCCallback};
    static constexpr uint32_t kContext[3] = {kAuxAContext, kAuxBContext, kAuxCContext};
    static constexpr uint32_t kClear[3] = {kAuxAClear, kAuxBClear, kAuxCClear};
    FlatWrite32(kCallback[bus], callback);
    FlatWrite32(kContext[bus], context);
    if (callback == 0)
        std::memset(GuestFlat::Host(kClear[bus]), 1, 3);
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

// __AXCLInit, 0x80470390; __AXCLQuit, 0x804703E0, does nothing.
void MH3_AXCLInit(CpuContext*) {
    CLInit();
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

// __AXOutInit, 0x80470930 (r3 output buffer mode), and __AXOutQuit, 0x80470B30.
void MH3_AXOutInit(CpuContext* cpu) {
    OutInit(cpu, cpu->gpr[3]);
}
void MH3_AXOutQuit(CpuContext* cpu) {
    OutQuit(cpu);
}

// AXRegisterCallback, 0x80470BA0: the per-frame user callback; returns the old one.
void MH3_AXRegisterCallback(CpuContext* cpu) {
    const uint32_t old = FlatRead32(kUserCallback);
    FlatWrite32(kUserCallback, cpu->gpr[3]);
    cpu->gpr[3] = old;
}

// __AXSPBInit, 0x80470E60.
void MH3_AXSPBInit(CpuContext*) {
    SPBInit();
}

// __AXSetPBDefault 0x804718B0, __AXVPBInit 0x804718F0, __AXPBInit 0x80471920,
// __AXVPBQuit 0x80471BB0.
void MH3_AXSetPBDefault(CpuContext* cpu) {
    SetPBDefault(cpu->gpr[3]);
}
void MH3_AXVPBInit(CpuContext*) {
    VPBInit();
}
void MH3_AXPBInit(CpuContext*) {
    PBInit();
}
void MH3_AXVPBQuit(CpuContext*) {
    VPBQuit();
}

// AXSetVoiceSrcType, 0x80471BD0: 0 none, 1 linear, 2-4 polyphase (coefficient set 0-2).
void MH3_AXSetVoiceSrcType(CpuContext* cpu) {
    const uint32_t voice = cpu->gpr[3], type = cpu->gpr[4];
    if (type == 0) {
        FlatWrite16(voice + 0x30, 2);
    } else if (type == 1) {
        FlatWrite16(voice + 0x30, 1);
    } else if (type <= 4) {
        FlatWrite16(voice + 0x30, 0);
        FlatWrite16(voice + 0x32, uint16_t(type - 2));
    }
    FlatWrite32(voice + kVpbSync, FlatRead32(voice + kVpbSync) | kSyncSrcSelect);
}

// AXSetVoiceState, 0x80471C90.
void MH3_AXSetVoiceState(CpuContext* cpu) {
    const uint32_t voice = cpu->gpr[3], state = cpu->gpr[4];
    if (FlatRead16(voice + 0x38) == state)  // cmplw of the zero-extended halfword
        return;
    FlatWrite16(voice + 0x38, uint16_t(state));
    FlatWrite32(voice + kVpbSync, FlatRead32(voice + kVpbSync) | kSyncState);
    if (state == 0)
        FlatWrite32(voice + kVpbDepop, 1);
}

// AXSetVoiceCurrentAddr, 0x80471D00.
void MH3_AXSetVoiceCurrentAddr(CpuContext* cpu) {
    const uint32_t voice = cpu->gpr[3], address = cpu->gpr[4];
    FlatWrite16(voice + 0xA2, uint16_t(address >> 16));
    FlatWrite16(voice + 0xA4, uint16_t(address));
    FlatWrite32(voice + kVpbSync, FlatRead32(voice + kVpbSync) | kSyncCurrAddr);
}

// AXSetVoiceSrcRatio, 0x80471D60: f1 ratio, stored as 16.16 (fmuls by 65536.0f).
void MH3_AXSetVoiceSrcRatio(CpuContext* cpu) {
    const uint32_t voice = cpu->gpr[3];
    const uint32_t ratio = ToUnsigned(double(float(65536.0 * cpu->fpr[1].d)));
    FlatWrite16(voice + 0xD0, uint16_t(ratio));
    FlatWrite16(voice + 0xCE, uint16_t(ratio >> 16));
    FlatWrite32(voice + kVpbSync, FlatRead32(voice + kVpbSync) | kSyncSrcRatio);
}

// AXSetVoiceAdpcmLoop, 0x80471DE0: r4 points at {pred_scale, yn1, yn2}.
void MH3_AXSetVoiceAdpcmLoop(CpuContext* cpu) {
    const uint32_t voice = cpu->gpr[3], loop = cpu->gpr[4];
    for (uint32_t i = 0; i < 3; ++i)
        FlatWrite16(voice + 0xDC + i * 2, FlatRead16(loop + i * 2));
    FlatWrite32(voice + kVpbSync, FlatRead32(voice + kVpbSync) | kSyncAdpcmLoop);
}

// AXSetVoiceLpf, 0x80471E40: r4 points at {on, yn1, a0, b0}.
void MH3_AXSetVoiceLpf(CpuContext* cpu) {
    const uint32_t voice = cpu->gpr[3], lpf = cpu->gpr[4];
    for (uint32_t i = 0; i < 4; ++i)
        FlatWrite16(voice + 0xE2 + i * 2, FlatRead16(lpf + i * 2));
    FlatWrite32(voice + kVpbSync, FlatRead32(voice + kVpbSync) | kSyncLpf);
}

// AXSetVoiceLpfCoefs, 0x80471EB0: r4 a0, r5 b0.
void MH3_AXSetVoiceLpfCoefs(CpuContext* cpu) {
    const uint32_t voice = cpu->gpr[3];
    FlatWrite16(voice + 0xE6, uint16_t(cpu->gpr[4]));
    FlatWrite16(voice + 0xE8, uint16_t(cpu->gpr[5]));
    FlatWrite32(voice + kVpbSync, FlatRead32(voice + kVpbSync) | kSyncLpfCoef);
}

// AXGetLpfCoefs, 0x80471F10: r3 cutoff (Hz), r4 -> a0, r5 -> b0.
// c = 2 - cos(2 pi f / 32000); b0 = 32768 (c - sqrt(c^2 - 1)); a0 = 0x7FFF - b0,
// in the original's single-precision steps.
void MH3_AXGetLpfCoefs(CpuContext* cpu) {
    const float frequency = float(cpu->gpr[3]);
    const float w = float(6.2831854820251465f * frequency) / 32000.0f;
    const float c = 2.0f - float(std::cos(double(w)));
    const float root = float(std::sqrt(double(c * c - 1.0f)));
    const float scaled = 32768.0f * -(root - c);
    const uint32_t b0 = uint32_t(int32_t(scaled)) & 0xFFFF;
    FlatWrite16(cpu->gpr[5], uint16_t(b0));
    FlatWrite16(cpu->gpr[4], uint16_t(0x7FFF - b0));
}

// AXGetMaxVoices, 0x80471FD0.
void MH3_AXGetMaxVoices(CpuContext* cpu) {
    cpu->gpr[3] = FlatRead32(kVoiceCount);
}

} // extern "C"

REGISTER_NATIVE_FUNCTION_AS(0x8046EA50, MH3_AXInit, "AXInit");
REGISTER_NATIVE_FUNCTION_AS(0x8046EAB0, MH3_AXInitEx, "AXInitEx");
REGISTER_NATIVE_FUNCTION_AS(0x8046EB10, MH3_AXQuit, "AXQuit");
REGISTER_NATIVE_FUNCTION_AS(0x8046EB60, MH3_AXIsInit, "AXIsInit");
REGISTER_NATIVE_FUNCTION_AS(0x8046EB70, MH3_AXGetStackHead, "__AXGetStackHead");
REGISTER_NATIVE_FUNCTION_AS(0x8046EB90, MH3_AXServiceCallbackStack, "__AXServiceCallbackStack");
REGISTER_NATIVE_FUNCTION_AS(0x8046EC40, MH3_AXAllocInit, "__AXAllocInit");
REGISTER_NATIVE_FUNCTION_AS(0x8046ECF0, MH3_AXAllocQuit, "__AXAllocQuit");
REGISTER_NATIVE_FUNCTION_AS(0x8046EDA0, MH3_AXPushFreeStack, "__AXPushFreeStack");
REGISTER_NATIVE_FUNCTION_AS(0x8046EDC0, MH3_AXPushCallbackStack, "__AXPushCallbackStack");
REGISTER_NATIVE_FUNCTION_AS(0x8046EDD0, MH3_AXRemoveFromStack, "__AXRemoveFromStack");
REGISTER_NATIVE_FUNCTION_AS(0x8046EE60, MH3_AXFreeVoice, "AXFreeVoice");
REGISTER_NATIVE_FUNCTION_AS(0x8046EEE0, MH3_AXAcquireVoice, "AXAcquireVoice");
REGISTER_NATIVE_FUNCTION_AS(0x8046F070, MH3_AXSetVoicePriority, "AXSetVoicePriority");
REGISTER_NATIVE_FUNCTION_AS(0x8046F110, MH3_AXAuxInit, "__AXAuxInit");
REGISTER_NATIVE_FUNCTION_AS(0x8046F230, MH3_AXAuxQuit, "__AXAuxQuit");
REGISTER_NATIVE_FUNCTION_AS(0x8046F820, MH3_AXRegisterAuxACallback, "AXRegisterAuxACallback");
REGISTER_NATIVE_FUNCTION_AS(0x8046F890, MH3_AXRegisterAuxBCallback, "AXRegisterAuxBCallback");
REGISTER_NATIVE_FUNCTION_AS(0x8046F900, MH3_AXRegisterAuxCCallback, "AXRegisterAuxCCallback");
REGISTER_NATIVE_FUNCTION_AS(0x8046F970, MH3_AXGetAuxACallback, "AXGetAuxACallback");
REGISTER_NATIVE_FUNCTION_AS(0x80470390, MH3_AXCLInit, "__AXCLInit");
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
REGISTER_NATIVE_FUNCTION_AS(0x80470930, MH3_AXOutInit, "__AXOutInit");
REGISTER_NATIVE_FUNCTION_AS(0x80470B30, MH3_AXOutQuit, "__AXOutQuit");
REGISTER_NATIVE_FUNCTION_AS(0x80470BA0, MH3_AXRegisterCallback, "AXRegisterCallback");
REGISTER_NATIVE_FUNCTION_AS(0x80470E60, MH3_AXSPBInit, "__AXSPBInit");
REGISTER_NATIVE_FUNCTION_AS(0x804718B0, MH3_AXSetPBDefault, "__AXSetPBDefault");
REGISTER_NATIVE_FUNCTION_AS(0x804718F0, MH3_AXVPBInit, "__AXVPBInit");
REGISTER_NATIVE_FUNCTION_AS(0x80471920, MH3_AXPBInit, "__AXPBInit");
REGISTER_NATIVE_FUNCTION_AS(0x80471BB0, MH3_AXVPBQuit, "__AXVPBQuit");
REGISTER_NATIVE_FUNCTION_AS(0x80471BD0, MH3_AXSetVoiceSrcType, "AXSetVoiceSrcType");
REGISTER_NATIVE_FUNCTION_AS(0x80471C90, MH3_AXSetVoiceState, "AXSetVoiceState");
REGISTER_NATIVE_FUNCTION_AS(0x80471D00, MH3_AXSetVoiceCurrentAddr, "AXSetVoiceCurrentAddr");
REGISTER_NATIVE_FUNCTION_AS(0x80471D60, MH3_AXSetVoiceSrcRatio, "AXSetVoiceSrcRatio");
REGISTER_NATIVE_FUNCTION_AS(0x80471DE0, MH3_AXSetVoiceAdpcmLoop, "AXSetVoiceAdpcmLoop");
REGISTER_NATIVE_FUNCTION_AS(0x80471E40, MH3_AXSetVoiceLpf, "AXSetVoiceLpf");
REGISTER_NATIVE_FUNCTION_AS(0x80471EB0, MH3_AXSetVoiceLpfCoefs, "AXSetVoiceLpfCoefs");
REGISTER_NATIVE_FUNCTION_AS(0x80471F10, MH3_AXGetLpfCoefs, "AXGetLpfCoefs");
REGISTER_NATIVE_FUNCTION_AS(0x80471FD0, MH3_AXGetMaxVoices, "AXGetMaxVoices");
