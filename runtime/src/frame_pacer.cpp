// Frame pacer, RMHP08. C reconstruction of the main loop's end-of-frame wait
// (0x800401A8, called by the main loop 0x8003F278 after the display copy),
// decompiled with Ghidra (scripts/dol2elf.py -> ELF import).
//
// The game runs one logic/render frame per `retracesPerFrame` VI retraces. Its
// logic is fixed-step: timers are seconds * 30.0 (0x8065B4D0, 60 / vcnt at
// init) counted down once per frame, so this wait is the only thing that ties
// game speed to real time. The original counts VI retraces (its VI callbacks
// increment `retracesThisFrame`): it waits until one retrace short of the
// budget, flips the framebuffer (VISetNextFrameBuffer + VIFlush), then waits
// out the rest of the budget and resets the count.
//
// Here the budget is measured on the host clock instead: frame N ends at
// start + N * budget * retrace period. Emulated retraces are only delivered at
// cooperative pump points, so counting them loses time whenever a frame runs
// long; a clock deadline instead lets late frames run back to back until the
// game catches up (capped, so a long stall doesn't cause a burst of frames).
#include "abi_bridge.h"
#include "controller_host.h"
#include "memory.h"
#include "timebase_contract.h"
#include "video_interface.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <initializer_list>

#ifdef MH3_HAS_AURORA
void ReportHostSpeed(uint64_t ticks, double nominalHz);
#endif

namespace {

// Small-data globals (offsets from r13; lower-case names are mh3.sel exports).
enum : int32_t {
    kBlackPending = -0x8000,       // u8: clear VI black on the next flip
    kWidemodeParam = -0x7FFF,      // u8 set_widemode_param: 4:3/16:9 mode to apply (0xFF = none)
    kWidemodeFlag = -0x4690,       // u8 set_widemode_flag: a widescreen switch is pending
    kFrameCount = -0x45B8,         // u32: frames paced so far
    kRetracesThisFrame = -0x4680,  // s32: retraces since the last flip (reset only)
    kRetracesPerFrame = -0x4684,   // s32: frame budget in retraces
    kViFlag = -0x467C,             // u8: value passed to SetViFlag
    kViFlagCurrent = -0x4C81,      // u8: SetViFlag's stored value (VI library)
    kViChanged = -0x3790,          // u32: VI registers to rewrite at the next retrace
    kFrameBuffers = -0x46B0,       // u32[2]: the two XFBs
    kBackBufferIndex = -0x46A4,    // u32: 0/1
    kNextFrameBuffer = -0x46A8,    // u32: XFB to show at the next flip
    kDriveStatus = -0x4688,        // s32: DVDGetDriveStatus at the last retrace
};

using Clock = std::chrono::steady_clock;
// Deadline of the current frame, and total paced retrace periods (for stats).
Clock::time_point frameEnd;
bool started = false;
uint64_t pacedTicks = 0;
uint64_t pacedFrames = 0;
// Game time a slow host may still catch up on; older debt is dropped.
constexpr auto kMaxDebt = std::chrono::milliseconds(100);

enum : uint32_t {
    ApplyWidemode = 0x8003FA48,       // reconfigures VI/copy for 4:3 or 16:9
    OSSleepTicks = 0x804D5320,
    VIFlush = 0x804E9010,
    VISetNextFrameBuffer = 0x804E9130,
    VISetBlack = 0x804E91A0,
    SystemCall = 0x804777D0, // `sc; blr` (a no-op in this runtime)
    DVDGetDriveStatus = 0x804AAD90,
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
    // OSSleepTicks keeps pumping audio, alarms, IOS and VI callbacks while waiting.
    auto sleepUntil = [&](Clock::time_point t) {
        const auto now = Clock::now();
        if (now >= t) return;
        const uint64_t ticks = TimeBaseContract::NanosecondsToTicks(
            uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(t - now).count()));
        os(OSSleepTicks, {uint32_t(ticks >> 32), uint32_t(ticks)});
    };
#ifdef MH3_DEVELOPMENT
    // MH3_VCNT=n: force the game's retrace budget (and its fps float, 60 / n).
    static const int32_t forcedVcnt = [] {
        const char* v = std::getenv("MH3_VCNT");
        return v ? std::atoi(v) : 0;
    }();
    if (forcedVcnt > 0) {
        Memory::Write32(at(kRetracesPerFrame), uint32_t(forcedVcnt));
        Memory::WriteFloat32(0x8065B4D0, 60.0f / float(forcedVcnt));
    }
#endif
    // 0x8003F588, an inlined getter. SetSystemVcnt clamps it to >= 1.
    const int32_t budget = std::max<int32_t>(1, int32_t(Memory::Read32(at(kRetracesPerFrame))));
    const auto period = std::chrono::nanoseconds(VideoRetracePeriodNs());
    const auto now = Clock::now();
    if (!started) frameEnd = now;
    started = true;
    frameEnd += budget * period;
    if (now - frameEnd > kMaxDebt) frameEnd = now; // too far behind: drop the debt

    // Flip one retrace before the end, as the original does.
    sleepUntil(frameEnd - period);

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

    sleepUntil(frameEnd);
    pacedTicks += uint64_t(budget);
    SetInputFrame(++pacedFrames); // input record/playback is indexed by paced frames
#ifdef MH3_HAS_AURORA
    ReportHostSpeed(pacedTicks, 1e9 / double(period.count()));
#endif
    Memory::Write32(at(kFrameCount), Memory::Read32(at(kFrameCount)) + 1);
    Memory::Write32(at(kRetracesThisFrame), 0);

    *ctx = saved;
}
REGISTER_NATIVE_FUNCTION_AS(0x800401A8, MH3_FramePacer, "FramePacer");

// The two sources of `retracesThisFrame`, installed by 0x8003F5F0: a VI
// post-retrace callback (0x8003F5C8), or an OSSetPeriodicAlarm handler
// (0x8003F590) when the game is set to count on the OS timer instead. Both
// bump the count (0x8003F5B8, inlined) and sample DVDGetDriveStatus. The count
// now only feeds the game's own reset in the pacer; it no longer paces frames.
extern "C" void MH3_CountRetrace(CpuContext* ctx) {
    const CpuContext saved = *ctx;
    const uint32_t sda = saved.gpr[13];
    auto at = [sda](int32_t offset) { return sda + uint32_t(offset); };
    Memory::Write32(at(kRetracesThisFrame), Memory::Read32(at(kRetracesThisFrame)) + 1);
    ctx->gpr[1] = (saved.gpr[1] - 0x10) & ~0xFu;
    Memory::Write32(ctx->gpr[1], saved.gpr[1]);
    InvokeIndirectCpu(DVDGetDriveStatus, ctx);
    Memory::Write32(at(kDriveStatus), ctx->gpr[3]);
    *ctx = saved;
}
REGISTER_NATIVE_FUNCTION_AS(0x8003F590, MH3_CountRetrace, "CountRetraceAlarm");
REGISTER_NATIVE_FUNCTION_AS(0x8003F5C8, MH3_CountRetrace, "CountRetraceCallback");
