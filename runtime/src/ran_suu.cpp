// The game's random numbers, RMHP08 (ran_suu 0x800CEF20 and the seed setter
// 0x800CEF00), ported natively. Four u16 seeds at system_w+0x18
// (0x8065AA78): 0 effects and sounds, 1 monsters, the task loop and the quest
// reward equipment (talismans, shards), 2 and 3 the ones an online host
// shares. Each draw is seed * 176 mod 65363 (a zero seed counts as 1), so a
// stream never leaves its cycle of the multiplier: twelve cycles of 5400
// seeds, two of 200 (multiples of 163) and three of 54 (multiples of 401).
// Both are precise natives: translated callers call them directly.
#include "ran_suu.h"
#include "port_check.h"
#include "abi_bridge.h"
#include "memory.h"

#include <cstdint>

namespace {

constexpr uint32_t kSeeds = 0x8065AA78; // system_w+0x18: u16 per stream

[[maybe_unused]] void RanSuuPort(CpuContext* cpu) {
    cpu->gpr[3] = RanSuu(cpu->gpr[3]);
}

// 0x800CEF00: every stream to the low byte of `value`.
void SeedAll(uint32_t value) {
    for (uint32_t stream = 0; stream < 4; ++stream)
        Memory::Write16(kSeeds + 2 * stream, uint16_t(value & 0xFF));
}

[[maybe_unused]] void SeedAllPort(CpuContext* cpu) {
    SeedAll(cpu->gpr[3]);
}

} // namespace

uint32_t RanSuu(uint32_t stream) {
    const uint32_t address = kSeeds + 2 * stream;
    const uint32_t seed = Memory::Read16(address);
    const uint32_t next = (seed ? seed : 1) * 176 % 65363;

    Memory::Write16(address, uint16_t(next));
    return next;
}

extern "C" void func_800CEF20(CpuContext* ctx);
extern "C" void MH3_RanSuu(CpuContext* ctx) {
#ifdef MH3_DEVELOPMENT
    if (PortCheck::RunPrecise("ran_suu", ctx->gpr[1], func_800CEF20, RanSuuPort)) [[unlikely]]
        return;
#endif
    ctx->gpr[3] = RanSuu(ctx->gpr[3]);
}
REGISTER_NATIVE_FUNCTION_AS(0x800CEF20, MH3_RanSuu, "ran_suu");

extern "C" void func_800CEF00(CpuContext* ctx);
extern "C" void MH3_RandSeedAll(CpuContext* ctx) {
#ifdef MH3_DEVELOPMENT
    if (PortCheck::RunPrecise("RandSeedAll", ctx->gpr[1], func_800CEF00, SeedAllPort)) [[unlikely]]
        return;
#endif
    SeedAll(ctx->gpr[3]);
}
REGISTER_NATIVE_FUNCTION_AS(0x800CEF00, MH3_RandSeedAll, "RandSeedAll");
