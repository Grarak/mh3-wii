// Flying enemy glide, RMHP08. C reconstruction of 0x80135064 (enemy, flags,
// motion table), the step of a flying monster gliding toward its target
// (+0x36C), called by the flight AI (0x80149DD0, ...); decompiled from the
// disassembly.
//
// Each step it leans (+0x464, EnemyBlendToward) by the angle left to the
// target, turns the yaw (+0x1C0) toward it by at most a rate that grows with
// the lean (128 + 1664 * (|lean| - 0.1)), rolls (+0x1C4) toward ten times that
// rate (at most 0x3000) by 0x280, and while leaning hard brakes the forward
// speed (+0x318) by 1.7 * (|lean| - 0.1). The pitch (+0x1BC) turns by at most
// 128 (flag 1 off). With the glide motion playing (table +0x10) the speed
// gains 0.8 (1.6 with flag 2) below a cap (table +0x24); below another (+0x28)
// the other motion starts. +0x786 counts passes of the target (the angle left
// is then 0); returns whether it passed.
//
// All of it ran once per step. The port takes each amount over this step's
// length: the speed changes times the scale, the yaw and roll steps their
// GameStep::Share (EnemyBlendToward and EnemyTurnToward scale themselves). At
// the stock step it is the original.
#include "abi_bridge.h"
#include "game_step.h"
#include "guest_call.h"
#include "memory.h"
#include "port_check.h"

#include <cmath>
#include <cstdint>

namespace {

enum : uint32_t {
    kPosition = 0x188, // VEC3
    kPitch = 0x1BC,    // u32 (u16 angle)
    kYaw = 0x1C0,
    kRoll = 0x1C4,
    kSpeed = 0x318,    // f32 forward speed (velocity z)
    kTarget = 0x36C,   // VEC3
    kLean = 0x464,     // f32
    kGliding = 0x784,  // u8
    kPassed = 0x786,   // u8
    kPassFrom = 0x788, // VEC3
};

// Motion table (r5).
enum : uint32_t {
    kTurnMotion = 0x00,   // u16, then u32 x3 of its start
    kOtherStart = 0x14,   // u32 x3
    kGlideMotion = 0x10,  // u16
    kSpeedMotions = 0x20, // u16 x2
    kSpeedCap = 0x24,     // f32
    kSlowCap = 0x28,      // f32
};

enum : uint32_t {
    kNoPitch = 1,
    kFastGain = 2,
    kNoMotionSpeed = 0x20
};

enum : uint32_t {
    VecDot = 0x80052278,
    VecYaw = 0x80050B08,   // calcVecAng3
    VecPitch = 0x80050BAC, // calcVecAngX
    BlendToward = 0x8012F8C4,
    TurnToward = 0x80133E14,
    PitchSetup = 0x80133CA0,
    MotionNo = 0x8012F960, // em_get_mot_no
    MotionEnded = 0x8012F9A0,
    StartMotion = 0x8012F568,
    StartMotionDone = 0x8012F874,
    MotionSpeed = 0x8012F950,
    MotionSpeedSet = 0x8012F838,
};

float F(uint32_t a) {
    return Memory::ReadFloat32(a);
}

void SetF(uint32_t a, float v) {
    Memory::WriteFloat32(a, v);
}

void Sub(uint32_t out, uint32_t a, uint32_t b) {
    for (uint32_t k = 0; k < 12; k += 4)
        SetF(out + k, F(a + k) - F(b + k));
}

// fctiwz: toward zero, saturating.
int32_t Truncate(double v) {
    if (std::isnan(v))
        return INT32_MIN;

    if (v >= 2147483648.0)
        return INT32_MAX;

    if (v < -2147483648.0)
        return INT32_MIN;

    return int32_t(v);
}

} // namespace

static void EnemyGlide(CpuContext* ctx) {
    uint32_t result;
    {
        GuestFrame frame{ctx, 0x60};
        GuestCall call{ctx};
        const uint32_t e = frame.Arg(3), flags = frame.Arg(4), table = frame.Arg(5);
        const float scale = float(GameStep::Scale());
        const uint32_t toTarget = frame[44], fromPass = frame[32];

        uint32_t turn = frame.Arg(31); // kept as the caller left it past the second pass
        Memory::Write8(e + kGliding, 1);
        Sub(toTarget, e + kTarget, e + kPosition);
        const uint8_t passed = Memory::Read8(e + kPassed);

        if (passed == 0) {
            Sub(fromPass, e + kTarget, e + kPassFrom);
            call.Call<VecDot>({toTarget, fromPass});

            if (call.F1() < 0.0f) {
                Memory::Write8(e + kPassed, passed + 1);
                turn = 0;
            } else {
                turn = ((call.Call<VecYaw>({toTarget}) & 0xFFFF) - Memory::Read32(e + kYaw)) & 0xFFFF;
            }
        } else if (passed == 1) {
            turn = 0;
        }

        float bend = 0.0f;

        if (turn != 0)
            bend = 360.0f * float(int16_t(turn)) / 65536.0f / 90.0f;

        call.Call<BlendToward>({e}, {bend, 0.05f});

        const float lean = std::fabs(F(e + kLean));
        uint32_t rate = 128, roll = 0;

        if (!(lean <= 0.1f)) {
            const float over = lean - 0.1f;
            rate = uint32_t(Truncate(128.0f + 1664.0f * over));
            SetF(e + kSpeed, F(e + kSpeed) - 1.7f * over * scale);
            const int32_t tilt = int32_t(rate & 0xFFFF) * 10;
            roll = tilt > 0x3000 ? 0x3000 : uint32_t(tilt) & 0xFFFF;

            if (F(e + kLean) > 0.0f)
                roll = (0u - roll) & 0xFFFF;
        }

        if (!(flags & kNoPitch)) {
            call.Call<PitchSetup>({e});
            const uint32_t goal = call.Call<VecPitch>({toTarget}) & 0xFFFF;
            const uint32_t pitch = call.Call<TurnToward>({goal, Memory::Read32(e + kPitch) & 0xFFFF, 128});
            Memory::Write32(e + kPitch, pitch & 0xFFFF);
        }

        const uint32_t step = uint32_t(GameStep::Share(int32_t(rate & 0xFFFF)));
        const uint32_t yaw = Memory::Read32(e + kYaw);

        if (F(e + kLean) < 0.0f)
            Memory::Write32(e + kYaw, int32_t(turn) < int32_t(0x10000 - step) ? yaw - step : yaw + turn);
        else
            Memory::Write32(e + kYaw, turn > step ? yaw + step : yaw + turn);

        const uint32_t rollStep = uint32_t(GameStep::Share(0x280));
        const uint32_t now = (Memory::Read32(e + kRoll) + 0x8000) & 0xFFFF;
        const uint32_t goalBiased = roll + 0x8000, goal = goalBiased & 0xFFFF;
        uint32_t next;

        if (now < goal) {
            next = now + rollStep;

            if ((next & 0xFFFF) > goal)
                next = goalBiased;
        } else {
            next = now - rollStep;

            if ((next & 0xFFFF) < goal)
                next = goalBiased;
        }

        Memory::Write32(e + kRoll, (next & 0xFFFF) - 0x8000);

        const uint16_t glide = Memory::Read16(table + kGlideMotion);

        if (glide == uint16_t(call.Call<MotionNo>({e}))) {
            if (call.Call<MotionEnded>({e}) == 1) {
                call.Call<StartMotion>({e, Memory::Read16(table + kTurnMotion), Memory::Read32(table + 4), Memory::Read32(table + 8), Memory::Read32(table + 12)});
                call.Call<StartMotionDone>({e});
            }

            const float speed = F(e + kSpeed);

            if (speed < F(table + kSpeedCap))
                SetF(e + kSpeed, speed + (flags & kFastGain ? 1.6f : 0.8f) * scale);
        } else if (F(e + kSpeed) <= F(table + kSlowCap)) {
            call.Call<StartMotion>({e, glide, Memory::Read32(table + kOtherStart), Memory::Read32(table + kOtherStart + 4), Memory::Read32(table + kOtherStart + 8)});
            call.Call<StartMotionDone>({e});
        }

        if (!(flags & kNoMotionSpeed) && Memory::Read16(table + kTurnMotion) == uint16_t(call.Call<MotionNo>({e}))) {
            call.Call<MotionSpeed>({e});
            const uint32_t speed = uint32_t(Truncate(ctx->fpr[1].d));
            call.Call<MotionSpeedSet>({e, Memory::Read16(table + kSpeedMotions), Memory::Read16(table + kSpeedMotions + 2), speed}, {F(e + kLean)});
        }

        result = Memory::Read8(e + kPassed) != 0;
    }

    ctx->gpr[3] = result;
}

extern "C" void func_80135064(CpuContext* ctx);

extern "C" void MH3_EnemyGlide(CpuContext* ctx) {
    PortCheck::Call(ctx, "EnemyGlide", func_80135064, EnemyGlide);
}
REGISTER_NATIVE_FUNCTION_AS(0x80135064, MH3_EnemyGlide, "EnemyGlide");
