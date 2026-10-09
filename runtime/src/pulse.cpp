// Shared pulse, RMHP08. C reconstruction of the pulse update (0x802D9F18,
// called once per step via 0x802D9F08 from 0x801FC814 and 0x8028CDAC),
// decompiled with Ghidra (scripts/dol2elf.py -> ELF import). Blinking UI reads
// the value through 0x802D9F0C.
//
// A phase counts steps 0..39 and the value is 0.5 * (1 + sin(phase * pi/20)),
// clamped to [0, 1]: one pulse per 40 steps. Here the phase counts ticks of
// game time (whole ticks in the byte, the tick clock's fraction on top), so it
// pulses at the same rate and smoothly at any step length. At the stock 1/30 s
// step the fraction is 0 and this is the original.
#include "game_step.h"
#include "port_check.h"
#include "guest_call.h"

#include <algorithm>

namespace {
constexpr uint32_t kPhase = 0x80797038; // u8: r13-17064
constexpr uint32_t kValue = 0x8079703C; // f32
constexpr uint32_t kPeriod = 40;
constexpr uint32_t SinRad = 0x800AB6BC; // nw4r::math::SinRad (f1 radians) -> f1

// sda2 constants (r2-12840..-12828).
constexpr uint32_t kHalf = 0x8079CD38, kOne = 0x8079CD3C, kRadPerStep = 0x8079CD40, kZero = 0x8079CD44;
} // namespace

static void PulseUpdatePort(CpuContext* ctx) {
    const CpuContext saved = *ctx;
    GuestCall game{ctx};

    // The original: phase + 1, back to 0 past 39.
    const uint32_t phase = (Memory::Read8(kPhase) + GameStep::Ticks()) % kPeriod;
    Memory::Write8(kPhase, uint8_t(phase));
    const float at = float(double(phase) + GameStep::Fraction());
    game.Call<SinRad>({}, {Memory::ReadFloat32(kRadPerStep) * at});
    float value = Memory::ReadFloat32(kHalf) * (Memory::ReadFloat32(kOne) + game.F1());
    value = std::min(std::max(value, Memory::ReadFloat32(kZero)), Memory::ReadFloat32(kOne));
    Memory::WriteFloat32(kValue, value);
    *ctx = saved;
}

extern "C" void func_802D9F18(CpuContext* ctx);
extern "C" void MH3_PulseUpdate(CpuContext* ctx) {
    PortCheck::Call(ctx, "PulseUpdate", func_802D9F18, PulseUpdatePort);
}
REGISTER_NATIVE_FUNCTION_AS(0x802D9F18, MH3_PulseUpdate, "PulseUpdate");
