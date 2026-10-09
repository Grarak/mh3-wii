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
// results blended, except across a cut (two keys less than a frame apart),
// where the original jumps between shots: there the earlier frame's pose
// holds until the counter passes the cut. At the stock 1/30 s step (one tick,
// no fraction) the script wait runs the original unchanged.
//
// The path evaluation itself is native: the original walked the keys from the
// first on every call (three calls a step at a fractional frame), each key a
// folded guest load. Path kinds with the orbit (flag 1) or ground (flag 2)
// adjustment still run the original.
#include "game_step.h"
#include "guest_call.h"
#include "guest_flat_memory.h"
#include "memory.h"
#include "port_check.h"

#include <algorithm>
#include <cmath>
#include <cstring>

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
    static constexpr uint32_t kOffsets[8] = {kEye, kEye + 4, kEye + 8, kTarget, kTarget + 4, kTarget + 8, kFovY, kAspect};
    void Read(uint32_t script) {
        for (int i = 0; i < 8; ++i)
            v[i] = Memory::ReadFloat32(script + kOffsets[i]);
    }

    void Write(uint32_t script) const {
        for (int i = 0; i < 8; ++i)
            Memory::WriteFloat32(script + kOffsets[i], v[i]);
    }
};

// This step's time past the ticks counted before it: the counters run after
// the evaluation, so it is ticks + fraction - 1 (0 at the stock step).
double Pending() {
    return double(GameStep::Ticks()) + GameStep::Fraction() - 1.0;
}

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

namespace {

// A path (camera work +0x7C) is 36-byte keys, ended by a negative frame:
//   +0 s32 frame (half frames), +4 the f32 for +0x34, +8 the f32 for +0x30,
//   +0xC eye, +0x18 target
// The path kind (+0xFD) selects a u16 of flags at 0x805D3326 + 4 * kind:
//   1 orbit the result around the work, 2 keep it above the ground,
//   8 hold the last key past the end (the path never reports done).
enum : uint32_t {
    kPathKeys = 0x7C,
    kPathKind = 0xFD,
    kPathFlags = 0x805D3326,
    kKeyBytes = 36,
};

inline uint32_t Word(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return __builtin_bswap32(v);
}

inline float Float(const uint8_t* p) {
    const uint32_t v = Word(p);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

inline void PutFloat(uint8_t* p, float f) {
    uint32_t v;
    std::memcpy(&v, &f, 4);
    v = __builtin_bswap32(v);
    std::memcpy(p, &v, 4);
}

// Key `k`'s pose into the work `w` (word copies, as lfs/stfs copy them).
void CopyKey(uint8_t* w, const uint8_t* k) {
    std::memcpy(w + kEye, k + 0xC, 12);
    std::memcpy(w + kTarget, k + 0x18, 12);
    std::memcpy(w + kFovY, k + 8, 4);
    std::memcpy(w + kAspect, k + 4, 4);
}

uint32_t PathFlags(uint32_t script) {
    return MemoryInline::FlatRead16(kPathFlags + 4 * MemoryInline::FlatRead8(script + kPathKind));
}

// The frame (s16) as the keys count it, in half frames.
int32_t HalfFrames(int32_t frame) {
    return int32_t(int16_t(frame)) * 2;
}

// Where a walk of `keys` for `half` (half frames) or any later frame may
// start: the first key that is negative or at or past `half`, since every key
// before it is passed. Remembered for the last path, as frames only grow
// while a path plays; an earlier frame or another path walks from the first
// key again. The keys are resource data and do not change while they play;
// the key before the remembered one is checked anyway.
uint32_t SeekKey(uint32_t keys, int32_t half) {
    static uint32_t lastKeys, lastIndex;
    static int32_t lastHalf;
    const uint8_t* base = GuestFlat::Host(keys);
    uint32_t i = 0;

    if (keys == lastKeys && half >= lastHalf && lastIndex) {
        const int32_t before = int32_t(Word(base + (lastIndex - 1) * kKeyBytes));

        if (before >= 0 && before < lastHalf)
            i = lastIndex;
    }

    for (;; ++i) {
        const int32_t key = int32_t(Word(base + i * kKeyBytes));

        if (key < 0 || key >= half)
            break;
    }

    lastKeys = keys, lastIndex = i, lastHalf = half;
    return i;
}

// 0x802BD938 for a path without adjustments (flags & 3 clear), walking the
// keys from `start` (SeekKey for this frame or an earlier one): the pose at
// the work's frame, between the two keys around it; 1 once a path that does
// not hold has run past its last key.
uint32_t EvaluatePath(uint32_t script, uint32_t flags, uint32_t keys, uint32_t start) {
    const int32_t frame = HalfFrames(MemoryInline::FlatRead16(script + kFrame));
    uint8_t* w = GuestFlat::Host(script);
    const uint8_t* k = GuestFlat::Host(keys) + start * kKeyBytes;
    int32_t key;

    for (;; k += kKeyBytes) {
        key = int32_t(Word(k));

        if (key < 0)
            break;

        if (key == frame) {
            CopyKey(w, k);
            break;
        }

        if (frame < key) {
            // t and every step rounded to single, as fdivs and the paired-single
            // subtract, scale and add round them.
            const uint8_t* prev = k - kKeyBytes;
            const int32_t at = int32_t(Word(prev));
            const float t = float(int32_t(uint32_t(frame) - uint32_t(at))) / float(int32_t(uint32_t(key) - uint32_t(at)));
            auto lerp = [&](uint32_t to, uint32_t from) {
                const float a = Float(prev + from);
                float d = Float(k + from) - a;
                d = d * t;
                PutFloat(w + to, d + a);
            };

            for (uint32_t i = 0; i < 12; i += 4) {
                lerp(kEye + i, 0xC + i);
                lerp(kTarget + i, 0x18 + i);
            }

            lerp(kFovY, 8);
            lerp(kAspect, 4);
            break;
        }
    }

    if (key >= 0)
        return 0;

    if (flags & 8) {
        CopyKey(w, k - kKeyBytes);
        return 0;
    }

    return 1;
}

// Whether the keys from `start` cut between frame `low` and the next: two keys
// less than a frame apart there. The original shows whole frames only, so it
// jumps from one shot to the next; a blend across them would fly the camera
// between the two shots.
bool CutAfter(uint32_t keys, uint32_t start, int32_t low) {
    const uint8_t* k = GuestFlat::Host(keys) + start * kKeyBytes;
    const int32_t from = HalfFrames(low), to = HalfFrames(low + 1);
    int32_t before = -1;

    for (;; k += kKeyBytes) {
        const int32_t key = int32_t(Word(k));

        if (key < 0 || key > to)
            return false;

        if (before >= from && key - before < 2)
            return true;

        before = key;
    }
}

} // namespace

static void CameraPathEvaluatePort(CpuContext* ctx) {
    const uint32_t script = ctx->gpr[4];
    const int16_t frame = int16_t(Memory::Read16(script + kFrame));
    const uint32_t flags = PathFlags(script);
    const uint32_t keys = MemoryInline::FlatRead32(script + kPathKeys);
    const double pending = Pending();

    if (pending == 0.0) {
        if (flags & 3)
            func_802BD938(ctx);
        else
            ctx->gpr[3] = EvaluatePath(script, flags, keys, SeekKey(keys, HalfFrames(frame)));

        return;
    }

    const double position = std::max(0.0, double(frame) + pending);
    const int32_t low = int32_t(std::floor(position));
    float blend = float(position - low);

    // Evaluate at the counted frame for the result (done or not), then at the
    // two frames around the position for the pose.
    if (!(flags & 3)) {
        const uint32_t start = SeekKey(keys, std::min({HalfFrames(frame), HalfFrames(low), HalfFrames(low + 1)}));

        if (CutAfter(keys, start, low))
            blend = 0.0f;

        auto evaluate = [&](int32_t at, PathPose* pose) {
            Memory::Write16(script + kFrame, uint16_t(int16_t(at)));
            const uint32_t done = EvaluatePath(script, flags, keys, start);

            if (pose)
                pose->Read(script);

            return done;
        };

        const uint32_t result = evaluate(frame, nullptr);
        PathPose a, b;
        evaluate(low, &a);
        evaluate(low + 1, &b);

        for (int i = 0; i < 8; ++i)
            a.v[i] += (b.v[i] - a.v[i]) * blend;

        a.Write(script);
        Memory::Write16(script + kFrame, uint16_t(frame));
        ctx->gpr[3] = result;
        return;
    }

    if (CutAfter(keys, SeekKey(keys, HalfFrames(low)), low))
        blend = 0.0f;

    // The original changes volatile registers: each run starts from the entry's.
    const CpuContext entry = *ctx;
    auto evaluate = [&](int32_t at, PathPose* pose) {
        *ctx = entry;
        Memory::Write16(script + kFrame, uint16_t(int16_t(at)));
        func_802BD938(ctx);

        if (pose)
            pose->Read(script);

        return ctx->gpr[3];
    };

    const uint32_t result = evaluate(frame, nullptr);
    const CpuContext after = *ctx;
    PathPose a, b;
    evaluate(low, &a);
    evaluate(low + 1, &b);

    for (int i = 0; i < 8; ++i)
        a.v[i] += (b.v[i] - a.v[i]) * blend;

    a.Write(script);
    Memory::Write16(script + kFrame, uint16_t(frame));
    *ctx = after;
    ctx->gpr[3] = result;
}

extern "C" void MH3_CameraPathEvaluate(CpuContext* ctx) {
    PortCheck::Call(ctx, "CameraPathEvaluate", func_802BD938, CameraPathEvaluatePort);
}
REGISTER_NATIVE_FUNCTION_AS(0x802BD938, MH3_CameraPathEvaluate, "CameraPathEvaluate");
