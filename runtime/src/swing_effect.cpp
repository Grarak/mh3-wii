// Swinging effect models, RMHP08. C reconstruction of 0x80116B64 (called from
// its effect's create 0x80116B00 and update), decompiled with Ghidra.
//
// The effect owns a list of MHchars (work+0x38: count, then MHchar pointers,
// then per model s16 arrays and parameter pointers). Each step, per model:
// a flip-book frame counts up through a range of joints and only that joint is
// shown; a swing phase counts up and sets the model's yaw to
// base + amplitude * sin(phase); at the ends of the swing (sin exactly +-1)
// the phase holds for a number of steps. All of it counts steps, so the
// counting runs once per whole tick of game time (GameStep::Ticks), and the
// yaw is then evaluated at the time past the last tick (GameStep::Fraction):
// the swing moves at the Wii's rate at any step length. At the stock 1/30 s
// step (one tick, no fraction) this is the original.
#include "game_step.h"
#include "guest_call.h"
#include "port_check.h"

#include <cstdint>

namespace {

enum : uint32_t {
    SinF = 0x80464560,        // sinf(f1) -> f1
    MoveMHchar = 0x800E078C,  // MHchar::move(u16)
    JointCount = 0x800E0C44,  // MHchar::get_joint_num() -> u32
    SetVisibility = 0x800E3154, // MHchar::setVisibility(u32 joint, bool)
    SubmitModel = 0x800F943C, // (work, MHchar**, 2, 1, 0)
};

enum : uint32_t {
    kWorkSteps = 0x10, // u32 work: +1 per model per step
    kWorkList = 0x38,  // list*
    // list
    kCount = 0x00,     // s32 models
    kModels = 0x04,    // MHchar*[count]
    kPhase = 0x18,     // s16[count] swing phase
    kFrame = 0x22,     // s16[count] flip-book joint
    kHold = 0x2C,      // s16[count] steps held at an end of the swing
    kParams = 0x38,    // params*[count]
    // params
    kFirstFrame = 0x10, // u8 first flip-book joint
    kFrames = 0x11,     // u8 flip-book length
    kBaseYaw = 0x12,    // u16
    kAmplitude = 0x14,  // f32 degrees
    kPhaseShift = 0x18, // u8 phase << shift = angle (0x10000 a turn)
    kHoldSteps = 0x1A,  // s16 steps to hold at the ends (<= 0: none)
    // MHchar
    kYaw = 0x2C,        // u32
};

float Sin(GuestCall& game, float x) {
    game(SinF, {}, {x});
    return game.F1();
}

// sin of the phase, as the original computes it: an angle of 0x10000 a turn.
float SwingSin(GuestCall& game, double phase, uint32_t shift) {
    const float angle = float(phase * double(1u << shift));
    return Sin(game, float(float(6.2831855f * angle) / 65536.0f));
}

} // namespace

extern "C" void func_80116B64(CpuContext* ctx);

static void SwingEffectUpdatePort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x20);
    GuestCall game{ctx};
    const uint32_t work = frame.Arg(3);
    const uint32_t list = Memory::Read32(work + kWorkList);
    const uint32_t ticks = GameStep::Ticks();
    const double fraction = GameStep::Fraction();
    for (int32_t i = 0; i < int32_t(Memory::Read32(list + kCount)); ++i) {
        const uint32_t model = Memory::Read32(list + kModels + 4 * i);
        const uint32_t params = Memory::Read32(list + kParams + 4 * i);
        const uint32_t phaseAt = list + kPhase + 2 * i, frameAt = list + kFrame + 2 * i, holdAt = list + kHold + 2 * i;
        const uint32_t shift = Memory::Read8(params + kPhaseShift);
        for (uint32_t t = 0; t < ticks; ++t) {
            const int16_t first = int16_t(Memory::Read8(params + kFirstFrame));
            int16_t flip = int16_t(Memory::Read16(frameAt) + 1);
            if (int32_t(first) + Memory::Read8(params + kFrames) <= flip) flip = first;
            Memory::Write16(frameAt, uint16_t(flip));
            Memory::Write32(work + kWorkSteps, Memory::Read32(work + kWorkSteps) + 1);
            int16_t phase = int16_t(Memory::Read16(phaseAt) + 1);
            const float s = SwingSin(game, double((uint32_t(int32_t(phase)) << shift) & 0xFFFF) / (1u << shift), shift);
            if ((s >= 1.0f || s <= -1.0f) && int16_t(Memory::Read16(params + kHoldSteps)) > 0) {
                const int16_t hold = int16_t(Memory::Read16(holdAt) + 1);
                if (int16_t(Memory::Read16(params + kHoldSteps)) < hold) {
                    Memory::Write16(holdAt, 0);
                } else {
                    Memory::Write16(holdAt, uint16_t(hold));
                    --phase;
                }
            }
            Memory::Write16(phaseAt, uint16_t(phase));
        }
        const int16_t flip = int16_t(Memory::Read16(frameAt));
        const uint32_t joints = game(JointCount, {model});
        for (uint32_t joint = 0; joint < joints; ++joint)
            game(SetVisibility, {model, joint, joint == uint32_t(int32_t(flip)) ? 1u : 0u});
        // While held at an end the stored phase is one short of the end shown.
        const int16_t phase = int16_t(Memory::Read16(phaseAt));
        const bool held = int16_t(Memory::Read16(holdAt)) > 0;
        const double at = held ? double((uint32_t(int32_t(phase) + 1) << shift) & 0xFFFF) / (1u << shift)
                               : double((uint32_t(int32_t(phase)) << shift) & 0xFFFF) / (1u << shift) + fraction;
        const float s = SwingSin(game, at, shift);
        const float yaw = 0.5f + float(65536.0f * float(s * Memory::ReadFloat32(params + kAmplitude))) / 360.0f;
        Memory::Write32(model + kYaw, (uint32_t(int32_t(yaw)) & 0xFFFF) + Memory::Read16(params + kBaseYaw));
        game(MoveMHchar, {model, 0});
        game(SubmitModel, {work, list + kModels + 4 * i, 2, 1, 0});
    }
}

extern "C" void MH3_SwingEffectUpdate(CpuContext* ctx) {
    PortCheck::Call(ctx, "SwingEffectUpdate", func_80116B64, SwingEffectUpdatePort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80116B64, MH3_SwingEffectUpdate, "SwingEffectUpdate");
