// Ambient flutterer step, RMHP08. C reconstruction of 0x803BC584 (the step of
// the small ambient creatures that drift in a straight line and flap), decompiled
// from the disassembly.
//
// State 0 sets the creature up: a size (+0x1D4), a heading (+0x1E8 pitch,
// +0x1EC yaw) that turns the forward speed (+0x1C8 vector) and two random flap
// angles (+0x1D8, +0x1DC); then a random wait (+0x10). State 1, while the
// area is active (func_803BA700), counts the wait down (substate +9 = 0), then
// flies for a random 35..40 more ticks (substate 1):
//   the flap angles move by 4096 (22.5 deg) and scale the model's x and y by
//     size - sin * size / 2 (a negative sine counts as positive);
//   the position (+0x14) moves by the velocity;
//   creatures with +3 == 1 also circle: an orbit angle (+0x1F0) moves by
//     8192 and the model sits 5 units off the position along it;
// and then the MHchar at +0x38 is moved. States 2 and 3 free it.
//
// The angles and the position moved by a fixed amount per step and the waits
// counted steps, so at short steps the creatures flapped and flew faster and
// left sooner. The port moves them by the step's share and counts whole ticks.
// At the stock step it is the original.
#include "abi_bridge.h"
#include "game_step.h"
#include "guest_call.h"
#include "memory.h"
#include "port_check.h"

namespace {

enum : uint32_t {
    kActive = 0x01,     // u8: func_803BA700's answer
    kCircles = 0x03,    // u8: 1 = circles, 0 = the small kind
    kArea = 0x04,       // u8
    kKind = 0x05,       // u8: 0xFF = not drawn, 0x81 = always lit
    kState = 0x08,      // u8
    kSubstate = 0x09,   // u8
    kWait = 0x10,       // s32
    kPos = 0x14,        // VEC3
    kChar = 0x38,       // MHchar
    kModelPos = 0x3C,   // VEC3 (in the MHchar)
    kModelScale = 0x54, // VEC3 (in the MHchar)
    kLight = 0x6D,      // u8 (in the MHchar)
    kG3dWork = 0x144,
    kModel = 0x150,
    kVelocity = 0x1C8, // VEC3
    kSize = 0x1D4,     // f32
    kFlap0 = 0x1D8,    // u32 (u16 range)
    kFlap1 = 0x1DC,
    kPitch = 0x1E8, // u32
    kYaw = 0x1EC,   // u32
    kOrbit = 0x1F0, // u32 (u16 range)
};

constexpr uint32_t kWaits = 0x80795B60;      // s16[4] (r13 - 22400)
constexpr uint32_t kLightTable = 0x805F89E0; // u8 by kind
constexpr uint32_t kSpeedTable = 0x805F8AD0; // f32[4]
constexpr int32_t kSdaRoot = -17580;         // r13: _pRoot

enum : uint32_t {
    AreaActive = 0x803BA700,
    Free = 0x803BA1CC,
    Place = 0x803BA754,
    Random = 0x800CEF20,
    SinDegrees = 0x80059210,
    RotVecX = 0x8005101C,
    RotVecY = 0x800510C8,
    VecAdd = 0x800513DC,
    CharMove = 0x800E078C, // MHchar::move(unsigned short)
    RootDraw = 0x8007F130,
    PushG3dWork = 0x800D30BC,
};

void Copy3(uint32_t to, uint32_t from) {
    for (uint32_t i = 0; i < 12; i += 4)
        Memory::Write32(to + i, Memory::Read32(from + i));
}

// The flap scale of one axis: size - sin * (size / 2), with |sin|.
float FlapScale(GuestCall& call, uint32_t angle, float size) {
    call.Call<SinDegrees>({}, {360.0f * float(angle) / 65536.0f});
    float sin = call.F1();
    if (sin < 0.0f)
        sin = sin * -1.0f;
    return size - sin * (size * 0.5f);
}

} // namespace

static void AmbientFlutter(CpuContext* ctx) {
    GuestFrame frame{ctx, 0x50};
    GuestCall call{ctx};
    const uint32_t w = frame.Arg(3);
    auto random = [&] {
        return call.Call<Random>({0}) & 0xFFFF;
    };
    auto wait = [&] {
        return int32_t(int16_t(Memory::Read16(kWaits + 2 * (random() & 3))));
    };

    switch (Memory::Read8(w + kState)) {
    case 0: {
        if (!Memory::Read32(w + kModel)) {
            call.Call<Free>({w});
            return;
        }
        Memory::Write8(w + kState, 1);
        if (!Memory::Read8(w + kCircles)) {
            const float size = 2.0f * (0.5f + 0.1f * float(int32_t(random() % 6)));
            Memory::WriteFloat32(w + kSize, size);
            Memory::Write32(w + kPitch, (random() & 15) * 224 - 1792);
            Memory::Write32(w + kYaw, ((random() & 31) << 11) & 0xFFFF);
            Memory::WriteFloat32(w + kVelocity + 4, size * Memory::ReadFloat32(kSpeedTable + 4 * (random() & 3)));
        } else {
            Memory::WriteFloat32(w + kSize, 1.5f);
            Memory::Write32(w + kPitch, (random() & 15) * 768 - 6144);
            Memory::Write32(w + kYaw, ((random() & 31) << 11) & 0xFFFF);
            Memory::WriteFloat32(w + kVelocity + 4, 12.0f);
        }
        Memory::WriteFloat32(w + kVelocity + 8, 0.0f);
        Memory::WriteFloat32(w + kVelocity, 0.0f);
        Memory::Write32(w + kFlap0, ((random() & 31) << 11) & 0xFFFF);
        Memory::Write32(w + kFlap1, ((random() & 31) << 11) & 0xFFFF);
        call.Call<RotVecX>({w + kVelocity, Memory::Read32(w + kPitch)});
        call.Call<RotVecY>({w + kVelocity, Memory::Read32(w + kYaw)});
        Memory::Write32(w + kOrbit, 0);
        Memory::Write32(w + kWait, uint32_t(wait()));
        Memory::Write8(w + kSubstate, 0);
        call.Call<Place>({w});
        return;
    }
    case 1: {
        const uint8_t active = uint8_t(call.Call<AreaActive>({Memory::Read8(w + kArea)}));
        Memory::Write8(w + kActive, active);
        if (!active) {
            Memory::Write8(w + kState, 2);
            return;
        }
        const int32_t ticks = int32_t(GameStep::Ticks());
        int32_t flying = ticks; // ticks to count off the flight
        const uint8_t substate = Memory::Read8(w + kSubstate);
        if (substate == 0) {
            const int32_t left = int32_t(Memory::Read32(w + kWait)) - ticks;
            Memory::Write32(w + kWait, uint32_t(left));
            if (left > 0)
                return;
            Memory::Write8(w + kSubstate, 1);
            const int32_t base = wait();
            Memory::Write32(w + kWait, uint32_t(base + (Memory::Read8(w + kCircles) ? 35 : 40)));
            flying = 1; // the tick the wait ran out in
        } else if (substate != 1) {
            return;
        }
        const int32_t left = int32_t(Memory::Read32(w + kWait)) - flying;
        Memory::Write32(w + kWait, uint32_t(left));
        if (left <= 0) {
            Memory::Write8(w + kState, 2);
            return;
        }
        const uint8_t kind = Memory::Read8(w + kKind);
        if (kind != 0xFF) {
            const float size = Memory::ReadFloat32(w + kSize);
            const uint32_t flap0 = Memory::Read32(w + kFlap0), flap1 = Memory::Read32(w + kFlap1);
            const float scaleX = FlapScale(call, flap0, size);
            const float scaleY = FlapScale(call, flap1, size);
            const int32_t flap = GameStep::Share(4096);
            Memory::Write32(w + kFlap0, (flap0 + flap) & 0xFFFF);
            Memory::Write32(w + kFlap1, (flap1 + flap) & 0xFFFF);

            const float s = float(GameStep::Scale());
            for (uint32_t i = 0; i < 12; i += 4)
                Memory::WriteFloat32(w + kPos + i, Memory::ReadFloat32(w + kPos + i) + Memory::ReadFloat32(w + kVelocity + i) * s);

            if (Memory::Read8(w + kCircles) == 1) {
                const uint32_t orbit = (Memory::Read32(w + kOrbit) + GameStep::Share(8192)) & 0xFFFF;
                Memory::Write32(w + kOrbit, orbit);
                const uint32_t offset = frame[20], at = frame[8];
                Memory::WriteFloat32(offset, 5.0f);
                Memory::WriteFloat32(offset + 4, 0.0f);
                Memory::WriteFloat32(offset + 8, 0.0f);
                call.Call<RotVecY>({offset, orbit});
                call.Call<RotVecX>({offset, Memory::Read32(w + kPitch)});
                call.Call<RotVecY>({offset, Memory::Read32(w + kYaw)});
                call.Call<VecAdd>({at, w + kPos, offset});
                Copy3(w + kModelPos, at);
            } else {
                Copy3(w + kModelPos, w + kPos);
            }
            Memory::WriteFloat32(w + kModelScale, scaleX);
            Memory::WriteFloat32(w + kModelScale + 4, scaleY);
            Memory::WriteFloat32(w + kModelScale + 8, size);
            // (The original first adds the table entry to +0x6D, then overwrites it.)
            Memory::Write8(w + kLight, Memory::Read8(kLightTable + kind));
        }
        call.Call<CharMove>({w + kChar, 0});
        call.Call<RootDraw>({Memory::Read32(frame.Arg(13) + uint32_t(kSdaRoot)), Memory::Read32(w + kModel)});
        return;
    }
    case 2:
        call.Call<PushG3dWork>({Memory::Read32(w + kG3dWork)});
        Memory::Write8(w + kState, 3);
        [[fallthrough]];
    case 3:
        call.Call<Free>({w});
        return;
    default:
        return;
    }
}
extern "C" void func_803BC584(CpuContext* ctx);

extern "C" void MH3_AmbientFlutter(CpuContext* ctx) {
    PortCheck::Call(ctx, "AmbientFlutter", func_803BC584, AmbientFlutter);
}
REGISTER_NATIVE_FUNCTION_AS(0x803BC584, MH3_AmbientFlutter, "AmbientFlutter");
