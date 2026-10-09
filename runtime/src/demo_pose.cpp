// Event-demo poses, RMHP08. C reconstructions of the two enemy helpers that
// place a monster in an in-engine cutscene from keyframe tables (decompiled
// with Ghidra): DemoPose (0x8014620C) sets the position and the angles,
// DemoPoseAxes (0x80146308) chosen axes of them. The demo actions call them
// once per step with the event demo clock (EventDemoGetSteps 0x803C48A4, a
// tick site) or with a step count of their own, and the tables are keyed in
// frames of half a step (frame = 2 * steps).
//
// The clock counts whole ticks, so at more than 30 steps a second the monster
// held its pose between ticks and jumped at each, while the camera path,
// which camera_script.cpp evaluates at the fractional frame, moved on every
// step. The ports read the tables at the step's game time: the steps plus
// the fraction of a tick past the last whole one. At the stock 1/30 s step
// that fraction is 0 and these are the originals
// (MH3_PORT_CHECK=DemoPose,DemoPoseAxes MH3_VARIABLE_STEP=0).
#include "abi_bridge.h"
#include "game_step.h"
#include "guest_call.h"
#include "memory.h"
#include "port_check.h"

#include <cstdint>

namespace {

// Enemy work fields.
enum : uint32_t {
    kPosition = 0x188,     // VEC3
    kPrevPosition = 0x194, // VEC3
    kAngleX = 0x1BC,       // s32 angle units
    kAngleY = 0x1C0,
    kAngleZ = 0x1C4,
};

enum : uint32_t {
    KeyData = 0x80052364,     // getKeyData (keys; f1 frame) -> f1
    KeyData2 = 0x800523D4,    // (keys, float* a, float* b; f1 frame)
    KeyData3 = 0x8005246C,    // getKeyData3 (keys, float* a, float* b, float* c; f1 frame)
    SetPosition = 0x801460BC, // (work; f1-f3): the position and the previous position
    SetAngles = 0x80146170,   // (work; f1-f3 degrees)
    ToAngle = 0x801460F8,     // (f1 degrees) -> s32 angle units
    Vec3Copy = 0x80041EA4,    // (to, from)
};

// Which values a DemoPoseAxes table gives (r7 for the position, r8 the angles).
enum Axes : uint8_t {
    kNone = 0,
    kX = 1,
    kY = 2,
    kXY = 3,
    kZ = 4,
    kXZ = 5,
    kYZ = 6,
    kXYZ = 7,
};

// The tables' frame at the step's game time: 2 * steps at the stock step.
double Frame(int16_t steps) {
    return float(2.0 * (double(steps) + GameStep::Fraction()));
}

float ReadF(uint32_t address) {
    return Memory::ReadFloat32(address);
}

void DemoPosePort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x50);
    GuestCall game{ctx};
    const uint32_t work = frame.Arg(3);
    const int16_t steps = int16_t(frame.Arg(4));
    const uint32_t positionKeys = frame.Arg(5), angleKeys = frame.Arg(6);
    const double at = Frame(steps);
    const uint32_t a = frame[0x10], b = frame[0x14], c = frame[0x18];

    if (positionKeys != 0) {
        game.Call<KeyData3>({positionKeys, a, b, c}, {at});
        game.Call<SetPosition>({work}, {ReadF(a), ReadF(b), ReadF(c)});
    }

    if (angleKeys != 0) {
        game.Call<KeyData3>({angleKeys, a, b, c}, {at});
        game.Call<SetAngles>({work}, {ReadF(a), ReadF(b), ReadF(c)});
    }
}

void DemoPoseAxesPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x50);
    GuestCall game{ctx};
    const uint32_t work = frame.Arg(3);
    const int16_t steps = int16_t(frame.Arg(4));
    const uint32_t positionKeys = frame.Arg(5), angleKeys = frame.Arg(6);
    const uint8_t positionAxes = uint8_t(frame.Arg(7)), angleAxes = uint8_t(frame.Arg(8));
    const double at = Frame(steps);
    const uint32_t a = frame[0x10], b = frame[0x14], c = frame[0x18];

    const auto one = [&](uint32_t keys) {
        game.Call<KeyData>({keys}, {at});
        return game.F1();
    };

    const auto two = [&](uint32_t keys) { game.Call<KeyData2>({keys, a, b}, {at}); };

    const auto angle = [&](float degrees) { return game.Call<ToAngle>({}, {degrees}); };

    if (positionKeys != 0 && positionAxes != kNone) {
        switch (positionAxes) {
        case kX:
            Memory::WriteFloat32(work + kPosition, one(positionKeys));
            break;
        case kY:
            Memory::WriteFloat32(work + kPosition + 4, one(positionKeys));
            break;
        case kZ:
            Memory::WriteFloat32(work + kPosition + 8, one(positionKeys));
            break;
        case kXYZ:
            game.Call<KeyData3>({positionKeys, a, b, c}, {at});
            Memory::WriteFloat32(work + kPosition, ReadF(a));
            Memory::WriteFloat32(work + kPosition + 4, ReadF(b));
            Memory::WriteFloat32(work + kPosition + 8, ReadF(c));
            break;
        default:
            two(positionKeys);

            if (positionAxes == kXY) {
                Memory::WriteFloat32(work + kPosition, ReadF(a));
                Memory::WriteFloat32(work + kPosition + 4, ReadF(b));
            } else if (positionAxes == kXZ) {
                Memory::WriteFloat32(work + kPosition, ReadF(a));
                Memory::WriteFloat32(work + kPosition + 8, ReadF(b));
            } else if (positionAxes == kYZ) {
                Memory::WriteFloat32(work + kPosition + 4, ReadF(a));
                Memory::WriteFloat32(work + kPosition + 8, ReadF(b));
            }

            break;
        }

        game.Call<Vec3Copy>({work + kPrevPosition, work + kPosition});
    }

    if (angleKeys == 0 || angleAxes == kNone)
        return;

    switch (angleAxes) {
    case kX:
        Memory::Write32(work + kAngleX, angle(one(angleKeys)));
        break;
    case kY:
        Memory::Write32(work + kAngleY, angle(one(angleKeys)));
        break;
    case kZ:
        Memory::Write32(work + kAngleZ, angle(one(angleKeys)));
        break;
    case kXYZ:
        game.Call<KeyData3>({angleKeys, a, b, c}, {at});
        game.Call<SetAngles>({work}, {ReadF(a), ReadF(b), ReadF(c)});
        break;
    default:
        two(angleKeys);

        if (angleAxes == kXY) {
            Memory::Write32(work + kAngleX, angle(ReadF(a)));
            Memory::Write32(work + kAngleY, angle(ReadF(b)));
        } else if (angleAxes == kXZ) {
            Memory::Write32(work + kAngleX, angle(ReadF(a)));
            Memory::Write32(work + kAngleZ, angle(ReadF(b)));
        } else if (angleAxes == kYZ) {
            Memory::Write32(work + kAngleY, angle(ReadF(a)));
            Memory::Write32(work + kAngleZ, angle(ReadF(b)));
        }

        break;
    }
}

} // namespace

extern "C" void func_8014620C(CpuContext* ctx);
extern "C" void MH3_DemoPose(CpuContext* ctx) {
    PortCheck::Call(ctx, "DemoPose", func_8014620C, DemoPosePort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8014620C, MH3_DemoPose, "DemoPose");

extern "C" void func_80146308(CpuContext* ctx);
extern "C" void MH3_DemoPoseAxes(CpuContext* ctx) {
    PortCheck::Call(ctx, "DemoPoseAxes", func_80146308, DemoPoseAxesPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80146308, MH3_DemoPoseAxes, "DemoPoseAxes");
