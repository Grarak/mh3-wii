// Field camera input, RMHP08. C reconstructions of the parts of the field
// camera's update (0x802B5CBC) that turn it by stick, decompiled with Ghidra:
//   0x802B6864  CameraYawInput (camera, camera work, PLW): the right stick
//               (player commands 6/8) turns the camera's yaw target; else the
//               camera swings back behind the player
//   0x802B6D30  CameraPitchInput (camera, camera work, PLW): swimming, the
//               right stick (commands 2/4) tilts the pitch target; on land
//               flicks (commands 1/3) step the distance level
//   0x802BDC90  CameraAngleEase (s16 target, s16 angle, shift): the step of
//               `angle += (target - angle) >> shift`, the camera's angle eases
//
// The originals turn by a fixed amount a step, so at the VM's 500-700 steps a
// second the stick turned the camera 15-20 times as fast as on the Wii:
//   - yaw: 1035 angle units a step at full speed (843 or 747 in some modes),
//     reached over 5 steps (10 swimming) of holding, the ramp falling back by
//     1 a step once released;
//   - pitch while swimming: 560 (368) a step at full speed, over 20 steps,
//     held for 30 steps after the stick is let go before the camera levels;
//   - swinging back behind the player: an amount a step (0x802B6C30);
//   - the eases close 1/2^shift of the gap a step.
// Here the turns are GameStep::Share of the amount (the amount times the
// step's length in ticks), the ramps and the hold count whole ticks (a ramp
// starts at 1 on the first step held, as the original's first step does), and
// an ease closes 1 - (1 - 2^-shift)^s of the gap, dithered over steps as
// Share() is. At the stock 1/30 s step all of this is the original.
#include "game_step.h"
#include "guest_call.h"
#include "port_check.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace {

enum : uint32_t {
    kSystem = 0x8065AA60,  // system_w
    kCameraFrozen = 0x2A,  // u8 system_w: no camera input (event)
    kFlags = 0x49C,        // u8 camera: bit 0 inverts yaw, bit 1 pitch
    kPitchTarget = 0x64,   // s32 (s16 angle)
    kYawTarget = 0x68,
    kDistance = 0x7C,      // u8 distance level 0..4
    kSlowA = 0x88,         // u8: slower turn (843 yaw)
    kFollow = 0x9C,        // s8: swing-back mode (<0 none)
    kFollowing = 0x9E,     // u8: swung back this step
    kSnapped = 0x9F,       // u8: snapped behind the player this step
    kPitchRamp = 0xA0,     // s8 0..20
    kYawRamp = 0xA1,       // s8 0..5 (10 swimming)
    kManualPitch = 0xAA,   // u8: tilted by stick this step
    kPitchHold = 0xAB,     // u8 countdown after tilting (30)
    kYawSnap = 0xAD,       // u8, the snapped yaw (u16) at +0xAE
    kPitchSnap = 0xB0,     // u8, the snapped pitch (u16) at +0xB2
    kSlowB = 0xB5,         // u8: slower turn (747 yaw, 368 pitch)
    kTurned = 0x91,        // u8, cleared when the stick turns the camera
};

enum : uint32_t {
    kPlwState = 9,       // u8: 3 swimming
    kPlwPlayer = 8,      // u8 player index
    kPlwPitch = 0x54,    // u32 (u16 angle)
    kPlwYaw = 0x58,
    kPlwCommands = 0xDC, // u32 command bits (PlCommandBit)
};

enum : uint32_t {
    CameraUsable = 0x802BE2F0,  // (PLW) -> 1 when the stick may move the camera
    CameraMode = 0x802B7880,    // (camera work) -> 0 in the normal follow mode
    OptionConfig = 0x803BED04,  // get_option_cfg (player, option) -> u8
    TargetHeld = 0x8027A304,    // (PLW, 0) -> 1: swing behind while targeting
    SwingBack = 0x802B6C30,     // (f1 lo, f2 hi, f3 gain; r3 u16 gap) -> s16 turn this step
    StickTilted = 0x8026F96C,   // (PLW, stick) -> 0 when the stick is near the middle
    AimingUp = 0x802B79E4,      // (PLW) -> 1
    AimAllowed = 0x802BE0EC,    // () -> nonzero
};

constexpr uint32_t kSwingLo = 0x8079C9F0, kSwingHi = 0x8079C9F4, kSwingGain = 0x8079C9F8; // f32
constexpr uint32_t kFloatZero = 0x8079C998;                                                // f32 0.0
constexpr uint32_t kPitchRampSteps = 0x8079CA10;                                           // f32 20.0

bool Command(uint32_t plw, uint32_t bit) {
    return (Memory::Read32(plw + kPlwCommands) >> bit) & 1;
}

// A ramp of whole ticks: +Ticks() up to `top`, at least 1 on a step held.
void RampUp(uint32_t ramp, int32_t top) {
    const int32_t value = int8_t(Memory::Read8(ramp));

    if (value < top)
        Memory::Write8(ramp, uint8_t(std::clamp<int32_t>(value + int32_t(GameStep::Ticks()), 1, top)));
}

void RampDown(uint32_t ramp) {
    const int32_t value = int8_t(Memory::Read8(ramp));

    if (value > 0)
        Memory::Write8(ramp, uint8_t(value - std::min<int32_t>(value, int32_t(GameStep::Ticks()))));
}

float GuestFloat(uint32_t address) {
    return Memory::ReadFloat32(address);
}

// The camera faces the player's back: the yaw target snaps behind them.
void SnapBehind(uint32_t work, uint32_t plw) {
    const uint32_t yaw = (Memory::Read32(plw + kPlwYaw) + 0x8000) & 0xFFFF;
    Memory::Write32(work + kYawTarget, yaw);
    Memory::Write8(work + kYawRamp, 10);
    Memory::Write8(work + kYawSnap, 1);
    Memory::Write16(work + kYawSnap + 1, uint16_t(yaw));
}

} // namespace

static void CameraYawInputPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x40);
    GuestCall game{ctx};
    const uint32_t camera = frame.Arg(3), work = frame.Arg(4), plw = frame.Arg(5);

    if (Memory::Read8(kSystem + kCameraFrozen) != 0)
        return;

    uint32_t speed = 1035;

    if (Memory::Read8(work + kSlowA) != 0)
        speed = 843;
    else if (Memory::Read8(work + kSlowB) == 1)
        speed = 747;

    uint32_t following = 0, snapped = 0;
    uint32_t yaw = Memory::Read32(work + kYawTarget);

    if (Command(plw, 9) && (Command(plw, 6) || Command(plw, 8)) && game.Call<CameraUsable>({plw}) == 1) {
        // Turning by stick: the speed ramps up over 5 ticks held (10 swimming).
        const int32_t top = Memory::Read8(plw + kPlwState) == 3 ? 10 : 5;
        RampUp(work + kYawRamp, top);
        const float ramp = float(int8_t(Memory::Read8(work + kYawRamp)));
        const uint32_t amount = uint32_t(float(speed) * (ramp / float(top))) & 0xFFFF;
        uint32_t turn = uint32_t(GameStep::Share(int32_t(amount))) & 0xFFFF;

        if (Memory::Read8(camera + kFlags) & 1)
            turn = -turn & 0xFFFF;

        if (Command(plw, 6))
            yaw += turn;
        else if (Command(plw, 8))
            yaw -= turn;

        Memory::Write32(work + kYawTarget, yaw);
        Memory::Write8(work + kTurned, 0);
        Memory::Write8(work + kFollowing, 0);
        Memory::Write8(work + kSnapped, 0);
        return;
    }

    if (Memory::Read8(plw + kPlwState) == 3)
        Memory::Write8(work + kYawRamp, 0);
    else
        RampDown(work + kYawRamp);
    // The swing back behind the player: an amount a step, here its share.
    const auto swingBack = [&](uint32_t lo) {
        const uint32_t gap = (((Memory::Read32(plw + kPlwYaw) & 0xFFFF) ^ 0x8000) - Memory::Read32(work + kYawTarget)) & 0xFFFF;
        const int32_t turn = int32_t(game.Call<SwingBack>({gap}, {GuestFloat(lo), GuestFloat(kSwingHi), GuestFloat(kSwingGain)}));
        Memory::Write32(work + kYawTarget, Memory::Read32(work + kYawTarget) + uint32_t(GameStep::Share(turn)));
    };

    const int32_t follow = int8_t(Memory::Read8(work + kFollow));

    if (game.Call<CameraMode>({work}) == 0 && follow >= 0 && (game.Call<OptionConfig>({Memory::Read8(plw + kPlwPlayer), 11}) & 0xFF) != 1) {
        following = 1;

        if (Memory::Read8(work + kFollowing) == 0 && follow == 1)
            SnapBehind(work, plw);

        if (follow == 0 || follow == 2 || follow == 3) {
            swingBack(kSwingLo);

            if (follow != 0)
                following = 0;
        } else if (follow == 1) {
            swingBack(kFloatZero);
        }
    } else if (game.Call<TargetHeld>({plw, 0}) == 1) {
        if (Memory::Read8(work + kSnapped) == 0)
            SnapBehind(work, plw);

        swingBack(kFloatZero);
    } else if (Command(plw, 0) && game.Call<CameraUsable>({plw}) == 1) {
        SnapBehind(work, plw);
        snapped = 1;
    }

    Memory::Write8(work + kFollowing, uint8_t(following));
    Memory::Write8(work + kSnapped, uint8_t(snapped));
}

static void CameraPitchInputPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x40);
    GuestCall game{ctx};
    const uint32_t camera = frame.Arg(3), work = frame.Arg(4), plw = frame.Arg(5);

    if (Memory::Read8(kSystem + kCameraFrozen) != 0 || !Command(plw, 9) || game.Call<CameraUsable>({plw}) == 0)
        return;

    const bool swapped = Memory::Read8(camera + kFlags) & 2;

    if (Memory::Read8(plw + kPlwState) != 3) {
        // On land the stick's flicks (edges) step the distance level.
        const uint32_t closer = swapped ? 3 : 1, farther = swapped ? 1 : 3;
        const uint32_t level = Memory::Read8(work + kDistance);

        if (Command(plw, closer))
            Memory::Write8(work + kDistance, uint8_t(std::min<uint32_t>((level + 1) & 0xFF, 4)));
        else if (Command(plw, farther) && level != 0)
            Memory::Write8(work + kDistance, uint8_t(level - 1));

        Memory::Write8(work + kPitchRamp, 0);
        Memory::Write8(work + kPitchSnap, 0);
        return;
    }

    // Swimming: the stick tilts the pitch target, ramping up over 20 ticks.
    uint32_t speed = 560;

    if (Memory::Read8(work + kSlowA) == 0 && Memory::Read8(work + kSlowB) == 1)
        speed = 368;

    uint32_t pitch = Memory::Read32(work + kPitchTarget);
    Memory::Write8(work + kManualPitch, 0);

    if (Command(plw, 2) || Command(plw, 4)) {
        RampUp(work + kPitchRamp, 20);
        const float ramp = float(int8_t(Memory::Read8(work + kPitchRamp)));
        const int32_t amount = int32_t(float(speed) * (ramp / GuestFloat(kPitchRampSteps)));
        const uint32_t tilt = uint32_t(GameStep::Share(amount)) & 0xFFFF;
        const uint32_t up = swapped ? 4 : 2, down = swapped ? 2 : 4;

        if (Command(plw, up)) {
            pitch += tilt;

            if (int16_t(pitch) > 16020)
                pitch = 16020;
        } else if (Command(plw, down)) {
            pitch -= tilt;

            if (int16_t(pitch) < -16020)
                pitch = uint32_t(-16020);
        }

        Memory::Write32(work + kPitchTarget, pitch);
        Memory::Write8(work + kManualPitch, 1);
        Memory::Write8(work + kPitchHold, 30);
    } else {
        if (Command(plw, 0) && (game.Call<StickTilted>({plw, 0}) & 0xFF) == 0 &&
            (game.Call<AimingUp>({plw}) != 1 || game.Call<AimAllowed>() != 0)) {
            const uint32_t level = -Memory::Read32(plw + kPlwPitch) & 0xFFFF;
            Memory::Write32(work + kPitchTarget, level);
            Memory::Write8(work + kPitchRamp, 10);
            Memory::Write8(work + kPitchSnap, 1);
            Memory::Write16(work + kPitchSnap + 2, uint16_t(level));
        }

        Memory::Write8(work + kPitchRamp, 0);
        const uint32_t hold = Memory::Read8(work + kPitchHold);

        if (hold != 0)
            Memory::Write8(work + kPitchHold, uint8_t(hold - std::min(hold, GameStep::Ticks())));
    }

    Memory::Write8(work + kDistance, 2);
}

// `angle += CameraAngleEase(target, angle, shift)` closes 1/2^shift of the
// gap each step; over a step of s ticks it closes 1 - (1 - 2^-shift)^s of it.
// The whole units are dithered as GameStep::Share does, so a small gap still
// closes at high step rates.
static void CameraAngleEasePort(CpuContext* ctx) {
    const int32_t gap = int16_t(uint16_t(ctx->gpr[3] - ctx->gpr[4]));
    const uint32_t shift = ctx->gpr[5] & 0x3F;
    const double s = GameStep::Scale();

    if (s == 1.0) {
        ctx->gpr[3] = uint32_t(gap >> std::min<uint32_t>(shift, 31));
        return;
    }

    const double closed = 1.0 - std::pow(1.0 - std::ldexp(1.0, -int32_t(std::min<uint32_t>(shift, 31))), s);
    const double perTick = gap * closed / s;
    const double elapsed = GameStep::Elapsed();
    ctx->gpr[3] = uint32_t(int32_t(std::lround(perTick * elapsed) - std::lround(perTick * (elapsed - s))));
}

extern "C" void func_802B6864(CpuContext* ctx);
extern "C" void MH3_CameraYawInput(CpuContext* ctx) {
    PortCheck::Call(ctx, "CameraYawInput", func_802B6864, CameraYawInputPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x802B6864, MH3_CameraYawInput, "CameraYawInput");

extern "C" void func_802B6D30(CpuContext* ctx);
extern "C" void MH3_CameraPitchInput(CpuContext* ctx) {
    PortCheck::Call(ctx, "CameraPitchInput", func_802B6D30, CameraPitchInputPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x802B6D30, MH3_CameraPitchInput, "CameraPitchInput");

extern "C" void func_802BDC90(CpuContext* ctx);
extern "C" void MH3_CameraAngleEase(CpuContext* ctx) {
    PortCheck::RunPure(ctx, "CameraAngleEase", PortCheck::Returns::Gpr3, func_802BDC90, CameraAngleEasePort);
}
REGISTER_NATIVE_FUNCTION_AS(0x802BDC90, MH3_CameraAngleEase, "CameraAngleEase");
