// Scripted camera moves, RMHP08. The event camera (mode 3, camera work+0x244)
// plays a script (interpreter 0x802BCA94): a wait op runs for a number of
// steps, counting `left` (+0x44) down from `total` (+0x46) and moving the eye,
// target and field of view linearly between two keys with t = left / total
// (0x802BD3A0). The countdown is a tick site (tick_sites.inc): it counts ticks
// of game time. Here the interpolation sees the time since the last tick as
// well, so the camera moves smoothly at any step length: `left` less the part
// of this step's time the countdown has not counted yet.
//
// t is computed from the two s16 fields as loaded, so they are scaled up
// around the original's call to carry the fraction, and restored after.
//
// Keyframed camera paths (0x802BD78C) play the same way: a frame counter at
// +0x44 counts up (a tick site), and 0x802BD938 sets the eye, target and field
// of view from the path at that frame, linearly between keys. Here the path is
// evaluated at the two frames around the step's fractional frame and the
// results blended. At the stock 1/30 s step (one tick, no fraction) the
// originals run unchanged.
#include "game_step.h"
#include "guest_call.h"
#include "port_check.h"

#include <algorithm>
#include <cmath>

namespace {

enum : uint32_t {
    kLeft = 0x44,   // s16 steps left in the wait, done below 0
    kTotal = 0x46,  // s16 steps of the wait (< 1: no interpolation)
    kFrame = 0x44,  // s16 path frame
    kEye = 0x00,    // VEC3
    kTarget = 0x0C, // VEC3
    kFovY = 0x30,   // f32
    kAspect = 0x34, // f32
};

// What 0x802BD938 sets: eye, target, and the two floats at +0x30.
struct PathPose {
    float v[8];
    static constexpr uint32_t kOffsets[8] = {kEye, kEye + 4, kEye + 8, kTarget, kTarget + 4, kTarget + 8,
                                             kFovY, kAspect};
    void Read(uint32_t script) {
        for (int i = 0; i < 8; ++i) v[i] = Memory::ReadFloat32(script + kOffsets[i]);
    }
    void Write(uint32_t script) const {
        for (int i = 0; i < 8; ++i) Memory::WriteFloat32(script + kOffsets[i], v[i]);
    }
};

// This step's time past the ticks counted before it: the counters run after
// the evaluation, so it is ticks + fraction - 1 (0 at the stock step).
double Pending() { return double(GameStep::Ticks()) + GameStep::Fraction() - 1.0; }

} // namespace

extern "C" void func_802BD3A0(CpuContext* ctx);

static void CameraScriptInterpolatePort(CpuContext* ctx) {
    const uint32_t script = ctx->gpr[4];
    const int16_t left = int16_t(Memory::Read16(script + kLeft));
    const int16_t total = int16_t(Memory::Read16(script + kTotal));
    // The countdown runs after this call: it has counted every tick before this
    // step, so this step's time is ticks + fraction - 1 past `left`.
    const double pending = Pending();
    if (total < 1 || pending == 0.0) {
        func_802BD3A0(ctx);
        return;
    }
    const double now = std::clamp(double(left) - pending, 0.0, double(total));
    const int32_t scale = std::max(1, std::min(256, 32767 / total));
    Memory::Write16(script + kLeft, uint16_t(int16_t(std::lround(now * scale))));
    Memory::Write16(script + kTotal, uint16_t(int16_t(total * scale)));
    func_802BD3A0(ctx);
    Memory::Write16(script + kLeft, uint16_t(left));
    Memory::Write16(script + kTotal, uint16_t(total));
}

extern "C" void MH3_CameraScriptInterpolate(CpuContext* ctx) {
    PortCheck::Call(ctx, "CameraScriptInterpolate", func_802BD3A0, CameraScriptInterpolatePort);
}
REGISTER_NATIVE_FUNCTION_AS(0x802BD3A0, MH3_CameraScriptInterpolate, "CameraScriptInterpolate");

extern "C" void func_802BD938(CpuContext* ctx);

static void CameraPathEvaluatePort(CpuContext* ctx) {
    const uint32_t script = ctx->gpr[4];
    const int16_t frame = int16_t(Memory::Read16(script + kFrame));
    const double pending = Pending();
    if (pending == 0.0) {
        func_802BD938(ctx);
        return;
    }
    const CpuContext entry = *ctx;
    const double position = std::max(0.0, double(frame) + pending);
    const int32_t low = int32_t(std::floor(position));
    const float blend = float(position - low);
    // Evaluate at the counted frame for the result (done or not), then at the
    // two frames around the position for the pose.
    auto evaluate = [&](int32_t at, PathPose* pose) {
        *ctx = entry;
        Memory::Write16(script + kFrame, uint16_t(int16_t(at)));
        func_802BD938(ctx);
        if (pose) pose->Read(script);
        return ctx->gpr[3];
    };
    const uint32_t result = evaluate(frame, nullptr);
    const CpuContext after = *ctx;
    PathPose a, b;
    evaluate(low, &a);
    evaluate(low + 1, &b);
    for (int i = 0; i < 8; ++i) a.v[i] += (b.v[i] - a.v[i]) * blend;
    a.Write(script);
    Memory::Write16(script + kFrame, uint16_t(frame));
    *ctx = after;
    ctx->gpr[3] = result;
}

extern "C" void MH3_CameraPathEvaluate(CpuContext* ctx) {
    PortCheck::Call(ctx, "CameraPathEvaluate", func_802BD938, CameraPathEvaluatePort);
}
REGISTER_NATIVE_FUNCTION_AS(0x802BD938, MH3_CameraPathEvaluate, "CameraPathEvaluate");
