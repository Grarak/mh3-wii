// Desert object group shuttle, RMHP08. C reconstruction of 0x80368DCC,
// decompiled from the disassembly. Called by the group step 0x80369F48 as
// (object, slot, target set, random) and returns 1 when a trip is over.
//
// The object (+0x38: the group) keeps per slot a member pointer (+0x04 + 4i),
// a step count (+0x40 + 4i), a state byte (+0x60 + i) and a speed index byte
// (+0x35 + i) into u16 speeds (+0x6E + 2j). The member's z (+0x0C) moves out
// and back in six states:
//   0: speed = 20 (+ a random 0..4);
//   1: count up, z -= count / 2 until that reaches the speed, then cruise for
//      500 / speed steps (random: less) plus the XZ distance to the target
//      (0x805EE670 + 12 * (set + index)) over the speed;
//   2: z -= speed, count down to 0;
//   3: count up, z -= speed - count / 2 down to 0, pick the next speed;
//   4-6: the same with z += (6 ends the trip: state 0, returns 1).
// All of it ran once per step, so at short steps the members moved faster.
// The port counts GameStep::Ticks() and moves z by the step's share of the
// same speed; the tests keep the unscaled speed. At the stock step it is the
// original.
#include "abi_bridge.h"
#include "game_step.h"
#include "guest_call.h"
#include "memory.h"
#include "port_check.h"

#include <cmath>
#include <cstdint>

namespace {

enum : uint32_t {
    kGroup = 0x38,         // object: the group
    kMember = 0x04,        // group, u32 per slot
    kSpeedIndex = 0x35,    // group, u8 per slot
    kCount = 0x40,         // group, s32 per slot
    kState = 0x60,         // group, u8 per slot
    kSpeed = 0x6E,         // group, u16 per speed index
    kMemberZ = 0x0C,       // member, f32
    kTargets = 0x805EE670, // Vec per target
};

enum : uint32_t {
    Rand = 0x800CEF20,
    VecToMhVec3 = 0x8005001C,
    DistanceXZ = 0x80050FE4,
};

constexpr float kBaseSpeed = 20.0f, kCruiseDistance = 500.0f, kAccel = 0.5f;

// fctiwz: toward zero, saturating (the speed can still be 0 on the first call).
int32_t Truncate(float v) {
    if (std::isnan(v))
        return INT32_MIN;
    if (v >= 2147483648.0f)
        return INT32_MAX;
    if (v < -2147483648.0f)
        return INT32_MIN;
    return int32_t(v);
}

} // namespace

static void GroupShuttle(CpuContext* ctx) {
    uint32_t result = 0;
    {
        GuestFrame frame{ctx, 0x60};
        GuestCall call{ctx};
        const uint32_t slot = frame.Arg(4), set = frame.Arg(5) & 0xFF;
        const uint32_t group = Memory::Read32(frame.Arg(3) + kGroup);
        const uint32_t speedAt = group + kSpeed + 2 * Memory::Read8(group + slot + kSpeedIndex);
        const float speed = float(Memory::Read16(speedAt));

        int32_t extra = 0, cruise = Truncate(kCruiseDistance / speed);
        if (frame.Arg(6)) {
            extra = int32_t(call.Call<Rand>({1}) & 0xFFFF) % 5;
            const int32_t roll = int32_t(call.Call<Rand>({1}) & 0xFFFF);
            cruise = cruise ? roll % cruise : roll; // divw by 0 leaves 0, so roll - 0
        }

        const uint32_t stateAt = group + slot + kState, countAt = group + 4 * slot + kCount;
        const uint32_t member = Memory::Read32(group + 4 * slot + kMember);
        const uint8_t state = Memory::Read8(stateAt);
        const int32_t ticks = int32_t(GameStep::Ticks());
        const float scale = float(GameStep::Scale());
        const float sign = state >= 4 ? 1.0f : -1.0f;
        auto move = [&](float amount) {
            const float z = Memory::ReadFloat32(member + kMemberZ);
            Memory::WriteFloat32(member + kMemberZ, sign > 0 ? z + amount * scale : z - amount * scale);
        };
        auto nextSpeed = [&] {
            Memory::Write16(speedAt, uint16_t(Truncate(kBaseSpeed + float(extra))));
        };

        switch (state) {
        case 0:
            Memory::Write8(stateAt, 1);
            nextSpeed();
            break;
        case 1:
        case 4: { // speed up
            const int32_t count = int32_t(Memory::Read32(countAt)) + ticks;
            Memory::Write32(countAt, uint32_t(count));
            const float step = kAccel * float(count);
            move(step);
            if (step >= speed) {
                Memory::Write8(stateAt, state + 1);
                const uint32_t target = frame[8];
                call.Call<VecToMhVec3>({target, kTargets + 12 * (set + Memory::Read8(group + slot + kSpeedIndex))});
                call.Call<DistanceXZ>({member + 4, target});
                Memory::Write32(countAt, uint32_t(Truncate(float(cruise) + call.F1() / speed)));
            }
            break;
        }
        case 2:
        case 5: { // cruise
            move(speed);
            const int32_t count = int32_t(Memory::Read32(countAt)) - ticks;
            Memory::Write32(countAt, uint32_t(count));
            if (count < 1) {
                Memory::Write8(stateAt, state + 1);
                Memory::Write32(countAt, 0);
            }
            break;
        }
        case 3:
        case 6: { // slow down
            const int32_t count = int32_t(Memory::Read32(countAt)) + ticks;
            Memory::Write32(countAt, uint32_t(count));
            const float step = speed - kAccel * float(count);
            move(step);
            if (step <= 0.0f) {
                Memory::Write32(countAt, 0);
                if (state == 3) {
                    Memory::Write8(stateAt, 4);
                    nextSpeed();
                } else {
                    Memory::Write8(stateAt, 0);
                    result = 1;
                }
            }
            break;
        }
        default:
            break;
        }
    }
    ctx->gpr[3] = result;
}
extern "C" void func_80368DCC(CpuContext* ctx);

extern "C" void MH3_GroupShuttle(CpuContext* ctx) {
    PortCheck::Call(ctx, "GroupShuttle", func_80368DCC, GroupShuttle);
}
REGISTER_NATIVE_FUNCTION_AS(0x80368DCC, MH3_GroupShuttle, "GroupShuttle");
