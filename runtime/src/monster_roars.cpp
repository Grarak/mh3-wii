// Monster roars, RMHP08. C reconstructions (decompiled with Ghidra) of eleven
// enemy roar actions (the threat when a monster spots the hunter), which the
// enemy update calls once per step while they run:
//
//   0x8014858C, 0x8014B3DC  kinds 1/2 (Rathian, Rathalos)
//   0x8017766C, 0x80178EBC  em015 (Lagiacrus)
//   0x801BE084              kind 6 (Diablos)
//   0x801C344C, 0x801C4AD0, 0x801C5DEC  kind 4 (Gigginox)
//   0x801CB3B4              kind 5 (Barioth)
//   0x8036F238              em020 (Ceadeus)
//   0x803798A0              em019 (Agnaktor)
//
// State (+5) 0 sets the motion up; 1 roars (the voice, a rumble and a burst
// effect) as the motion passes a frame, then breathes steam (effect kind 1,
// or 0xD0) every 8 calls while the motion is in a range, counting the calls
// at +0x20: at 120 steps a second that was four times as many effects. The
// count takes the step's whole ticks and the steam comes on the step whose
// ticks include a multiple of 8 (Em::Enemy::CountEvery). Some also puff on
// the global step counter's multiples of 4 (`system_w+0xC & 3`, a hooked
// test in the originals): StepCounter::Every here. When the motion ends the
// action hands back to the AI (0x80127FAC or a variant).
//
// At the stock 1/30 s step these are the originals (MH3_PORT_CHECK=Em006Roar,
// ... MH3_VARIABLE_STEP=0; the arena's Barroth 9002 and Lagiacrus 9005 roar
// on sight, the others through the enemy fuzz's A: entries).
//
// The kinds (work +3) were relabeled on 2026-10-06 by the dispatcher that
// reaches each function (docs/monster-control.md); the descriptions of what a
// monster does were written under the earlier labels, wrong for kinds 1-12.
#include "abi_bridge.h"
#include "enemy_work.h"
#include "guest_call.h"
#include "memory.h"
#include "port_check.h"
#include "step_counter.h"

#include <cstdint>

namespace {

enum : uint32_t {
    kArea = 0x1E1,       // u8: the area the monster is in
    kRandom = 0x1EC,     // u16: the monster's random word
};

enum : uint32_t {
    RoarVoice = 0x801293A0,   // (work, channel, voice, length)
    RoarVoice0 = 0x801296CC,  // (work, channel, voice): RoarVoice with length 0
    SetMotionEx = 0x8012F568, // (work, motion, blend, flags, 1)
    SetMotionSpeed = 0x8012F92C, // (work; f1)
    Adjust8A0 = 0x80130D40,   // (work, s16 delta): 0x80130BD0 against +0x8A0
    SetUpCarried = 0x801302AC, // (work) -> f1, then SetScale takes that f1
    SetScale = 0x80130628,    // (work; f1)
    SetUpPart = 0x80130724,   // (work, n)
    Em001Step = 0x80154D08,   // (work): kinds 1/2's per-step helper
    QuakeMark = 0x802B440C,   // (VEC3* at, u8 area, u16 random)
    ActionDone = 0x80127FAC,  // (work): back to the AI
    ActionDone94 = 0x80128094, // variants of it with other set-ups
    ActionDone110 = 0x80128110,
    ActionDone1A0 = 0x801281A0,
    ActionDone1F8 = 0x801281F8,
};

struct Action {
    GuestCall game;
    Em::Enemy em;
    uint32_t work;
    uint32_t at; // the original's local VEC3 at sp+8

    Action(CpuContext* ctx, const GuestFrame& frame) : game{ctx}, em{game, frame.Arg(3)}, work(frame.Arg(3)), at(frame[0x08]) {}

    void SetUp(uint32_t kind) { game.Call<Em::SetUp>({work, kind}); }

    void Motion(uint32_t motion, uint32_t flags, uint32_t blend) const { em.SetMotion(motion, flags, blend); }

    void Roar(uint32_t channel, uint32_t voice, uint32_t length) { game.Call<RoarVoice>({work, channel, voice, length}); }

    // setVector3 (0x8005000C, inlined) on the local VEC3.
    void Offset(float x, float y, float z) const {
        Memory::WriteFloat32(at, x);
        Memory::WriteFloat32(at + 4, y);
        Memory::WriteFloat32(at + 8, z);
    }

    void Spawn(uint32_t kind, uint32_t joint) const { em.Spawn(1.0f, kind, joint, at); }

    void Rumble(uint32_t kind, uint32_t length) const { Em::Rumble(work, kind, length); }

    // The steam: effect `kind` every 8 ticks while the motion is in [from, to].
    void Steam(float from, float to, uint32_t kind, uint32_t joint) const {
        if (!em.At(from, 3, to))
            return;

        if (em.CountEvery(8))
            Spawn(kind, joint);

        em.CountTicks();
    }

    void Done(uint32_t fn) {
        if (em.Ended())
            game(fn, {work});
    }
};

// kinds 1/2 0x8014858C.
void Em001RoarPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x20);
    Action a(ctx, frame);

    if (a.em.State() == 0) {
        a.em.State(1);
        a.SetUp(0);
        a.Motion(5, 10, 0);
        a.game.Call<RoarVoice0>({a.work, 0, 0x13});
        a.em.Count(0);
        return;
    }

    if (a.em.State() != 1)
        return;

    if (a.em.At(110.0f)) {
        a.Roar(1, 8, 5);
        a.Rumble(0x1B, 10);
    }

    a.Offset(0.0f, -20.0f, 80.0f);

    if (a.em.At(110.0f))
        a.Spawn(0, 0x1A);

    a.Steam(112.0f, 214.0f, 1, 0x1A);
    a.Done(ActionDone);
}

// kinds 1/2 0x8014B3DC.
void Em001Roar2Port(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x20);
    Action a(ctx, frame);
    a.game.Call<Em001Step>({a.work});

    if (a.em.State() == 0) {
        a.em.State(1);
        a.SetUp(3);
        a.Motion(0x1C, 6, 0);
        a.em.Count(0);
        return;
    }

    if (a.em.State() != 1)
        return;

    if (a.em.At(90.0f)) {
        a.Roar(0, 8, 5);
        a.Rumble(0x1B, 10);
    }

    a.Offset(0.0f, -20.0f, 80.0f);

    if (a.em.At(90.0f))
        a.Spawn(0, 0x1A);

    a.Steam(92.0f, 204.0f, 1, 0x1A);
    a.Done(ActionDone110);
}

// em015 0x8017766C and em019 0x803798A0: the same roar, the voice differing.
void LagiacrusAgnaktorRoar(CpuContext* ctx, uint32_t voice) {
    GuestFrame frame(ctx, 0x20);
    Action a(ctx, frame);

    if (a.em.State() == 0) {
        a.em.State(1);
        a.SetUp(0);
        a.Motion(0x1A, 10, 0);
        a.em.Count(0);
        return;
    }

    if (a.em.State() != 1)
        return;

    a.Offset(0.0f, -50.0f, 200.0f);

    if (a.em.At(106.0f))
        a.Spawn(0, 0x18);

    if (a.em.At(108.0f)) {
        a.Rumble(0x1A, 10);
        a.Roar(0, voice, 5);
    }

    a.Steam(112.0f, 220.0f, 1, 0x18);
    a.Done(ActionDone);
}

void Em015RoarPort(CpuContext* ctx) {
    LagiacrusAgnaktorRoar(ctx, 0x1C);
}

void Em019RoarPort(CpuContext* ctx) {
    LagiacrusAgnaktorRoar(ctx, 0xC);
}

// em015 0x80178EBC: puffs (kind 2) every 4 steps of the step counter while
// the motion is in [6, 112], the roar at 114, the steam in [114, 258], a last
// burst (kind 3) at 280.
void Em015Roar2Port(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x20);
    Action a(ctx, frame);

    if (a.em.State() == 0) {
        a.em.State(1);
        a.SetUp(2);
        a.game.Call<SetMotionEx>({a.work, 0x35, 0x14, 0, 1});
        a.em.Count(0);
        return;
    }

    if (a.em.State() != 1)
        return;

    if (a.em.At(6.0f, 3, 112.0f) && StepCounter::Every(4)) {
        a.Offset(0.0f, -50.0f, 100.0f);
        a.Spawn(2, 0x19);
    }

    a.Offset(0.0f, -50.0f, 100.0f);

    if (a.em.At(114.0f)) {
        a.Roar(0, 0x14, 5);
        a.Rumble(0x1A, 10);
    }

    if (a.em.At(112.0f))
        a.Spawn(0, 0x19);

    a.Steam(114.0f, 258.0f, 1, 0x19);

    if (a.em.At(280.0f)) {
        a.Offset(0.0f, -50.0f, 100.0f);
        a.Spawn(3, 0x19);
    }

    a.Done(ActionDone94);
}

// kind 6 0x801BE084.
void Em006RoarPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x20);
    Action a(ctx, frame);

    if (a.em.State() == 0) {
        a.em.State(1);
        a.SetUp(0);
        a.Motion(0xD3, 6, 0);
        a.game.Call<Adjust8A0>({a.work, uint32_t(-6)});
        a.em.Count(0);
        return;
    }

    if (a.em.State() != 1)
        return;

    if (a.em.At(110.0f)) {
        a.Roar(0, 8, 5);
        a.Rumble(0x19, 0x78);
    }

    a.Offset(0.0f, 20.0f, 50.0f);

    if (a.em.At(110.0f))
        a.Spawn(0, 0x18);

    a.Steam(116.0f, 286.0f, 1, 0x18);
    a.Done(ActionDone);
}

// kind 4's roars: the voice and a ground quake mark at 114, the steam (kind
// `steam`) in [120, 240].
void DiablosRoar(Action& a, uint32_t steam, uint32_t done) {
    if (a.em.At(114.0f)) {
        a.Roar(0, 8, 5);
        a.Rumble(0x1C, 10);
        a.game.Call<QuakeMark>({a.work + Em::kPosition, Memory::Read8(a.work + kArea), Memory::Read16(a.work + kRandom)});
    }

    a.Offset(0.0f, 0.0f, 50.0f);

    if (a.em.At(114.0f))
        a.Spawn(0, 0x1C);

    a.Steam(120.0f, 240.0f, steam, 0x1C);
    a.Done(done);
}

// kind 4 0x801C344C.
void Em004RoarPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x20);
    Action a(ctx, frame);

    if (a.em.State() == 0) {
        a.em.State(1);
        a.SetUp(0);
        a.Motion(5, 4, 0);
        a.em.Count(0);
    } else if (a.em.State() == 1) {
        DiablosRoar(a, 0xD0, ActionDone);
    }
}

// kind 4 0x801C4AD0 and 0x801C5DEC: set up with 0x801302AC's result as the
// scale (its f1 goes on to SetScale), part `part`, motion `motion`.
void DiablosRoarCarried(CpuContext* ctx, uint32_t part, uint32_t motion, bool clear, uint32_t done) {
    GuestFrame frame(ctx, 0x20);
    Action a(ctx, frame);

    if (a.em.State() == 0) {
        a.em.State(1);
        a.game.Call<SetUpCarried>({a.work});
        a.game.Call<SetScale>({a.work});
        a.game.Call<SetUpPart>({a.work, part});
        a.Motion(motion, 4, 0);

        if (clear)
            a.em.Count(0);
    } else if (a.em.State() == 1) {
        DiablosRoar(a, 1, done);
    }
}

void Em004Roar2Port(CpuContext* ctx) {
    DiablosRoarCarried(ctx, 2, 0xCA, true, ActionDone1A0);
}

void Em004Roar3Port(CpuContext* ctx) {
    DiablosRoarCarried(ctx, 1, 0xE0, false, ActionDone1F8);
}

// kind 5 0x801CB3B4 (r4: 1 = slowed, no voice nor steam).
void Em005RoarPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x20);
    Action a(ctx, frame);
    const bool slowed = uint8_t(frame.Arg(4)) == 1;

    if (a.em.State() == 0) {
        a.em.State(1);
        a.SetUp(0);
        a.Motion(5, 4, 0);

        if (slowed)
            a.game.Call<SetMotionSpeed>({a.work}, {0.75f});

        a.em.Count(0);
        return;
    }

    if (a.em.State() != 1)
        return;

    if (uint8_t(frame.Arg(4)) == 0) {
        if (a.em.At(124.0f)) {
            a.Roar(0, 0xF, 5);
            a.Rumble(0x1D, 10);
        }

        if (a.em.At(124.0f)) {
            a.Offset(0.0f, 0.0f, 200.0f);
            a.Spawn(0, 0x1B);
        }

        // The steam: every 8 ticks while the motion is in [142, 264].
        if (a.em.At(142.0f, 3, 264.0f)) {
            if (a.em.CountEvery(8)) {
                a.Offset(0.0f, 0.0f, 200.0f);
                a.Spawn(1, 0x1B);
            }

            a.em.CountTicks();
        }
    }

    a.Done(ActionDone);
}

// em020 0x8036F238: puffs (kind 2) every 4 steps of the step counter while
// the motion is in [6, 80], the roar at 228, the steam in [230, 580], a last
// burst (kind 3) at 290.
void Em020RoarPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x20);
    Action a(ctx, frame);

    if (a.em.State() == 0) {
        a.em.State(1);
        a.SetUp(2);
        a.game.Call<SetMotionEx>({a.work, 0x35, 0x14, 0, 1});
        a.game.Call<RoarVoice0>({a.work, 0, 0x23});
        a.em.Count(0);
        return;
    }

    if (a.em.State() != 1)
        return;

    a.Offset(0.0f, -45.0f, 120.0f);

    if (a.em.At(6.0f, 3, 80.0f) && StepCounter::Every(4))
        a.Spawn(2, 0x17);

    if (a.em.At(228.0f))
        a.Rumble(0x18, 10);

    if (a.em.At(228.0f))
        a.Spawn(0, 0x17);

    a.Steam(230.0f, 580.0f, 1, 0x17);

    if (a.em.At(290.0f))
        a.Spawn(3, 0x17);

    a.Done(ActionDone94);
}

} // namespace

extern "C" void func_8014858C(CpuContext* ctx);
extern "C" void MH3_Em004Roar(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em001Roar", func_8014858C, Em001RoarPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8014858C, MH3_Em004Roar, "Em001Roar");

extern "C" void func_8014B3DC(CpuContext* ctx);
extern "C" void MH3_Em004Roar2(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em001Roar2", func_8014B3DC, Em001Roar2Port);
}
REGISTER_NATIVE_FUNCTION_AS(0x8014B3DC, MH3_Em004Roar2, "Em001Roar2");

extern "C" void func_8017766C(CpuContext* ctx);
extern "C" void MH3_Em015Roar(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em015Roar", func_8017766C, Em015RoarPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8017766C, MH3_Em015Roar, "Em015Roar");

extern "C" void func_80178EBC(CpuContext* ctx);
extern "C" void MH3_Em015Roar2(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em015Roar2", func_80178EBC, Em015Roar2Port);
}
REGISTER_NATIVE_FUNCTION_AS(0x80178EBC, MH3_Em015Roar2, "Em015Roar2");

extern "C" void func_801BE084(CpuContext* ctx);
extern "C" void MH3_Em008Roar(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em006Roar", func_801BE084, Em006RoarPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x801BE084, MH3_Em008Roar, "Em006Roar");

extern "C" void func_801C344C(CpuContext* ctx);
extern "C" void MH3_Em006Roar(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em004Roar", func_801C344C, Em004RoarPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x801C344C, MH3_Em006Roar, "Em004Roar");

extern "C" void func_801C4AD0(CpuContext* ctx);
extern "C" void MH3_Em006Roar2(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em004Roar2", func_801C4AD0, Em004Roar2Port);
}
REGISTER_NATIVE_FUNCTION_AS(0x801C4AD0, MH3_Em006Roar2, "Em004Roar2");

extern "C" void func_801C5DEC(CpuContext* ctx);
extern "C" void MH3_Em006Roar3(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em004Roar3", func_801C5DEC, Em004Roar3Port);
}
REGISTER_NATIVE_FUNCTION_AS(0x801C5DEC, MH3_Em006Roar3, "Em004Roar3");

extern "C" void func_801CB3B4(CpuContext* ctx);
extern "C" void MH3_Em007Roar(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em005Roar", func_801CB3B4, Em005RoarPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x801CB3B4, MH3_Em007Roar, "Em005Roar");

extern "C" void func_8036F238(CpuContext* ctx);
extern "C" void MH3_Em020Roar(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em020Roar", func_8036F238, Em020RoarPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8036F238, MH3_Em020Roar, "Em020Roar");

extern "C" void func_803798A0(CpuContext* ctx);
extern "C" void MH3_Em019Roar(CpuContext* ctx) {
    PortCheck::Call(ctx, "Em019Roar", func_803798A0, Em019RoarPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x803798A0, MH3_Em019Roar, "Em019Roar");
