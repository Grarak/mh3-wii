// Field camera, RMHP08. C reconstruction of the field camera's per-step
// update 0x802B5CBC FieldCameraUpdate (camera; the PLW at +0x494), called by
// CameraUpdate 0x802B579C, decompiled with Ghidra.
//
// Each step it picks the distance and height row, takes the stick (the input
// ports in camera_input.cpp), eases the pitch and yaw toward their targets,
// places the target and the eye behind the player and blends a camera reset.
// What the original does once per step, and what this port does instead over
// a step of s ticks (GameStep):
//   - pitch levelling: the pitch target returns toward 0 by 10 (30) a step
//     once within 2560 of it, and while looking up (0x802B79E4, a ledge or a
//     climb) moves toward -0x1000 by 48 (144) a step: here GameStep::Share
//     of those amounts;
//   - the eases of pitch, yaw and distance angle (`+= gap >> shift`) go
//     through CameraAngleEase (camera_input.cpp), which closes the gap for the
//     step's length;
//   - the target and eye heights ease by 1/d of the gap a step (d from
//     0x8079C9BC..0x8079C9DC): here by 1/MH3_EaseDivisor(d), the gap closed
//     over s ticks;
//   - the swim and settle timers (+0xA8) count whole ticks;
//   - a camera reset (+0x42) blends the eye and target from where they were
//     over 19 steps: each step keeps t (0x802BB0B0 of the steps left) of the
//     old position. Here the count is in whole ticks and each step keeps t^s,
//     so the blend takes as long as on the Wii.
// At the stock 1/30 s step every one of these is the original.
#include "game_step.h"
#include "guest_call.h"
#include "port_check.h"
#ifdef MH3_DEVELOPMENT
#include "cheats.h"
#include "controller_host.h"
#include "dev_options.h"
#include "native_frame_packet.h"
#include <chrono>
#include <cstdio>
#include <iterator>
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>

extern "C" double MH3_EaseDivisor(double d); // tick_sites.cpp

namespace {

enum : uint32_t {
    GetOnlineMode = 0x800CF26C,
    OptionConfig = 0x803BED04,  // get_option_cfg (player, option) -> u8
    CameraFollowMode = 0x802B8AF0, // (camera, PLW): +0x9C
    CameraPosition = 0x802BDD44,   // get_camera_pos (VEC3*)
    CameraTarget = 0x802BDE14,     // (VEC3*)
    AngleBetween = 0x80050AF4,     // calcVecAng2 (VEC3*, VEC3*) -> yaw
    AngleX = 0x80050BAC,           // calcVecAngX (VEC3*) -> pitch
    CameraTargetMode = 0x802B8C4C, // (camera, PLW), when +0x92
    CameraClimbMode = 0x802B7880,  // (camera) -> 1 in the climbing view
    CameraPitchInput = 0x802B6D30, // (camera, camera, PLW)
    CameraYawInput = 0x802B6864,   // (camera, camera, PLW)
    PlCarried = 0x8027CC90,        // (PLW) -> 1
    VecSubtract = 0x80050D04,      // (out, a, b): a - b
    VecAdd = 0x800513DC,           // (out, a, b): a + b
    VecCopy = 0x80041EA4,          // (to, from)
    VecSet = 0x8005000C,           // setVector3 (VEC3*; f1, f2, f3)
    RotY = 0x800510C8,             // rotVecY (VEC3*, angle)
    RotX = 0x8005101C,             // rotVecX (VEC3*, angle)
    LookingUp = 0x802B79E4,        // (PLW) -> nonzero
    ActionCheck = 0x8026FECC,      // Pl_act_ck (PLW, u8 kind, u16 action) -> 1
    PlStateCheck = 0x8027D850,     // (PLW, 2) -> 1
    CameraEvent = 0x802B7A1C,      // (PLW, kind) -> 1
    JointPosition = 0x8026A3EC,    // pl_get_joint_wpos (PLW, joint, VEC3*)
    FrameCheck = 0x8026A378,       // Pl_frame_check (PLW, 0; f1, f2) -> nonzero
    CameraAngleEase = 0x802BDC90,  // (target, angle, shift) -> the step
    CameraCollide = 0x802B8244,    // (camera, PLW)
    CameraPush = 0x802B8940,       // (camera), when +0xA6
    VecAddInPlace = 0x80073FCC,    // (a, b): a += b
    BindCamera = 0x802B8ED0,       // (camera, PLW): a monster's hold of the player (below)
    CameraLimit = 0x802B72BC,      // (camera, camera, PLW)
    ResetCurve = 0x802BB0B0,       // (r3 s16 steps left; f1 rate, f2) -> f1 old weight
    Lerp = 0x80052298,             // (out, a, b; f1 t): a*t + b*(1-t)
    CameraSet = 0x802BE158,        // (kind, eye, target; f1, f2)
};

// Camera work.
enum : uint32_t {
    kEye = 0x00,          // VEC3
    kTarget = 0x0C,       // VEC3
    kOldEye = 0x18,       // VEC3, the reset blend's
    kOldTarget = 0x24,    // VEC3
    kFovB = 0x30,         // f32
    kFovA = 0x34,         // f32
    kReset = 0x42,        // u8: 1 start a reset blend, 2 blending
    kResetLeft = 0x44,    // s16 steps (here ticks) of the blend left
    kResetRate = 0x48,    // f32: 1 / 20
    kEyeWanted = 0x4C,    // VEC3
    kTargetWanted = 0x58, // VEC3
    kPitchTarget = 0x64,  // s32 (s16 angle)
    kYawTarget = 0x68,
    kPitch = 0x6C,
    kYaw = 0x70,
    kTiltTarget = 0x74,
    kTilt = 0x78,
    kDistance = 0x7C,     // u8 0..4
    kRow = 0x80,          // u32: the distance row (28 bytes)
    kInstant = 0x88,      // u8: 2 = no height ease
    kTargetMode = 0x92,   // u8
    kFollowing = 0x9E,    // u8
    kPitchRamp = 0xA0,    // s8
    kPush = 0xA6,         // u8
    kSettle = 0xA8,       // s16 timer (ticks)
    kManualPitch = 0xAA,  // u8
    kPitchHold = 0xAB,    // u8
    kYawSnap = 0xAD,      // u8, u16 angle at +0xAE
    kPitchSnap = 0xB0,    // u8, u16 angle at +0xB2
    kLookUp = 0xB6,       // u8: 1 tilting up, 2 there
    kFast = 0xB7,         // u8: faster levelling
    kOrbit = 0x1F0,       // VEC3
    kPlayerWork = 0x494,  // u32 PLW
    kView = 0x49B,        // u8 view (0xFF none)
    kFlags = 0x49C,       // u8: bit 0 inverts yaw, bit 1 pitch
    kShakeEye = 0xC4,     // VEC3
    kShakeTarget = 0xDC,  // VEC3
};

enum : uint32_t {
    kPlwPlayer = 8,  // u8
    kPlwState = 9,   // u8: 1 ..., 3 swimming
    kPlwAction = 10, // u8
    kPlwPosition = 0x3C,
    kPlwYaw = 0x58,
};

constexpr uint32_t kRows = 0x805D03A8;       // distance rows, 28 bytes: +4 height, +8 back, +16 target height
constexpr uint32_t kRowsSwimming = 0x805D0488;
constexpr uint32_t kRowClimb = 0x805D0450;
constexpr uint32_t kRowCarried = 0x805D0530;
constexpr uint32_t kRowRidingMid = kRows + 140;

constexpr uint32_t kZero = 0x8079C998;     // f32 0
constexpr uint32_t kSitDrop = 0x8079C9A4;  // f32
constexpr uint32_t kOne = 0x8079C9AC;      // f32 1
constexpr uint32_t kNearest = 0x8079C9B0;  // f32: start of the nearest-row search
constexpr uint32_t kBack = 0x8079C9B4;     // f32
constexpr uint32_t kSitHeight = 0x8079C9B8;
constexpr uint32_t kEaseSit = 0x8079C9BC;  // f32 divisors
constexpr uint32_t kHeightSwim = 0x8079C9C0;
constexpr uint32_t kHeightState = 0x8079C9C4;
constexpr uint32_t kEaseTarget = 0x8079C9C8;
constexpr uint32_t kEaseEye = 0x8079C9CC;
constexpr uint32_t kJointUp = 0x8079C9D0;
constexpr uint32_t kSwimFrame = 0x8079C9D4;
constexpr uint32_t kEaseSwimEye = 0x8079C9D8;
constexpr uint32_t kEaseSwimTarget = 0x8079C9DC;
constexpr uint32_t kResetShape = 0x8079C9E0;

float F(uint32_t address) {
    return Memory::ReadFloat32(address);
}

void WF(uint32_t address, float value) {
    Memory::WriteFloat32(address, value);
}

uint32_t R32(uint32_t a) {
    return Memory::Read32(a);
}

void W32(uint32_t a, uint32_t v) {
    Memory::Write32(a, v);
}

// `x += (wanted - x) / d` over this step: d becomes MH3_EaseDivisor(d).
void EaseHeight(uint32_t value, uint32_t wanted, float d) {
    const float divisor = float(MH3_EaseDivisor(d));
    const float k = F(kOne) / divisor;
    const float gap = F(wanted) - F(value);
    const float step = k * gap;
    WF(value, F(value) + step);
}

#ifdef MH3_DEVELOPMENT
constexpr uint32_t kMovePosition = 0x188;                // VEC3, a monster work's position (enemy_glide.cpp)
constexpr uint32_t kMoveChar = 0x24;                     // a monster work's MHchar (embedded; its model +0x118 is the work's +0x13C)
constexpr uint32_t kSceneRoot = 0x80796e34;              // the scene root (native_scene_queue.cpp)
constexpr uint32_t kChildren = 0xDC, kChildCount = 0xE4; // a scene group's children (scene_world.cpp)
constexpr uint32_t kCalcWorldCallback = 0x11C;           // ScnMdlSimple: its CalcWorld callback object
constexpr uint32_t WorldMatrix = 0x8007B5A8;             // (object, 1): the object's world matrix

// MH3_TRACE=monsters: each monster's move work and position once a second.
// monster-steps=N: every logic step, monster N's position, the step, its
// MHchar's position, model offset and first channels, and the world
// translation of the scene root's models near it.
void ReportMonsters(GuestCall& game) {
    static const bool enabled = TraceOptions().Has("monsters");
    static const uint64_t stepped = TraceOptions().Number("monster-steps", UINT64_MAX);
    static auto last = std::chrono::steady_clock::now();

    if (stepped != UINT64_MAX) {
        if (const uint32_t m = Cheats::MoveWork(Cheats::Kind::Monster, uint32_t(stepped)); m && Memory::Read16(m)) {
            const uint32_t c = m + kMoveChar;
            std::fprintf(stderr, "MH3 monster step: packet %llu scale %.3f ticks %u position %.1f %.1f %.1f", static_cast<unsigned long long>(NativeFramePacketSequence()), GameStep::Scale(),
                         GameStep::Ticks(), F(m + kMovePosition), F(m + kMovePosition + 4), F(m + kMovePosition + 8));
            std::fprintf(stderr, " char %08x pos %.1f %.1f %.1f offset %.1f %.1f %.1f flags %x", c, F(c + 4), F(c + 8), F(c + 12), F(c + 0x10), F(c + 0x14), F(c + 0x18), R32(c + 0x4C));

            // The work's angles (s32[3] at +0x1BC, monster_attacks.cpp) and its turn goal (+0x37C).
            std::fprintf(stderr, " rot %d %d %d goal %04x", int32_t(R32(m + 0x1BC)), int32_t(R32(m + 0x1C0)), int32_t(R32(m + 0x1C4)), Memory::Read16(m + 0x37C));

            for (uint32_t i = 0; i < 3; ++i)
                std::fprintf(stderr, " ch%u %.2f>%.2f r%.2f e%.1f w%.2f", i, F(c + 0x8C + 4 * i), F(c + 0x74 + 4 * i), F(c + 0x5C + 4 * i), F(c + 0xA4 + 4 * i), F(c + 0xD4 + 4 * i));

            // The scene root's children within 4000 units of it (x, z): their
            // world matrices' translation as this step draws them, and their
            // CalcWorld callback's class.
            const uint32_t root = R32(kSceneRoot);
            const uint32_t count = root ? R32(root + kChildCount) : 0, list = root ? R32(root + kChildren) : 0;

            for (uint32_t i = 0; i < count && i < 64; ++i) {
                const uint32_t child = R32(list + 4 * i), callback = R32(child + kCalcWorldCallback);
                const uint32_t world = game.Call<WorldMatrix>({child, 1});
                const float dx = F(world + 12) - F(m + kMovePosition), dz = F(world + 44) - F(m + kMovePosition + 8);

                if (dx * dx + dz * dz < 4000.0f * 4000.0f)
                    std::fprintf(stderr, " model %08x/%08x world %08x at %.1f %.1f %.1f", child, callback ? R32(callback) : 0, world, F(world + 12), F(world + 28), F(world + 44));
            }

            std::fprintf(stderr, "\n");
        }
    }

    // monster-spins: every step, each monster whose yaw (+0x1C0) turns more
    // than 2000 units (11 degrees) a tick: a spin added once a step instead of
    // by the step's game time turns the same per step at any step length.
    static const bool spins = TraceOptions().Has("monster-spins");

    if (spins) {
        static uint32_t lastYaw[16]{};
        static uint32_t lastWork[16]{};
        const uint32_t n = std::min<uint32_t>(Cheats::MoveWorkCount(Cheats::Kind::Monster), 16);

        for (uint32_t i = 0; i < n; ++i) {
            const uint32_t m = Cheats::MoveWork(Cheats::Kind::Monster, i);

            if (!m || !Memory::Read16(m))
                continue;

            const uint32_t yaw = R32(m + 0x1C0);
            const int32_t turn = int32_t(int16_t(uint16_t(yaw - lastYaw[i])));

            if (lastWork[i] == m && GameStep::Scale() > 0 && std::abs(turn) / GameStep::Scale() > 2000) {
                const uint32_t c = m + kMoveChar;
                std::fprintf(stderr, "MH3 monster spin: [%u] work %08x words %08x %08x %08x turn %d scale %.3f ticks %u motion %.2f>%.2f r%.2f e%.1f\n", i, m, R32(m + 4), R32(m + 8),
                             R32(m + 0x10), turn, GameStep::Scale(), GameStep::Ticks(), F(c + 0x8C), F(c + 0x74), F(c + 0x5C), F(c + 0xA4));
            }

            lastYaw[i] = yaw;
            lastWork[i] = m;
        }
    }

    if (!enabled || std::chrono::steady_clock::now() - last < std::chrono::seconds(1))
        return;

    last = std::chrono::steady_clock::now();
    const uint32_t count = Cheats::MoveWorkCount(Cheats::Kind::Monster);

    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t m = Cheats::MoveWork(Cheats::Kind::Monster, i);

        if (m && Memory::Read16(m))
            std::fprintf(stderr, "MH3 monsters: [%u] work %08x id %04x position %.1f %.1f %.1f\n", i, m, Memory::Read16(m), F(m + kMovePosition), F(m + kMovePosition + 4),
                         F(m + kMovePosition + 8));
    }
}

// MH3_TRACE=camera-blends: how long each camera blend ran, in ticks of game
// time, read a step late at the next field camera update, and on how many of
// its steps the values it blends moved (a blend that moves only on ticks
// steps at 30 Hz): the field camera's reset (+0x42; the eye), the collision
// camera's release blends (+0x100 height, +0x101 sideways; their offsets),
// the aim view's blend (+0x234 steps left; its eye +0x1F0), the zone camera's
// blend (the zone +0x42; the region's kind, +1; the zone's eye +0x00 and
// distance and angles +0x60), the scope zoom (+0x1DC, +0x1E0 changing) and a
// monster's hold (BindCamera's count +0x114; the eye and target).
constexpr uint32_t kZone = 0x348; // the zone camera (0x802BA400)

void ReportBlends(uint32_t cam) {
    static const bool enabled = TraceOptions().Has("camera-blends");

    if (!enabled)
        return;

    struct Words {
        uint32_t offset, count;
    };

    struct Blend {
        const char* name;
        Words watched[2];
        double began;
        uint32_t steps, moved, last;
    };

    static Blend blends[] = {
        {"reset", {{kEye, 3}, {0, 0}}, -1, 0, 0, 0},
        {"collision height", {{0xC8, 1}, {0xE0, 1}}, -1, 0, 0, 0},
        {"collision sideways", {{0xC4, 3}, {0xDC, 3}}, -1, 0, 0, 0},
        {"aim", {{0x1F0, 3}, {0, 0}}, -1, 0, 0, 0},
        {"zone", {{kZone, 3}, {kZone + 0x60, 4}}, -1, 0, 0, 0},
        {"zoom", {{0x1DC, 2}, {0, 0}}, -1, 0, 0, 0},
        {"bind", {{kEye, 3}, {kTarget, 3}}, -1, 0, 0, 0},
    };

    static float zoom[2], zoomFrom[2];
    const float zoomNow[2] = {F(cam + 0x1DC), F(cam + 0x1E0)};
    const uint32_t region = R32(cam + kZone + 0x58);
    const bool active[] = {
        Memory::Read8(cam + kReset) != 0,
        Memory::Read8(cam + 0x100) != 0,
        Memory::Read8(cam + 0x101) != 0,
        int16_t(Memory::Read16(cam + 0x234)) > 0,
        Memory::Read8(cam + kZone + 0x42) != 0,
        zoomNow[0] != zoom[0] || zoomNow[1] != zoom[1],
        int32_t(R32(cam + 0x114)) > 0,
    };

    for (size_t i = 0; i < std::size(blends); ++i) {
        Blend& blend = blends[i];
        uint32_t hash = 0;

        for (const Words& words : blend.watched)
            for (uint32_t k = 0; k < words.count; ++k)
                hash = hash * 31 + R32(cam + words.offset + 4 * k);

        if (active[i] && blend.began < 0) {
            blend.began = GameStep::Elapsed();
            blend.steps = blend.moved = 0;

            if (i == 5) {
                zoomFrom[0] = zoom[0];
                zoomFrom[1] = zoom[1];
            }
        } else if (active[i]) {
            ++blend.steps;
            blend.moved += hash != blend.last;
        } else if (blend.began >= 0) {
            const double ticks = GameStep::Elapsed() - blend.began;
            char what[64] = "";

            if (i == 4)
                std::snprintf(what, sizeof(what), " (kind %u)", region ? Memory::Read8(region + 1) : 0xFFu);
            else if (i == 5)
                std::snprintf(what, sizeof(what), " %.2f %.2f -> %.2f %.2f", zoomFrom[0], zoomFrom[1], zoomNow[0], zoomNow[1]);

            std::fprintf(stderr, "MH3 camera blend: %s%s for %.1f ticks from tick %.1f, moved on %u of %u steps\n", blend.name, what, ticks, blend.began, blend.moved, blend.steps);
            blend.began = -1;
        }

        blend.last = hash;
    }

    zoom[0] = zoomNow[0];
    zoom[1] = zoomNow[1];
}
#endif
} // namespace

static void FieldCameraUpdatePort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x100);
    GuestCall game{ctx};
    const uint32_t cam = frame.Arg(3);
    const uint32_t plw = R32(cam + kPlayerWork);
    const uint32_t player = frame[164], offset = frame[176], local = frame[140];
    const auto act = [&](uint32_t kind, uint32_t action) {
        return game.Call<ActionCheck>({plw, kind, action});
    };
#ifdef MH3_DEVELOPMENT
    ReportBlends(cam);
#endif

    if (Memory::Read8(plw) == 0 || (game.Call<GetOnlineMode>() & 0xFF) == 2)
        return;

    game.Call<CameraFollowMode>({cam, plw});
    Memory::Write8(cam + kFlags, 0);

    switch (game.Call<OptionConfig>({Memory::Read8(plw + kPlwPlayer), 8}) & 0xFF) {
    case 1:
        Memory::Write8(cam + kFlags, Memory::Read8(cam + kFlags) | 2);
        break;
    case 2:
        Memory::Write8(cam + kFlags, Memory::Read8(cam + kFlags) | 1);
        break;
    case 3:
        Memory::Write8(cam + kFlags, Memory::Read8(cam + kFlags) | 3);
        break;
    }

    const uint32_t view = Memory::Read8(cam + kView);

    if (view != 0xFF && view != 2 && view != 3) {
        game.Call<CameraPosition>({frame[92]});
        game.Call<VecCopy>({frame[128], frame[92]});
        game.Call<CameraTarget>({frame[80]});
        game.Call<VecCopy>({frame[116], frame[80]});
        const uint32_t yaw = game.Call<AngleBetween>({frame[116], frame[128]});
        W32(cam + kYaw, yaw);
        W32(cam + kYawTarget, yaw);
    }

    if (Memory::Read8(cam + kTargetMode) != 0)
        game.Call<CameraTargetMode>({cam, plw});

    game.Call<VecCopy>({player, plw + kPlwPosition});

    if (game.Call<CameraClimbMode>({cam}) == 1) {
        W32(cam + kRow, kRowClimb);
    } else {
        game.Call<CameraPitchInput>({cam, cam, plw});

        if (game.Call<PlCarried>({plw}) == 1)
            W32(cam + kRow, kRowCarried);
        else if (Memory::Read8(plw + kPlwState) == 3)
            W32(cam + kRow, kRowsSwimming + Memory::Read8(cam + kDistance) * 28);
        else if (Memory::Read8(plw + kPlwState) == 1 && Memory::Read8(cam + kDistance) == 2)
            W32(cam + kRow, kRowRidingMid);
        else
            W32(cam + kRow, kRows + Memory::Read8(cam + kDistance) * 28);
    }

    game.Call<CameraYawInput>({cam, cam, plw});

    // Entering view 2: a reset blend, and the distance row nearest the
    // orbit's height.
    if (Memory::Read8(cam + kView) == 2 && Memory::Read8(cam + kFollowing) == 0) {
        Memory::Write8(cam + kReset, 1);
        Memory::Write16(cam + kResetLeft, 19);
        WF(cam + kResetRate, F(kOne) / float(20));
        game.Call<VecSubtract>({frame[68], cam + kOrbit, player});
        game.Call<VecCopy>({frame[104], frame[68]});
        float best = F(kNearest);
        const float height = F(frame[108]);
        uint32_t row = 0;

        for (uint32_t i = 1; i <= 4; ++i) {
            const float diff = F(kRows + 4 + 28 * i) - height;
            const float d = diff * diff;

            if (best > d) {
                row = i;
                best = d;
            }
        }

        if (Memory::Read8(plw + kPlwState) == 3) {
            const uint32_t pitch = game.Call<AngleX>({frame[104]});
            W32(cam + kPitch, pitch);
            W32(cam + kPitchTarget, pitch);
        } else if ((game.Call<OptionConfig>({Memory::Read8(plw + kPlwPlayer), 11}) & 0xFF) != 0) {
            Memory::Write8(cam + kDistance, uint8_t(row));
            W32(cam + kRow, kRows + row * 28);
        }
    }

    // Pitch levelling, by GameStep::Share of the original's amount a step.
    uint32_t pitchShift = 2;
    Memory::Write8(cam + kLookUp, 0);
    const bool manual = Memory::Read8(cam + kManualPitch) != 0 || Memory::Read8(cam + kPitchHold) != 0;

    if ((game.Call<OptionConfig>({Memory::Read8(plw + kPlwPlayer), 10}) & 0xFF) == 1 && Memory::Read8(cam + kPitchSnap) == 1) {
        pitchShift = 3;
    } else if (game.Call<LookingUp>({plw}) != 0) {
        if (!manual) {
            Memory::Write8(cam + kLookUp, 1);
            const uint32_t step = uint32_t(GameStep::Share(Memory::Read8(cam + kFast) == 0 ? 48 : 144));
            const uint32_t pitch = R32(cam + kPitchTarget);

            if (int16_t(pitch) < -0x1000) {
                W32(cam + kPitchTarget, pitch + step);

                if (int16_t(pitch + step) > -0x1000) {
                    W32(cam + kPitchTarget, 0xF000);
                    Memory::Write8(cam + kLookUp, 2);
                }
            } else {
                W32(cam + kPitchTarget, pitch - step);

                if (int16_t(pitch - step) < -0x1000) {
                    W32(cam + kPitchTarget, 0xF000);
                    Memory::Write8(cam + kLookUp, 2);
                }
            }
        }

        W32(cam + kTiltTarget, 0);
    } else {
        if (!manual) {
            const uint32_t step = uint32_t(GameStep::Share(Memory::Read8(cam + kFast) == 0 ? 10 : 30));
            const uint32_t pitch = R32(cam + kPitchTarget);
            const int16_t angle = int16_t(pitch);

            if (angle > 0 && angle < 2560) {
                W32(cam + kPitchTarget, pitch - step);

                if (int16_t(pitch - step) <= 0)
                    W32(cam + kPitchTarget, 0);
            } else if (angle < 0 && angle > -2560) {
                W32(cam + kPitchTarget, pitch + step);

                if (int16_t(pitch + step) >= 0)
                    W32(cam + kPitchTarget, 0);
            }
        }

        if (act(0, 27) == 0 && act(1, 18) == 0 && Memory::Read8(plw + kPlwAction) != 4)
            W32(cam + kTiltTarget, 0);
    }

    W32(cam + kPitch, R32(cam + kPitch) + (game.Call<CameraAngleEase>({R32(cam + kPitchTarget), R32(cam + kPitch), pitchShift}) & 0xFFFF));
    W32(cam + kYaw, R32(cam + kYaw) + game.Call<CameraAngleEase>({R32(cam + kYawTarget), R32(cam + kYaw), 2}));
    W32(cam + kTilt, R32(cam + kTilt) + game.Call<CameraAngleEase>({R32(cam + kTiltTarget), R32(cam + kTilt), 5}));

    // A snap is done once the angle is within 256 of it.
    if (Memory::Read8(cam + kYawSnap) != 0 && ((Memory::Read16(cam + kYawSnap + 1) - R32(cam + kYaw)) & 0xFFFF) - 256 > 65024) {
        Memory::Write8(cam + kYawSnap, 0);
        Memory::Write16(cam + kYawSnap + 1, 0);
    }

    if (Memory::Read8(cam + kPitchSnap) != 0 && ((Memory::Read16(cam + kPitchSnap + 2) - R32(cam + kPitch)) & 0xFFFF) - 256 > 65024) {
        Memory::Write8(cam + kPitchSnap, 0);
        Memory::Write16(cam + kPitchSnap + 2, 0);
    }

    // The target behind the player, its height from the row.
    const uint32_t zero = R32(kZero);
    W32(offset, zero);
    W32(offset + 4, zero);
    W32(offset + 8, zero);
    W32(cam + kTargetWanted, R32(player));
    W32(cam + kTargetWanted + 8, R32(player + 8));
    float easeTarget, easeEye;

    if (act(0, 6) != 0 || act(0, 7) != 0) {
        const uint32_t row = R32(cam + kRow);
        WF(offset + 4, F(row + 4) - F(kSitDrop));
        W32(offset + 8, R32(row + 8));
        game.Call<VecSet>({local}, {F(kZero), F(kZero), F(kBack)});
        game.Call<RotY>({local, R32(plw + kPlwYaw)});
        game.Call<VecSubtract>({frame[56], player, local});
        game.Call<VecCopy>({local, frame[56]});
        W32(cam + kTargetWanted, R32(local));
        WF(cam + kTargetWanted + 4, F(kSitHeight) + F(player + 4));
        W32(cam + kTargetWanted + 8, R32(local + 8));
        easeTarget = F(kEaseSit);
    } else {
        const uint32_t row = R32(cam + kRow);
        W32(offset + 4, R32(row + 4));

        if (Memory::Read8(plw + kPlwState) == 3)
            W32(offset + 4, act(1, 21) == 1 ? R32(kHeightSwim) : zero);

        W32(offset + 8, R32(R32(cam + kRow) + 8));

        if (act(1, 21) == 1)
            WF(cam + kTargetWanted + 4, F(kHeightSwim) + F(player + 4));
        else if (game.Call<PlStateCheck>({plw, 2}) == 1)
            WF(cam + kTargetWanted + 4, F(kHeightState) + F(player + 4));
        else
            WF(cam + kTargetWanted + 4, F(player + 4) + F(R32(cam + kRow) + 16));

        easeTarget = F(kEaseTarget);
    }

    easeEye = F(kEaseEye);

    if (game.Call<CameraEvent>({plw, 0}) == 1) {
        W32(cam + kYawTarget, (R32(plw + kPlwYaw) + 0x8000) & 0xFFFF);
        Memory::Write8(cam + kDistance, 2);
    }

    if (game.Call<CameraEvent>({plw, 1}) == 1) {
        game.Call<JointPosition>({plw, 1, player});
        game.Call<VecSet>({local}, {F(kZero), F(kJointUp), F(kBack)});
        game.Call<RotY>({local, R32(plw + kPlwYaw)});
        game.Call<VecAdd>({frame[44], player, local});
        game.Call<VecCopy>({cam + kTargetWanted, frame[44]});
    }

    // Swimming pitch and the settle timer, in whole ticks.
    const auto settleDown = [&](int32_t floor) {
        const int32_t left = int16_t(Memory::Read16(cam + kSettle));

        if (left <= floor)
            return false;

        Memory::Write16(cam + kSettle, uint16_t(std::max(left - int32_t(GameStep::Ticks()), floor)));
        return true;
    };

    if (Memory::Read8(plw + kPlwState) == 3) {
        if (int16_t(Memory::Read16(cam + kSettle)) == 0) {
            Memory::Write16(cam + kSettle, 1);
            W32(cam + kPitch, 0xF000);
            W32(cam + kPitchTarget, act(0, 151) == 1 ? 0 : R32(cam + kPitch));
        }

        if (act(1, 20) != 0) {
            if (game.Call<FrameCheck>({plw, 0}, {F(kSwimFrame), F(kZero)}) != 0 && int8_t(Memory::Read8(cam + kPitchRamp)) == 0)
                W32(cam + kPitchTarget, 0);

            Memory::Write16(cam + kSettle, 5);
        } else if (act(1, 21) != 0) {
            W32(cam + kPitchTarget, 0);
            Memory::Write16(cam + kSettle, 10);
        }

        if (settleDown(1)) {
            easeEye = F(kEaseSwimEye);
            easeTarget = F(kEaseSwimTarget);
        } else {
            easeEye = F(kOne);
            easeTarget = easeEye;
        }

        game.Call<RotX>({offset, R32(cam + kPitch) + R32(cam + kTilt)});
    } else if (settleDown(0)) {
        easeEye = F(kEaseSit);
        easeTarget = easeEye;
    }

    game.Call<RotY>({offset, R32(cam + kYaw)});
    game.Call<VecAdd>({frame[32], offset, player});
    game.Call<VecCopy>({cam + kEyeWanted, frame[32]});
    Memory::Write8(cam + kPush, 0);
    game.Call<CameraCollide>({cam, plw});

    // The target and eye ease toward their wanted heights over this step.
    W32(cam + kTarget, R32(cam + kTargetWanted));

    if (Memory::Read8(cam + kInstant) == 2)
        W32(cam + kTarget + 4, R32(cam + kTargetWanted + 4));
    else
        EaseHeight(cam + kTarget + 4, cam + kTargetWanted + 4, easeTarget);

    W32(cam + kTarget + 8, R32(cam + kTargetWanted + 8));
    W32(cam + kEye, R32(cam + kEyeWanted));

    if (Memory::Read8(cam + kInstant) == 2)
        W32(cam + kEye + 4, R32(cam + kEyeWanted + 4));
    else
        EaseHeight(cam + kEye + 4, cam + kEyeWanted + 4, easeEye);

    W32(cam + kEye + 8, R32(cam + kEyeWanted + 8));

    if (Memory::Read8(cam + kPush) != 0)
        game.Call<CameraPush>({cam});

    game.Call<VecAddInPlace>({cam + kEye, cam + kShakeEye});
    game.Call<VecAddInPlace>({cam + kTarget, cam + kShakeTarget});
    game.Call<BindCamera>({cam, plw});
    game.Call<CameraLimit>({cam, cam, plw});

    if (game.Call<CameraEvent>({plw, 3}) == 1) {
        game.Call<VecCopy>({cam + kEye, cam + kOldEye});
        game.Call<VecCopy>({cam + kTarget, cam + kOldTarget});
    }

    // A reset blends from the old eye and target: each tick keeps t of the
    // old position (t falls to 0 over the 19 ticks); a step keeps t^s.
    uint8_t reset = Memory::Read8(cam + kReset);

    if (reset == 1) {
        game.Call<CameraPosition>({frame[20]});
        game.Call<VecCopy>({cam + kOldEye, frame[20]});
        game.Call<CameraTarget>({frame[8]});
        game.Call<VecCopy>({cam + kOldTarget, frame[8]});
        Memory::Write8(cam + kReset, uint8_t(reset + 1));
    }

    if (reset == 1 || reset == 2) {
        const int32_t left = int16_t(Memory::Read16(cam + kResetLeft));
        game.Call<ResetCurve>({uint32_t(left)}, {F(cam + kResetRate), F(kResetShape)});
        float keep = game.F1();
        const double s = GameStep::Scale();

        if (s != 1.0 && keep > 0.0f)
            keep = float(std::pow(double(keep), s));

        const int32_t next = left - int32_t(GameStep::Ticks());
        Memory::Write16(cam + kResetLeft, uint16_t(next > 0 ? next : 0));
        game.Call<Lerp>({cam + kEye, cam + kOldEye, cam + kEye}, {keep});
        game.Call<Lerp>({cam + kTarget, cam + kOldTarget, cam + kTarget}, {keep});
    }

    game.Call<VecCopy>({cam + kOldEye, cam + kEye});
    game.Call<VecCopy>({cam + kOldTarget, cam + kTarget});
#ifdef MH3_DEVELOPMENT
    ReportMonsters(game);

    // MH3_INPUT=camera=N[:D]: the camera looks at monster N instead, from the
    // game's eye or from D units away on the line to it (raised by D/3); the
    // camera work keeps its own eye and target, so the game's eases are
    // unchanged.
    float distance = 0.0f;

    if (const int monster = DebugCameraMonster(&distance); monster >= 0) {
        if (const uint32_t m = Cheats::MoveWork(Cheats::Kind::Monster, uint32_t(monster)); m && Memory::Read16(m)) {
            const uint32_t target = frame[8], eye = frame[20];
            game.Call<VecCopy>({target, m + kMovePosition});
            game.Call<VecCopy>({eye, cam + kEye});

            if (distance > 0.0f) {
                float away[3], length = 0.0f;

                for (unsigned k = 0; k < 3; ++k) {
                    away[k] = F(cam + kEye + 4 * k) - F(target + 4 * k);
                    length += away[k] * away[k];
                }

                length = length > 0.0f ? std::sqrt(length) : 1.0f;

                for (unsigned k = 0; k < 3; ++k)
                    Memory::WriteFloat32(eye + 4 * k, F(target + 4 * k) + away[k] / length * distance + (k == 1 ? distance / 3.0f : 0.0f));
            }

            game.Call<CameraSet>({0, eye, target}, {F(cam + kFovA), F(cam + kFovB)});
            return;
        }
    }
#endif
    game.Call<CameraSet>({0, cam + kEye, cam + kTarget}, {F(cam + kFovA), F(cam + kFovB)});
}

// 0x802B8ED0 BindCamera (camera, PLW), each step after the field camera
// update: while a monster holds the player (0x8012D4FC; kinds +3 1, 2, 4 and
// 7) it moves the eye and target along the hold's path. A count of steps
// since the hold began (+0x114) picks the time in keyed paths (kinds 4 and 7,
// two phases at +0x144) or drives eases of an orbit (kinds 1 and 2: the yaw
// +0x11A and pitch +0x11C by 1/n of the gap a step, the offset +0x138). All
// of it counted steps, so at 400 fps the path ran 13 times as fast. Here the
// original runs once per tick of the step. Past the last tick (the step's
// fraction) it runs once more for the next tick's place, its state
// (+0x114..+0x145) put back after, and the eye, target and field of view sit
// that fraction of the way from the last tick's place to the next: the path
// moves on every step, at the Wii's speed.
constexpr uint32_t kBindSteps = 0x114, kBindStateEnd = 0x146, kBindMode = 0x7C;

// The words a hold places: the eye, the target, the field of view (+0x34)
// and the yaws (+0x68, +0x70; the angle from the target to the eye).
struct BindPlace {
    float eye[3], target[3], fov;
    uint32_t yawTarget, yaw;
    uint8_t mode;

    void Read(uint32_t cam) {
        for (uint32_t i = 0; i < 3; ++i) {
            eye[i] = F(cam + kEye + 4 * i);
            target[i] = F(cam + kTarget + 4 * i);
        }

        fov = F(cam + kFovA);
        yawTarget = R32(cam + kYawTarget);
        yaw = R32(cam + kYaw);
        mode = Memory::Read8(cam + kBindMode);
    }

    void Write(uint32_t cam) const {
        for (uint32_t i = 0; i < 3; ++i) {
            WF(cam + kEye + 4 * i, eye[i]);
            WF(cam + kTarget + 4 * i, target[i]);
        }

        WF(cam + kFovA, fov);
        W32(cam + kYawTarget, yawTarget);
        W32(cam + kYaw, yaw);
        Memory::Write8(cam + kBindMode, mode);
    }
};

extern "C" void func_802B8ED0(CpuContext* ctx);

// One tick of the original; true when it placed the eye and target (a hold
// past its delay). The eye's x is marked first to tell: no path reads it.
static bool BindTick(CpuContext* ctx, uint32_t cam, uint32_t plw) {
    constexpr uint32_t kUnplaced = 0x7FBADBAD; // a NaN no path writes
    const uint32_t x = R32(cam + kEye);

    W32(cam + kEye, kUnplaced);
    ctx->gpr[3] = cam;
    ctx->gpr[4] = plw;
    func_802B8ED0(ctx);

    if (R32(cam + kEye) != kUnplaced)
        return true;

    W32(cam + kEye, x);
    return false;
}

static void BindCameraPort(CpuContext* ctx) {
    const uint32_t cam = ctx->gpr[3], plw = ctx->gpr[4], ticks = GameStep::Ticks();
    const float fraction = float(GameStep::Fraction());
    static bool placed = false; // the last tick placed the eye and target
    static BindPlace last;
#ifdef MH3_DEVELOPMENT
    // MH3_TRACE=bind-camera: each hold's length in ticks of game time and
    // the count it reached, and each change of its state (+0x118), phase
    // (+0x144) and next-phase signal (+0x145, set by 0x802B95B0).
    static const bool traced = TraceOptions().Has("bind-camera");
    static double began = -1;
    static int32_t reached = 0;
    const int32_t before = int32_t(Memory::Read32(cam + kBindSteps));
    const uint32_t flags = Memory::Read8(cam + 0x118) << 16 | Memory::Read8(cam + 0x144) << 8 | Memory::Read8(cam + 0x145);
#endif

    for (uint32_t i = 0; i < ticks; ++i) {
        placed = BindTick(ctx, cam, plw);

        if (placed)
            last.Read(cam);
    }

    if (ticks == 0 || fraction > 0) {
        uint8_t state[kBindStateEnd - kBindSteps];
        BindPlace now;

        for (uint32_t i = 0; i < sizeof(state); ++i)
            state[i] = Memory::Read8(cam + kBindSteps + i);

        now.Read(cam);
        const bool next = BindTick(ctx, cam, plw);

        for (uint32_t i = 0; i < sizeof(state); ++i)
            Memory::Write8(cam + kBindSteps + i, state[i]);

        if (placed && next) {
            GuestCall game{ctx};

            for (uint32_t i = 0; i < 3; ++i) {
                WF(cam + kEye + 4 * i, last.eye[i] + (F(cam + kEye + 4 * i) - last.eye[i]) * fraction);
                WF(cam + kTarget + 4 * i, last.target[i] + (F(cam + kTarget + 4 * i) - last.target[i]) * fraction);
            }

            WF(cam + kFovA, last.fov + (F(cam + kFovA) - last.fov) * fraction);
            const uint32_t yaw = game.Call<AngleBetween>({cam + kTarget, cam + kEye});
            W32(cam + kYawTarget, yaw);
            W32(cam + kYaw, yaw);
        } else if (placed) {
            last.Write(cam);
        } else {
            now.Write(cam);
        }
    }

#ifdef MH3_DEVELOPMENT
    if (traced) {
        const int32_t after = int32_t(Memory::Read32(cam + kBindSteps));
        const uint32_t now = Memory::Read8(cam + 0x118) << 16 | Memory::Read8(cam + 0x144) << 8 | Memory::Read8(cam + 0x145);

        if (now != flags)
            std::fprintf(stderr, "MH3 bind camera: tick %.2f (%u ticks) state %d -> %d, phase %u -> %u, signal %u -> %u, count %d -> %d\n", GameStep::Elapsed(), ticks, int8_t(flags >> 16),
                         int8_t(now >> 16), (flags >> 8) & 0xFF, (now >> 8) & 0xFF, flags & 0xFF, now & 0xFF, before, after);

        if (before <= 0 && after > 0)
            began = GameStep::Elapsed() - 1;

        reached = std::max(reached, after);

        if (before > 0 && after == 0 && began >= 0) {
            std::fprintf(stderr, "MH3 bind camera: hold of %.1f ticks, count %d\n", GameStep::Elapsed() - began, reached);
            began = -1;
            reached = 0;
        }
    }
#endif
}

extern "C" void MH3_BindCamera(CpuContext* ctx) {
    PortCheck::Call(ctx, "BindCamera", func_802B8ED0, BindCameraPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x802B8ED0, MH3_BindCamera, "BindCamera");

// 0x802BB0B0 CameraResetCurve (r3 steps left; f1 1 / length, f2 power) -> f1,
// the share of the old pose a camera blend keeps: with x = left / length,
// (2x)^p / 2 below 1/2 and 1 - (2(1 - x))^p / 2 above, an ease in and out.
// The collision camera's release blends (CameraCollide 0x802B8244), the
// offset blend (0x802B9C68) and the zone camera's angle and point blends
// (0x802BAC0C, 0x802BAE68) count their steps left down right after the call,
// once a tick (tick sites), so the weight held between ticks and at a high
// frame rate those blends moved the camera 30 times a second. For these calls
// the count is read at the step's game time: the step's n ticks would have
// used left .. left - n + 1, and a step ending f of a tick past its last tick
// is f of the way on to the next, kept between the blend's first count and 0.
// Other calls (the field camera's reset, FieldCameraUpdatePort) keep it.
struct TimedCurve {
    uint32_t lr;
    float first; // the blend's first count, less its length
};

constexpr TimedCurve kTimedCurves[] = {
    {0x802B884C, 0.0f},  // CameraCollide: release blends (u8 +0x102, +0x103)
    {0x802B9F4C, 0.0f},  // offset blend (s16 +0x234)
    {0x802BACE4, -1.0f}, // zone camera, kind 0: distance and angles (s16 zone +0x44)
    {0x802BAF18, -1.0f}, // zone camera, kind 1: the eye's point
};

static void CameraResetCurvePort(CpuContext* ctx) {
    const float rate = float(ctx->fpr[1].d), power = float(ctx->fpr[2].d);
    const bool stock = GameStep::Ticks() == 1 && GameStep::Fraction() == 0.0;
    float left = float(int32_t(ctx->gpr[3]));

    for (const TimedCurve& curve : kTimedCurves) {
        if (curve.lr != ctx->lr)
            continue;

        if (!stock && rate > 0.0f) {
            const float first = std::max(std::nearbyint(1.0f / rate) + curve.first, 0.0f);
            left = std::clamp(left + 1.0f - float(GameStep::Ticks()) - float(GameStep::Fraction()), 0.0f, first);
        }

        break;
    }

    const float x = left * rate;
    float keep;

    if (x < 0.5f) {
        const float d = 2.0f * x;
        keep = 0.5f * float(std::pow(double(d), double(power)));
    } else {
        const float d = 2.0f * (1.0f - x);
        keep = 1.0f - 0.5f * float(std::pow(double(d), double(power)));
    }

    ctx->fpr[1].d = double(keep);
}

extern "C" void func_802BB0B0(CpuContext* ctx);
extern "C" void MH3_CameraResetCurve(CpuContext* ctx) {
    if (PortCheck::Enabled("CameraResetCurve")) [[unlikely]]
        PortCheck::RunPure(ctx, "CameraResetCurve", PortCheck::Returns::Fpr1, func_802BB0B0, CameraResetCurvePort);
    else
        CameraResetCurvePort(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x802BB0B0, MH3_CameraResetCurve, "CameraResetCurve");

// 0x802B9C68 AimCamera (camera), each step after the field camera: the
// gunner's aim view (the aim pitch PLW +0x64F). While it is on (+0x232, and
// +0x9E this step) its yaw offset (s16 +0x23E) eases toward +-0x800 (the sign
// of +0x240) by 6/64 of the gap a step, so at 400 fps it swung 13 times as
// fast. Here the offset is first moved back so that the original's step lands
// where a step of s ticks would, with the gap shrunk by (58/64)^s; if the
// original did not ease it, it is put back. The 9 steps blending the view in
// (+0x234) are a tick site. Unverified in play: never aimed at a variable
// step.
constexpr uint32_t kAimOn = 0x232, kAimWanted = 0x9E, kAimOffset = 0x23E, kAimSide = 0x240;

extern "C" void func_802B9C68(CpuContext* ctx);

static void AimCameraPort(CpuContext* ctx) {
    const uint32_t cam = ctx->gpr[3];
    const double s = GameStep::Scale();
    const int32_t offset = int16_t(Memory::Read16(cam + kAimOffset));
    const int32_t goal = int16_t(Memory::Read16(cam + kAimSide)) < 0 ? -0x800 : 0x800;
    const bool on = Memory::Read8(cam + kAimOn) != 0 && Memory::Read8(cam + kAimWanted) != 0;
    const bool easing = s != 1.0 && on && (goal < 0 ? offset > goal : offset < goal);
    int32_t from = offset;

    if (easing) {
        from = std::clamp(goal - int32_t(std::lround((goal - offset) * std::pow(58.0 / 64.0, s) * 64.0 / 58.0)), -0x8000, 0x7FFF);
        Memory::Write16(cam + kAimOffset, uint16_t(from));
    }

    func_802B9C68(ctx);

    if (easing && int16_t(Memory::Read16(cam + kAimOffset)) == from)
        Memory::Write16(cam + kAimOffset, uint16_t(offset));
}

extern "C" void MH3_AimCamera(CpuContext* ctx) {
    PortCheck::Call(ctx, "AimCamera", func_802B9C68, AimCameraPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x802B9C68, MH3_AimCamera, "AimCamera");

extern "C" void func_802B5CBC(CpuContext* ctx);
extern "C" void MH3_FieldCameraUpdate(CpuContext* ctx) {
    PortCheck::Call(ctx, "FieldCameraUpdate", func_802B5CBC, FieldCameraUpdatePort);
}
REGISTER_NATIVE_FUNCTION_AS(0x802B5CBC, MH3_FieldCameraUpdate, "FieldCameraUpdate");
