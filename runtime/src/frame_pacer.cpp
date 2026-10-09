// Frame pacer, RMHP08. C reconstruction of the main loop's end-of-frame wait
// (0x800401A8, called by the main loop 0x8003F278 after the display copy),
// decompiled with Ghidra (scripts/dol2elf.py -> ELF import).
//
// The game runs one logic/render frame per `retracesPerFrame` VI retraces. Its
// VI post-retrace callback counts retraces into `retracesThisFrame`; this
// function waits until one retrace short of the budget, flips the framebuffer
// (VISetNextFrameBuffer + VIFlush), then waits out the rest of the budget and
// resets the count. Frames that took longer than the budget do not wait.
// Each wait step is OSSleepTicks(1 ms).
#include "abi_bridge.h"
#include "memory.h"
#include <initializer_list>

namespace {

// Small-data globals (offsets from r13; lower-case names are mh3.sel exports).
enum : int32_t {
    kBlackPending = -0x8000,       // u8: clear VI black on the next flip
    kWidemodeParam = -0x7FFF,      // u8 set_widemode_param: 4:3/16:9 mode to apply (0xFF = none)
    kWidemodeFlag = -0x4690,       // u8 set_widemode_flag: a widescreen switch is pending
    kFrameCount = -0x45B8,         // u32: frames paced so far
    kRetracesThisFrame = -0x4680,  // s32: retraces since the last flip
    kRetracesPerFrame = -0x4684,   // s32: frame budget in retraces
    kViFlag = -0x467C,             // u8: value passed to SetViFlag
    kViFlagCurrent = -0x4C81,      // u8: SetViFlag's stored value (VI library)
    kViChanged = -0x3790,          // u32: VI registers to rewrite at the next retrace
    kFrameBuffers = -0x46B0,       // u32[2]: the two XFBs
    kBackBufferIndex = -0x46A4,    // u32: 0/1
    kNextFrameBuffer = -0x46A8,    // u32: XFB to show at the next flip
};

constexpr uint32_t kBusClock = 0x800000F8; // OS globals: bus clock in Hz

enum : uint32_t {
    ApplyWidemode = 0x8003FA48,       // reconfigures VI/copy for 4:3 or 16:9
    OSSleepTicks = 0x804D5320,
    VIFlush = 0x804E9010,
    VISetNextFrameBuffer = 0x804E9130,
    VISetBlack = 0x804E91A0,
    SystemCall = 0x804777D0, // `sc; blr` (a no-op in this runtime)
};

// 0x804EB2F0, a VI library leaf the translator inlines: stores a VI setting and
// marks VI register group 0x20 for rewrite when it changes. Not matched to an
// SDK name yet.
void SetViFlag(uint32_t sda, uint8_t value) {
    if (Memory::Read8(sda + uint32_t(kViFlagCurrent)) == value) return;
    Memory::Write8(sda + uint32_t(kViFlagCurrent), value);
    Memory::Write32(sda + uint32_t(kViChanged), Memory::Read32(sda + uint32_t(kViChanged)) | 0x20);
}

struct Guest {
    CpuContext* cpu;
    uint32_t operator()(uint32_t fn, std::initializer_list<uint32_t> ints = {}) {
        uint32_t r = 3;
        for (uint32_t v : ints) cpu->gpr[r++] = v;
        InvokeIndirectCpu(fn, cpu);
        return cpu->gpr[3];
    }
};

} // namespace

extern "C" void MH3_FramePacer(CpuContext* ctx) {
    const CpuContext saved = *ctx;
    const uint32_t sda = saved.gpr[13];
    auto at = [sda](int32_t offset) { return sda + uint32_t(offset); };
    // Guest stack frame below the caller's (keep the back chain aligned).
    ctx->gpr[1] = (saved.gpr[1] - 0x20) & ~0xFu;
    Memory::Write32(ctx->gpr[1], saved.gpr[1]);
    Guest os{ctx};
    const uint32_t oneMs = (Memory::Read32(kBusClock) >> 2) / 1000; // timebase = bus / 4
    auto retraces = [&] { return int32_t(Memory::Read32(at(kRetracesThisFrame))); };
    // 0x8003F588, an inlined getter.
    auto budget = [&] { return int32_t(Memory::Read32(at(kRetracesPerFrame))); };

    while (retraces() < budget() - 1) os(OSSleepTicks, {0, oneMs});

    os(VISetNextFrameBuffer, {Memory::Read32(at(kNextFrameBuffer))});
    if (Memory::Read8(at(kBlackPending))) {
        os(VISetBlack, {0});
        Memory::Write8(at(kBlackPending), 0);
    }
    if (Memory::Read8(at(kWidemodeFlag))) {
        os(ApplyWidemode, {Memory::Read8(at(kWidemodeParam))});
        Memory::Write8(at(kWidemodeFlag), 0);
        Memory::Write8(at(kWidemodeParam), 0xFF);
    }
    os(VIFlush);
    SetViFlag(sda, Memory::Read8(at(kViFlag)));
    const uint32_t back = Memory::Read32(at(kBackBufferIndex)) ^ 1;
    Memory::Write32(at(kBackBufferIndex), back);
    Memory::Write32(at(kNextFrameBuffer), Memory::Read32(at(kFrameBuffers) + back * 4));
    os(SystemCall);

    while (retraces() < budget()) os(OSSleepTicks, {0, oneMs});
    Memory::Write32(at(kFrameCount), Memory::Read32(at(kFrameCount)) + 1);
    Memory::Write32(at(kRetracesThisFrame), 0);

    *ctx = saved;
}
REGISTER_NATIVE_FUNCTION_AS(0x800401A8, MH3_FramePacer, "FramePacer");
