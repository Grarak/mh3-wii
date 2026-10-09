// Fish, RMHP08: the effect objects of 0x8033FF88 (kind 0x22 baits, the fish
// that swim around and take them), stepped once a step by the effect object
// update. Their timers, speeds and scale changes are tick sites
// (tick_sites.inc, "fish"); their turns go through one helper, decompiled
// from the disassembly:
//   0x8034027C (&angle, &target, rate): the target masked to u16 and stored
//     back, then the angle moved toward it by at most rate (the short way
//     round). The swim (0x803408B4, rate +0x44), the bait states (0x80340D14:
//     0x100 each step, 0x800 while hooked, 0xFFFF to face the bait at once)
//     and the line (0x803419F4, 0x800) call it once a step.
// The rate is per step. Here it is the step's share (GameStep::Share), so a
// fish turns at the same speed at any step length; a rate of 0x8000 or more
// always reaches the target and is kept. At the stock 1/30 s step this is the
// original.
#include "game_step.h"
#include "abi_bridge.h"
#include "ppc_runtime.h"

#include <cstdint>

extern "C" void func_8034027C(CpuContext* ctx);
extern "C" void MH3_FishTurnToward(CpuContext* ctx) {
    const int32_t rate = int32_t(ctx->gpr[5]);

    if (rate > 0 && rate < 0x8000)
        ctx->gpr[5] = uint32_t(GameStep::Share(rate));

    func_8034027C(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x8034027C, MH3_FishTurnToward, "FishTurnToward");
