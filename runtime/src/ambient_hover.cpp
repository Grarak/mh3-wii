// Ambient flyer hover, RMHP08. C reconstruction of 0x803BB570 (called by the
// ambient creature step 0x803BBDA4 before its MHchar move), decompiled from the
// disassembly.
//
// In states 12 and 16 the creature circles a centre point and bobs:
//   a u16 phase (+0x1A4) ping-pongs between 0 and 0x8000 by a rate (+0x1B4),
//     and at each end picks, one time in two, a height speed of +-2;
//   one tick in four the orbit angle (+0x1CC, u16) jumps by 4096 (22.5 deg),
//     one tick in 32 it picks a new random target (+0x20..+0x28);
//   x, z = centre (+0x1D0, +0x1D8) + 20 * (sin, cos) of the angle;
//   y (+0x18) moves by the height speed (+0x1C8), which turns back below
//     base + range / 3 and above base + range (+0x1D4, +0x1F4).
//
// All of it ran once per step, so at short steps the creature bobbed and
// circled faster (the height by 2 units a step). The port moves the phase by
// the step's share of the rate and the height by speed * scale, and rolls the
// per-tick chances once per whole tick of the step. At a bound the phase rests
// until the next whole tick, as the Wii's clamp does. At the stock step it is
// the original.
#include "abi_bridge.h"
#include "game_step.h"
#include "guest_call.h"
#include "memory.h"
#include "port_check.h"

#include <cmath>

namespace {

enum : uint32_t {
    kState = 0x05,  // u8
    kTarget = 0x20, // u32 x3
    kPosX = 0x14,   // f32
    kPosY = 0x18,
    kPosZ = 0x1C,
    kPhase = 0x1A4,       // u32 (u16 range)
    kPhaseMirror = 0x1B0, // u32: 0x10000 - phase
    kPhaseRate = 0x1B4,   // u16
    kRising = 0x1B6,      // u8: phase counting up
    kPhaseFlag = 0x1B7,   // u8, cleared every step
    kHeightSpeed = 0x1C8, // f32
    kAngle = 0x1CC,       // u32 (u16 range)
    kCentreX = 0x1D0,     // f32
    kBaseY = 0x1D4,       // f32
    kCentreZ = 0x1D8,     // f32
    kRangeY = 0x1F4,      // f32
};

constexpr int32_t kSdaTargetSource = -22408; // r13: u32 copied to +0x20

// The phase's way back from a bound. The Wii clamps it to the bound on the
// tick that crosses it, so it rests there until the next whole tick and only
// then turns. Of the time since the crossing (over / perTick ticks), the part
// after that tick boundary moves it back; at the stock step there is none.
uint32_t Back(uint32_t over, uint32_t perTick) {
    if (!perTick)
        return 0;
    const double past = double(over) / perTick, fraction = GameStep::Fraction();
    if (past < fraction)
        return 0;
    return uint32_t(perTick * (fraction + std::floor(past - fraction)));
}

enum : uint32_t {
    Random = 0x800CEF20, // ran_suu(long)
    SinDegrees = 0x80059210,
    CosDegrees = 0x803BB7EC, // CosFIdx(deg * 256 / 360)
};

} // namespace

static void AmbientHover(CpuContext* ctx) {
    GuestFrame frame{ctx, 0x30};
    GuestCall call{ctx};
    const uint32_t w = frame.Arg(3);
    const uint8_t state = Memory::Read8(w + kState);
    if (state != 12 && state != 16)
        return;
    auto random = [&] {
        return call(Random, {0}) & 0xFFFF;
    };

    const uint32_t perTick = Memory::Read16(w + kPhaseRate);
    const int32_t rate = GameStep::Share(int32_t(perTick));
    uint32_t phase = Memory::Read32(w + kPhase);
    if (!Memory::Read8(w + kRising)) {
        const uint32_t over = uint32_t(rate) - phase; // past 0, when it wraps
        phase = (phase - rate) & 0xFFFF;
        if (phase >= 0x8000) {
            phase = Back(over, perTick);
            Memory::Write8(w + kRising, 1);
            if (random() & 1)
                Memory::WriteFloat32(w + kHeightSpeed, 2.0f);
        }
    } else {
        const uint32_t over = phase + uint32_t(rate) - 0x8000; // past 0x8000, when it gets there
        phase = (phase + rate) & 0xFFFF;
        if (phase >= 0x8000) {
            phase = 0x8000 - Back(over, perTick);
            Memory::Write8(w + kRising, 0);
            if (random() & 1)
                Memory::WriteFloat32(w + kHeightSpeed, -2.0f);
        }
    }
    Memory::Write32(w + kPhase, phase);
    Memory::Write32(w + kPhaseMirror, (0x10000 - phase) & 0xFFFF);
    Memory::Write8(w + kPhaseFlag, 0);

    for (uint32_t tick = 0, ticks = GameStep::Ticks(); tick < ticks; ++tick) {
        if ((random() & 3) == 0)
            Memory::Write32(w + kAngle, (Memory::Read32(w + kAngle) + 4096) & 0xFFFF);
        if ((random() & 31) == 0) {
            Memory::Write32(w + kTarget, Memory::Read32(frame.Arg(13) + uint32_t(kSdaTargetSource)));
            Memory::Write32(w + kTarget + 4, random());
            Memory::Write32(w + kTarget + 8, 0);
        }
    }

    const float degrees = 360.0f * float(Memory::Read32(w + kAngle)) / 65536.0f;
    call(SinDegrees, {}, {degrees});
    const float sin = call.F1();
    call(CosDegrees, {}, {degrees});
    const float cos = call.F1();
    Memory::WriteFloat32(w + kPosX, Memory::ReadFloat32(w + kCentreX) + 20.0f * sin);
    Memory::WriteFloat32(w + kPosZ, Memory::ReadFloat32(w + kCentreZ) + 20.0f * cos);

    const float speed = Memory::ReadFloat32(w + kHeightSpeed);
    const float y = Memory::ReadFloat32(w + kPosY) + speed * float(GameStep::Scale());
    Memory::WriteFloat32(w + kPosY, y);
    if (y < Memory::ReadFloat32(w + kBaseY) + Memory::ReadFloat32(w + kRangeY) / 3.0f && speed < 0.0f)
        Memory::WriteFloat32(w + kHeightSpeed, speed * -1.0f);
    const float top = Memory::ReadFloat32(w + kBaseY) + Memory::ReadFloat32(w + kRangeY);
    const float now = Memory::ReadFloat32(w + kHeightSpeed);
    if (Memory::ReadFloat32(w + kPosY) > top && now > 0.0f)
        Memory::WriteFloat32(w + kHeightSpeed, now * -1.0f);
}
extern "C" void func_803BB570(CpuContext* ctx);

extern "C" void MH3_AmbientHover(CpuContext* ctx) {
    PortCheck::Call(ctx, "AmbientHover", func_803BB570, AmbientHover);
}
REGISTER_NATIVE_FUNCTION_AS(0x803BB570, MH3_AmbientHover, "AmbientHover");
