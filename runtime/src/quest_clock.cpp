// Quest clock, RMHP08. C reconstruction of the quest time-limit countdown
// (0x803A9014), called once per step by the quest step 0x803B0228.
//
// The time left is kept as a double count of frames (quest work +0x6A60) and
// copied, truncated, to the s32 at +0x24 that the HUD and the time-up check
// read. Each call subtracts the real time since the previous call (OSGetTime),
// converted to frames with the frame period in microseconds at +0x6A58, but
// never less than one frame. At variable step the countdown uses the game time
// of the step instead: a whole frame per call would run the clock several times
// too fast at high frame rates.
//
// Cheats (cheats.h): the infinite time codes stop the countdown (Argon's made
// the limit test pass, Skiller's subtracted 0) and run the time out on their
// buttons (Skiller's stored 0; Argon's wrote a byte, quest work +0x2440, that
// is 0 throughout a quest here); Instant flex runs it out under a minute
// (the final clamp at 1800 frames instead of 0).
#include "cheats.h"
#include "game_step.h"
#include "guest_call.h"
#include "port_check.h"

#include <cstdint>

namespace {

enum : uint32_t {
    QuestWork = 0x803A880C, // () -> quest work, 0 without a quest
    OSGetTime = 0x804D53D0,
};

enum : uint32_t {
    kTimeLeft = 0x24,        // s32 frames, for the HUD and the time-up check
    kNow = 0x6A48,           // f64 OSGetTime at this call
    kPrevious = 0x6A50,      // f64 OSGetTime at the previous call
    kFramePeriod = 0x6A58,   // f64 microseconds per frame
    kTimeLeftExact = 0x6A60, // f64 frames
};

constexpr uint32_t kFrameRate = 0x8065B4D0; // f32
constexpr uint32_t kBusClock = 0x800000F8;
constexpr float kNoLimitMinutes = 99.0f;

double ReadF64(uint32_t a) {
    return Memory::ReadFloat64(a);
}

// 0x803A9014(running): counts the time limit down; `running` 0 holds it.
void QuestClockTick(CpuContext* ctx) {
    const int32_t running = int32_t(ctx->gpr[3]);
    GuestFrame frame(ctx, 0x20);
    GuestCall game{ctx};
    const uint32_t work = game.Call<QuestWork>();

    if (!work)
        return;
    // 99 minutes or more: no time limit.
    const float left = float(int32_t(Memory::Read32(work + kTimeLeft)));

    if (left >= kNoLimitMinutes * (60.0f * Memory::ReadFloat32(kFrameRate)))
        return;

    const bool stopped = Cheats::On(Cheats::Id::ArgonInfiniteTime) || Cheats::On(Cheats::Id::SkillerInfiniteTime);

    if (stopped && Cheats::QuestEndPressed()) {
        Memory::WriteFloat64(work + kTimeLeftExact, 0.0);
        Memory::Write32(work + kTimeLeft, 0);
        return;
    }

    if (Cheats::On(Cheats::Id::ArgonInfiniteTime))
        return;

    Memory::WriteFloat64(work + kPrevious, ReadF64(work + kNow));
    game.Call<OSGetTime>();
    const auto now = int64_t(uint64_t(ctx->gpr[3]) << 32 | ctx->gpr[4]);
    Memory::WriteFloat64(work + kNow, double(now));
    const double previous = ReadF64(work + kPrevious);

    if (previous > 0.0) {
        double frames;

        if (GameStep::Variable()) {
            frames = GameStep::Scale() / 30.0 * 1e6 / ReadF64(work + kFramePeriod);
        } else {
            const double timerPerTick = double((Memory::Read32(kBusClock) >> 2) / 125000u);
            frames = (ReadF64(work + kNow) - previous) * 8000.0 / timerPerTick / 1000.0 / ReadF64(work + kFramePeriod);

            if (frames < 1.0)
                frames = 1.0;
        }

        if (running == 0 || stopped)
            frames = 0.0;

        Memory::WriteFloat64(work + kTimeLeftExact, ReadF64(work + kTimeLeftExact) - frames);
    } else if (running == 1 && !stopped) {
        Memory::WriteFloat64(work + kTimeLeftExact, ReadF64(work + kTimeLeftExact) - 1.0);
    }

    if (int32_t(Memory::Read32(work + kTimeLeft)) > 0) {
        int32_t frames = int32_t(ReadF64(work + kTimeLeftExact));

        if (frames < (Cheats::On(Cheats::Id::InstantFlex) ? 1800 : 0))
            frames = 0;

        Memory::Write32(work + kTimeLeft, uint32_t(frames));
    }
}

} // namespace

extern "C" void func_803A9014(CpuContext* ctx);

extern "C" void MH3_QuestClockTick(CpuContext* ctx) {
    PortCheck::Call(ctx, "QuestClockTick", func_803A9014, QuestClockTick);
}
REGISTER_NATIVE_FUNCTION_AS(0x803A9014, MH3_QuestClockTick, "QuestClockTick");
