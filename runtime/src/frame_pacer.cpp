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
#include "field_jump.h"
#include "game_step.h"
#include "memory.h"
#include "timebase_contract.h"
#include "video_interface.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <initializer_list>

#ifdef MH3_HAS_AURORA
void ReportHostSpeed(double ticks, double nominalHz);
#endif

void PumpDevices(CpuContext* cpu); // os_sleep.cpp

namespace {

// Small-data globals (offsets from r13; lower-case names are mh3.sel exports).
enum : int32_t {
    kBlackPending = -0x8000,      // u8: clear VI black on the next flip
    kWidemodeParam = -0x7FFF,     // u8 set_widemode_param: 4:3/16:9 mode to apply (0xFF = none)
    kWidemodeFlag = -0x4690,      // u8 set_widemode_flag: a widescreen switch is pending
    kFrameCount = -0x45B8,        // u32: frames paced so far
    kRetracesThisFrame = -0x4680, // s32: retraces since the last flip (reset only)
    kRetracesPerFrame = -0x4684,  // s32: frame budget in retraces
    kViFlag = -0x467C,            // u8: value passed to SetViFlag
    kViFlagCurrent = -0x4C81,     // u8: SetViFlag's stored value (VI library)
    kViChanged = -0x3790,         // u32: VI registers to rewrite at the next retrace
    kFrameBuffers = -0x46B0,      // u32[2]: the two XFBs
    kBackBufferIndex = -0x46A4,   // u32: 0/1
    kNextFrameBuffer = -0x46A8,   // u32: XFB to show at the next flip
    kDriveStatus = -0x4688,       // s32: DVDGetDriveStatus at the last retrace
};

using Clock = std::chrono::steady_clock;

// Deadline of the current frame (fixed step) or start of the current step
// (variable step), and game time so far in retrace periods (stats, input).
Clock::time_point frameEnd;
bool started = false;
double pacedTicks = 0;
uint64_t pacedFrames = 0;

// Longest variable step; a longer stall (loading, debugger) is not caught up.
constexpr auto kMaxStep = std::chrono::milliseconds(100);

// Game time a slow host may still catch up on; older debt is dropped.
constexpr auto kMaxDebt = std::chrono::milliseconds(100);

enum : uint32_t {
    ApplyWidemode = 0x8003FA48, // reconfigures VI/copy for 4:3 or 16:9
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
    if (Memory::Read8(sda + uint32_t(kViFlagCurrent)) == value)
        return;

    Memory::Write8(sda + uint32_t(kViFlagCurrent), value);
    Memory::Write32(sda + uint32_t(kViChanged), Memory::Read32(sda + uint32_t(kViChanged)) | 0x20);
}

struct Guest {
    CpuContext* cpu;
    uint32_t operator()(uint32_t fn, std::initializer_list<uint32_t> ints = {}) {
        uint32_t r = 3;

        for (uint32_t v : ints)
            cpu->gpr[r++] = v;

        InvokeIndirectCpu(fn, cpu);
        return cpu->gpr[3];
    }
};

} // namespace

extern "C" void MH3_FramePacer(CpuContext* ctx) {
    const CpuContext saved = *ctx;
    const uint32_t sda = saved.gpr[13];
    auto at = [sda](int32_t offset) {
        return sda + uint32_t(offset);
    };

    // Guest stack frame below the caller's (keep the back chain aligned).
    ctx->gpr[1] = (saved.gpr[1] - 0x20) & ~0xFu;
    Memory::Write32(ctx->gpr[1], saved.gpr[1]);
    Guest os{ctx};

    // OSSleepTicks keeps pumping audio, alarms, IOS and VI callbacks while waiting.
    auto sleepUntil = [&](Clock::time_point t) {
        const auto now = Clock::now();

        if (now >= t)
            return;

        const uint64_t ticks = TimeBaseContract::NanosecondsToTicks(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(t - now).count()));
        os(OSSleepTicks, {uint32_t(ticks >> 32), uint32_t(ticks)});
    };

    const auto period = std::chrono::nanoseconds(VideoRetracePeriodNs());

    // Each step advances the game by the host time the previous one took, with
    // no fixed rate.
    bool variableStep = true;
#ifdef MH3_DEVELOPMENT
    // MH3_VARIABLE_STEP=0: the game's own fixed steps (its retrace budget);
    // MH3_VCNT=n forces that budget to n retraces.
    static const int32_t forcedVcnt = [] {
        const char* v = std::getenv("MH3_VCNT");
        return v ? std::atoi(v) : 0;
    }();
    static const bool variable = [] {
        const char* v = std::getenv("MH3_VARIABLE_STEP");
        return !(v && v[0] == '0');
    }();
    variableStep = variable;

    if (forcedVcnt > 0)
        Memory::Write32(at(kRetracesPerFrame), uint32_t(forcedVcnt));
#endif
    // 0x8003F588, an inlined getter. SetSystemVcnt clamps it to >= 1.
    const int32_t budget = std::max<int32_t>(1, int32_t(Memory::Read32(at(kRetracesPerFrame))));
    const auto now = Clock::now();

    if (!started)
        frameEnd = now - budget * period;

    started = true;
    GameStep::SetVariable(variableStep);
    double stepTicks; // game time of the step that just ran, in retraces
    if (variableStep) {
        const auto step = std::min<Clock::duration>(now - frameEnd, kMaxStep);
        frameEnd = now;
        stepTicks = std::chrono::duration<double>(step) / period;

        // The next step is assumed to take as long as this one.
        double next = std::max(stepTicks, 1e-3) / 2;
#ifdef MH3_DEVELOPMENT
        // MH3_STEP_SCALE=s (debugging): every step is s ticks long, whatever
        // time it took.
        static const double forcedScale = [] {
            const char* v = std::getenv("MH3_STEP_SCALE");
            return v ? std::atof(v) : 0.0;
        }();

        if (forcedScale > 0)
            next = forcedScale;
#endif
        GameStep::Begin(next);

        // No wait: only deliver the device work due (audio, alarms, IOS, VI).
        const CpuContext frame = *ctx;
        PumpDevices(ctx);
        *ctx = frame;
    } else {
        frameEnd += budget * period;

        if (now - frameEnd > kMaxDebt)
            frameEnd = now; // too far behind: drop the debt
        stepTicks = budget;
        GameStep::Begin(budget / 2.0);

        // Flip one retrace before the end, as the original does.
        sleepUntil(frameEnd - period);
    }

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

    if (!variableStep)
        sleepUntil(frameEnd);

    pacedTicks += stepTicks;
    ++pacedFrames;

    // Input record/playback is indexed by game time in 30 Hz frames (2 retraces),
    // so a recording replays in real time at any logic rate.
    SetInputFrame(uint64_t(pacedTicks / 2));
    FieldJumpFrame(uint64_t(pacedTicks / 2));
#ifdef MH3_DEVELOPMENT
    // MH3_RAM_DUMP=first,count: write MEM1 then MEM2 to mh3-ram-<frame>.bin after each of
    // `count` frames starting at paced frame `first` (offline state diffing).
    static const auto ramDump = [] {
        std::pair<uint64_t, uint64_t> range{0, 0};

        if (const char* v = std::getenv("MH3_RAM_DUMP")) {
            char* end = nullptr;
            range.first = std::strtoull(v, &end, 10);
            range.second = *end == ',' ? std::strtoull(end + 1, nullptr, 10) : 1;
        }

        return range;
    }();

    // MH3_TRACE_PLAYER: log player 0's position and motion frame every paced
    // frame, against game time in 30 Hz frames (logic-rate comparisons).
    static const bool tracePlayer = std::getenv("MH3_TRACE_PLAYER") != nullptr;

    if (tracePlayer) {
        const uint32_t table = Memory::Read32(0x8065AB04); // move work table
        const uint32_t plw = table ? Memory::Read32(table + 16 + 2 * 4) : 0;

        if (plw && Memory::Read8(plw)) {
            const uint32_t chr = Memory::Read32(plw + 0x13C); // MHchar
            std::fprintf(stderr, "MH3 player: t=%.1f pos %.2f %.2f %.2f motion %u frame %.2f rate %.2f\n", pacedTicks / 2, Memory::ReadFloat32(plw + 0x3C), Memory::ReadFloat32(plw + 0x40),
                         Memory::ReadFloat32(plw + 0x44), chr ? Memory::Read16(chr + 0x54) : 0, chr ? Memory::ReadFloat32(chr + 0x78) : 0.0f, chr ? Memory::ReadFloat32(chr + 0x60) : 0.0f);
        }
    }

    // MH3_TRACE_INPUT: once per tick of game time, the input as the game read
    // it and what it moved: the field camera's yaw and pitch (current and
    // target), player 0's position and facing, and how many steps since the
    // last line reported a key repeat (comparing step modes by game time).
    static const bool traceInput = std::getenv("MH3_TRACE_INPUT") != nullptr;

    if (traceInput) {
        constexpr uint32_t kPad = 0x8065B7D0;    // Psw, channel 0 (0x350 bytes a channel)
        constexpr uint32_t kCamera = 0x806BDC60; // field camera work
        static int64_t lastTick = -1;
        static uint32_t steps = 0, padRepeats = 0, keyRepeats = 0;
        ++steps;
        padRepeats += Memory::Read16(kPad + 0xF8) != 0;  // Classic repeat buttons (0x8065B8C8)
        keyRepeats += Memory::Read16(kPad + 0x2D4) != 0; // game key repeat (0x8065BAA4)
        const int64_t tick = int64_t(GameStep::Elapsed());

        if (tick != lastTick) {
            lastTick = tick;
            const uint32_t table = Memory::Read32(0x8065AB04);
            const uint32_t plw = table ? Memory::Read32(table + 16 + 2 * 4) : 0;
            const bool player = plw && Memory::Read8(plw);
            std::fprintf(stderr,
                         "MH3 input: game %.3f s %.3f stick %d %d %d %d keys %04x camera yaw %u -> %u pitch %d -> %d player %.2f %.2f %.2f facing %u steps %u repeats %u %u "
                         "commands %08x stick-camera %u ramp %d distance %u state %u\n",
                         GameStep::Elapsed(), GameStep::Scale(), int16_t(Memory::Read16(kPad + 0x8E)), int16_t(Memory::Read16(kPad + 0x90)), int16_t(Memory::Read16(kPad + 0x92)),
                         int16_t(Memory::Read16(kPad + 0x94)), Memory::Read16(kPad + 0x2C0), Memory::Read32(kCamera + 0x70) & 0xFFFF, Memory::Read32(kCamera + 0x68) & 0xFFFF,
                         int16_t(Memory::Read32(kCamera + 0x6C)), int16_t(Memory::Read32(kCamera + 0x64)), player ? Memory::ReadFloat32(plw + 0x3C) : 0.0f, player ? Memory::ReadFloat32(plw + 0x40) : 0.0f,
                         player ? Memory::ReadFloat32(plw + 0x44) : 0.0f, player ? Memory::Read32(plw + 0x58) & 0xFFFF : 0, steps, padRepeats, keyRepeats,
                         player ? Memory::Read32(plw + 0xDC) : 0, player ? Memory::Read8(plw + 0x5C5) : 0, int8_t(Memory::Read8(kCamera + 0xA1)), Memory::Read8(kCamera + 0x7C),
                         player ? Memory::Read8(plw + 9) : 0);
            steps = padRepeats = keyRepeats = 0;
        }
    }

    if (ramDump.first && pacedFrames >= ramDump.first && pacedFrames < ramDump.first + ramDump.second) {
        const std::string name = "mh3-ram-" + std::to_string(pacedFrames) + ".bin";

        if (FILE* f = std::fopen(name.c_str(), "wb")) {
            std::fwrite(Memory::GetPointer(Memory::kMem1CachedBase, Memory::kMem1Size), 1, Memory::kMem1Size, f);
            std::fwrite(Memory::GetPointer(Memory::kMem2CachedBase, Memory::kMem2Size), 1, Memory::kMem2Size, f);
            std::fclose(f);
        }
    }
#endif
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
    auto at = [sda](int32_t offset) {
        return sda + uint32_t(offset);
    };

    Memory::Write32(at(kRetracesThisFrame), Memory::Read32(at(kRetracesThisFrame)) + 1);
    ctx->gpr[1] = (saved.gpr[1] - 0x10) & ~0xFu;
    Memory::Write32(ctx->gpr[1], saved.gpr[1]);
    InvokeIndirectCpu(DVDGetDriveStatus, ctx);
    Memory::Write32(at(kDriveStatus), ctx->gpr[3]);
    *ctx = saved;
}
REGISTER_NATIVE_FUNCTION_AS(0x8003F590, MH3_CountRetrace, "CountRetraceAlarm");
REGISTER_NATIVE_FUNCTION_AS(0x8003F5C8, MH3_CountRetrace, "CountRetraceCallback");
