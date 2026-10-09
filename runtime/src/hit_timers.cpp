// Hit work timers, RMHP08. C reconstruction of 0x80299F5C (called by the hit
// step 0x8029A3BC for each active hit work), decompiled from the disassembly.
//
// A hit work (_HIT_W) belongs to the player, an enemy or an AI NPC (+6: 0, 1,
// 3; its work at +0x10). With flag 0x100 it lives only while its owner plays
// one of four motions (u16 +0x18..+0x1E). Each step it takes an amount off its
// float timers, in 1/60 s frames: 2, or with flag 0x200 the owner's motion
// speed (player +0x358, enemy +0x80, NPC +0x64). A delay (+0x38) runs first;
// then the active time (+0x3C) runs and within it two phases (+0x50, +0x54),
// each marked in +0x33 when it starts (bits 1, 2) and the first ended (bit 2).
// Returns 1 while the active time runs.
//
// The amount was taken once per step, so at short steps hits ended early. The
// port takes the step's share of it; the one-step phase starts are kept. At
// the stock step it is the original.
#include "abi_bridge.h"
#include "game_step.h"
#include "guest_call.h"
#include "memory.h"
#include "port_check.h"

#include <cstdint>

namespace {

enum : uint32_t {
    kActive = 0x05,      // u8
    kOwnerKind = 0x06,   // u8: 0 player, 1 enemy, 3 AI NPC
    kOwner = 0x10,       // u32 owner work
    kFlags = 0x20,       // u32
    kMotions = 0x18,     // u16 x4
    kPhases = 0x33,      // u8: bit 1 first phase started, bit 2 second
    kDelay = 0x38,       // f32
    kTime = 0x3C,        // f32
    kFirst = 0x50,       // f32
    kSecond = 0x54,      // f32
};
enum : uint32_t { kFlagMotions = 0x100, kFlagOwnerSpeed = 0x200 };
enum : uint32_t { kPlayerSpeed = 0x358, kEnemySpeed = 0x80, kNpcSpeed = 0x64 };

enum : uint32_t {
    PlayerMotion = 0x8026A36C, // Get_motion_no(PLW*)
    EnemyMotion = 0x8012F960,  // em_get_mot_no(ENEMY_WORK*)
    NpcMotion = 0x802D2A14,    // ai_get_motion_no(AINPC_W*)
};

constexpr float kFrames = 2.0f;

float F(uint32_t a) { return Memory::ReadFloat32(a); }

} // namespace

static void HitTimerStep(CpuContext* ctx) {
    uint32_t result = 0;
    {
        GuestFrame frame{ctx, 0x10};
        GuestCall call{ctx};
        const uint32_t w = frame.Arg(3);
        const uint32_t flags = Memory::Read32(w + kFlags);
        const uint8_t kind = Memory::Read8(w + kOwnerKind);
        const uint32_t owner = Memory::Read32(w + kOwner);
        auto run = [&]() -> uint32_t {
            if (!Memory::Read8(w + kActive)) return 0;
            if ((flags & kFlagMotions) && (kind == 0 || kind == 1 || kind == 3)) {
                const uint32_t fn = kind == 0 ? PlayerMotion : kind == 1 ? EnemyMotion : NpcMotion;
                const uint16_t motion = uint16_t(call(fn, {owner}));
                bool playing = false;
                for (uint32_t i = 0; i < 4; ++i) playing |= motion == Memory::Read16(w + kMotions + 2 * i);
                if (!playing) {
                    Memory::Write8(w + kActive, 0);
                    return 0;
                }
            }
            float amount = kFrames;
            if (flags & kFlagOwnerSpeed) {
                if (kind == 1) amount = F(owner + kEnemySpeed);
                else if (kind == 0) amount = F(owner + kPlayerSpeed);
                else if (kind == 3) amount = F(owner + kNpcSpeed);
            }
            amount = float(amount * GameStep::Scale());

            const float delay = F(w + kDelay);
            if (!(delay <= 0.0f)) {
                if (delay > 0.0f) {
                    const float left = delay - amount;
                    Memory::WriteFloat32(w + kDelay, left <= 0.0f ? 0.0f : left);
                }
                return 0;
            }
            const float time = F(w + kTime);
            if (!(time > 0.0f)) {
                Memory::Write8(w + kActive, 0);
                return 0;
            }
            Memory::WriteFloat32(w + kTime, time - amount);
            const uint8_t phases = Memory::Read8(w + kPhases);
            const float first = F(w + kFirst);
            if (first > 0.0f) {
                if (!(phases & 1)) {
                    Memory::Write8(w + kPhases, phases | 1);
                } else {
                    const float left = first - amount;
                    Memory::WriteFloat32(w + kFirst, left);
                    if (left <= 0.0f) {
                        Memory::WriteFloat32(w + kFirst, 0.0f);
                        Memory::Write8(w + kPhases, phases | 2);
                    }
                }
            } else {
                const float second = F(w + kSecond);
                if (second > 0.0f) {
                    if (!(phases & 2)) {
                        Memory::Write8(w + kPhases, phases | 2);
                    } else {
                        const float left = second - amount;
                        Memory::WriteFloat32(w + kSecond, left <= 0.0f ? 0.0f : left);
                    }
                }
            }
            return 1;
        };
        result = run();
    }
    ctx->gpr[3] = result;
}
extern "C" void func_80299F5C(CpuContext* ctx);

extern "C" void MH3_HitTimerStep(CpuContext* ctx) {
    PortCheck::Call(ctx, "HitTimerStep", func_80299F5C, HitTimerStep);
}
REGISTER_NATIVE_FUNCTION_AS(0x80299F5C, MH3_HitTimerStep, "HitTimerStep");
