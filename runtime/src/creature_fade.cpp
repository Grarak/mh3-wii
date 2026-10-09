// Volcano creature fade, RMHP08. C reconstruction of 0x803822D8 (creature,
// part, frames), a leaf called by the volcano creature step 0x80382374 for
// each part; decompiled from the disassembly.
//
// Each part has a fade value (f32 +0x338 + 4 * part) that runs between 0 and
// 1 by 1 / frames a step, up while its direction byte (+0x328 + part) is 0
// and down while it is 1; the byte flips at either end. Frames <= 0 leaves it.
// The amount was taken per step, so at short steps the parts pulsed faster:
// here it is taken over the step's length. At the stock step it is the
// original.
#include "abi_bridge.h"
#include "game_step.h"
#include "memory.h"
#include "port_check.h"

#include <cstdint>

namespace {
enum : uint32_t {
    kFalling = 0x328,
    kFade = 0x338
};
} // namespace

static void CreatureFade(CpuContext* ctx) {
    const float frames = float(ctx->fpr[1].d);

    if (frames <= 0.0f)
        return;

    const uint32_t creature = ctx->gpr[3], part = ctx->gpr[4] & 0xFF;
    const uint32_t falling = creature + kFalling + part, fade = creature + kFade + 4 * part;
    const float amount = 1.0f / frames * float(GameStep::Scale());

    if (!Memory::Read8(falling)) {
        const float v = Memory::ReadFloat32(fade) + amount;
        Memory::WriteFloat32(fade, v >= 1.0f ? 1.0f : v);

        if (v >= 1.0f)
            Memory::Write8(falling, 1);
    } else {
        const float v = Memory::ReadFloat32(fade) - amount;
        Memory::WriteFloat32(fade, v <= 0.0f ? 0.0f : v);

        if (v <= 0.0f)
            Memory::Write8(falling, 0);
    }
}

extern "C" void func_803822D8(CpuContext* ctx);

extern "C" void MH3_CreatureFade(CpuContext* ctx) {
    PortCheck::Call(ctx, "CreatureFade", func_803822D8, CreatureFade);
}
REGISTER_NATIVE_FUNCTION_AS(0x803822D8, MH3_CreatureFade, "CreatureFade");
