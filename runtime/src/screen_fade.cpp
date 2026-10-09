// Screen fades, RMHP08. C reconstruction of TickFades (0x800581A4), called
// once per step by the main loop after DrawFadeOverlay (0x800582EC) has drawn
// them. Four fades, each a column of the word arrays at 0x8066D108 (4 bytes
// from one fade to the next):
//   +0   level (the high byte, 0..255) over the colour (the low 24 bits)
//   +16  state: 1 fading up, 2 fading down, 3 up and holding, 0 idle
//   +48  steps left
//   +80  target level
//   +96  level change per step (negative when fading down)
// A fading level moves by its change each step until it reaches the target,
// where it stops (and its steps left drop to 0). When the steps left run out
// the fade ends: one fading up holds (3), one fading down goes idle. Nothing
// runs during a soft reset.
//
// Here the level moves by the change's share of this step (GameStep::Share)
// and the steps left count ticks, so fades take the same time at any step
// length. At the stock 1/30 s step this is the original.
#include "game_step.h"
#include "memory.h"
#include "port_check.h"

namespace {

constexpr uint32_t kResetting = 0x8065AA65; // u8: soft reset in progress (reset_fade.cpp)
constexpr uint32_t kFades = 0x8066D108;
constexpr uint32_t kFadeCount = 4;
enum : uint32_t {
    kLevel = 0,
    kState = 16,
    kStepsLeft = 48,
    kTarget = 80,
    kChange = 96
};
enum : int32_t {
    kIdle = 0,
    kFadingUp = 1,
    kFadingDown = 2,
    kHolding = 3
};

void TickFadesPort(CpuContext*) {
    if (Memory::Read8(kResetting))
        return;
    const int32_t ticks = int32_t(GameStep::Ticks());
    for (uint32_t fade = kFades; fade < kFades + 4 * kFadeCount; fade += 4) {
        const int32_t state = int32_t(Memory::Read32(fade + kState));
        if (state != kFadingUp && state != kFadingDown)
            continue;
        const bool up = state == kFadingUp;
        const int32_t target = int32_t(Memory::Read32(fade + kTarget));
        int32_t level = Memory::Read8(fade + kLevel) + GameStep::Share(int32_t(Memory::Read32(fade + kChange)));
        int32_t stepsLeft = int32_t(Memory::Read32(fade + kStepsLeft));
        if (up ? level >= target : level <= target) {
            level = target;
            stepsLeft = 0;
        }
        stepsLeft -= ticks;
        if (stepsLeft < 0) {
            stepsLeft = 0;
            Memory::Write32(fade + kState, uint32_t(up ? kHolding : kIdle));
        }
        Memory::Write32(fade + kStepsLeft, uint32_t(stepsLeft));
        Memory::Write8(fade + kLevel, uint8_t(level));
    }
}

} // namespace

extern "C" void func_800581A4(CpuContext* ctx);
extern "C" void MH3_TickFades(CpuContext* ctx) {
    PortCheck::Call(ctx, "TickFades", func_800581A4, TickFadesPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x800581A4, MH3_TickFades, "TickFades");
