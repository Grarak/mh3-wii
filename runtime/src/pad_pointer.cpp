// The Wii Remote pointer, RMHP08. C reconstructions of the pad module's
// pointer code, decompiled with Ghidra (functions with paired-single
// prologues from the translation):
//
// The game carries its own copy of KPAD, the SDK's Wii Remote library, for
// channel 0, in the KPADInsideStatus at 0x8065D310 (kpad.c's names):
//   0x80044FF4  KpadPosScale (kp): calc_dpd2pos_scale
//   0x800450AC  KpadInit (): the status cleared, the sensor bar position and
//               IR sensitivity from the system settings, then KpadReset
//   0x800451DC  KpadReset (): reset_kpad, and the sample ring's indices
//   0x80045720  KpadReadAcc (sample): read_kpad_acc
//   0x800441E4  KpadClamp (f1 value, f2 limit) -> f1
//   0x800453B0  KpadPlay (kp, f32* value; f1 sample): the play-radius ease
//   0x80045464  KpadAccHorizon (kp): calc_acc_horizon
//   0x800455F8  KpadAccVertical (kp): calc_acc_vertical
//   0x800468A0  KpadReadDpd (sample): read_kpad_dpd, then the game's pointer
//               (small-data: valid, x and y in -1..1)
//   0x800458BC  KpadGetObjects (kp, &obj[3]): get_kobj
//   0x80045960  KpadObjectsOutside (kp, samples): check_kobj_outside_frame
//   0x800459D8  KpadObjectsSame (samples): check_kobj_same_position
//   0x80045A4C  KpadSelect2Continue (kp) -> 2 or 0: select_2obj_continue
//   0x80045CA8  KpadSelect1Continue (kp) -> +-1 or 0: select_1obj_continue
//   0x80045E48  KpadPairDirection (kp, a, b, VEC2* out) -> f1 distance
//   0x80045F14  KpadSelect2First (kp) -> 2 or 0: select_2obj_first
//   0x80046074  KpadSelect1First (kp) -> -1 or 0: select_1obj_first
//   0x80046220  KpadPairHorizon (kp): calc_obj_horizon
//   0x800462EC  KpadDpdVariable (kp, s8 found): calc_dpd_variable
//
// The game's pointer on top of it:
//   0x80044DDC  PadPointerRead (): KPAD over the Wii Remote samples since the
//               last step (the 100-entry sampling rings of PadConnectCallback)
//   0x800440BC  PadPointerStart (): the pointer in use this step; the first
//               call centres it, later ones switch the IR camera on and move
//               the cursor
//   0x80044BA0  PadPointerOn (u8 channel): WPADControlDpd and the data format
//               with IR
//   0x80044C68  PadPointerTimeout (u8 channel): the IR camera off after 300
//               steps without PadPointerStart (PollControllers)
//   0x80043F10  PadPointerToScreen (): -1..1 to screen pixels, and a 16-entry
//               history
//   0x80043D1C  PadPointerSmooth (): the cursor eases towards the pointer
//   0x80043C10  PadPointerReset ()
//
// The pointing in the village and the field (log_pointer.cpp) is their only
// user. Two things count steps: the switch-off wait counts whole ticks here
// (GameStep::Ticks()), and the cursor closes 1 - (3/4)^s of its gap over a
// step of s ticks instead of a quarter a step. At the stock 1/30 s step both
// are the original.
#include "game_step.h"
#include "guest_call.h"
#include "port_check.h"
#include "ps_vec_math.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace {

constexpr uint32_t kKpad = 0x8065D310; // KPADInsideStatus, 0x1B8 bytes

// KPADInsideStatus.
enum : uint32_t {
    kAcc = 0x0C,             // VEC3, eased by the play radius
    kAccValue = 0x18,
    kAccSpeed = 0x1C,
    kPos = 0x20,             // VEC2, -1..1
    kVec = 0x28,
    kSpeed = 0x30,
    kHorizon = 0x34,         // VEC2
    kHoriVec = 0x3C,
    kHoriSpeed = 0x44,
    kDist = 0x48,
    kDistVec = 0x4C,
    kDistSpeed = 0x50,
    kAccVertical = 0x54,     // VEC2
    kDpdValid = 0x5E,        // s8: 2 two dots, 1 one; negative: the other one guessed
    kPosPlay = 0xB0,         // f32 radius, sensitivity: pos, hori, dist, acc
    kHoriPlay = 0xB8,
    kDistPlay = 0xC0,
    kAccPlay = 0xC8,
    kDistUnit = 0xD0,        // the distance KpadReset starts from
    kScreenRotation = 0xD4,  // VEC2 (0, -1)
    kSensorRotation = 0xDC,  // VEC2 (1, 0)
    kCenter = 0xE4,          // VEC2: the sensor bar's place (y -+0.2 above or below the screen)
    kPosScale = 0xEC,
    kSamples = 0xF0,         // KObj[4], this sample's dots
    kPair = 0x120,           // KObj[2], the two dots tracked
    kObjects = 0x138,        // s16: samples seen
    kPairLength = 0x13C,
    kPairDir = 0x140,        // VEC2, unit
    kPairDist = 0x148,
    kTwoDotLength = 0x14C,   // the pair's length after 200 two-dot samples
    kRawAcc = 0x150,         // VEC3
    kPairHorizon = 0x15C,    // VEC2: kPairDir turned by kSensorRotation
    kAccHorizon = 0x164,     // VEC2
    kAhCircle = 0x16C,       // VEC2
    kAhCircleCount = 0x174,  // u16: samples left before kAccHorizon is trusted
    kTwoDotCount = 0x176,    // u8
    kAccScale = 0x178,       // f32[3]: 1 / gravity unit
    kFrame = 0x184,          // f32 low x, low y, high x, high y
    kDistSpeedUp = 0x194,    // f32 +-1 / kp_err_dist_speed
    kDistSpeedDown = 0x198,
    kAhCircleRadius2 = 0x19C,
    kDistScale = 0x1A0,      // the distance of a pair of length 1
    kDistMin = 0x1A4,
    kPosMode = 0x1A8,        // s32: 0 eased inside the play radius, else held there
    kHoriMode = 0x1AC,
    kDistMode = 0x1B0,
    kAccMode = 0x1B4,
    kKpadSize = 0x1B8,
};

// KObj (12 bytes).
enum : uint32_t {
    kObjX = 0,
    kObjY = 4,
    kObjError = 8, // s8: 0 seen, -1 none, 1 outside the frame, 2 a duplicate
    kObjState = 9, // s8: 0 seen, -1 placed from the other dot
    kObjSize = 12,
};

// kpad.c's tunables (small data).
enum : uint32_t {
    kDistUnitInit = 0x80793348,       // f32 1.0
    kScreenRotationInit = 0x80793350, // VEC2
    kSensorRotationInit = 0x80793358, // VEC2
    kObjInterval = 0x80793360,        // f32 0.2
    kAccHorizonPw = 0x80793364,       // f32 0.05
    kAhCirclePw = 0x80793368,         // f32 0.06
    kAhCircleCt = 0x8079336C,         // u16 100
    kAhCircleRadius = 0x80793370,     // f32 0.07
    kErrOutsideFrame = 0x80793374,    // f32 0.05
    kErrDistMax = 0x80793378,         // f32: 1 + the IR sensitivity (KpadInit)
    kErrDistSpeed = 0x8079337C,       // f32 0.04
    kErrFirstInpr = 0x80793380,       // f32 0.9
    kErrNextInpr = 0x80793384,        // f32 0.9
    kErrAccInpr = 0x80793388,         // f32 0.9
    kErrUpInpr = 0x8079338C,          // f32 0.7
    kErrNearPos = 0x80793390,         // f32 0.1
    kAccMax = 0x80793394,             // f32 3.4
    kCenterInit = 0x80796CB8,         // VEC2
    kErrDistMin = 0x80796CC0,         // f32: interval / tan (KpadInit)
    kDistScaleInit = 0x80796CC4,
    kVec2Zero = 0x80796CC8,
    kTanHalfView = 0x80798000,        // f32 0.383864
    kNoGravityScale = 0x80797FF0,     // f32 0.01
    kUnitGravity = 0x80797FE8,        // s16[3] (1, 1, 1)
};

// The game's pointer (small data).
enum : uint32_t {
    kRingRead = 0x80796CD0,       // u16: the first sample of this step
    kRingLatest = 0x80796CD2,     // u16
    kPointerValid = 0x80796CD4,   // s32: 0 none, 1 on screen, 2 appeared this step
    kPointerWasValid = 0x80796CD8,
    kCentreX = 0x80796CDC,        // f32 half the screen
    kCentreY = 0x80796CE0,
    kCentreCopy = 0x80796CE4,     // f32[2]
    kPointerX = 0x80796CEC,       // f32 -1..1
    kPointerY = 0x80796CF0,
    kIrOn = 0x80796CF4,           // u8
    kPointerUsed = 0x80796CF5,    // u8: PadPointerStart this step
    kIrOffWait = 0x80796CF8,      // s32 steps (ticks here)
    kPointerStarted = 0x80796D08, // u8
    kPointerD0C = 0x80796D0C,     // s32
    kPointerD10 = 0x80796D10,     // s16[4]
    kGap = 0x80796D18,            // f32: the cursor's distance to the pointer
    kDeadZone = 0x80796D1C,       // f32 8 pixels
    kPointerFrozen = 0x80796D20,  // u8
    kPointerD21 = 0x80796D21,     // u8
};

constexpr uint32_t kHistoryX = 0x8066B8F8, kHistoryY = 0x8066B918; // s16[16] screen pixels
constexpr uint32_t kCursor = 0x8066B938, kCursorVel = 0x8066B944;  // VEC2 screen pixels
constexpr uint32_t kScreenWidth = 0x8065B4C0, kScreenHeight = 0x8065B4C4;
constexpr uint32_t kCursorEase = 0x80797FD8; // f32 0.25
constexpr uint32_t kDeadZoneInit = 0x80797FD4; // f32 8.0

// The Wii Remote samples (WPADSetAutoSamplingBuf), 100 each.
constexpr uint32_t kClassicRing = 0x8065D4C8, kClassicSampleSize = 0x36;
constexpr uint32_t kNunchukRing = 0x80662928, kNunchukSampleSize = 0x32;
constexpr uint32_t kRingLength = 100;

enum : uint32_t {
    GetControlType = 0x80043BAC,       // get_ControlType (channel) -> u8: 0 remote, 1 Nunchuk, 2 Classic, 255 none
    WPADGetDataFormat = 0x804EF250,    // (channel) -> format
    WPADGetAccGravityUnit = 0x804EEE30, // (channel, 0, s16[3]*)
    WPADIsDpdEnabled = 0x804F1A30,     // (channel) -> 0/1
    WPADControlDpd = 0x804F1B20,       // (channel, command, callback) -> 0 done
    WPADSetDataFormat = 0x804EF2A0,    // (channel, format)
    WPADGetDpdSensitivity = 0x804F19F0, // () -> u8
    WPADGetSensorBarPosition = 0x804EDD00, // () -> u8: 1 above the screen
};

float Float(uint32_t a) {
    return Memory::ReadFloat32(a);
}

void SetFloat(uint32_t a, float v) {
    Memory::WriteFloat32(a, v);
}

float Sqrt(float v) {
    return float(std::sqrt(double(v)));
}

void CopyVec2(uint32_t to, uint32_t from) {
    Memory::Write32(to, Memory::Read32(from));
    Memory::Write32(to + 4, Memory::Read32(from + 4));
}

void CopyObject(uint32_t to, uint32_t from) {
    CopyVec2(to + kObjX, from + kObjX);

    for (uint32_t i = 8; i < kObjSize; ++i)
        Memory::Write8(to + i, Memory::Read8(from + i));
}

int8_t ObjectError(uint32_t obj) {
    return int8_t(Memory::Read8(obj + kObjError));
}

bool InsideFrame(uint32_t kp, float x, float y) {
    return Float(kp + kFrame) < x && x < Float(kp + kFrame + 8) && Float(kp + kFrame + 4) < y && y < Float(kp + kFrame + 12);
}

// (d / radius)^4 inside the play radius, 1 outside.
float PlayFactor(float d, float radius) {
    if (d >= radius)
        return 1.0f;

    float f = d / radius;
    f = f * f;
    return f * f;
}

// The share of a gap `d` an eased value moves by this sample, from its play
// radius and sensitivity (f32 at `play`): mode 0 eases inside the radius,
// mode 1 holds there (false) and follows only what lies beyond it.
bool Pull(uint32_t play, uint32_t mode, float d, float& pull) {
    const float radius = Float(play), sensitivity = Float(play + 4);

    if (Memory::Read32(mode) == 0) {
        pull = PlayFactor(d, radius) * sensitivity;
        return true;
    }

    if (d > radius) {
        pull = sensitivity * ((d - radius) / d);
        return true;
    }

    return false;
}

} // namespace

static float KpadClamp(float value, float limit) {
    if (value < 0.0f)
        return value < -limit ? -limit : value;

    return value > limit ? limit : value;
}

static void KpadPlay(uint32_t kp, uint32_t value, float sample) {
    const float old = Float(value);
    const float diff = sample - old;
    const float radius = Float(kp + kAccPlay), sensitivity = Float(kp + kAccPlay + 4);

    if (Memory::Read32(kp + kAccMode) == 0) {
        SetFloat(value, old + PlayFactor(diff < 0.0f ? -diff : diff, radius) * sensitivity * diff);
    } else if (diff < -radius) {
        SetFloat(value, old + sensitivity * (diff + radius));
    } else if (diff > radius) {
        SetFloat(value, old + sensitivity * (diff - radius));
    }
}

static void KpadPosScale(uint32_t kp) {
    const float diagonal = Sqrt(1.0f * 1.0f + 0.75f * 0.75f);
    const float x = Float(kp + kCenter), y = Float(kp + kCenter + 4);
    const float across = x < 0.0f ? 1.0f + x : 1.0f - x;
    const float down = y < 0.0f ? 0.75f + y : 0.75f - y;
    SetFloat(kp + kPosScale, diagonal / (across < down ? across : down));
}

static void KpadReset() {
    const uint32_t kp = kKpad;
    const float margin = Float(kErrOutsideFrame);
    SetFloat(kp + kFrame, -1.0f + margin);
    SetFloat(kp + kFrame + 8, 1.0f - margin);
    SetFloat(kp + kFrame + 4, -0.75f + margin);
    SetFloat(kp + kFrame + 12, 0.75f - margin);
    const float speed = Float(kErrDistSpeed);
    SetFloat(kp + kDistSpeedUp, 1.0f / speed);
    SetFloat(kp + kDistSpeedDown, -1.0f / speed);
    const float radius = Float(kAhCircleRadius);
    SetFloat(kp + kAhCircleRadius2, radius * radius);
    Memory::Write32(kp + kDistMin, Memory::Read32(kErrDistMin));
    Memory::Write32(kp + kDistScale, Memory::Read32(kDistScaleInit));
    Memory::Write8(kp + kDpdValid, 0);
    Memory::Write8(kp + kTwoDotCount, 0);

    CopyVec2(kp + kVec, kVec2Zero);
    CopyVec2(kp + kPos, kp + kVec);
    SetFloat(kp + kSpeed, 0.0f);

    for (uint32_t horizon : {kPairHorizon, kAccHorizon, kHorizon}) {
        SetFloat(kp + horizon, 1.0f);
        SetFloat(kp + horizon + 4, 0.0f);
    }

    CopyVec2(kp + kHoriVec, kVec2Zero);
    SetFloat(kp + kHoriSpeed, 0.0f);
    SetFloat(kp + kAccVertical, 1.0f);
    SetFloat(kp + kAccVertical + 4, 0.0f);

    const float unit = Float(kp + kDistUnit);
    SetFloat(kp + kDist, unit);
    SetFloat(kp + kDistSpeed, 0.0f);
    SetFloat(kp + kDistVec, 0.0f);
    SetFloat(kp + kPairDist, unit);
    const float length = Float(kp + kDistScale) / unit;
    SetFloat(kp + kTwoDotLength, length);
    SetFloat(kp + kPairLength, length);
    CopyVec2(kp + kPairDir, kp + kSensorRotation);

    SetFloat(kp + kAcc + 8, 0.0f);
    SetFloat(kp + kAcc, 0.0f);
    SetFloat(kp + kAcc + 4, -1.0f);
    SetFloat(kp + kAccValue, 1.0f);
    SetFloat(kp + kAccSpeed, 0.0f);

    for (uint32_t i = 0; i < 3; ++i)
        Memory::Write32(kp + kRawAcc + i * 4, Memory::Read32(kp + kAcc + i * 4));

    CopyVec2(kp + kAhCircle, kp + kAccHorizon);
    Memory::Write16(kp + kAhCircleCount, Memory::Read16(kAhCircleCt));
    Memory::Write16(kp + kObjects, 0);

    for (uint32_t i = 0; i < 4; ++i)
        Memory::Write8(kp + kSamples + i * kObjSize + kObjError, 0xFF);

    for (uint32_t i = 0; i < 2; ++i)
        Memory::Write8(kp + kPair + i * kObjSize + kObjError, 0xFF);

    Memory::Write16(kRingRead, 0);
    Memory::Write16(kRingLatest, 0);
}

static void KpadInit(GuestCall& game) {
    const uint32_t kp = kKpad;
    std::memset(Memory::GetPointer(kp, kKpadSize), 0, kKpadSize);
    SetFloat(kErrDistMax, 1.0f + float(game.Call<WPADGetDpdSensitivity>() & 0xFF));
    Memory::Write32(kp + kDistUnit, Memory::Read32(kDistUnitInit));
    CopyVec2(kp + kScreenRotation, kScreenRotationInit);
    CopyVec2(kp + kSensorRotation, kSensorRotationInit);
    CopyVec2(kp + kCenter, kCenterInit);
    SetFloat(kp + kCenter + 4, (game.Call<WPADGetSensorBarPosition>() & 0xFF) == 1 ? 0.2f : -0.2f);
    SetFloat(kp + kCenter, 0.0f);
    SetFloat(kp + kCenter + 4, -Float(kp + kCenter + 4));
    KpadPosScale(kp);

    for (uint32_t play : {kPosPlay, kHoriPlay, kDistPlay, kAccPlay}) {
        SetFloat(kp + play, 0.0f);
        SetFloat(kp + play + 4, 1.0f);
    }

    for (uint32_t mode : {kPosMode, kHoriMode, kDistMode, kAccMode})
        Memory::Write32(kp + mode, 0);

    const float scale = Float(kObjInterval) / Float(kTanHalfView);
    SetFloat(kErrDistMin, scale);
    SetFloat(kDistScaleInit, scale);
    KpadReset();
}

static void KpadAccHorizon(uint32_t kp) {
    const float ax = Float(kp + kRawAcc), ay = Float(kp + kRawAcc + 4);
    float length = Sqrt(ax * ax + ay * ay);

    if (length == 0.0f || length >= 2.0f)
        return;

    const float nx = ax / length, ny = ay / length;

    if (length > 1.0f)
        length = 2.0f - length;

    const float pull = length * (length * Float(kAccHorizonPw));
    const float rx = Float(kp + kScreenRotation), ry = Float(kp + kScreenRotation + 4);
    const float hx = rx * nx + ry * ny, hy = ry * nx - rx * ny;
    const float ox = Float(kp + kAccHorizon), oy = Float(kp + kAccHorizon + 4);
    const float x = ox + pull * (hx - ox), y = oy + pull * (hy - oy);
    const float norm = Sqrt(x * x + y * y);

    if (norm == 0.0f)
        return;

    const float ux = x / norm, uy = y / norm;
    SetFloat(kp + kAccHorizon, ux);
    SetFloat(kp + kAccHorizon + 4, uy);

    // The horizon is trusted once it stays inside a small circle for a while.
    const float pw = Float(kAhCirclePw);
    const float cx = Float(kp + kAhCircle) + pw * (ux - Float(kp + kAhCircle));
    SetFloat(kp + kAhCircle, cx);
    const float cy = Float(kp + kAhCircle + 4) + pw * (uy - Float(kp + kAhCircle + 4));
    SetFloat(kp + kAhCircle + 4, cy);
    const float dx = ux - cx, dy = uy - cy;

    if (dx * dx + dy * dy <= Float(kp + kAhCircleRadius2)) {
        if (const uint16_t count = Memory::Read16(kp + kAhCircleCount))
            Memory::Write16(kp + kAhCircleCount, uint16_t(count - 1));
    } else {
        Memory::Write16(kp + kAhCircleCount, Memory::Read16(kAhCircleCt));
    }
}

static void KpadAccVertical(uint32_t kp) {
    const float ax = Float(kp + kRawAcc), ay = Float(kp + kRawAcc + 4);
    const float across = ax * ax + ay * ay;
    const float horizontal = Sqrt(across);
    const float vz = -Float(kp + kRawAcc + 8);
    float length = Sqrt(across + vz * vz);

    if (length == 0.0f || length >= 2.0f)
        return;

    const float nx = horizontal / length, ny = vz / length;

    if (length > 1.0f)
        length = 2.0f - length;

    const float pull = length * (length * Float(kAccHorizonPw));
    const float ox = Float(kp + kAccVertical), oy = Float(kp + kAccVertical + 4);
    const float x = ox + pull * (nx - ox), y = oy + pull * (ny - oy);
    const float norm = Sqrt(x * x + y * y);

    if (norm == 0.0f)
        return;

    SetFloat(kp + kAccVertical, x / norm);
    SetFloat(kp + kAccVertical + 4, y / norm);
}

static void KpadReadAcc(uint32_t sample) {
    const uint32_t kp = kKpad;
    const float limit = Float(kAccMax);
    const auto axis = [&](uint32_t offset, bool negate, uint32_t scale) {
        const int32_t raw = int16_t(Memory::Read16(sample + offset));
        return KpadClamp(float(negate ? -raw : raw) * Float(kp + kAccScale + scale), limit);
    };

    // KPAD's frame: x left, y towards the screen's bottom, z out of the face.
    SetFloat(kp + kRawAcc, axis(2, true, 0));
    SetFloat(kp + kRawAcc + 4, axis(6, true, 8));
    SetFloat(kp + kRawAcc + 8, axis(4, false, 4));

    float previous[3];

    for (uint32_t i = 0; i < 3; ++i)
        previous[i] = Float(kp + kAcc + i * 4);

    for (uint32_t i = 0; i < 3; ++i)
        KpadPlay(kp, kp + kAcc + i * 4, Float(kp + kRawAcc + i * 4));

    const float x = Float(kp + kAcc), y = Float(kp + kAcc + 4), z = Float(kp + kAcc + 8);
    SetFloat(kp + kAccValue, Sqrt(z * z + (x * x + y * y)));
    const float dx = previous[0] - x, dy = previous[1] - y, dz = previous[2] - z;
    SetFloat(kp + kAccSpeed, Sqrt(dz * dz + (dx * dx + dy * dy)));
    KpadAccHorizon(kp);
    KpadAccVertical(kp);
}

// The dots of a WPADStatus (x 0..1023, y 0..767, size 0 for none), last first,
// as -1..1 across and -0.75..0.75 down.
static void KpadGetObjects(uint32_t kp, uint32_t last) {
    for (uint32_t i = 0; i < 4; ++i) {
        const uint32_t obj = last - i * 8, k = kp + kSamples + (3 - i) * kObjSize;

        if (Memory::Read16(obj + 4) == 0) {
            Memory::Write8(k + kObjError, 0xFF);
            continue;
        }

        SetFloat(k + kObjX, 0.001953125f * float(int16_t(Memory::Read16(obj))) - 0.9990234375f);
        SetFloat(k + kObjY, 0.001953125f * float(int16_t(Memory::Read16(obj + 2))) - 0.7490234375f);
        Memory::Write8(k + kObjError, 0);
        Memory::Write8(k + kObjState, 0);
    }
}

static void KpadObjectsOutside(uint32_t kp, uint32_t samples) {
    for (uint32_t i = 4; i-- > 0;) {
        const uint32_t obj = samples + i * kObjSize;

        if (ObjectError(obj) >= 0 && !InsideFrame(kp, Float(obj + kObjX), Float(obj + kObjY)))
            Memory::Write8(obj + kObjError, Memory::Read8(obj + kObjError) | 1);
    }
}

static void KpadObjectsSame(uint32_t samples) {
    for (uint32_t i = 0; i < 3; ++i) {
        const uint32_t a = samples + i * kObjSize;

        if (ObjectError(a) != 0)
            continue;

        for (uint32_t j = i + 1; j < 4; ++j) {
            const uint32_t b = samples + j * kObjSize;

            if (ObjectError(b) == 0 && Float(a + kObjX) == Float(b + kObjX) && Float(a + kObjY) == Float(b + kObjY))
                Memory::Write8(b + kObjError, 2);
        }
    }
}

// The pair (a, b): its direction turned by the sensor rotation into out[2],
// and its distance.
static float KpadPairDirection(uint32_t kp, uint32_t a, uint32_t b, float out[2]) {
    float dx = Float(b + kObjX) - Float(a + kObjX), dy = Float(b + kObjY) - Float(a + kObjY);
    const float inverse = 1.0f / Sqrt(dx * dx + dy * dy);
    dx = dx * inverse;
    dy = dy * inverse;
    const float sx = Float(kp + kSensorRotation), sy = Float(kp + kSensorRotation + 4);
    out[0] = sx * dx + sy * dy;
    out[1] = sy * dx - sx * dy;
    return Float(kp + kDistScale) * inverse;
}

// Two dots, the pair kept from the last sample: the pair whose distance and
// direction moved least.
static int8_t KpadSelect2Continue(uint32_t kp) {
    float best = 2.0f;
    uint32_t first = 0, second = 0;
    const uint32_t samples = kp + kSamples;

    for (uint32_t a = samples; a < samples + 3 * kObjSize; a += kObjSize) {
        if (ObjectError(a) != 0)
            continue;

        for (uint32_t b = a + kObjSize; b <= samples + 3 * kObjSize; b += kObjSize) {
            if (ObjectError(b) != 0)
                continue;

            const float dx = Float(b + kObjX) - Float(a + kObjX), dy = Float(b + kObjY) - Float(a + kObjY);
            const float inverse = 1.0f / Sqrt(dx * dx + dy * dy);
            const float nx = dx * inverse, ny = dy * inverse;
            float dist = inverse * Float(kp + kDistScale);

            if (dist <= Float(kp + kDistMin) || dist >= Float(kErrDistMax))
                continue;

            dist = dist - Float(kp + kPairDist);
            dist = dist * Float(kp + (dist < 0.0f ? kDistSpeedDown : kDistSpeedUp));

            if (dist >= 1.0f)
                continue;

            float dot = Float(kp + kPairDir) * nx + Float(kp + kPairDir + 4) * ny;
            const bool reversed = dot < 0.0f;

            if (reversed)
                dot = -dot;

            const float limit = Float(kErrNextInpr);

            if (dot <= limit)
                continue;

            const float score = dist + (1.0f - dot) / (1.0f - limit);

            if (score < best) {
                best = score;
                first = reversed ? b : a;
                second = reversed ? a : b;
            }
        }
    }

    if (best == 2.0f)
        return 0;

    CopyObject(kp + kPair, first);
    CopyObject(kp + kPair + kObjSize, second);
    return 2;
}

// One dot near one of the pair's: the other one placed from the last
// two-dot length along the accelerometer's horizon.
static int8_t KpadSelect1Continue(uint32_t kp) {
    const float near = Float(kErrNearPos);
    float best = near * near;
    uint32_t chosen = 0, source = 0;

    for (uint32_t p = kp + kPair; p < kp + kPair + 2 * kObjSize; p += kObjSize) {
        if (ObjectError(p) != 0 || int8_t(Memory::Read8(p + kObjState)) != 0)
            continue;

        for (uint32_t s = kp + kSamples; s < kp + kPair; s += kObjSize) {
            if (ObjectError(s) != 0)
                continue;

            const float dx = Float(p + kObjX) - Float(s + kObjX), dy = Float(p + kObjY) - Float(s + kObjY);
            const float d = dx * dx + dy * dy;

            if (d < best) {
                best = d;
                chosen = p;
                source = s;
            }
        }
    }

    if (best == near * near)
        return 0;

    CopyObject(chosen, source);
    const float sx = Float(kp + kSensorRotation), sy = Float(kp + kSensorRotation + 4);
    const float ax = Float(kp + kAccHorizon), ay = Float(kp + kAccHorizon + 4);
    const float hx = sx * ax + sy * ay, hy = sy * ax - sx * ay;
    SetFloat(kp + kPairDir, hx);
    SetFloat(kp + kPairDir + 4, hy);
    const float length = Float(kp + kPairLength);
    const float ox = length * hx, oy = length * hy;
    uint32_t placed;

    if (chosen == kp + kPair) {
        placed = kp + kPair + kObjSize;
        SetFloat(placed + kObjX, Float(chosen + kObjX) + ox);
        SetFloat(placed + kObjY, Float(chosen + kObjY) + oy);
    } else {
        placed = kp + kPair;
        SetFloat(placed + kObjX, Float(chosen + kObjX) - ox);
        SetFloat(placed + kObjY, Float(chosen + kObjY) - oy);
    }

    Memory::Write8(placed + kObjError, 0);
    Memory::Write8(placed + kObjState, 0xFF);
    return int8_t(Memory::Read8(kp + kDpdValid)) < 0 ? -1 : 1;
}

// Two dots, no pair yet: the pair lying closest to the accelerometer's
// horizon.
static int8_t KpadSelect2First(uint32_t kp) {
    const float limit = Float(kErrFirstInpr);
    float best = limit;
    uint32_t first = 0, second = 0;
    const uint32_t samples = kp + kSamples;

    for (uint32_t a = samples; a < samples + 3 * kObjSize; a += kObjSize) {
        if (ObjectError(a) != 0)
            continue;

        for (uint32_t b = a + kObjSize; b <= samples + 3 * kObjSize; b += kObjSize) {
            if (ObjectError(b) != 0)
                continue;

            float direction[2];
            const float dist = KpadPairDirection(kp, a, b, direction);

            if (dist <= Float(kp + kDistMin) || dist >= Float(kErrDistMax))
                continue;

            const float dot = Float(kp + kAccHorizon) * direction[0] + Float(kp + kAccHorizon + 4) * direction[1];

            if (dot < 0.0f) {
                if (-dot > best) {
                    best = -dot;
                    first = b;
                    second = a;
                }
            } else if (dot > best) {
                best = dot;
                first = a;
                second = b;
            }
        }
    }

    if (best == Float(kErrFirstInpr))
        return 0;

    CopyObject(kp + kPair, first);
    CopyObject(kp + kPair + kObjSize, second);
    return 2;
}

// One dot, no pair yet: the other one placed along the accelerometer's
// horizon where only one side of it would be outside the camera's view.
static int8_t KpadSelect1First(uint32_t kp) {
    const float sx = Float(kp + kSensorRotation), sy = Float(kp + kSensorRotation + 4);
    const float ax = Float(kp + kAccHorizon), ay = Float(kp + kAccHorizon + 4);
    const float length = Float(kp + kTwoDotLength);
    const float ox = (sx * ax + sy * ay) * length, oy = (sy * ax - sx * ay) * length;

    for (uint32_t s = kp + kSamples; s < kp + kPair; s += kObjSize) {
        if (ObjectError(s) != 0)
            continue;

        const float x = Float(s + kObjX), y = Float(s + kObjY);
        const float bx = x - ox, by = y - oy, ex = x + ox, ey = y + oy;

        if (!InsideFrame(kp, bx, by)) {
            if (InsideFrame(kp, ex, ey)) {
                CopyObject(kp + kPair + kObjSize, s);
                SetFloat(kp + kPair + kObjX, bx);
                SetFloat(kp + kPair + kObjY, by);
                Memory::Write8(kp + kPair + kObjError, 0);
                Memory::Write8(kp + kPair + kObjState, 0xFF);
                return -1;
            }
        } else if (!InsideFrame(kp, ex, ey)) {
            CopyObject(kp + kPair, s);
            SetFloat(kp + kPair + kObjSize + kObjX, ex);
            SetFloat(kp + kPair + kObjSize + kObjY, ey);
            Memory::Write8(kp + kPair + kObjSize + kObjError, 0);
            Memory::Write8(kp + kPair + kObjSize + kObjState, 0xFF);
            return -1;
        }
    }

    return 0;
}

static void KpadPairHorizon(uint32_t kp) {
    float dx = Float(kp + kPair + kObjSize + kObjX) - Float(kp + kPair + kObjX);
    float dy = Float(kp + kPair + kObjSize + kObjY) - Float(kp + kPair + kObjY);
    const float length = Sqrt(dx * dx + dy * dy);
    SetFloat(kp + kPairLength, length);
    const float inverse = 1.0f / length;
    SetFloat(kp + kPairDist, Float(kp + kDistScale) * inverse);
    dx = dx * inverse;
    SetFloat(kp + kPairDir, dx);
    dy = dy * inverse;
    SetFloat(kp + kPairDir + 4, dy);
    const float sx = Float(kp + kSensorRotation), sy = Float(kp + kSensorRotation + 4);
    SetFloat(kp + kPairHorizon, sx * dx + sy * dy);
    SetFloat(kp + kPairHorizon + 4, sy * dx - sx * dy);
}

// Moves a unit vector towards (tx, ty) by its play radius and sensitivity,
// renormalised; its change and the change's length to `vec` and `speed`.
static void PlayUnit(uint32_t value, uint32_t vec, uint32_t speed, uint32_t play, uint32_t mode, float tx, float ty) {
    const float dx = tx - Float(value), dy = ty - Float(value + 4);
    float pull;

    if (!Pull(play, mode, Sqrt(dx * dx + dy * dy), pull)) {
        CopyVec2(vec, kVec2Zero);
        SetFloat(speed, 0.0f);
        return;
    }

    float x = Float(value) + pull * dx, y = Float(value + 4) + pull * dy;
    const float norm = Sqrt(x * x + y * y);
    x = x / norm;
    y = y / norm;
    const float vx = x - Float(value), vy = y - Float(value + 4);
    SetFloat(vec, vx);
    SetFloat(vec + 4, vy);
    SetFloat(speed, Sqrt(vx * vx + vy * vy));
    SetFloat(value, x);
    SetFloat(value + 4, y);
}

static void KpadDpdVariable(uint32_t kp, int8_t found) {
    if (found == 0) {
        Memory::Write8(kp + kDpdValid, 0);
        return;
    }

    const bool first = int8_t(Memory::Read8(kp + kDpdValid)) == 0;
    const float px = Float(kp + kPairDir), py = Float(kp + kPairDir + 4);
    const float sx = Float(kp + kSensorRotation), sy = Float(kp + kSensorRotation + 4);

    // The horizon: the pair's direction.
    const float hx = sx * px + sy * py, hy = sy * px - sx * py;

    if (first) {
        SetFloat(kp + kHorizon, hx);
        SetFloat(kp + kHorizon + 4, hy);
        CopyVec2(kp + kHoriVec, kVec2Zero);
        SetFloat(kp + kHoriSpeed, 0.0f);
    } else {
        PlayUnit(kp + kHorizon, kp + kHoriVec, kp + kHoriSpeed, kp + kHoriPlay, kp + kHoriMode, hx, hy);
    }

    // The distance: from the pair's length.
    const float dist = Float(kp + kDistScale) / Float(kp + kPairLength);

    if (first) {
        SetFloat(kp + kDist, dist);
        SetFloat(kp + kDistVec, 0.0f);
        SetFloat(kp + kDistSpeed, 0.0f);
    } else {
        const float diff = dist - Float(kp + kDist);
        float pull;

        if (Pull(kp + kDistPlay, kp + kDistMode, diff < 0.0f ? -diff : diff, pull)) {
            const float step = pull * diff;
            SetFloat(kp + kDistVec, step);
            SetFloat(kp + kDistSpeed, step < 0.0f ? -step : step);
            SetFloat(kp + kDist, Float(kp + kDist) + step);
        } else {
            SetFloat(kp + kDistVec, 0.0f);
            SetFloat(kp + kDistSpeed, 0.0f);
        }
    }

    // The position: the pair's middle, turned upright, centred on the sensor
    // bar and scaled so the screen spans -1..1.
    const float mx = 0.5f * (Float(kp + kPair + kObjX) + Float(kp + kPair + kObjSize + kObjX));
    const float my = 0.5f * (Float(kp + kPair + kObjY) + Float(kp + kPair + kObjSize + kObjY));
    const float c = px * sx + py * sy, s = -py * sx + px * sy;
    const float u = c * mx - s * my, v = s * mx + c * my;
    const float scale = Float(kp + kPosScale);
    const float bx = scale * (Float(kp + kCenter) - u), by = scale * (Float(kp + kCenter + 4) - v);
    const float rx = Float(kp + kScreenRotation), ry = Float(kp + kScreenRotation + 4);
    const float tx = -ry * bx + rx * by, ty = -rx * bx - ry * by;

    if (first) {
        SetFloat(kp + kPos, tx);
        SetFloat(kp + kPos + 4, ty);
        CopyVec2(kp + kVec, kVec2Zero);
        SetFloat(kp + kSpeed, 0.0f);
    } else {
        const float dx = tx - Float(kp + kPos), dy = ty - Float(kp + kPos + 4);
        float pull;

        if (Pull(kp + kPosPlay, kp + kPosMode, Sqrt(dx * dx + dy * dy), pull)) {
            const float vx = pull * dx, vy = pull * dy;
            SetFloat(kp + kVec, vx);
            SetFloat(kp + kVec + 4, vy);
            SetFloat(kp + kSpeed, Sqrt(vx * vx + vy * vy));
            SetFloat(kp + kPos, Float(kp + kPos) + vx);
            SetFloat(kp + kPos + 4, Float(kp + kPos + 4) + vy);
        } else {
            CopyVec2(kp + kVec, kVec2Zero);
            SetFloat(kp + kSpeed, 0.0f);
        }
    }

    Memory::Write8(kp + kDpdValid, uint8_t(found));
}

static void KpadReadDpd(GuestCall& game, uint32_t sample) {
    const uint32_t kp = kKpad;

    if (game.Call<WPADIsDpdEnabled>({0}) == 0) {
        for (uint32_t i = 0; i < 4; ++i)
            Memory::Write8(kp + kSamples + i * kObjSize + kObjError, 0xFF);
    } else {
        KpadGetObjects(kp, sample + 0x20);
    }

    KpadObjectsOutside(kp, kp + kSamples);
    KpadObjectsSame(kp + kSamples);
    int16_t count = 0;

    for (uint32_t i = 0; i < 4; ++i)
        count += ObjectError(kp + kSamples + i * kObjSize) == 0;

    Memory::Write16(kp + kObjects, uint16_t(count));
    int8_t found = 0;

    // Only a remote held roughly level (not pointing at the floor or the
    // ceiling) tracks dots.
    if (Float(kp + kAccVertical) > Float(kErrUpInpr)) {
        const int8_t valid = int8_t(Memory::Read8(kp + kDpdValid));

        if (valid == 2 || valid == -2) {
            if (count >= 2)
                found = KpadSelect2Continue(kp);

            if (found == 0 && count >= 1)
                found = KpadSelect1Continue(kp);
        } else if (valid == 1 || valid == -1) {
            if (count >= 2)
                found = KpadSelect2First(kp);

            if (found == 0 && count >= 1)
                found = KpadSelect1Continue(kp);
        } else {
            if (count >= 2)
                found = KpadSelect2First(kp);

            if (found == 0 && count == 1)
                found = KpadSelect1First(kp);
        }
    }

    if (found == 0) {
        Memory::Write8(kp + kTwoDotCount, 0);
    } else {
        KpadPairHorizon(kp);

        // A pair lying across the accelerometer's (trusted) horizon is not the
        // sensor bar.
        const float dot = Float(kp + kPairHorizon) * Float(kp + kAccHorizon) + Float(kp + kPairHorizon + 4) * Float(kp + kAccHorizon + 4);

        if (Memory::Read16(kp + kAhCircleCount) == 0 && dot <= Float(kErrAccInpr)) {
            found = 0;
            Memory::Write8(kp + kPair + kObjSize + kObjError, 1);
            Memory::Write8(kp + kPair + kObjError, 1);
        }

        if (int8_t(Memory::Read8(kp + kDpdValid)) == 2 && found == 2) {
            const uint8_t samples = Memory::Read8(kp + kTwoDotCount);

            if (samples == 200)
                SetFloat(kp + kTwoDotLength, Float(kp + kPairLength));
            else
                Memory::Write8(kp + kTwoDotCount, uint8_t(samples + 1));
        } else {
            Memory::Write8(kp + kTwoDotCount, 0);
        }
    }

    KpadDpdVariable(kp, found);

    // The game's pointer: on screen while dots are tracked and the position
    // is inside -1..1.
    const float x = Float(kp + kPos), y = Float(kp + kPos + 4);
    const bool outside = x < -1.0f || x > 1.0f || y < -1.0f || y > 1.0f;
    Memory::Write32(kPointerValid, found != 0 && !outside ? 1 : 0);
    SetFloat(kPointerX, x < -1.0f ? -1.0f : x > 1.0f ? 1.0f : x);
    SetFloat(kPointerY, y < -1.0f ? -1.0f : y > 1.0f ? 1.0f : y);
}

static void PadPointerReadPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x40);
    GuestCall game{ctx};
    Memory::Write32(kPointerWasValid, Memory::Read32(kPointerValid));

    if ((game.Call<GetControlType>({0}) & 0xFF) != 0xFF) {
        const uint32_t format = game.Call<WPADGetDataFormat>({0});

        if (format == 8 || format == 5) {
            const uint32_t unit = frame[8];
            Memory::Write32(unit, Memory::Read32(kUnitGravity));
            Memory::Write16(unit + 4, Memory::Read16(kUnitGravity + 4));
            game.Call<WPADGetAccGravityUnit>({0, 0, unit});
            const int32_t x = int16_t(Memory::Read16(unit)), y = int16_t(Memory::Read16(unit + 2)), z = int16_t(Memory::Read16(unit + 4));

            if (uint32_t(z) * uint32_t(x) * uint32_t(y) == 0) {
                for (uint32_t i = 0; i < 3; ++i)
                    Memory::Write32(kKpad + kAccScale + i * 4, Memory::Read32(kNoGravityScale));
            } else {
                SetFloat(kKpad + kAccScale, 1.0f / float(x));
                SetFloat(kKpad + kAccScale + 4, 1.0f / float(y));
                SetFloat(kKpad + kAccScale + 8, 1.0f / float(z));
            }

            const uint32_t ring = format == 8 ? kClassicRing : kNunchukRing;
            const uint32_t size = format == 8 ? kClassicSampleSize : kNunchukSampleSize;

            for (uint32_t i = Memory::Read16(kRingRead); i != Memory::Read16(kRingLatest); i = (i + 1) % kRingLength) {
                KpadReadAcc(ring + i * size);
                KpadReadDpd(game, ring + i * size);
            }
        }
    }

    if (Memory::Read32(kPointerValid) != 0 && Memory::Read32(kPointerWasValid) == 0)
        Memory::Write32(kPointerValid, 2);
}

static void PadPointerResetPort(CpuContext*) {
    for (uint32_t i = 0; i < 16; ++i) {
        Memory::Write16(kHistoryX + i * 2, 0);
        Memory::Write16(kHistoryY + i * 2, 0);
    }

    Memory::Write8(kPointerStarted, 0);

    for (uint32_t i = 0; i < 4; ++i)
        Memory::Write16(kPointerD10 + i * 2, 0);

    Memory::Write32(kPointerD0C, 0);
    SetFloat(kCursor, Float(kScreenWidth) * 0.5f);
    SetFloat(kCursor + 4, Float(kScreenHeight) * 0.5f);
    SetFloat(kCursorVel, 0.0f);
    SetFloat(kCursorVel + 4, 0.0f);
    SetFloat(kGap, 0.0f);
    Memory::Write32(kDeadZone, Memory::Read32(kDeadZoneInit));
    Memory::Write8(kPointerFrozen, 0);
    Memory::Write8(kPointerD21, 0);
}

// The cursor follows the newest pointer position, a quarter of the gap a
// tick (1 - (3/4)^s over s ticks), and rests within the dead zone.
static void PadPointerSmoothPort(CpuContext*) {
    if (Memory::Read8(kPointerFrozen) != 0)
        return;

    const float x = float(int16_t(Memory::Read16(kHistoryX))), y = float(int16_t(Memory::Read16(kHistoryY)));

    if (Memory::Read32(kPointerWasValid) == 0) {
        SetFloat(kCursor, x);
        SetFloat(kCursor + 4, y);
        return;
    }

    const float gap = PsVec::Mag(Float(kCursor) - x, Float(kCursor + 4) - y, 0.0f);
    SetFloat(kGap, gap);

    if (gap < Float(kDeadZone)) {
        SetFloat(kCursorVel, 0.0f);
        SetFloat(kCursorVel + 4, 0.0f);
    } else {
        const double s = GameStep::Scale();
        const float quarter = Float(kCursorEase);
        const float ease = s == 1.0 ? quarter : float(1.0 - std::pow(1.0 - double(quarter), s));
        SetFloat(kCursorVel, (x - Float(kCursor)) * ease);
        SetFloat(kCursorVel + 4, (y - Float(kCursor + 4)) * ease);
    }

    SetFloat(kCursor, Float(kCursor) + Float(kCursorVel));
    SetFloat(kCursor + 4, Float(kCursor + 4) + Float(kCursorVel + 4));
}

static void PadPointerToScreenPort(CpuContext*) {
    for (uint32_t i = 15; i > 0; --i) {
        Memory::Write16(kHistoryX + i * 2, Memory::Read16(kHistoryX + (i - 1) * 2));
        Memory::Write16(kHistoryY + i * 2, Memory::Read16(kHistoryY + (i - 1) * 2));
    }

    const float cx = Float(kCentreX), cy = Float(kCentreY);
    const float x = cx + Float(kPointerX) * cx, y = cy + Float(kPointerY) * cy;
    const float width = Float(kScreenWidth), height = Float(kScreenHeight);
    Memory::Write16(kHistoryX, uint16_t(int32_t(x < 0.0f ? 0.0f : x > width ? width : x)));
    Memory::Write16(kHistoryY, uint16_t(int32_t(y < 0.0f ? 0.0f : y > height ? height : y)));
}

static void PadPointerOnPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x10);
    GuestCall game{ctx};
    const uint32_t channel = frame.Arg(3) & 0xFF;

    if (game.Call<WPADIsDpdEnabled>({channel}) == 1) {
        Memory::Write8(kIrOn, 1);
        return;
    }

    const uint32_t type = game.Call<GetControlType>({channel}) & 0xFF;

    if (type != 1 && type != 2)
        return;

    if (game.Call<WPADControlDpd>({channel, 1, 0}) != 0) {
        Memory::Write8(kIrOn, 0);
        return;
    }

    game.Call<WPADSetDataFormat>({channel, type == 1 ? 5u : 8u});
    Memory::Write8(kIrOn, 1);
}

static void PadPointerStartPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x10);
    GuestCall game{ctx};
    Memory::Write8(kPointerUsed, 1);
    Memory::Write32(kIrOffWait, 300);

    switch (Memory::Read8(kPointerStarted)) {
    case 0: {
        // The pointer in the screen's middle (0x80044214, inlined).
        Memory::Write32(kPointerValid, 0);
        Memory::Write32(kPointerWasValid, 0);
        const float x = Float(kScreenWidth) * 0.5f, y = Float(kScreenHeight) * 0.5f;
        SetFloat(kCentreX, x);
        SetFloat(kCentreY, y);
        SetFloat(kCentreCopy, x);
        SetFloat(kCentreCopy + 4, y);
        Memory::Write8(kPointerStarted, 1);
        Memory::Write32(kPointerD0C, 16);
        SetFloat(kPointerX, 0.0f);
        SetFloat(kPointerY, 0.0f);

        for (uint32_t i = 0; i < 16; ++i) {
            Memory::Write16(kHistoryX + i * 2, 0);
            Memory::Write16(kHistoryY + i * 2, 0);
        }

        break;
    }
    case 1:
        game.Call<0x80044BA0>({0});

        if (Memory::Read32(kPointerValid) != 0 && Memory::Read8(kPointerFrozen) == 0) {
            game.Call<0x80043F10>();
            game.Call<0x80043D1C>();
        }

        break;
    }
}

// The IR camera goes off once the pointer was not used for 300 ticks.
static void PadPointerTimeoutPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x10);
    GuestCall game{ctx};
    const uint32_t channel = frame.Arg(3) & 0xFF;

    if (Memory::Read8(kPointerUsed) != 0)
        return;

    const int32_t wait = int32_t(Memory::Read32(kIrOffWait));

    if (wait > 0) {
        Memory::Write32(kIrOffWait, uint32_t(wait - int32_t(std::min<uint32_t>(GameStep::Ticks(), uint32_t(wait)))));
        return;
    }

    if (game.Call<WPADIsDpdEnabled>({channel}) == 0)
        Memory::Write8(kIrOn, 0);
    else
        Memory::Write8(kIrOn, game.Call<WPADControlDpd>({channel, 0, 0}) != 0);
}

// The ports as the game calls them (r3.. and f1.. in, r3 or f1 out).
static void KpadInitPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x10);
    GuestCall game{ctx};
    KpadInit(game);
}

static void KpadReadDpdPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x10);
    GuestCall game{ctx};
    KpadReadDpd(game, frame.Arg(3));
}

static void KpadResetPort(CpuContext*) {
    KpadReset();
}

static void KpadPosScalePort(CpuContext* ctx) {
    KpadPosScale(ctx->gpr[3]);
}

static void KpadReadAccPort(CpuContext* ctx) {
    KpadReadAcc(ctx->gpr[3]);
}

static void KpadPlayPort(CpuContext* ctx) {
    KpadPlay(ctx->gpr[3], ctx->gpr[4], float(ctx->fpr[1].d));
}

static void KpadAccHorizonPort(CpuContext* ctx) {
    KpadAccHorizon(ctx->gpr[3]);
}

static void KpadAccVerticalPort(CpuContext* ctx) {
    KpadAccVertical(ctx->gpr[3]);
}

static void KpadGetObjectsPort(CpuContext* ctx) {
    KpadGetObjects(ctx->gpr[3], ctx->gpr[4]);
}

static void KpadObjectsOutsidePort(CpuContext* ctx) {
    KpadObjectsOutside(ctx->gpr[3], ctx->gpr[4]);
}

static void KpadObjectsSamePort(CpuContext* ctx) {
    KpadObjectsSame(ctx->gpr[3]);
}

static void KpadPairHorizonPort(CpuContext* ctx) {
    KpadPairHorizon(ctx->gpr[3]);
}

static void KpadDpdVariablePort(CpuContext* ctx) {
    KpadDpdVariable(ctx->gpr[3], int8_t(ctx->gpr[4]));
}

static void KpadSelect2ContinuePort(CpuContext* ctx) {
    ctx->gpr[3] = uint32_t(int32_t(KpadSelect2Continue(ctx->gpr[3])));
}

static void KpadSelect1ContinuePort(CpuContext* ctx) {
    ctx->gpr[3] = uint32_t(int32_t(KpadSelect1Continue(ctx->gpr[3])));
}

static void KpadSelect2FirstPort(CpuContext* ctx) {
    ctx->gpr[3] = uint32_t(int32_t(KpadSelect2First(ctx->gpr[3])));
}

static void KpadSelect1FirstPort(CpuContext* ctx) {
    ctx->gpr[3] = uint32_t(int32_t(KpadSelect1First(ctx->gpr[3])));
}

static void KpadPairDirectionPort(CpuContext* ctx) {
    float out[2];
    ctx->fpr[1].d = KpadPairDirection(ctx->gpr[3], ctx->gpr[4], ctx->gpr[5], out);
    SetFloat(ctx->gpr[6], out[0]);
    SetFloat(ctx->gpr[6] + 4, out[1]);
}

static void KpadClampPort(CpuContext* ctx) {
    ctx->fpr[1].d = KpadClamp(float(ctx->fpr[1].d), float(ctx->fpr[2].d));
}

#define MH3_POINTER_PORT(address, name)                                                                                                                                                                \
    extern "C" void func_##address(CpuContext* ctx);                                                                                                                                                   \
    extern "C" void MH3_##name(CpuContext* ctx) {                                                                                                                                                      \
        PortCheck::Call(ctx, #name, func_##address, name##Port);                                                                                                                                       \
    }

MH3_POINTER_PORT(80044DDC, PadPointerRead)
REGISTER_NATIVE_FUNCTION_AS(0x80044DDC, MH3_PadPointerRead, "PadPointerRead");
MH3_POINTER_PORT(800440BC, PadPointerStart)
REGISTER_NATIVE_FUNCTION_AS(0x800440BC, MH3_PadPointerStart, "PadPointerStart");
MH3_POINTER_PORT(80044BA0, PadPointerOn)
REGISTER_NATIVE_FUNCTION_AS(0x80044BA0, MH3_PadPointerOn, "PadPointerOn");
MH3_POINTER_PORT(80044C68, PadPointerTimeout)
REGISTER_NATIVE_FUNCTION_AS(0x80044C68, MH3_PadPointerTimeout, "PadPointerTimeout");
MH3_POINTER_PORT(80043F10, PadPointerToScreen)
REGISTER_NATIVE_FUNCTION_AS(0x80043F10, MH3_PadPointerToScreen, "PadPointerToScreen");
MH3_POINTER_PORT(80043D1C, PadPointerSmooth)
REGISTER_NATIVE_FUNCTION_AS(0x80043D1C, MH3_PadPointerSmooth, "PadPointerSmooth");
MH3_POINTER_PORT(80043C10, PadPointerReset)
REGISTER_NATIVE_FUNCTION_AS(0x80043C10, MH3_PadPointerReset, "PadPointerReset");
MH3_POINTER_PORT(800450AC, KpadInit)
REGISTER_NATIVE_FUNCTION_AS(0x800450AC, MH3_KpadInit, "KpadInit");
MH3_POINTER_PORT(800451DC, KpadReset)
REGISTER_NATIVE_FUNCTION_AS(0x800451DC, MH3_KpadReset, "KpadReset");
MH3_POINTER_PORT(80044FF4, KpadPosScale)
REGISTER_NATIVE_FUNCTION_AS(0x80044FF4, MH3_KpadPosScale, "KpadPosScale");
MH3_POINTER_PORT(80045720, KpadReadAcc)
REGISTER_NATIVE_FUNCTION_AS(0x80045720, MH3_KpadReadAcc, "KpadReadAcc");
MH3_POINTER_PORT(800453B0, KpadPlay)
REGISTER_NATIVE_FUNCTION_AS(0x800453B0, MH3_KpadPlay, "KpadPlay");
MH3_POINTER_PORT(80045464, KpadAccHorizon)
REGISTER_NATIVE_FUNCTION_AS(0x80045464, MH3_KpadAccHorizon, "KpadAccHorizon");
MH3_POINTER_PORT(800455F8, KpadAccVertical)
REGISTER_NATIVE_FUNCTION_AS(0x800455F8, MH3_KpadAccVertical, "KpadAccVertical");
MH3_POINTER_PORT(800468A0, KpadReadDpd)
REGISTER_NATIVE_FUNCTION_AS(0x800468A0, MH3_KpadReadDpd, "KpadReadDpd");
MH3_POINTER_PORT(800458BC, KpadGetObjects)
REGISTER_NATIVE_FUNCTION_AS(0x800458BC, MH3_KpadGetObjects, "KpadGetObjects");
MH3_POINTER_PORT(80045960, KpadObjectsOutside)
REGISTER_NATIVE_FUNCTION_AS(0x80045960, MH3_KpadObjectsOutside, "KpadObjectsOutside");
MH3_POINTER_PORT(800459D8, KpadObjectsSame)
REGISTER_NATIVE_FUNCTION_AS(0x800459D8, MH3_KpadObjectsSame, "KpadObjectsSame");
MH3_POINTER_PORT(80045A4C, KpadSelect2Continue)
REGISTER_NATIVE_FUNCTION_AS(0x80045A4C, MH3_KpadSelect2Continue, "KpadSelect2Continue");
MH3_POINTER_PORT(80045CA8, KpadSelect1Continue)
REGISTER_NATIVE_FUNCTION_AS(0x80045CA8, MH3_KpadSelect1Continue, "KpadSelect1Continue");
MH3_POINTER_PORT(80045E48, KpadPairDirection)
REGISTER_NATIVE_FUNCTION_AS(0x80045E48, MH3_KpadPairDirection, "KpadPairDirection");
MH3_POINTER_PORT(80045F14, KpadSelect2First)
REGISTER_NATIVE_FUNCTION_AS(0x80045F14, MH3_KpadSelect2First, "KpadSelect2First");
MH3_POINTER_PORT(80046074, KpadSelect1First)
REGISTER_NATIVE_FUNCTION_AS(0x80046074, MH3_KpadSelect1First, "KpadSelect1First");
MH3_POINTER_PORT(80046220, KpadPairHorizon)
REGISTER_NATIVE_FUNCTION_AS(0x80046220, MH3_KpadPairHorizon, "KpadPairHorizon");
MH3_POINTER_PORT(800462EC, KpadDpdVariable)
REGISTER_NATIVE_FUNCTION_AS(0x800462EC, MH3_KpadDpdVariable, "KpadDpdVariable");

extern "C" void func_800441E4(CpuContext* ctx);
extern "C" void MH3_KpadClamp(CpuContext* ctx) {
    if (PortCheck::Enabled("KpadClamp")) [[unlikely]]
        PortCheck::RunPure(ctx, "KpadClamp", PortCheck::Returns::Fpr1, func_800441E4, KpadClampPort);
    else
        KpadClampPort(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x800441E4, MH3_KpadClamp, "KpadClamp");
