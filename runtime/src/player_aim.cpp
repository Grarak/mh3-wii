// Player aiming, RMHP08. C reconstruction of 0x8027A5E0 PlAimInput (PLW, u16
// motion, u8 mode), decompiled with Ghidra. The field actions that aim (the
// gunner's stance 0x80249944, motions 7/8, 63/66, 1001, item use 0x80251470)
// call it once per step while their motion plays: the d-pad (player commands
// 0-3, or 56-59 in mode 2) or, while aiming, the stick (PLW+0xC8 directions,
// tilt +0xD4 through the table at 0x805C68D8) moves the aim:
//   - yaw: the wanted yaw (+0xA8) by 384 (512 in mode 2, 76 or the table's
//     amount by stick) a step;
//   - pitch: by 5 (1 or the table's by stick) a step, through 0x8027A064
//     (mode 0: s8 +0x64F), 0x8027A0A8 (mode 1: s16 +0x5E8, the amount * 64)
//     or 0x8027A0F0 (mode 2: s8 +0x583).
// Diagonals scale both by the factor at 0x8079C5B0 (the pitch at least 1).
// Those amounts are per step, so at 500-700 steps a second the aim
// swung 15-20 times as fast as on the Wii. Here each is GameStep::Share of the
// amount (the amount times the step's length in ticks); at the stock 1/30 s
// step that is the amount: the original.
//
// Unverified in play: checked against the original at the stock step through
// the player port fuzz only (MH3_PORT_CHECK=P:0x8027A5E0, 64 states), never
// played with a bowgun or at a variable step.
#include "game_step.h"
#include "guest_call.h"
#include "port_check.h"

#include <cstdint>
#include <utility>

namespace {

enum : uint32_t {
    MasterCheck = 0x8026FE38,  // Pl_master_ck(PLW) -> nonzero for the local player
    MotionNo = 0x8026A36C,     // Get_motion_no(PLW) -> u16
    CommandTest = 0x8026A758,  // PlCommandTest(PLW, bit) -> 1 when +0xDC has it
    CommandBit = 0x8026A708,   // PlCommandBit(PLW, bit): +0xF0 bit array
    CommandBitE0 = 0x8026A6A8, // PlCommandBitE0(PLW, bit): +0xE0 bit array
    TurnLocked = 0x8027A3A4,   // (PLW) -> nonzero
    AimReady = 0x802872A8,     // (PLW, 0) -> 1
    OptionConfig = 0x803BED04, // get_option_cfg (player, option) -> u8
    AimStickScale = 0x802B97A4, // (PLW, u8* out, u8* out, f32* scale)
    AimPitch64F = 0x8027A064,  // (PLW, s8 amount): +0x64F within -100..100
    AimPitch5E8 = 0x8027A0A8,  // (PLW, s16 amount): +0x5E8 within -0x2000..0x2000
    AimPitch583 = 0x8027A0F0,  // (PLW, s8 amount): +0x583, and +0x5E8 from it
    AimMotion = 0x80279F20,    // (PLW, motion, motion)
};

enum : uint32_t {
    kPlayer = 8,          // u8
    kState = 10,          // u8
    kYawBase = 0x38,      // u16 (mode 2: the aim stays within 90 degrees of it)
    kWantedYaw = 0xA8,    // u32 (u16 angle)
    kStickDirs = 0xC8,    // u16 stick direction keys (0x3C00)
    kStickTilt = 0xD4,    // u16
    kAimBlocked = 0x36A,  // s8
    kAiming = 0x5E5,      // u8: 5 aiming pitch, 10 aiming yaw
    kScoped = 0x5E6,      // u8
};

constexpr uint32_t kStickTable = 0x805C68D8;    // {s32 tilt, pitch lo, pitch hi, yaw lo, yaw hi}[], ends at tilt -1
constexpr uint32_t kStickScale = 0x8079C540;    // f32
constexpr uint32_t kDiagonalScale = 0x8079C5B0; // f32

} // namespace

static void PlAimInputPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x60);
    GuestCall game{ctx};
    const uint32_t plw = frame.Arg(3), motion = frame.Arg(4) & 0xFFFF, mode = frame.Arg(5) & 0xFF;
    uint32_t yaw = Memory::Read32(plw + kWantedYaw);

    if (game.Call<MasterCheck>({plw}) == 0 || (game.Call<MotionNo>({plw}) & 0xFFFF) != motion || int8_t(Memory::Read8(plw + kAimBlocked)) != 0)
        return;

    const auto aiming = [&] {
        return Memory::Read8(plw + kAiming) != 0 || Memory::Read8(plw + kScoped) != 0;
    };

    if (aiming())
        Memory::Write8(plw + kAiming, 10);

    bool up = false, down = false, right = false, left = false;
    int32_t pitchStep = 0, yawStep = 0;

    // The directions as the options (u8 2 for the d-pad, 9 for the stick) turn
    // them: 1 or 3 swaps up and down, 2 or 3 right and left.
    const auto option = [&](uint32_t index) {
        const uint32_t value = game.Call<OptionConfig>({Memory::Read8(plw + kPlayer), index}) & 0xFF;

        if (value == 1 || value == 3)
            std::swap(up, down);

        if (value == 2 || value == 3)
            std::swap(right, left);
    };

    const auto dpad = [&](uint32_t fn, uint32_t first) {
        up = game(fn, {plw, first}) != 0;
        down = !up && game(fn, {plw, first + 1}) != 0;
        right = game(fn, {plw, first + 2}) != 0;
        left = !right && game(fn, {plw, first + 3}) != 0;
        option(2);
        return up || down || right || left;
    };

    bool apply = true; // false: straight to storing the aim unchanged
    bool scale = false;

    if (!aiming()) {
        if (game.Call<CommandTest>({plw, 11}) == 1)
            apply = false;
        else if (mode == 0 && Memory::Read8(plw + kState) != 4 && game.Call<TurnLocked>({plw}) == 0)
            apply = false;
        else if (mode == 2 && game.Call<CommandBitE0>({plw, 55}) == 0)
            apply = false;
    }

    if (apply) {
        bool stick = true;

        if (mode == 0 && (game.Call<TurnLocked>({plw}) != 0 || (Memory::Read8(plw + kScoped) == 0 && game.Call<AimReady>({plw, 0}) == 1))) {
            stick = false;

            if (dpad(CommandBit, 0)) {
                pitchStep = 5;
                yawStep = 384;
                scale = true;
            } else {
                apply = false;
            }
        } else if (mode == 2 && Memory::Read8(plw + kScoped) == 0) {
            stick = false;

            if (dpad(CommandBitE0, 56)) {
                pitchStep = 5;
                yawStep = 512;
                scale = true;
            } else {
                apply = false;
            }
        }

        if (stick) {
            scale = true;
            const uint32_t dirs = Memory::Read16(plw + kStickDirs);

            if (aiming() && (dirs & 0x3C00) != 0 && Memory::Read16(plw + kStickTilt) >= 50) {
                up = dirs & 0x2000;
                down = !up && (dirs & 0x1000);
                right = dirs & 0x800;
                left = !right && (dirs & 0x400);
                option(9);
                const uint32_t factor = frame[12];
                Memory::Write32(factor, Memory::Read32(kStickScale));
                game.Call<AimStickScale>({plw, frame[9], frame[8], factor});
                const float f = Memory::ReadFloat32(factor);
                pitchStep = 1;
                yawStep = 76;
                const int32_t tilt = Memory::Read16(plw + kStickTilt);

                for (uint32_t entry = kStickTable; int32_t(Memory::Read32(entry)) != -1; entry += 20) {
                    if (tilt < int32_t(Memory::Read32(entry)))
                        continue;

                    const auto lerp = [&](uint32_t lo) {
                        const float a = float(int32_t(Memory::Read32(entry + lo)));
                        const float b = float(int32_t(Memory::Read32(entry + lo + 4)));
                        return int32_t(a + f * (b - a));
                    };

                    pitchStep = lerp(4);
                    yawStep = lerp(12);
                    break;
                }
            }
        }
    }

    int32_t pitch = 0;

    if (apply && scale) {
        const bool vertical = up || down;

        if (vertical && (right || left)) {
            const float k = Memory::ReadFloat32(kDiagonalScale);
            yawStep = int32_t(k * float(yawStep));
            pitchStep = int32_t(k * float(pitchStep));

            if (pitchStep < 1)
                pitchStep = 1;
        }

        if (vertical) {
            const int32_t step = GameStep::Share(pitchStep);
            pitch = up ? step : -step;
            Memory::Write8(plw + kAiming, 5);
        }

        if (right) {
            Memory::Write8(plw + kAiming, 10);
            yaw += uint32_t(GameStep::Share(yawStep));
        }

        if (left) {
            Memory::Write8(plw + kAiming, 10);
            yaw -= uint32_t(GameStep::Share(yawStep));
        }
    }

    switch (mode) {
    case 0:
        Memory::Write32(plw + kWantedYaw, yaw);
        game.Call<AimPitch64F>({plw, uint32_t(pitch)});

        if (motion == 1001)
            game.Call<AimMotion>({plw, 1030, 1031});
        else if (motion == 1050)
            game.Call<AimMotion>({plw, 1080, 1081});

        break;
    case 1:
        Memory::Write32(plw + kWantedYaw, yaw);
        game.Call<AimPitch5E8>({plw, uint32_t(int32_t(int16_t(uint16_t(uint32_t(pitch) << 6))))});
        break;
    case 2: {
        Memory::Write32(plw + kWantedYaw, yaw);
        const uint32_t base = Memory::Read16(plw + kYawBase);
        const uint32_t offset = (yaw - base) & 0xFFFF;

        if (offset - 0x4001 <= 0x3FFF)
            Memory::Write32(plw + kWantedYaw, base + 0x4000);
        else if (offset - 0x8000 <= 0x3FFF)
            Memory::Write32(plw + kWantedYaw, base - 0x4000);

        game.Call<AimPitch583>({plw, uint32_t(int32_t(int8_t(uint8_t(pitch))))});
        break;
    }
    default:
        break;
    }
}

extern "C" void func_8027A5E0(CpuContext* ctx);
extern "C" void MH3_PlAimInput(CpuContext* ctx) {
    PortCheck::Call(ctx, "PlAimInput", func_8027A5E0, PlAimInputPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8027A5E0, MH3_PlAimInput, "PlAimInput");
