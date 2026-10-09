// Doudley's movement codes, RMHP08: leaps and jumps, dashes, evades, the
// weapon's reach, walls and monsters to pass through, slow motion.
//
// The codes made the game load their own words (from 0x8079D800, past the US
// disc's .bss) in place of its constants, and wrote those words every frame.
// Here:
//   - the translated functions they patched see the code's value in the
//     constant (r2 + offset) for the length of one call (ConstantSwap), when
//     neither the function's other loads nor its callees read that constant;
//   - the ports of the others take the values from Jump, RunStep and the
//     village dash's functions (cheats.h);
//   - StepMovementCodes writes the constants the codes changed every frame.
//
// The codes' two-hunter check (u16 0x9014AE80, the first hunter's +0x340
// 0x0101) keeps the game's values with two hunters, as in cheat_hunter.cpp.
#include "cheats.h"

#include "abi_bridge.h"
#include "ppc_runtime.h"

#include <cstdint>

namespace {

using Cheats::Id;
using Cheats::Kind;
using Cheats::On;
using Cheats::Toggled;

// The guest's sda2 constants (r2 + offset), which the translated code loads
// as it runs.
constexpr uint32_t kR2 = 0x8079FF60;

float Constant(int32_t offset) {
    return Memory::ReadFloat32(kR2 + offset);
}

void SetConstant(int32_t offset, float value) {
    Memory::WriteFloat32(kR2 + offset, value);
}

// A constant at `value` for the length of one call (when `on`).
class ConstantSwap {
public:
    ConstantSwap(int32_t offset, float value, bool on) : address_(kR2 + offset), saved_(Memory::Read32(kR2 + offset)), on_(on) {
        if (on_)
            Memory::WriteFloat32(address_, value);
    }

    ~ConstantSwap() {
        if (on_)
            Memory::Write32(address_, saved_);
    }

    ConstantSwap(const ConstantSwap&) = delete;
    ConstantSwap& operator=(const ConstantSwap&) = delete;

private:
    uint32_t address_;
    uint32_t saved_;
    bool on_;
};

// Constants the codes changed.
enum : int32_t {
    kDropCheck = -15260,  // -60: how far a walk or a run may drop before it jumps off (0x80277E10)
    kJumpGravity = -14988, // -1: a jump's gravity for its rise (0x8026A608, its only load)
    kStickScale = -30396, // 127: the stick's full tilt (the pad functions at 0x800D8xxx)
};

uint32_t Hunter(uint32_t index) {
    return Cheats::MoveWork(Kind::Player, index);
}

// Doudley's two-hunter check (u16 0x9014AE80): the first hunter's +0x340.
bool OneHunter() {
    const uint32_t first = Hunter(0);
    return first == 0 || Memory::Read16(first + 0x340) != 0x0101;
}

// Channel 0's pad work: the left stick as d-pad bits (the Classic
// Controller's), and how far it is pushed (0 to 127).
constexpr uint32_t kStickDirs = 0x8065B7D0 + 0x2C8, kStickLevel = 0x8065B7D0 + 0x2DA;

bool PressedA() {
    return (Cheats::Pressed() & Cheats::kA) != 0;
}

// Slow-mo: on from its buttons until the stick moves.
bool g_slow = false;

} // namespace

namespace Cheats {

JumpValues Jump(uint32_t plw) {
    JumpValues v;

    if (plw == 0 || plw != LocalPlayer())
        return v;

    // Moonjump (US 0x80254A68, EU 0x80254ACC): the jump's rise 48 as A is
    // pressed. Enhanced leap at will (US 0x80254B30, EU 0x80254B94): the
    // fall's forward speed at least 60 as A is pressed.
    if (On(Id::DoudleyMoonJump) && PressedA())
        v.rise = 48.0f;

    if (On(Id::EnhancedLeap) && PressedA())
        v.floorZ = 60.0f;

    // Glider (US 0x80254A60, 0x80254B1C and 0x80254B30; EU 0x80254AC4,
    // 0x80254B80 and 0x80254B94): with A held (+0xC4) as it starts, the jump
    // starts at -60 sideways and slows by 60 a step both ways, down to the
    // least speeds the stick sets (60 either way, else 0).
    if (On(Id::Glider) && OneHunter()) {
        if ((Memory::Read16(plw + 0xC4) & 0x20) != 0)
            v.sideways = -60.0f;

        const uint16_t dirs = Memory::Read16(kStickDirs);
        v.floorZ = 0.0f;

        if ((dirs & kUp) != 0)
            v.floorZ = 60.0f;

        if ((dirs & kDown) != 0)
            v.floorZ = -60.0f;

        if ((dirs & kLeft) != 0)
            v.floorX = 60.0f;

        if ((dirs & kRight) != 0)
            v.floorX = -60.0f;
    }

    // Jump at will (US 0x80254BB8, 0x80255074 and 0x802550FC; EU 0x80254C1C,
    // 0x802550D8 and 0x80255160): with A held a ledge is caught from 720
    // down and the climb's jump off is 32 (not 15); Jump very high, with R
    // and A held, makes that 60.
    if (On(Id::JumpAtWill)) {
        if (HeldAll(kA)) {
            v.reach = 720.0f;
            v.climbSpeed = 32.0f;
        }

        if (On(Id::JumpVeryHigh) && HeldAll(kR | kA)) {
            v.reach = 720.0f;
            v.climbSpeed = 60.0f;
        }
    }

    return v;
}

// Enhanced dashing: Run119's step forward 60 (US 0x80253310, EU 0x80253374);
// in the village (US 0x8020D2D4 and 0x8020D324, EU 0x8020D338 and
// 0x8020D388, with one hunter or two) the dash's motion speed 2.5 and its
// first push 48.
float RunStep() {
    return On(Id::EnhancedDashing) && OneHunter() ? 60.0f : 2.0f;
}

float VillageDashSpeed() {
    return On(Id::EnhancedDashing) ? 2.5f : 1.25f;
}

float VillageDashPush() {
    return On(Id::EnhancedDashing) ? 48.0f : 5.0f;
}

void StepMovementCodes() {
    // Enhanced leap at will and Moonjump: as A is pressed a walk or a run
    // drops off flat ground (the drop check's -60 made 60).
    if (On(Id::EnhancedLeap) || On(Id::DoudleyMoonJump))
        SetConstant(kDropCheck, PressedA() ? 60.0f : -60.0f);

    // Moonjump: as A is pressed the jump's gravity is 0.3125 of the game's;
    // Jump very high (with Jump at will): with R and A held a half.
    if (On(Id::DoudleyMoonJump))
        SetConstant(kJumpGravity, PressedA() ? -0.3125f : -1.0f);
    else if (On(Id::JumpAtWill) && On(Id::JumpVeryHigh))
        SetConstant(kJumpGravity, HeldAll(kR | kA) ? -0.5f : -1.0f);

    // Slow-mo: the stick's full tilt 10 (not 127) while it is on.
    if (On(Id::SlowMo)) {
        if (Activated(Id::SlowMo))
            g_slow = true;

        if (Memory::Read16(kStickLevel) != 0)
            g_slow = false;

        SetConstant(kStickScale, g_slow ? 10.0f : 127.0f);
    }
}

bool SlowMotion() {
    const uint32_t first = Hunter(0);
    return g_slow && first != 0 && Memory::Read8(first) == 1;
}

} // namespace Cheats

// 0x80277CF8 (plw, &height, f1 reach, f2): a ledge within reach below the
// player (its height out). The action command's check (0x8026303C, its call
// returning to 0x8026312C) climbs one it finds; Jump at will (US 0x802630B8,
// EU 0x8026311C) passes -60, so one is always found: A climbs from anywhere.
extern "C" void func_80277CF8(CpuContext* ctx);

extern "C" void MH3_CheatLedgeFind(CpuContext* ctx) {
    if (On(Id::JumpAtWill) && ctx->lr == 0x8026312C)
        ctx->fpr[1].d = -60.0;

    func_80277CF8(ctx);
}

REGISTER_NATIVE_FUNCTION_AS(0x80277CF8, MH3_CheatLedgeFind, "CheatLedgeFind");

// FieldSetAction (0x80275754, plw, ...): Enhanced movement speed (US
// 0x80275784, EU 0x802757E8) makes the action's motion speed 2; its
// constant (r2 - 14880, 1) has no other load there or in its callees.
// Crossbow rapid fire (US 0x80275790, EU 0x802757F4: +0x318's clear
// dropped) keeps the bowgun's firing state.
extern "C" void func_80275754(CpuContext* ctx);

extern "C" void MH3_CheatSetAction(CpuContext* ctx) {
    const uint32_t plw = ctx->gpr[3];
    const uint32_t crossbow = Memory::Read32(plw + 0x318);

    {
        ConstantSwap speed(-14880, 2.0f, On(Id::EnhancedMovement) && OneHunter());
        func_80275754(ctx);
    }

    if (On(Id::CrossbowRapidFire))
        Memory::Write32(plw + 0x318, crossbow);
}

REGISTER_NATIVE_FUNCTION_AS(0x80275754, MH3_CheatSetAction, "CheatSetAction");

// VillageSetAction (0x8020A448): the same in the village (US 0x8020A474, EU
// 0x8020A4D8: r2 - 16248).
extern "C" void func_8020A448(CpuContext* ctx);

extern "C" void MH3_CheatVillageSetAction(CpuContext* ctx) {
    ConstantSwap speed(-16248, 2.0f, On(Id::EnhancedMovement) && OneHunter());
    func_8020A448(ctx);
}

REGISTER_NATIVE_FUNCTION_AS(0x8020A448, MH3_CheatVillageSetAction, "CheatVillageSetAction");

// 0x80252C78 (plw): a walk (motion 101). Enhanced movement speed (US
// 0x80252E08 and 0x80252E14, EU 0x80252E6C and 0x80252E78) makes its two
// speeds (10.5 and 10) 32.
extern "C" void func_80252C78(CpuContext* ctx);

extern "C" void MH3_CheatWalk101(CpuContext* ctx) {
    const bool on = On(Id::EnhancedMovement) && OneHunter();
    ConstantSwap first(-15244, 32.0f, on);
    ConstantSwap second(-15496, 32.0f, on);
    func_80252C78(ctx);
}

REGISTER_NATIVE_FUNCTION_AS(0x80252C78, MH3_CheatWalk101, "CheatWalk101");

// Enhanced dashing in the field's runs (the constants' only loads there):
// 0x80252704 (motion 3): the motion speed 3 (not 1.25, US 0x80252738, EU
// 0x8025279C) and the step 60 (not 5, US 0x80252880, EU 0x802528E4).
extern "C" void func_80252704(CpuContext* ctx);

extern "C" void MH3_CheatRun3(CpuContext* ctx) {
    const bool on = On(Id::EnhancedDashing) && OneHunter();
    ConstantSwap speed(-15468, 3.0f, on);
    ConstantSwap step(-15256, 60.0f, on);
    func_80252704(ctx);
}

REGISTER_NATIVE_FUNCTION_AS(0x80252704, MH3_CheatRun3, "CheatRun3");

// 0x80253750 (motion 39): the motion speed 10 (not 1.5, US 0x802537E0, EU
// 0x80253844).
extern "C" void func_80253750(CpuContext* ctx);

extern "C" void MH3_CheatRun39(CpuContext* ctx) {
    ConstantSwap speed(-15328, 10.0f, On(Id::EnhancedDashing) && OneHunter());
    func_80253750(ctx);
}

REGISTER_NATIVE_FUNCTION_AS(0x80253750, MH3_CheatRun39, "CheatRun39");

// 0x802539F8 (motion 38): the step 32 (not 5, US 0x80253BB8, EU 0x80253C1C).
extern "C" void func_802539F8(CpuContext* ctx);

extern "C" void MH3_CheatRun38(CpuContext* ctx) {
    ConstantSwap step(-15256, 32.0f, On(Id::EnhancedDashing) && OneHunter());
    func_802539F8(ctx);
}

REGISTER_NATIVE_FUNCTION_AS(0x802539F8, MH3_CheatRun38, "CheatRun38");

// 0x80254190 (motion 115): the motion speed 4 (not 1.6, US 0x80254234, EU
// 0x80254298).
extern "C" void func_80254190(CpuContext* ctx);

extern "C" void MH3_CheatRun115(CpuContext* ctx) {
    ConstantSwap speed(-15232, 4.0f, On(Id::EnhancedDashing) && OneHunter());
    func_80254190(ctx);
}

REGISTER_NATIVE_FUNCTION_AS(0x80254190, MH3_CheatRun115, "CheatRun115");

// 0x8024F674 (plw): the panic dive (motions 46 to 48). Enhanced panic jump
// (US 0x8024F704, 0x8024F71C and 0x8024F738; EU 0x8024F768, 0x8024F780 and
// 0x8024F79C) makes its speeds 200, 128 and 32 (not 100, 16 and 3).
extern "C" void func_8024F674(CpuContext* ctx);

extern "C" void MH3_CheatPanicDive(CpuContext* ctx) {
    const bool on = On(Id::EnhancedPanicJump) && OneHunter();
    ConstantSwap forward(-15392, 200.0f, on);
    ConstantSwap up(-15292, 128.0f, on);
    ConstantSwap third(-15488, 32.0f, on);
    func_8024F674(ctx);
}

REGISTER_NATIVE_FUNCTION_AS(0x8024F674, MH3_CheatPanicDive, "CheatPanicDive");

namespace {

// Enhanced evade distances: the skill check for evade+ (182) passes at the
// evades' checks (C2 at US 0x80247BB0, 0x80248DA0, 0x8024A158, 0x8024A498
// and 0x8025A25C: the call's result made 1), returning here.
constexpr uint32_t kEvadeChecks[]{0x80247C14, 0x80248E04, 0x8024A1BC, 0x8024A4FC, 0x8025A2C0};

// The evade+ moves' distances: the game's five (Doudley's words at
// 0x8079D830 on), and the x2 and x10 codes'.
constexpr float kEvadeGame[5]{10.0f, 8.0f, -10.0f, -8.0f, -4.0f};
constexpr float kEvadeX2[5]{20.0f, 16.0f, -20.0f, -16.0f, -10.0f};
constexpr float kEvadeX10[5]{100.0f, 80.0f, -100.0f, -80.0f, -100.0f};

// Each move is turned by rotVecY or rotVecXYZ, called from `from`, with its
// distance (the game's constant, at three of them times another) in x (0)
// or z (8): which of the five it is when positive and when negative. The
// constants are loaded for the frame checks as well, so they stay; the
// moves change instead.
struct EvadeMove {
    uint32_t from;
    uint32_t component;
    int positive, negative;
};

constexpr EvadeMove kEvadeMoves[]{
    {0x80247C58, 8, 0, 0}, // US 0x80247BE0, EU 0x80247C44
    {0x80248EA4, 0, 1, 1}, // 0x80248E0C / 0x80248E70
    {0x80248F18, 0, 0, 0}, // 0x80248E80 / 0x80248EE4
    {0x80248F8C, 0, 1, 1}, // 0x80248EF4 / 0x80248F58
    {0x80248FEC, 8, 2, 2}, // 0x80248F74 / 0x80248FD8
    {0x8024A258, 0, 1, 3}, // 0x8024A1C4, 0x8024A1B8 / 0x8024A228, 0x8024A21C
    {0x8024A258, 8, 4, 4}, // 0x8024A1E0 / 0x8024A244
    {0x8024A55C, 8, 1, 3}, // 0x8024A4D8, 0x8024A4E4 / 0x8024A53C, 0x8024A548
    {0x8025A304, 8, 0, 0}, // 0x8025A28C / 0x8025A2F0
};

void EvadeDistance(uint32_t from, uint32_t vec) {
    if (!On(Id::EnhancedEvade) || !OneHunter())
        return;

    const float* distances = On(Id::EvadeDistanceX10) ? kEvadeX10 : On(Id::EvadeDistanceX2) ? kEvadeX2 : nullptr;

    if (distances == nullptr)
        return;

    for (const EvadeMove& move : kEvadeMoves) {
        if (move.from != from)
            continue;

        const float v = Memory::ReadFloat32(vec + move.component);

        if (v == 0.0f)
            continue;

        const int k = v > 0.0f ? move.positive : move.negative;
        Memory::WriteFloat32(vec + move.component, v * (distances[k] / kEvadeGame[k]));
    }
}

} // namespace

// Pl_Skill_ck (0x80271374, plw, skill): 1 when the hunter has the skill.
extern "C" void func_80271374(CpuContext* ctx);

extern "C" void MH3_CheatSkillCheck(CpuContext* ctx) {
    const uint32_t from = ctx->lr;
    const uint32_t skill = ctx->gpr[4];

    func_80271374(ctx);

    if (On(Id::EnhancedEvade) && skill == 182)
        for (const uint32_t check : kEvadeChecks)
            if (from == check)
                ctx->gpr[3] = 1;
}

REGISTER_NATIVE_FUNCTION_AS(0x80271374, MH3_CheatSkillCheck, "CheatSkillCheck");

// rotVecY (0x800510C8, vec, angle) and rotVecXYZ (0x80051224, vec, angles):
// the vector turned in place.
extern "C" void func_800510C8(CpuContext* ctx);

extern "C" void MH3_CheatRotVecY(CpuContext* ctx) {
    EvadeDistance(ctx->lr, ctx->gpr[3]);
    func_800510C8(ctx);
}

REGISTER_NATIVE_FUNCTION_AS(0x800510C8, MH3_CheatRotVecY, "CheatRotVecY");

extern "C" void func_80051224(CpuContext* ctx);

extern "C" void MH3_CheatRotVecXYZ(CpuContext* ctx) {
    EvadeDistance(ctx->lr, ctx->gpr[3]);
    func_80051224(ctx);
}

REGISTER_NATIVE_FUNCTION_AS(0x80051224, MH3_CheatRotVecXYZ, "CheatRotVecXYZ");

namespace {

// Exceed weapon size on attack: while the first hunter attacks (+0xA 4).
bool ReachOn() {
    const uint32_t first = Hunter(0);
    return On(Id::WeaponReach) && first != 0 && Memory::Read8(first + 0xA) == 4;
}

} // namespace

// 0x80225498 (attack, index, kind): an attack's hit shape; returns its size
// (f1), 1 for the kinds it sets at 0x80225680 (the kind's byte at +0x1241 of
// the entry 0, 1 or 4; kind 5 and 6 load theirs elsewhere). Exceed weapon
// size (US 0x8022561C, EU 0x80225680) makes that 2.
extern "C" void func_80225498(CpuContext* ctx);

extern "C" void MH3_CheatHitShape(CpuContext* ctx) {
    const uint32_t entry = ctx->gpr[3] + ctx->gpr[4] * 272;
    const uint32_t kind = ctx->gpr[5];

    func_80225498(ctx);

    if (kind != 5 && kind != 6 && ReachOn()) {
        const uint8_t shape = Memory::Read8(entry + 4673);

        if (shape == 0 || shape == 1 || shape == 4)
            ctx->fpr[1].d = 2.0;
    }
}

REGISTER_NATIVE_FUNCTION_AS(0x80225498, MH3_CheatHitShape, "CheatHitShape");

// 0x8029F268 (hit, data): a hit's setup; its size (+0x24) 1 (r2 - 14076, its
// only load there). Exceed weapon size (US 0x8029F2C0, EU 0x8029F324) makes
// it 2.
extern "C" void func_8029F268(CpuContext* ctx);

extern "C" void MH3_CheatHitSetup(CpuContext* ctx) {
    ConstantSwap size(-14076, 2.0f, ReachOn());
    func_8029F268(ctx);
}

REGISTER_NATIVE_FUNCTION_AS(0x8029F268, MH3_CheatHitSetup, "CheatHitSetup");

// LandSweepNormalizeVector (0x80293AE0, vec, f1 length): the wall sweep's
// vector scaled to length 1. Walk through walls (US 0x80293A7C, EU
// 0x80293AE0: 1 made -1), switched by its buttons, turns it round.
extern "C" void func_80293AE0(CpuContext* ctx);

extern "C" void MH3_CheatSweepNormalize(CpuContext* ctx) {
    const uint32_t vec = ctx->gpr[3];

    func_80293AE0(ctx);

    if (Toggled(Id::WalkThroughWalls))
        for (uint32_t k = 0; k < 12; k += 4)
            Memory::WriteFloat32(vec + k, -Memory::ReadFloat32(vec + k));
}

REGISTER_NATIVE_FUNCTION_AS(0x80293AE0, MH3_CheatSweepNormalize, "CheatSweepNormalize");

// 0x80298CA0 (capsule, plw): the hunter's body capsule against monsters: a
// segment between its joints, its ends pushed out by 40 (r2 - 14096), the
// radius 40 (+0x18). Pass through monsters (US 0x80298D70, EU 0x80298DD4),
// switched by its buttons, pushes them out by 2^65; the radius stays.
extern "C" void func_80298CA0(CpuContext* ctx);

extern "C" void MH3_CheatBodyCapsule(CpuContext* ctx) {
    const uint32_t capsule = ctx->gpr[3];
    const bool on = Toggled(Id::PassThroughMonsters);
    const float radius = Constant(-14096);

    {
        ConstantSwap push(-14096, 0x1p65f, on);
        func_80298CA0(ctx);
    }

    if (on)
        Memory::WriteFloat32(capsule + 0x18, radius);
}

REGISTER_NATIVE_FUNCTION_AS(0x80298CA0, MH3_CheatBodyCapsule, "CheatBodyCapsule");

// ShellSlotUpdate (0x800D9348): the stick's tilt scaled by 1/127 (r2 - 30376,
// its only load there); Slow-mo (US 0x800D9508, EU 0x800D956C) scales it by 1
// while it is on.
extern "C" void func_800D9348(CpuContext* ctx);

extern "C" void MH3_CheatShellSlotUpdate(CpuContext* ctx) {
    ConstantSwap scale(-30376, 1.0f, On(Id::SlowMo) && g_slow);
    func_800D9348(ctx);
}

REGISTER_NATIVE_FUNCTION_AS(0x800D9348, MH3_CheatShellSlotUpdate, "CheatShellSlotUpdate");

// 0x800E0E44 (mhchar, f1 frame, f2 rate, ...): a motion set; its rate (+0x5C)
// twice f2 (US 0x800E0F4C, EU 0x800E0FB0). Slow-mo makes it a quarter of f2
// (with the first hunter in play), so motions play at an eighth.
extern "C" void func_800E0E44(CpuContext* ctx);

extern "C" void MH3_CheatMotionSet(CpuContext* ctx) {
    if (Cheats::SlowMotion())
        ctx->fpr[2].d *= 0.125;

    func_800E0E44(ctx);
}

REGISTER_NATIVE_FUNCTION_AS(0x800E0E44, MH3_CheatMotionSet, "CheatMotionSet");
