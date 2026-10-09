// AX voice sync, RMHP08. C reconstruction of the Revolution SDK's __AXSyncPBs
// (0x80471590), __AXSyncPB (0x80471040), __AXDepopVoice (0x80470ED0) and
// __AXPushFreeStack (0x8046EDC0) from the disassembly.
//
// SyncPBs(lessDspCycles) walks the voices of priorities 31..1, estimating the
// DSP cycles each running voice costs (source select, mixer and remote mixer
// tables); a voice still within the budget has its dirty parameter block
// fields copied into the DSP's PB array (-0x3E48(r13), 0x140 bytes each), one
// past it is stopped, depopped and freed. Priority 0 voices only depop. The
// data cache invalidates and flushes around it do nothing here.
#include "abi_bridge.h"
#include "ax_dsp.h"
#include "memory.h"
#include "port_check.h"
#include "ppc_runtime.h"

#include <cstdint>
#include <cstring>

extern "C" void func_80471590(CpuContext* ctx);
extern "C" void func_80471040(CpuContext* ctx);
extern "C" void func_80470ED0(CpuContext* ctx);

namespace {

constexpr uint32_t kR13 = 0x8079B2E0u;
constexpr uint32_t kPbArray = kR13 - 0x3E48, kVoiceCount = kR13 - 0x3E3C, kSyncCount = kR13 - 0x3E38;
constexpr uint32_t kCycles = kR13 - 0x3E34, kMaxCycles = kR13 - 0x3E30, kFreeStack = kR13 - 0x3F50;
constexpr uint32_t kBaseCycles = kR13 - 0x3EEC;
constexpr uint32_t kStacks = 0x806F9970u;      // per-priority voice lists
constexpr uint32_t kMixerCycles = 0x806101E0u; // by 5-bit mixer control fields
constexpr uint32_t kRmtCycles = 0x80610260u;   // by 2-bit remote mixer fields

uint32_t Read32(uint32_t a) {
    return MemoryInline::FlatRead32(a);
}
uint32_t Read16(uint32_t a) {
    return MemoryInline::FlatRead16(a);
}
void Write32(uint32_t a, uint32_t v) {
    MemoryInline::FlatWrite32(a, v);
}
void Write16(uint32_t a, uint16_t v) {
    MemoryInline::FlatWrite16(a, v);
}
uint32_t Rotl(uint32_t v, unsigned n) {
    return n == 0 ? v : (v << n) | (v >> (32 - n));
}

void Copy(uint32_t dst, uint32_t src, uint32_t length) {
    std::memcpy(Memory::GetPointer(dst, length), Memory::GetPointer(src, length), length);
}

uint32_t Pb(uint32_t voice) {
    return Read32(kPbArray) + Read32(voice + 0x18) * 0x140;
}

// Adds the PB's last samples to the depop accumulators.
void Depop(uint32_t pb) {
    static constexpr struct {
        uint16_t field;
        uint16_t sda;
    } kFields[] = {
        {0x52, 0x3E4C}, {0x54, 0x3E58}, {0x56, 0x3E64}, {0x58, 0x3E70}, {0x5A, 0x3E50}, {0x5C, 0x3E5C},  {0x5E, 0x3E68},  {0x60, 0x3E74},  {0x62, 0x3E54},  {0x64, 0x3E60},
        {0x66, 0x3E6C}, {0x68, 0x3E78}, {0xFA, 0x3E7C}, {0xFC, 0x3E80}, {0xFE, 0x3E84}, {0x100, 0x3E88}, {0x102, 0x3E8C}, {0x104, 0x3E90}, {0x106, 0x3E94}, {0x108, 0x3E98},
    };
    for (const auto& f : kFields) {
        const uint32_t at = kR13 - f.sda;
        Write32(at, Read32(at) + uint32_t(int32_t(int16_t(Read16(pb + f.field)))));
    }
}

// Copies the voice's dirty fields (flags +0x1C) of its PB copy (+0x28) into the
// DSP's PB, and the DSP-owned ones back.
void SyncPb(uint32_t voice) {
    Write32(kSyncCount, Read32(kSyncCount) + 1);
    const uint32_t dst = Pb(voice), src = voice + 0x28, dirty = Read32(voice + 0x1C);
    const auto in = [&](uint32_t offset, uint32_t length) {
        Copy(dst + offset, src + offset, length);
    };
    const auto out = [&](uint32_t offset, uint32_t length) {
        Copy(src + offset, dst + offset, length);
    };
    if (dirty == 0) {
        out(0x10, 2);
        out(0x6A, 2);
        out(0x7A, 4);
        return;
    }
    if (dirty & 0x80000000u) {
        in(0, 0x140);
        return;
    }
    if (dirty & 1)
        in(8, 4);
    if (dirty & 2)
        in(0xC, 4);
    if (dirty & 4)
        in(0x10, 2);
    else
        out(0x10, 2);
    if (dirty & 8)
        in(0x12, 2);
    if (dirty & 0x10)
        in(0x14, 0x30);
    if (dirty & 0x40) {
        in(0x4E, 4);
    } else if (dirty & 0x20) {
        in(0x44, 0xE);
        std::memset(Memory::GetPointer(Read32(voice + 0x24), 0x40), 0, 0x40);
    }
    if (dirty & 0x80)
        in(0x52, 0x18);
    if (dirty & 0x200) {
        out(0x6A, 2);
        in(0x6C, 2);
    } else if (dirty & 0x100) {
        in(0x6A, 4);
    }
    if (dirty & 0x7800) {
        if (dirty & 0x800)
            in(0x6E, 2);
        if (dirty & 0x1000)
            in(0x72, 4);
        if (dirty & 0x2000)
            in(0x76, 4);
        if (dirty & 0x4000)
            in(0x7A, 4);
        else
            out(0x7A, 4);
    } else if (dirty & 0x400) {
        in(0x6E, 0x10);
    } else {
        out(0x7A, 4);
    }
    if (dirty & 0x8000)
        in(0x7E, 0x28);
    if (dirty & 0x20000)
        in(0xA6, 4);
    else if (dirty & 0x10000)
        in(0xA6, 0xE);
    if (dirty & 0x40000)
        in(0xB4, 6);
    if (dirty & 0x100000)
        in(0xBE, 4);
    else if (dirty & 0x80000)
        in(0xBA, 8);
    if (dirty & 0x400000)
        in(0xCC, 0xA);
    else if (dirty & 0x200000)
        in(0xC2, 0x14);
    if (dirty & 0x800000)
        in(0xD6, 2);
    if (dirty & 0x1000000)
        in(0xD8, 2);
    if (dirty & 0x2000000)
        in(0xDA, 0x20);
    if (dirty & 0x4000000)
        in(0xFA, 0x10);
    if (dirty & 0x8000000)
        in(0x10A, 0xA);
    if (dirty & 0x20000000)
        in(0x118, 4);
    else if (dirty & 0x40000000)
        in(0x11E, 0xA);
    else if (dirty & 0x10000000)
        in(0x114, 0x14);
}

// The voice's DSP cycle estimate, from its PB copy.
uint32_t VoiceCycles(uint32_t voice) {
    uint32_t cycles = 0x183;
    if (Read16(voice + 0xE2) != 0)
        cycles += 0x135;
    if (Read16(voice + 0xEA) != 0)
        cycles += 0x400;
    if (Read16(voice + 0x6C) == 1)
        cycles += 0x1B;
    const uint32_t srcSelect = Read16(voice + 0x30);
    const uint32_t ratio = Read16(voice + 0xCE) << 16 | Read16(voice + 0xD0);
    const uint32_t scaled = (((ratio << 9) + 0x8000) >> 16) & 0xFFFF;
    cycles += srcSelect == 0 ? scaled + 0x619 : srcSelect == 1 ? scaled + 0x5BA : 0x25D;
    const uint32_t mixer = Read32(voice + 0x34);
    for (unsigned n : {13u, 2u, 8u, 18u})
        cycles += Read32(kMixerCycles + (Rotl(mixer, n) & 0x7C));
    if (Read16(voice + 0xFE) == 1) {
        cycles += 0x265;
        const uint32_t remote = Read16(voice + 0x13C);
        if (remote == 1)
            cycles += 0x76;
        else if (remote == 2)
            cycles += 0x342;
        const uint32_t rmt = Read16(voice + 0x100);
        for (unsigned n : {30u, 2u, 20u, 22u, 24u, 26u, 28u, 0u})
            cycles += Read32(kRmtCycles + (Rotl(rmt, n) & 0xC));
    }
    return cycles;
}

void SyncPbs(uint32_t lessDspCycles) {
    Write32(kSyncCount, 0);
    uint32_t cycles = Read32(kBaseCycles) + Read32(kVoiceCount) * 0x258 + lessDspCycles + 0x20;
    for (uint32_t priority = 31; priority != 0; --priority) {
        for (uint32_t voice = Read32(kStacks + 4 * priority); voice != 0; voice = Read32(voice)) {
            if (Read16(voice + 0x6C) == 1)
                cycles += 0x81;
            if (Read32(voice + 0x20) != 0)
                Depop(Pb(voice));
            if (Read16(voice + 0x38) == 1) {
                cycles += VoiceCycles(voice);
                if (Read32(kMaxCycles) > cycles) {
                    SyncPb(voice);
                } else {
                    const uint32_t pb = Pb(voice);
                    if (Read16(pb + 0x10) == 1)
                        Depop(pb);
                    Write16(voice + 0x38, 0);
                    Write16(pb + 0x10, 0);
                    Write32(voice + 8, Read32(kFreeStack));
                    Write32(kFreeStack, voice);
                }
            } else {
                SyncPb(voice);
            }
            Write32(voice + 0x1C, 0);
            Write32(voice + 0x20, 0);
        }
    }
    Write32(kCycles, cycles);
    for (uint32_t voice = Read32(kStacks); voice != 0; voice = Read32(voice)) {
        if (Read32(voice + 0x20) != 0)
            Depop(Pb(voice));
        Write32(voice + 0x20, 0);
        Write16(Pb(voice) + 0x10, 0);
    }
}

void SyncPbsPort(CpuContext* ctx) {
    SyncPbs(ctx->gpr[3]);
}
void SyncPbPort(CpuContext* ctx) {
    SyncPb(ctx->gpr[3]);
}
void DepopPort(CpuContext* ctx) {
    Depop(ctx->gpr[3]);
}

} // namespace

extern "C" void MH3_AXSyncPBs(CpuContext* ctx) {
    AxDspHle::JoinMixWorker();
#ifdef MH3_DEVELOPMENT
    static const bool check = PortCheck::Enabled("AXSyncPBs");
    if (check) [[unlikely]] {
        PortCheck::Run(ctx, "AXSyncPBs", func_80471590, SyncPbsPort);
        return;
    }
#endif
    SyncPbsPort(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x80471590, MH3_AXSyncPBs, "AXSyncPBs");

extern "C" void MH3_AXSyncPB(CpuContext* ctx) {
    AxDspHle::JoinMixWorker();
#ifdef MH3_DEVELOPMENT
    static const bool check = PortCheck::Enabled("AXSyncPB");
    if (check) [[unlikely]] {
        PortCheck::Run(ctx, "AXSyncPB", func_80471040, SyncPbPort);
        return;
    }
#endif
    SyncPbPort(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x80471040, MH3_AXSyncPB, "AXSyncPB");

extern "C" void MH3_AXDepopVoice(CpuContext* ctx) {
    AxDspHle::JoinMixWorker();
#ifdef MH3_DEVELOPMENT
    static const bool check = PortCheck::Enabled("AXDepopVoice");
    if (check) [[unlikely]] {
        PortCheck::Run(ctx, "AXDepopVoice", func_80470ED0, DepopPort);
        return;
    }
#endif
    DepopPort(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x80470ED0, MH3_AXDepopVoice, "AXDepopVoice");
