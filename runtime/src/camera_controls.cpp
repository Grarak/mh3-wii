// Camera controls, RMHP08: C reconstructions of the field camera's helpers
// that decide when and how the stick (and the mouse, mouse_look.cpp) move
// it, from the translated code:
//   0x802B6C30  CameraSwingBack (r3 u16 gap; f1 lo, f2 hi, f3 gain) -> s16:
//               the turn a step that swings the camera back behind the
//               player, gain * (c (x - lo)^2) * (c (hi - x)^2) for a gap x
//               between lo and hi (mirrored for gaps past half a turn), 0
//               in the dead zones
//   0x802B8AF0  CameraFollowMode (camera, PLW): camera +0x9C, the swing-back
//               mode (-1 none, 0..3), a new mode taking over after 3 steps
//   0x802BE2F0  CameraUsable (PLW) -> 1 when the stick may move the camera
//   0x802B7A1C  CameraEvent (PLW, kind) -> 1 when the player's action calls
//               for a camera move (0..2: action 6/62 at a frame, 3: 6/54)
//   0x802B8C4C  CameraTargetMode (camera, PLW): snaps the camera behind the
//               player at its distance row and clears the shakes
//   0x802B79E4  CameraSurfaced (PLW) -> 1 swimming above the surface line
//               with +0x74 set
//   0x802BE0EC  CameraUnderwater () -> 1 when the camera is below 0 in a
//               water view (flags 1, 2 or 4 at +0x4F4)
//   0x8026F96C  PlStickLevel (PLW, stick) -> 0 near the middle, 1 or 3 by the
//               stick's length (s16 at +0xD4), r4 the length
// Float steps are single operations as the originals' fsubs and fmuls.
#include "guest_call.h"
#include "memory.h"
#include "port_check.h"

#include <cstdint>

namespace {

enum : uint32_t {
    kFollow = 0x9C,     // s8 camera: swing-back mode
    kFollowWait = 0x9D, // s8: steps before a new mode takes over
    kDistance = 0x7C,   // u8 0..4
    kRow = 0x80,        // u32: the distance row (28 bytes)
    kRowFlag = 0x84,    // u8
    kReset = 0x42,      // u8
    kTargetMode = 0x92, // u8
    kEye = 0x00,
    kTarget = 0x0C,
    kEyeWanted = 0x4C,
    kTargetWanted = 0x58,
    kPitchTarget = 0x64,
    kYawTarget = 0x68,
    kPitch = 0x6C,
    kYaw = 0x70,
    kShakes = 0xB8,     // 6 VEC3
};

enum : uint32_t {
    kPlwRide = 2,        // u8: 4..6 riding
    kPlwState = 9,       // u8: 1 ..., 3 swimming
    kPlwActionKind = 10, // u8
    kPlwAction = 12,     // u16
    kPlwRidden = 24,     // u8
    kPlwPosition = 0x3C, // VEC3
    kPlwYaw = 0x58,
    kPlwSurfaced = 0x74, // u8
    kPlwSticks = 0xD4,   // s16[]: stick lengths
    kPlwStickMode = 0x128, // u8: 2 wider thresholds
    kPlwNoCamera = 0x5E5,  // u8
    kPlwRiding = 0x5E6,    // u8
};

enum : uint32_t {
    FollowState = 0x8027A3A4,  // (PLW) -> 1 when the camera may follow
    CameraBusy = 0x802BE450,   // () -> 1
    CameraLocked = 0x802BE480, // () -> 1
    CameraAiming = 0x802B9618, // () -> 1
    TargetHeld = 0x8027A304,   // (PLW, 0) -> 1
    CameraEvent = 0x802B7A1C,
    CameraView = 0x802B8BCC,   // () -> s8 view
    ActionCheck = 0x8026FECC,  // Pl_act_ck (PLW, u8 kind, u16 action) -> 1
    FrameCheck = 0x8026A378,   // Pl_frame_check (PLW, n; f1, f2) -> 1
    CameraManager = 0x802BED34, // (r3) -> the camera
    CameraPosition = 0x802BDD44, // get_camera_pos (VEC3*)
    RotY = 0x800510C8,         // rotVecY (VEC3*, angle)
    VecAdd = 0x800513DC,       // (out, a, b)
    VecCopy = 0x80041EA4,      // (to, from) -> to
};

constexpr uint32_t kCameraBlocked = 0x806D4FA0; // u8: > 1 no camera input
constexpr uint32_t kRows = 0x805D03A8;          // distance rows, 28 bytes
constexpr uint32_t kRowsSwimming = 0x805D0488;
constexpr uint32_t kRowRidingMid = kRows + 140;
constexpr uint32_t kZero = 0x8079C998;          // f32 0
constexpr uint32_t kSurface = 0x8079CA2C;       // f32: the surfaced height
constexpr uint32_t kFull = 0x8079CA08;          // f32 65536
constexpr uint32_t kSwingScale = 0x8079CA0C;    // f32 c
constexpr uint32_t kFrameEvent0 = 0x8079C9E0;   // f32 frames
constexpr uint32_t kFrameEvent1 = 0x8079CA30;

float F(uint32_t address) {
    return Memory::ReadFloat32(address);
}

// fsubs and fmuls: the operation in double, rounded to single.
float Sub(float a, float b) {
    return float(double(a) - double(b));
}

float Mul(float a, float b) {
    return float(double(a) * double(b));
}

} // namespace

static void CameraSwingBackPort(CpuContext* ctx) {
    uint32_t gap = ctx->gpr[3];
    const float lo = float(ctx->fpr[1].d), hi = float(ctx->fpr[2].d);
    float gain = float(ctx->fpr[3].d);
    const float x = float(double(gap)), full = F(kFull);
    ctx->gpr[3] = 0;

    if (x <= lo)
        return;

    if (!(x < hi)) {
        // Past half a turn: the other way, unless in the far dead zone.
        if (x <= Sub(full, hi) || !(x < Sub(full, lo)))
            return;

        gap = 65536 - gap;
        gain = -gain;
    }

    const float v = float(double(int32_t(gap)));
    const float below = Sub(v, lo), above = Sub(hi, v), c = F(kSwingScale);
    const float turn = Mul(gain, Mul(Mul(c, Mul(below, below)), Mul(c, Mul(above, above))));
    ctx->gpr[3] = uint32_t(int32_t(int16_t(int32_t(turn))));
}

static void CameraFollowModePort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x20);
    GuestCall game{ctx};
    const uint32_t camera = frame.Arg(3), plw = frame.Arg(4);
    int8_t mode = -1;

    if (game.Call<FollowState>({plw}) == 1) {
        if (Memory::Read8(plw + kPlwState) == 3)
            mode = Memory::Read8(plw + kPlwActionKind) == 1 ? 2 : 3;
        else
            mode = Memory::Read8(plw + kPlwActionKind) != 1;
    }

    if (mode > 0) {
        // A mode replacing none waits 3 steps.
        if (int8_t(Memory::Read8(camera + kFollow)) < 0) {
            Memory::Write8(camera + kFollowWait, uint8_t(Memory::Read8(camera + kFollowWait) - 1));

            if (int8_t(Memory::Read8(camera + kFollowWait)) > 0)
                mode = -1;
        }
    } else {
        Memory::Write8(camera + kFollowWait, 3);
    }

    Memory::Write8(camera + kFollow, uint8_t(mode));
}

static void CameraUsablePort(CpuContext* ctx) {
    uint32_t usable;
    {
        GuestFrame frame(ctx, 0x10);
        GuestCall game{ctx};
        const uint32_t plw = frame.Arg(3);

        // The callees take no arguments; r3 carries the last result, as there.
        uint32_t r3 = game.Call<CameraBusy>({plw});

        if (r3 == 1 || Memory::Read8(kCameraBlocked) > 1 || (r3 = game.Call<CameraLocked>({0})) == 1 || (r3 = game.Call<CameraAiming>({r3})) == 1 ||
            Memory::Read8(plw + kPlwNoCamera) != 0 || game.Call<TargetHeld>({plw, 0}) == 1 || (r3 = game.Call<CameraEvent>({plw, 1})) == 1) {
            usable = 0;
        } else if (int8_t((r3 = game.Call<CameraView>({r3}))) == 0 || int8_t((r3 = game.Call<CameraView>({r3}))) == 1) {
            usable = 0;
        } else {
            usable = int8_t(game.Call<CameraView>({r3})) != 3;
        }
    }

    ctx->gpr[3] = usable;
}

static void CameraEventPort(CpuContext* ctx) {
    uint32_t event;
    {
        GuestFrame frame(ctx, 0x10);
        GuestCall game{ctx};
        const uint32_t plw = frame.Arg(3), kind = frame.Arg(4) & 0xFF;
        const auto acting = [&](uint32_t action) {
            return Memory::Read8(plw + kPlwActionKind) == 6 && Memory::Read16(plw + kPlwAction) == action;
        };

        const auto atFrame = [&](uint32_t n, uint32_t frameAt) { return game.Call<FrameCheck>({plw, n}, {F(frameAt), F(kZero)}) == 1; };

        switch (kind) {
        case 0:
            event = acting(62) && atFrame(0, kFrameEvent0);
            break;
        case 1:
            event = acting(62) && atFrame(2, kFrameEvent1);
            break;
        case 2:
            event = game.Call<ActionCheck>({plw, 6, 62}) == 1 && atFrame(0, kFrameEvent1);
            break;
        case 3:
            event = game.Call<ActionCheck>({plw, 6, 54}) == 1;
            break;
        default:
            event = 0;
        }
    }

    ctx->gpr[3] = event;
}

static void CameraTargetModePort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x40);
    GuestCall game{ctx};
    const uint32_t camera = frame.Arg(3), plw = frame.Arg(4);
    const uint32_t offset = frame[44], eye = frame[32], target = frame[20], rowTarget = frame[8];
    const uint32_t zero = Memory::Read32(kZero);

    for (uint32_t k = 0; k < 3; ++k)
        Memory::Write32(offset + 4 * k, zero);

    const uint32_t yaw = (Memory::Read32(plw + kPlwYaw) + 0x8000) & 0xFFFF;
    Memory::Write32(camera + kYaw, yaw);
    Memory::Write32(camera + kYawTarget, yaw);
    const uint32_t level = Memory::Read8(camera + kDistance);

    if (Memory::Read8(plw + kPlwState) == 3) {
        Memory::Write32(camera + kRow, kRowsSwimming + level * 28);
        Memory::Write8(camera + kRowFlag, 0);
        const uint32_t pitch = F(plw + kPlwPosition + 4) > F(kSurface) && Memory::Read8(plw + kPlwSurfaced) != 0 ? 0xF000 : 0;
        Memory::Write32(camera + kPitch, pitch);
        Memory::Write32(camera + kPitchTarget, pitch);
    } else if (Memory::Read8(plw + kPlwState) == 1 && level == 2) {
        Memory::Write32(camera + kRow, kRowRidingMid);
    } else {
        Memory::Write32(camera + kRow, kRows + level * 28);
    }

    // The eye behind the player at the row's height and distance, the
    // target at the row's offset.
    const uint32_t row = Memory::Read32(camera + kRow);
    Memory::Write32(offset + 4, Memory::Read32(row + 4));
    Memory::Write32(offset + 8, Memory::Read32(row + 8));
    game.Call<RotY>({offset, Memory::Read32(camera + kYawTarget)});
    game.Call<VecAdd>({eye, plw + kPlwPosition, offset});
    game.Call<VecCopy>({camera + kEye, game.Call<VecCopy>({camera + kEyeWanted, eye})});

    for (uint32_t k = 0; k < 3; ++k)
        Memory::Write32(rowTarget + 4 * k, Memory::Read32(row + 12 + 4 * k));

    game.Call<VecAdd>({target, plw + kPlwPosition, rowTarget});
    game.Call<VecCopy>({camera + kTarget, game.Call<VecCopy>({camera + kTargetWanted, target})});

    for (uint32_t k = 0; k < 18; ++k)
        Memory::Write32(camera + kShakes + 4 * k, zero);

    Memory::Write8(camera + kReset, 0);
    Memory::Write8(camera + kTargetMode, 0);
}

static void CameraSurfacedPort(CpuContext* ctx) {
    const uint32_t plw = ctx->gpr[3];
    ctx->gpr[3] = Memory::Read8(plw + kPlwState) == 3 && F(plw + kPlwPosition + 4) > F(kSurface) && Memory::Read8(plw + kPlwSurfaced) != 0;
}

static void CameraUnderwaterPort(CpuContext* ctx) {
    uint32_t under = 0;
    {
        GuestFrame frame(ctx, 0x20);
        GuestCall game{ctx};
        const uint32_t camera = game.Call<CameraManager>({frame.Arg(3)});

        if (Memory::Read16(camera + 0x4F4) & 7) {
            game.Call<CameraPosition>({frame[8]});
            under = F(frame[8] + 4) < F(kZero);
        }
    }

    ctx->gpr[3] = under;
}

static void PlStickLevelPort(CpuContext* ctx) {
    const uint32_t plw = ctx->gpr[3];
    const int32_t length = int16_t(Memory::Read16(plw + kPlwSticks + ctx->gpr[4] * 2));
    uint32_t level = 0;
    const uint32_t ride = Memory::Read8(plw + kPlwRide);

    // Riding a monster, the stick drives it.
    if (!(ride - 4 <= 2 && Memory::Read8(plw + kPlwRidden) == 1 && Memory::Read8(plw + kPlwRiding) == 1)) {
        const bool wide = Memory::Read8(plw + kPlwStickMode) == 2;

        if (length >= (wide ? 110 : 100))
            level = 3;
        else if (length >= (wide ? 50 : 40))
            level = 1;
    }

    ctx->gpr[3] = level;
    ctx->gpr[4] = uint32_t(length);
}

#define MH3_CAMERA_PORT(address, name) \
    extern "C" void func_##address(CpuContext* ctx); \
    extern "C" void MH3_##name(CpuContext* ctx) { \
        PortCheck::Call(ctx, #name, func_##address, name##Port); \
    }

#define MH3_CAMERA_PURE(address, name) \
    extern "C" void func_##address(CpuContext* ctx); \
    extern "C" void MH3_##name(CpuContext* ctx) { \
        if (PortCheck::Enabled(#name)) [[unlikely]] \
            PortCheck::RunPure(ctx, #name, PortCheck::Returns::Gpr3, func_##address, name##Port); \
        else \
            name##Port(ctx); \
    }

MH3_CAMERA_PURE(802B6C30, CameraSwingBack)
REGISTER_NATIVE_FUNCTION_AS(0x802B6C30, MH3_CameraSwingBack, "CameraSwingBack");

MH3_CAMERA_PORT(802B8AF0, CameraFollowMode)
REGISTER_NATIVE_FUNCTION_AS(0x802B8AF0, MH3_CameraFollowMode, "CameraFollowMode");

MH3_CAMERA_PORT(802BE2F0, CameraUsable)
REGISTER_NATIVE_FUNCTION_AS(0x802BE2F0, MH3_CameraUsable, "CameraUsable");

MH3_CAMERA_PORT(802B7A1C, CameraEvent)
REGISTER_NATIVE_FUNCTION_AS(0x802B7A1C, MH3_CameraEvent, "CameraEvent");

MH3_CAMERA_PORT(802B8C4C, CameraTargetMode)
REGISTER_NATIVE_FUNCTION_AS(0x802B8C4C, MH3_CameraTargetMode, "CameraTargetMode");

MH3_CAMERA_PURE(802B79E4, CameraSurfaced)
REGISTER_NATIVE_FUNCTION_AS(0x802B79E4, MH3_CameraSurfaced, "CameraSurfaced");

MH3_CAMERA_PURE(802BE0EC, CameraUnderwater)
REGISTER_NATIVE_FUNCTION_AS(0x802BE0EC, MH3_CameraUnderwater, "CameraUnderwater");

MH3_CAMERA_PURE(8026F96C, PlStickLevel)
REGISTER_NATIVE_FUNCTION_AS(0x8026F96C, MH3_PlStickLevel, "PlStickLevel");
