// Ceadeus's opening cutscene, RMHP08. C reconstruction of em020's intro
// action (0x80371730, decompiled with Ghidra), which the enemy update calls
// once per step while Ceadeus plays its arrival: the sprays and splashes of
// its fins and body as the motion passes given frames.
//
// State (work +5): 0 sets the motion, the position and the angles; 1 waits
// for demo step 130 (0x8014606C 0x104), 2 swims in (motion 0xCA), 3 surfaces
// (motion 0xD3).
// Each effect is an enemy effect (0x8030456C: work, kind, joint, offset; f1
// its scale) started on the step whose motion advance crosses its frame
// (em_frame_check mode 0, once per crossing since the frame check takes the
// step's advance, mhchar_motion.cpp). Two kinds repeat over a frame range:
//
//  - state 2's spray (kind 0xBF on joint 0x17) every 17 ticks while the
//    motion is in [82, 484]: the original counted the countdown at +0x20
//    down by one a call, so at a step shorter than a tick it sprayed every
//    17 steps (four times as often at 120 fps). It counts the step's whole
//    ticks;
//  - state 3's foam (0xCF on joint 1 in [532, 574], 0xBF on joint 0x22 in
//    [652, 1174]) on demo steps (the event demo clock, a tick site) that are
//    multiples of 10: the clock holds between ticks, so the original test
//    passed on every step of such a tick. It passes on the step whose ticks
//    include one (StepCounter::Every's test on the demo clock).
//
// At the stock 1/30 s step this is the original
// (MH3_PORT_CHECK=CeadeusIntro MH3_VARIABLE_STEP=0, the Ceadeus quest 2006).
#include "abi_bridge.h"
#include "game_step.h"
#include "guest_call.h"
#include "memory.h"
#include "port_check.h"

#include <cstdint>
#include <initializer_list>

namespace {

// Enemy work fields.
enum : uint32_t {
    kState = 0x05,        // u8
    kCountdown = 0x20,    // s32: calls (here ticks) to the next spray in state 2
    kPosition = 0x188,    // VEC3
    kPrevPosition = 0x194, // VEC3
    kHeight = 0x1AC,      // f32
    kHeightSet = 0x1DF,   // u8: 1 once the height was set
    kActed = 0x30F,       // u8: 1, set every call (0x80131E0C, inlined)
};

enum : uint32_t {
    SetUp = 0x801304DC,       // (work, 2)
    SetMotion = 0x8012F61C,   // (work, motion, 0, blend): 0x8012F568
    SetAngles = 0x80146170,   // (work; f1-f3 degrees)
    DemoReached = 0x8014606C, // (n) -> 1 from demo step n / 2
    FrameCheck = 0x8012F980,  // em_frame_check(work, mode; f1 frame, f2 end) -> 1
    MotionDone = 0x8012F9A0,  // (work) -> 1: 0x800E21FC on the MHchar
    DemoSteps = 0x80146048,   // () -> s16, the event demo clock
    Spawn = 0x8030456C,       // (work, kind, joint, offset or 0; f1 scale)
};

// 0x801460BC, inlined: the position, and the previous position the same.
void SetPosition(uint32_t work, float x, float y, float z) {
    for (uint32_t base : {kPosition, kPrevPosition}) {
        Memory::WriteFloat32(work + base, x);
        Memory::WriteFloat32(work + base + 4, y);
        Memory::WriteFloat32(work + base + 8, z);
    }
}

// 0x80130450, inlined.
void SetHeight(uint32_t work, float height) {
    Memory::WriteFloat32(work + kHeight, height);
    Memory::Write8(work + kHeightSet, 1);
}

void CeadeusIntroPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x20);
    GuestCall game{ctx};
    const uint32_t work = frame.Arg(3);
    const uint32_t offset = frame[8]; // the original's local VEC3 (its constructor 0x80043F0C is empty)
    Memory::Write8(work + kActed, 1);

    // Mode 0 unless given: the step's advance crosses `at`.
    const auto at = [&](float f, uint32_t mode = 0, float end = 0.0f) {
        return game.Call<FrameCheck>({work, mode}, {f, end}) == 1;
    };

    const auto any = [&](std::initializer_list<float> frames) {
        for (const float f : frames)
            if (at(f))
                return true;

        return false;
    };

    const auto spawn = [&](float scale, uint32_t kind, uint32_t joint) {
        game.Call<Spawn>({work, kind, joint, 0}, {scale});
    };

    const auto spawnAt = [&](float scale, uint32_t kind, uint32_t joint, float x, float y, float z) {
        Memory::WriteFloat32(offset, x);
        Memory::WriteFloat32(offset + 4, y);
        Memory::WriteFloat32(offset + 8, z);
        game.Call<Spawn>({work, kind, joint, offset}, {scale});
    };

    // A demo step that is a multiple of 10 among the step's ticks.
    const auto tenth = [&] {
        const int32_t steps = int16_t(game.Call<DemoSteps>());
        const uint32_t ticks = GameStep::Ticks();

        if (ticks == 1)
            return steps % 10 == 0;

        return steps >= 0 && uint32_t(steps % 10) < ticks;
    };

    switch (Memory::Read8(work + kState)) {
    case 0:
        Memory::Write8(work + kState, 1);
        game.Call<SetUp>({work, 2});
        game.Call<SetMotion>({work, 1, 0, 0});
        SetPosition(work, 4138.0f, -2741.0f, -7885.0f);
        game.Call<SetAngles>({work}, {0.0f, -78.75f, 0.0f});
        break;
    case 1:
        if (game.Call<DemoReached>({0x104}) == 1) {
            Memory::Write8(work + kState, 2);
            Memory::Write32(work + kCountdown, 0);
            game.Call<SetMotion>({work, 0xCA, 0, 0x2A});
        }

        break;
    case 2:
        if (at(82.0f, 3, 484.0f)) {
            const int32_t left = int32_t(Memory::Read32(work + kCountdown)) - int32_t(GameStep::Ticks());
            Memory::Write32(work + kCountdown, uint32_t(left));

            if (left < 0) {
                spawn(7.5f, 0xBF, 0x17);
                Memory::Write32(work + kCountdown, 16);
            }
        }

        if (any({162.0f, 182.0f}))
            spawn(5.0f, 0xBE, 0x17);

        if (any({202.0f, 222.0f}))
            spawn(7.0f, 0xBE, 0x17);

        if (at(240.0f))
            spawnAt(2.8f, 200, 0x17, -125.0f, -200.0f, 75.0f);

        if (at(250.0f))
            spawnAt(1.8f, 0xBD, 0x17, -100.0f, -125.0f, 100.0f);

        if (at(260.0f))
            spawnAt(2.2f, 0xBD, 0x17, 0.0f, -250.0f, 400.0f);

        if (any({300.0f, 320.0f, 340.0f, 360.0f, 380.0f}))
            spawn(1.2f, 0xC0, 0x17);

        if (at(402.0f))
            spawnAt(2.0f, 0xBD, 0x17, -150.0f, 0.0f, 50.0f);

        if (at(438.0f))
            spawn(1.0f, 0xC0, 0x13);

        if (at(468.0f))
            spawnAt(1.0f, 0xC0, 1, 200.0f, 0.0f, 0.0f);

        if (at(488.0f))
            spawnAt(1.0f, 0xC0, 1, -100.0f, 0.0f, -50.0f);

        if (at(508.0f))
            spawn(1.0f, 0xC0, 0x22);

        if (at(522.0f))
            spawn(6.0f, 0xBF, 0x23);

        if (at(528.0f))
            spawn(1.0f, 0xC0, 0x23);

        if (at(552.0f))
            spawn(3.0f, 0xBE, 0x24);

        if (at(574.0f))
            spawn(1.3f, 0xBD, 0x26);

        if (any({562.0f, 602.0f, 642.0f}))
            spawn(6.0f, 0xBF, 0x24);

        if (any({622.0f, 642.0f, 662.0f}))
            spawn(4.0f, 0xBE, 0x29);

        if (at(688.0f, 1)) // 0x80130460, inlined: the height down by 25
            SetHeight(work, float(double(Memory::ReadFloat32(work + kHeight)) - 25.0));

        if (game.Call<MotionDone>({work}) == 1) {
            Memory::Write8(work + kState, 3);
            Memory::Write32(work + kCountdown, 0);
            game.Call<SetMotion>({work, 0xD3, 0, 0});
            SetPosition(work, 4465.0f, -2432.0f, -3705.0f);
            SetHeight(work, 0.0f);
        }

        break;
    case 3:
        if (any({2.0f, 20.0f}))
            spawn(7.0f, 0xBF, 0x17);

        if (at(34.0f))
            spawnAt(2.0f, 0xC1, 0x17, -100.0f, -150.0f, 250.0f);

        if (at(82.0f))
            spawnAt(0.85f, 0xC3, 0xF, 0.0f, -40.0f, 150.0f);

        if (at(86.0f))
            spawn(7.0f, 0xBF, 1);

        if (at(102.0f))
            spawnAt(0.85f, 0xC3, 0xF, 0.0f, -80.0f, 0.0f);

        if (at(106.0f))
            spawn(5.0f, 0xBE, 1);

        if (at(126.0f))
            spawn(5.0f, 0xBF, 1);

        if (at(146.0f))
            spawnAt(0.85f, 0xC3, 0xF, -80.0f, -80.0f, 30.0f);

        if (any({202.0f, 222.0f}))
            spawnAt(1.1f, 0xC2, 3, 0.0f, -200.0f, 135.0f);

        if (at(342.0f))
            spawnAt(1.1f, 0xC2, 3, 175.0f, -200.0f, 135.0f);

        if (at(402.0f))
            spawn(7.0f, 0xBF, 0x17);

        if (at(426.0f))
            spawnAt(1.2f, 0xC9, 1, 50.0f, 0.0f, 1000.0f);

        if (at(434.0f))
            spawnAt(2.0f, 0xC1, 1, 50.0f, 0.0f, 1200.0f);

        if (any({442.0f, 482.0f, 522.0f, 562.0f, 602.0f, 642.0f}))
            spawn(6.0f, 0xBF, 1);

        if (at(444.0f))
            spawnAt(1.5f, 0xC9, 1, 50.0f, 0.0f, 1200.0f);

        if (any({492.0f, 512.0f, 532.0f, 552.0f, 572.0f, 592.0f, 612.0f}))
            spawn(1.2f, 0xC0, 1);

        if (at(532.0f, 3, 574.0f) && tenth())
            spawn(1.3f, 0xCF, 1);

        if (at(652.0f))
            spawn(1.5f, 0xC1, 0x24);

        if (at(652.0f, 3, 1174.0f) && tenth())
            spawn(6.0f, 0xBF, 0x22);

        break;
    }
}

} // namespace

extern "C" void func_80371730(CpuContext* ctx);
extern "C" void MH3_CeadeusIntro(CpuContext* ctx) {
    PortCheck::Call(ctx, "CeadeusIntro", func_80371730, CeadeusIntroPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80371730, MH3_CeadeusIntro, "CeadeusIntro");
