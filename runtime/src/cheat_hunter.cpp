// Cheats in the hunter's functions, RMHP08: items at camp and underwater,
// the status screen, sharpness, charges, ammo, resistances and hit
// reactions. The Gecko codes patched instructions of these functions (US
// address, then EU); here each is a wrapper around the translated function
// that changes its arguments, results or the fields it wrote, or a port of a
// small one with the code's change in it.
//
// Doudley's codes branch to code of their own that checks u16 0x9014AE80
// (the first hunter's work +0x340, 0x0101 with two hunters) and uses other
// values with two hunters; here they use the one-hunter values.
#include "cheats.h"

#include "abi_bridge.h"
#include "guest_call.h"
#include "ppc_runtime.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <iterator>

namespace {

using Cheats::Activated;
using Cheats::Id;
using Cheats::Kind;
using Cheats::On;
using Cheats::Toggled;
using Cheats::Value;

// Player work (PLW) fields.
enum : uint32_t {
    kType = 0x2,           // u8: the weapon's type (7 long sword)
    kActionGroup = 0x9,    // u8: 3 underwater
    kMapKind = 0x15,       // u8
    kArea = 0x16,          // u8
    kPosition = 0x3C,      // f32 x, y, z
    kHeld = 0xC0,          // u16 buttons
    kAmmoLoaded = 0x269,   // u8
    kAmmoClip = 0x26A,     // u8
    kAmmoPouch = 0x270,    // s16
    kTrapsSet = 0x30C,     // u8
    kBarrelsSet = 0x30E,   // u8
    kCharge = 0x310,       // s16 (charge time), +0x312 more of it
    kCrossbow = 0x318,     // u32
    kRunSpeed = 0x354,     // f32: the action's motion speed
    kGauge36C = 0x36C,     // s16, 0 to 100
    kStamina = 0x378,      // s16, maximum at +2
    kSpiritGauge = 0x384,  // s16, 0 to 100
    kAttack = 0x3B8,       // u16, at most 700
    kAttackRate = 0x3BC,   // f32
    kResistances = 0x3C4,  // f32 x5
    kStun = 0x3FC,         // s16
    kSpiritTime = 0x468,   // s16
    kSharpLevel = 0x568,   // u8
    kSharpness = 0x56C,    // s16, maximum at +2
    kGrill = 0x5B8,        // s16 time, u8 result at +2
    kActionState = 0x1D0,  // u8
};

// Game functions.
constexpr uint32_t PlMasterCk = 0x8026FE38; // (plw): 1 for a hunter this console runs
constexpr uint32_t PlSkillCk = 0x80271374;  // (plw, skill): 1 when it has the skill
constexpr uint32_t MoveWorkAddress = 0x800CFAF4; // get_move_work_adrs (kind)
constexpr uint32_t MapNumber = 0x802AFCD8;  // get_now_mapno
constexpr uint32_t MapKind = 0x802B06CC;    // (map)
constexpr uint32_t InCampArea = 0x802B06EC; // (position): 1 in the camp's area of maps of kind 9

// The guest's sda2 constants (r2 + offset).
constexpr uint32_t kR2 = 0x8079FF60;

float Constant(int32_t offset) {
    return Memory::ReadFloat32(kR2 + offset);
}

// PPC fctiwz: toward zero, saturating.
int32_t Fctiwz(float v) {
    if (std::isnan(v) || v <= float(INT32_MIN))
        return INT32_MIN;

    if (v >= float(INT32_MAX))
        return INT32_MAX;

    return int32_t(v);
}

int16_t Read16s(uint32_t address) {
    return int16_t(Memory::Read16(address));
}

uint32_t Hunter(uint32_t index) {
    return Cheats::MoveWork(Kind::Player, index);
}

// Doudley's two-hunter check (u16 0x9014AE80): the first hunter's +0x340.
bool TwoHunters() {
    const uint32_t first = Hunter(0);
    return first != 0 && Memory::Read16(first + 0x340) == 0x0101;
}

// Enhanced focus: the focus skill always, with these factors (charges grow
// five times as fast; the great sword's charge time a 75th).
constexpr float kFocusSpirit = 5.0f, kFocusGauge = 5.0f, kFocusCharge = 0.0133f, kFocusHammer = 5.0f;

bool Focus() {
    return On(Id::EnhancedFocus) && !TwoHunters();
}

} // namespace

// EnableChangeColor (0x8027E854, equipment): 1 when its pigment can be
// changed: armor (0x8027E3A8: its data) of kind 2, else 0x8027FD44 decides.
// Enable armor pigment for all armor (C2 at US 0x8027E814, EU 0x8027E878)
// takes every armor piece, except for callers that keep the save data in
// r23 (the save data's own checks).
extern "C" void func_8027E854(CpuContext* ctx);

extern "C" void MH3_CheatArmorPigment(CpuContext* ctx) {
    if (On(Id::ArmorPigment) && ctx->gpr[23] != Cheats::UserData()) {
        uint32_t data = 0;

        {
            GuestFrame frame(ctx, 0x10);
            data = GuestCall{ctx}.Call<0x8027E3A8>({frame.Arg(3)});
        }

        if (data != 0) {
            ctx->gpr[3] = 1;
            return;
        }
    }

    func_8027E854(ctx);
}

REGISTER_NATIVE_FUNCTION_AS(0x8027E854, MH3_CheatArmorPigment, "CheatArmorPigment");

// 0x802747F8 (plw): 1 when the hunter can leave the area. Can't leave area
// (US 0x802747DC, EU 0x80274840: the health check made to fail), switched by
// its buttons, says 0.
extern "C" void func_802747F8(CpuContext* ctx);

extern "C" void MH3_CheatLeaveArea(CpuContext* ctx) {
    if (Toggled(Id::CantLeaveArea)) {
        ctx->gpr[3] = 0;
        return;
    }

    func_802747F8(ctx);
}

REGISTER_NATIVE_FUNCTION_AS(0x802747F8, MH3_CheatLeaveArea, "CheatLeaveArea");

// 0x8027851C (plw): 1 when a farcaster can be used: not in area 2 of maps of
// kind 6 and 17, not in the camp's area. Farcaster at camp (US 0x80278528, EU
// 0x8027858C) leaves out the camp check, Farcaster in phase 2 (0x80278500/
// 0x80278564) the area 2 check.
extern "C" void func_8027851C(CpuContext* ctx);

extern "C" void MH3_CheatFarcaster(CpuContext* ctx) {
    if (!On(Id::FarcasterAtCamp) && !On(Id::FarcasterPhase2)) {
        func_8027851C(ctx);
        return;
    }

    const uint32_t plw = ctx->gpr[3];
    const uint8_t area = Memory::Read8(plw + kArea);
    uint32_t result = 1;

    {
        GuestFrame frame(ctx, 0x10);
        GuestCall game{ctx};
        const uint32_t map = game.Call<MapNumber>() & 0xFF;
        const uint32_t kind = game.Call<MapKind>({map}) & 0xFF;

        if (kind == 9) {
            result = game.Call<InCampArea>({plw + kPosition}) == 1 ? 0 : 1;
        } else {
            if ((kind == 6 || kind == 17) && area == 2 && !On(Id::FarcasterPhase2))
                result = 0;

            if (result != 0 && !On(Id::FarcasterAtCamp)) {
                const uint32_t camp = game.Call<MoveWorkAddress>({0});

                if (camp != 0 && area == Memory::Read8(camp + 0xF6))
                    result = 0;
            }
        }
    }

    ctx->gpr[3] = result;
}

REGISTER_NATIVE_FUNCTION_AS(0x8027851C, MH3_CheatFarcaster, "CheatFarcaster");

// 0x8027804C (plw): 1 while no trap of the hunter's is set. Set unlimited
// traps (US 0x80277FE8, EU 0x8027804C: the count's load made li r3,0) says 1.
extern "C" void MH3_TrapsFree(CpuContext* ctx) {
    const uint32_t set = Memory::Read8(ctx->gpr[3] + kTrapsSet);
    ctx->gpr[3] = On(Id::BullyUnlimitedTraps) ? 1 : (set - 1) >> 31;
}

REGISTER_NATIVE_FUNCTION_AS(0x8027804C, MH3_TrapsFree, "TrapsFree");

// 0x8027831C and 0x802784B4 (plw): 1 when a pit trap (a shock trap) can be
// set: no trap set (0x8027804C) and the place allows it (0x802781A8,
// 0x80278374). Pitfall at camp and Shock at camp (US 0x802782EC and
// 0x80278484, EU 0x80278350 and 0x802784E8) leave out the place.
extern "C" void func_8027831C(CpuContext* ctx);
extern "C" void func_802784B4(CpuContext* ctx);

namespace {

void TrapAnywhere(CpuContext* ctx, bool on, void (*original)(CpuContext*)) {
    const uint32_t plw = ctx->gpr[3];
    original(ctx);

    if (!on || ctx->gpr[3] == 1)
        return;

    uint32_t free = 0;

    {
        GuestFrame frame(ctx, 0x10);
        free = GuestCall{ctx}.Call<0x8027804C>({plw});
    }

    if (free == 1)
        ctx->gpr[3] = 1;
}

} // namespace

extern "C" void MH3_CheatPitfall(CpuContext* ctx) {
    TrapAnywhere(ctx, On(Id::PitfallAtCamp), func_8027831C);
}

REGISTER_NATIVE_FUNCTION_AS(0x8027831C, MH3_CheatPitfall, "CheatPitfall");

extern "C" void MH3_CheatShockTrap(CpuContext* ctx) {
    TrapAnywhere(ctx, On(Id::ShockAtCamp), func_802784B4);
}

REGISTER_NATIVE_FUNCTION_AS(0x802784B4, MH3_CheatShockTrap, "CheatShockTrap");

// 0x80277FB8 (plw): 1 when a large barrel bomb can be set: fewer than two set,
// not in the camp's area. Set unlimited barrel bombs (US 0x80277F68, EU
// 0x80277FCC: the count's load made li r0,0) leaves out the count, LBB at
// camp (0x80277FA4/0x80278008) the camp.
extern "C" void func_80277FB8(CpuContext* ctx);

extern "C" void MH3_CheatBarrelBomb(CpuContext* ctx) {
    if (!On(Id::BullyUnlimitedBarrelBombs) && !On(Id::BarrelBombAtCamp)) {
        func_80277FB8(ctx);
        return;
    }

    const uint32_t plw = ctx->gpr[3];
    const uint32_t set = On(Id::BullyUnlimitedBarrelBombs) ? 0 : Memory::Read8(plw + kBarrelsSet);
    uint32_t result = 1;

    if (set >= 2) {
        result = 0;
    } else {
        GuestFrame frame(ctx, 0x10);
        GuestCall game{ctx};

        if (Memory::Read8(plw + kMapKind) == 9) {
            if (game.Call<InCampArea>({plw + kPosition}) == 1)
                result = 0;
        } else if (!On(Id::BarrelBombAtCamp)) {
            const uint32_t camp = game.Call<MoveWorkAddress>({0});

            if (camp != 0 && Memory::Read8(plw + kArea) == Memory::Read8(camp + 0xF6))
                result = 0;
        }
    }

    ctx->gpr[3] = result;
}

REGISTER_NATIVE_FUNCTION_AS(0x80277FB8, MH3_CheatBarrelBomb, "CheatBarrelBomb");

// 0x8027C26C (plw, item): 1 when the item can be used now. Underwater (+9
// 3) smoke bombs, steaks and felvine bombs cannot; Smoke underwater (US
// 0x8027C65C, EU 0x8027C6C0), Steak underwater (0x8027C670/C6D4) and Felvine
// underwater (0x8027C7E4/C848) leave out that check: the call sees the
// hunter on land.
extern "C" void func_8027C26C(CpuContext* ctx);

extern "C" void MH3_CheatItemUsable(CpuContext* ctx) {
    static constexpr uint16_t kSmoke[]{345, 346, 347};
    static constexpr uint16_t kSteak[]{26, 27, 28, 166, 167, 208, 214};
    const uint32_t plw = ctx->gpr[3];
    const uint16_t item = uint16_t(ctx->gpr[4]);
    const auto in = [item](const auto& items) {
        return std::find(std::begin(items), std::end(items), item) != std::end(items);
    };

    const bool land = (On(Id::SmokeUnderwater) && in(kSmoke)) || (On(Id::SteakUnderwater) && in(kSteak)) ||
                      (On(Id::FelvineUnderwater) && item == 264);

    if (!land || Memory::Read8(plw + kActionGroup) != 3) {
        func_8027C26C(ctx);
        return;
    }

    Memory::Write8(plw + kActionGroup, 0);
    func_8027C26C(ctx);
    Memory::Write8(plw + kActionGroup, 3);
}

REGISTER_NATIVE_FUNCTION_AS(0x8027C26C, MH3_CheatItemUsable, "CheatItemUsable");

// 0x8030BAC8 (weapon type, raw): the raw attack as shown, scaled by the
// weapon type's factor (in percent). Show real raw (US 0x8030BA90, EU
// 0x8030BAF4: the factor made 100) shows it unscaled.
extern "C" void func_8030BAC8(CpuContext* ctx);

extern "C" void MH3_CheatRawShown(CpuContext* ctx) {
    if (On(Id::ShowRealRaw)) {
        ctx->gpr[3] = ctx->gpr[4] & 0xFFFF;
        return;
    }

    func_8030BAC8(ctx);
}

REGISTER_NATIVE_FUNCTION_AS(0x8030BAC8, MH3_CheatRawShown, "CheatRawShown");

// 0x80316290 and 0x80316534 (..., u8* kind, s16* value): the element (the
// status) as shown, ten times the weapon's; flag 1 of the result when there
// is one. Show real element and Show real status (US 0x80316420 and
// 0x803166D0, EU 0x80316484 and 0x80316734: the times 10 dropped) show the
// weapon's value.
extern "C" void func_80316290(CpuContext* ctx);
extern "C" void func_80316534(CpuContext* ctx);

namespace {

void ShowUnscaled(CpuContext* ctx, bool on, void (*original)(CpuContext*)) {
    const uint32_t value = ctx->gpr[7];
    original(ctx);

    if (on && (ctx->gpr[3] & 1) != 0)
        Memory::Write16(value, uint16_t(Read16s(value) / 10));
}

} // namespace

extern "C" void MH3_CheatElementShown(CpuContext* ctx) {
    ShowUnscaled(ctx, On(Id::ShowRealElement), func_80316290);
}

REGISTER_NATIVE_FUNCTION_AS(0x80316290, MH3_CheatElementShown, "CheatElementShown");

extern "C" void MH3_CheatStatusShown(CpuContext* ctx) {
    ShowUnscaled(ctx, On(Id::ShowRealStatus), func_80316534);
}

REGISTER_NATIVE_FUNCTION_AS(0x80316534, MH3_CheatStatusShown, "CheatStatusShown");

// 0x802653D8 (plw): a hit's conditions (stun, poison, and so on); 1 when the
// hit leaves the hunter without a reaction. Never stunned (US 0x802654E8, EU
// 0x8026554C: the stun gauge's load dropped) keeps the stun gauge; No
// knockback (US 0x80265FA0, EU 0x80266004: in the hit's function, 0x8026585C,
// the reaction always skipped) and Unknockable players (the same while the
// first hunter's work is in use) make it 1 for that caller.
extern "C" void func_802653D8(CpuContext* ctx);

extern "C" void MH3_CheatHitConditions(CpuContext* ctx) {
    const uint32_t plw = ctx->gpr[3];
    const uint32_t from = ctx->lr;
    const int16_t stun = Read16s(plw + kStun);

    func_802653D8(ctx);

    if (On(Id::NeverStunned) && Read16s(plw + kStun) > stun)
        Memory::Write16(plw + kStun, uint16_t(stun));

    const uint32_t first = Hunter(0);
    const bool unknockable = On(Id::UnknockablePlayers) && first != 0 && Memory::Read16(first) != 0;

    if (from == 0x80265F70 && (On(Id::NoKnockback) || unknockable))
        ctx->gpr[3] = 1;
}

REGISTER_NATIVE_FUNCTION_AS(0x802653D8, MH3_CheatHitConditions, "CheatHitConditions");

// 0x8027AC90 (plw, s8, s8): sets the weapon's sharpness levels (+0x568 from
// the weapon type's table for -1). Weapons always sharp (Seth, US 0x8027AC6C,
// 0x8027AC70 and 0x8027AC98, EU 0x8027ACD0, 0x8027ACD4 and 0x8027ACFC: the
// stores dropped) keeps +0x568 (but for 0x71 of the long sword's state).
extern "C" void func_8027AC90(CpuContext* ctx);

extern "C" void MH3_CheatSharpLevels(CpuContext* ctx) {
    const uint32_t plw = ctx->gpr[3];
    const uint8_t level = uint8_t(ctx->gpr[4]);
    const uint8_t before = Memory::Read8(plw + kSharpLevel);

    func_8027AC90(ctx);

    if (!On(Id::SethWeaponsSharp))
        return;

    const bool longSword = level == 0xFF && Memory::Read8(plw + kType) == 7 && Memory::Read8(plw + kSharpLevel) == 0x71;

    if (!longSword)
        Memory::Write8(plw + kSharpLevel, before);
}

REGISTER_NATIVE_FUNCTION_AS(0x8027AC90, MH3_CheatSharpLevels, "CheatSharpLevels");

// 0x8029FD60 (the item menu's entries): +134 and +158 of 0x806AED48 enable
// combining and the speed combos (0x8031AC68 decides). Combine never disabled
// and Speed combos never disabled (US 0x8029FD94 and 0x8029FDA4, EU
// 0x8029FDF8 and 0x8029FE08: li r0,1) enable them.
extern "C" void func_8029FD60(CpuContext* ctx);

extern "C" void MH3_CheatItemMenu(CpuContext* ctx) {
    constexpr uint32_t kMenu = 0x806AED48;
    func_8029FD60(ctx);

    if (Memory::Read8(kMenu + 25) != 0 || Memory::Read8(kMenu + 15) != 1)
        return;

    if (On(Id::CombineEnabled))
        Memory::Write8(kMenu + 134, 1);

    if (On(Id::SpeedCombosEnabled))
        Memory::Write8(kMenu + 158, 1);
}

REGISTER_NATIVE_FUNCTION_AS(0x8029FD60, MH3_CheatItemMenu, "CheatItemMenu");

namespace {

// The focus skill's factor for the four charge functions: skill 191 (focus)
// or 192 (its opposite), else 1; Enhanced focus (C2 at US 0x8027C92C,
// 0x8027D5F4, 0x80281200 and 0x80284418) gives the focus always with its own
// factor.
float FocusFactor(CpuContext* ctx, uint32_t plw, float focused, int32_t focus, int32_t unfocus, bool* hit = nullptr) {
    GuestFrame frame(ctx, 0x10);
    GuestCall game{ctx};

    if (Focus() || game.Call<PlSkillCk>({plw, 191}) == 1) {
        if (hit != nullptr)
            *hit = true;

        return Focus() ? focused : Constant(focus);
    }

    if (game.Call<PlSkillCk>({plw, 192}) == 1) {
        if (hit != nullptr)
            *hit = true;

        return Constant(unfocus);
    }

    return 1.0f;
}

} // namespace

// 0x8027C918 (plw, s16 change): the long sword's spirit gauge (0 to 100;
// full, it glows for 900 steps and does not go down). Long swords max
// sharpness (US 0x8027C96C, EU 0x8027C9D0: the sum made 0) holds the gauge
// at 0; Enhanced focus grows it five times as fast.
extern "C" void func_8027C918(CpuContext* ctx);

extern "C" void MH3_CheatSpiritGauge(CpuContext* ctx) {
    if (!On(Id::LongSwordsMaxSharpness) && !Focus()) {
        func_8027C918(ctx);
        return;
    }

    const uint32_t plw = ctx->gpr[3];
    const int16_t change = int16_t(ctx->gpr[4]);

    if (Memory::Read8(plw + kType) != 7)
        return;

    if (change < 0 && Read16s(plw + kSpiritTime) > 0)
        return;

    float amount = float(change);

    if (amount > 0.0f) {
        bool hit = false;
        const float factor = FocusFactor(ctx, plw, kFocusSpirit, -14860, -14756, &hit);

        if (hit)
            amount *= factor;
    }

    const int16_t gauge = On(Id::LongSwordsMaxSharpness) ? 0 : int16_t(Read16s(plw + kSpiritGauge) + Fctiwz(amount));
    Memory::Write16(plw + kSpiritGauge, uint16_t(gauge));

    if (amount >= 0.0f) {
        if (gauge < 100)
            return;

        Memory::Write16(plw + kSpiritGauge, 100);
        GuestFrame frame(ctx, 0x10);
        GuestCall game{ctx};

        if (game.Call<PlMasterCk>({plw}) == 1 && Read16s(plw + kSpiritTime) == 0)
            game.Call<0x80114C84>({plw, 1});

        game.Call<0x8027C900>({plw, 900});
    } else if (gauge < 0) {
        Memory::Write16(plw + kSpiritGauge, 0);
    }
}

REGISTER_NATIVE_FUNCTION_AS(0x8027C918, MH3_CheatSpiritGauge, "CheatSpiritGauge");

// PlAreaConditionTick (0x8027D608, plw, s16 change): the gauge at +0x36C (0
// to 100). Thunder weapons always charged (US 0x8027D634, EU 0x8027D698: the
// sum made 100) fills it; Enhanced focus grows it five times as fast.
extern "C" void func_8027D608(CpuContext* ctx);

extern "C" void MH3_CheatGauge36C(CpuContext* ctx) {
    if (!On(Id::ThunderAlwaysCharged) && !Focus()) {
        func_8027D608(ctx);
        return;
    }

    const uint32_t plw = ctx->gpr[3];
    float amount = float(int16_t(ctx->gpr[4]));

    if (amount > 0.0f) {
        bool hit = false;
        const float factor = FocusFactor(ctx, plw, kFocusGauge, -14860, -14756, &hit);

        if (hit)
            amount *= factor;
    }

    const int16_t gauge = On(Id::ThunderAlwaysCharged) ? 100 : int16_t(Read16s(plw + kGauge36C) + Fctiwz(amount));
    Memory::Write16(plw + kGauge36C, uint16_t(std::clamp<int16_t>(gauge, 0, 100)));
}

REGISTER_NATIVE_FUNCTION_AS(0x8027D608, MH3_CheatGauge36C, "CheatGauge36C");

// PlChargeTime (0x80281210, plw, s16 time): a charge's time, scaled by the
// focus skill, 9999 for 9999 and more. Great sword inf charge and Lv3 charge
// always (US 0x802811C8, EU 0x8028122C) makes every charge time 9999 (with
// the charge set below, in StepHunterCodes); Enhanced focus scales it by a
// 75th.
extern "C" void func_80281210(CpuContext* ctx);

extern "C" void MH3_CheatChargeTime(CpuContext* ctx) {
    const bool always = On(Id::GreatSwordCharge) && !TwoHunters();

    if (!always && !Focus()) {
        func_80281210(ctx);
        return;
    }

    const uint32_t plw = ctx->gpr[3];
    const int16_t time = int16_t(ctx->gpr[4]);

    if (always || time >= 9999) {
        ctx->gpr[3] = 9999;
        return;
    }

    bool hit = false;
    const float factor = FocusFactor(ctx, plw, kFocusCharge, -14644, -14700, &hit);
    const float scaled = hit ? float(time) * factor : float(time);
    ctx->gpr[3] = uint32_t(Fctiwz(scaled));
}

REGISTER_NATIVE_FUNCTION_AS(0x80281210, MH3_CheatChargeTime, "CheatChargeTime");

// PlChargeLevel (0x802812C0, plw): the charge's level from (s16 time, s16
// level) pairs at 0x805C7478: 30, 60, 70, 80 and 9999 for levels 0 to 3 and
// overcharged (2). Great sword no overcharging (US 0x80281280, EU
// 0x802812E4) stays at level 3.
extern "C" void func_802812C0(CpuContext* ctx);

extern "C" void MH3_CheatChargeLevel(CpuContext* ctx) {
    if (!On(Id::NoOvercharging) || TwoHunters()) {
        func_802812C0(ctx);
        return;
    }

    constexpr uint32_t kLevels = 0x805C7478;
    const uint32_t plw = ctx->gpr[3];
    uint32_t level = 0;

    {
        GuestFrame frame(ctx, 0x10);
        GuestCall game{ctx};

        for (uint32_t entry = kLevels;; entry += 4) {
            const int16_t time = int16_t(game.Call<0x80281210>({plw, uint32_t(int32_t(Read16s(entry)))}));

            // The last pair (9999) is the overcharge: the level before it.
            if (Read16s(entry) == 9999) {
                level = Memory::Read16(entry - 2) & 0xFF;
                break;
            }

            if (Read16s(plw + kCharge) < time) {
                level = Memory::Read16(entry + 2) & 0xFF;
                break;
            }
        }
    }

    ctx->gpr[3] = level;
}

REGISTER_NATIVE_FUNCTION_AS(0x802812C0, MH3_CheatChargeLevel, "CheatChargeLevel");

// 0x80284430 (plw): the hammer's charge counts up to 999. Hammer max charge
// always (US 0x802843D0, EU 0x80284434) sets 999 at once (with the charge
// set below, in StepHunterCodes).
extern "C" void func_80284430(CpuContext* ctx);

extern "C" void MH3_CheatHammerCharge(CpuContext* ctx) {
    if (On(Id::HammerMaxCharge) && !TwoHunters()) {
        Memory::Write16(ctx->gpr[3] + kCharge, 999);
        return;
    }

    func_80284430(ctx);
}

REGISTER_NATIVE_FUNCTION_AS(0x80284430, MH3_CheatHammerCharge, "CheatHammerCharge");

// 0x80284454 (plw): the hammer's charge time (30 steps, divided by the focus
// skill's factor). Enhanced focus divides it by 5.
extern "C" void func_80284454(CpuContext* ctx);

extern "C" void MH3_CheatHammerChargeTime(CpuContext* ctx) {
    if (!Focus()) {
        func_80284454(ctx);
        return;
    }

    const uint32_t plw = ctx->gpr[3];
    bool hit = false;
    const float factor = FocusFactor(ctx, plw, kFocusHammer, -14532, -14528, &hit);
    const float base = Constant(-14536);
    ctx->gpr[3] = uint32_t(Fctiwz(hit ? base / factor : base));
}

REGISTER_NATIVE_FUNCTION_AS(0x80284454, MH3_CheatHammerChargeTime, "CheatHammerChargeTime");

// PlSetConditionState (0x802768CC, plw, s16 change): stamina += change,
// within 0 and the maximum. Infinite stamina (Bully, US 0x802768AC and
// 0x802768B0, EU 0x80276910 and 0x80276914) sets stamina and maximum to 605.
extern "C" void func_802768CC(CpuContext* ctx);

extern "C" void MH3_CheatStaminaChange(CpuContext* ctx) {
    if (!On(Id::BullyInfiniteStamina)) {
        func_802768CC(ctx);
        return;
    }

    const uint32_t plw = ctx->gpr[3];
    const int16_t change = int16_t(ctx->gpr[4]);
    bool changes = false;

    {
        GuestFrame frame(ctx, 0x10);
        GuestCall game{ctx};
        changes = game.Call<PlMasterCk>({plw}) != 0 && (change >= 0 || game.Call<0x80276880>({plw}) != 1);
    }

    if (changes) {
        Memory::Write16(plw + kStamina + 2, 605);
        Memory::Write16(plw + kStamina, 605);
    }
}

REGISTER_NATIVE_FUNCTION_AS(0x802768CC, MH3_CheatStaminaChange, "CheatStaminaChange");

// 0x802786D8 (plw, s16 change, u8): sharpness += change (halved or doubled by
// skills when it goes down), within 0 and the maximum, then the level and its
// messages. Weapons always sharp (Bully, US 0x80278754, EU 0x802787B8; and
// Skiller's, C2 at 0x80278758/0x802787BC) set it to the maximum.
extern "C" void func_802786D8(CpuContext* ctx);

extern "C" void MH3_CheatSharpness(CpuContext* ctx) {
    if (On(Id::BullyWeaponsSharp) || On(Id::SkillerWeaponSharp)) {
        const uint32_t plw = ctx->gpr[3];
        const int32_t to = Read16s(plw + kSharpness + 2) - Read16s(plw + kSharpness);
        ctx->gpr[4] = uint32_t(int32_t(int16_t(to)));
    }

    func_802786D8(ctx);
}

REGISTER_NATIVE_FUNCTION_AS(0x802786D8, MH3_CheatSharpness, "CheatSharpness");

// 0x80287A30 (plw, ...): the bowgun's shot. A shot takes one round of the
// loaded ones (and one of the pouch, 0x80272E94) unless +6 is set; +5 0 starts
// it. Infinite ammo (Bully, US 0x80287DA0, EU 0x80287E04) reloads the clip
// (+0x26A) instead, No reload (Skiller, the same instruction) takes none;
// Rapidfire all ammo (US 0x80287A60, EU 0x80287AC4: 6 for 0 in +6 and +7),
// always or while A (0x20) is held, takes none either.
extern "C" void func_80287A30(CpuContext* ctx);

extern "C" void MH3_CheatBowgunShot(CpuContext* ctx) {
    const uint32_t plw = ctx->gpr[3];
    const uint8_t state = Memory::Read8(plw + 5);
    const uint8_t loaded = Memory::Read8(plw + kAmmoLoaded);
    const uint32_t first = Hunter(0), second = Hunter(1);
    const bool holding = (first != 0 && Memory::Read16(first + kHeld) == 0x20) || (second != 0 && Memory::Read16(second + kHeld) == 0x20);
    const bool rapid = (On(Id::RapidFire) && first != 0 && Memory::Read16(first) == 0x0101) || (On(Id::RapidFireHold) && holding);

    // The start stores the code's 6 in +7 where the original stores its 0:
    // without a kind (r7) or without the ammo's data (0x80279478).
    bool rapidCount = false;

    if (rapid && state == 0) {
        const uint32_t kind = ctx->gpr[7] & 0xFF;
        const auto f1 = ctx->fpr[1], f2 = ctx->fpr[2];

        {
            GuestFrame frame(ctx, 0x10);
            rapidCount = kind == 0 || GuestCall{ctx}.Call<0x80279478>({plw, Memory::Read8(plw + 0x26C)}) == 0;
        }

        ctx->fpr[1] = f1;
        ctx->fpr[2] = f2;
    }

    func_80287A30(ctx);

    if (rapid && state == 0 && Memory::Read8(plw + 5) == 1) {
        Memory::Write8(plw + 6, 6);

        if (rapidCount)
            Memory::Write8(plw + 7, 6);
    }

    const bool shot = loaded != 0 && Memory::Read8(plw + kAmmoLoaded) == loaded - 1 && Read16s(plw + kAmmoPouch) != 0;

    if (shot && On(Id::BullyInfiniteAmmo))
        Memory::Write8(plw + kAmmoLoaded, Memory::Read8(plw + kAmmoClip));
    else if (shot && On(Id::NoReload))
        Memory::Write8(plw + kAmmoLoaded, loaded);
}

REGISTER_NATIVE_FUNCTION_AS(0x80287A30, MH3_CheatBowgunShot, "CheatBowgunShot");

// FieldSetAction (0x80275754, plw, group, action, flags): starts an action,
// clearing its state (+0x318 among it) and setting the motion speed (+0x354,
// 1). Crossbow rapid fire (US 0x80275790, EU 0x802757F4: the clear dropped)
// keeps +0x318; Enhanced movement speed (US 0x80275784, EU 0x802757E8) makes
// the speed 2.
extern "C" void func_80275754(CpuContext* ctx);

extern "C" void MH3_CheatSetAction(CpuContext* ctx) {
    const uint32_t plw = ctx->gpr[3];
    const uint32_t crossbow = Memory::Read32(plw + kCrossbow);

    func_80275754(ctx);

    if (On(Id::CrossbowRapidFire))
        Memory::Write32(plw + kCrossbow, crossbow);

    if (On(Id::EnhancedMovement) && !TwoHunters())
        Memory::WriteFloat32(plw + kRunSpeed, 2.0f);
}

REGISTER_NATIVE_FUNCTION_AS(0x80275754, MH3_CheatSetAction, "CheatSetAction");

// 0x8027EE6C (equipment, &data, &...): the equipment's data; the weapon's
// stats (func_80273B78, from 0x80273C34) read its attack at +0xA. Weapon max
// attack base (C2 at US 0x80273C08, EU 0x80273C6C), with its buttons held,
// writes 0x7FFF there.
extern "C" void func_8027EE6C(CpuContext* ctx);

extern "C" void MH3_CheatEquipData(CpuContext* ctx) {
    const uint32_t out = ctx->gpr[4];
    const uint32_t from = ctx->lr;

    func_8027EE6C(ctx);

    if (On(Id::MaxAttackBase) && from == 0x80273C34 && ctx->gpr[3] == 0 && Activated(Id::MaxAttackBase)) {
        const uint32_t data = Memory::Read32(out);

        if (data != 0)
            Memory::Write16(data + 0xA, 0x7FFF);
    }
}

REGISTER_NATIVE_FUNCTION_AS(0x8027EE6C, MH3_CheatEquipData, "CheatEquipData");

// 0x8027ED10 (equipment): the weapon's data. Max sharp (C2 at US 0x803146DC,
// EU 0x80314740: +8 0x0505) and Max affinity (C2 at US 0x802743D8, EU
// 0x8027443C: +0xD 0x7F), with their buttons held, write to it for those
// callers.
extern "C" void func_8027ED10(CpuContext* ctx);

extern "C" void MH3_CheatWeaponData(CpuContext* ctx) {
    const uint32_t from = ctx->lr;
    func_8027ED10(ctx);

    const uint32_t data = ctx->gpr[3];

    if (data == 0)
        return;

    if (from == 0x8031471C && On(Id::MaxSharpness) && Activated(Id::MaxSharpness))
        Memory::Write16(data + 8, 0x0505);

    if (from == 0x8027443C && On(Id::MaxAffinity) && Activated(Id::MaxAffinity))
        Memory::Write8(data + 0xD, 0x7F);
}

REGISTER_NATIVE_FUNCTION_AS(0x8027ED10, MH3_CheatWeaponData, "CheatWeaponData");

// 0x80270458 (plw, u16 defense, u8* boosted): the defense with items' and
// skills' bonuses (item 598 adds 8). Add to defense (US 0x8027044C and
// 0x80270450, EU 0x802704B0 and 0x802704B4) adds its value always.
extern "C" void func_80270458(CpuContext* ctx);

extern "C" void MH3_CheatDefense(CpuContext* ctx) {
    if (!On(Id::AddToDefense)) {
        func_80270458(ctx);
        return;
    }

    const uint32_t boosted = ctx->gpr[5];
    ctx->gpr[4] = (ctx->gpr[4] + uint32_t(Value(Id::AddToDefense))) & 0xFFFF;
    func_80270458(ctx);
    Memory::Write8(boosted, 1);
}

REGISTER_NATIVE_FUNCTION_AS(0x80270458, MH3_CheatDefense, "CheatDefense");

// 0x802703C0 (plw): the attack (+0x3B8, at most 700) and its rate. Add 1000
// to attack (US 0x802703AC and 0x802703B0, EU 0x80270410 and 0x80270414)
// makes it 1000.
extern "C" void func_802703C0(CpuContext* ctx);

extern "C" void MH3_CheatAttack(CpuContext* ctx) {
    const uint32_t plw = ctx->gpr[3];
    func_802703C0(ctx);

    if (On(Id::Add1000Attack)) {
        Memory::Write16(plw + kAttack, 1000);
        Memory::WriteFloat32(plw + kAttackRate, float(1000.0f / Constant(-14940)));
    }
}

REGISTER_NATIVE_FUNCTION_AS(0x802703C0, MH3_CheatAttack, "CheatAttack");

// 0x80270D08 (plw): the five elemental resistances. Max fire, water, thunder,
// dragon and ice (US 0x80270D04 ..., EU 0x80270D68, 0x80270DA8, 0x80270DE8,
// 0x80270E28 and 0x80270E68: the skill's share made the int-to-float
// constant) make one of them 2^52 + 2^31.
extern "C" void func_80270D08(CpuContext* ctx);

extern "C" void MH3_CheatResistances(CpuContext* ctx) {
    static constexpr Id kElements[]{Id::MaxFire, Id::MaxWater, Id::MaxThunder, Id::MaxDragon, Id::MaxIce};
    const uint32_t plw = ctx->gpr[3];
    func_80270D08(ctx);

    for (uint32_t i = 0; i < std::size(kElements); ++i)
        if (On(kElements[i]))
            Memory::WriteFloat32(plw + kResistances + 4 * i, 4503601774854144.0);
}

REGISTER_NATIVE_FUNCTION_AS(0x80270D08, MH3_CheatResistances, "CheatResistances");

// 0x80274BC0 (plw, ammo, weapon, frame, barrel, u8* boosted): the level an
// ammo can be loaded at: the weapon's and the frame's (0x8027F06C), the
// barrel's, then the skills'. Enable all ammo (C2 at US 0x80274BC4, EU
// 0x80274C28) starts at 1 before the barrel: here the result is redone with
// the weapon's and the frame's share taken out.
extern "C" void func_80274BC0(CpuContext* ctx);

extern "C" void MH3_CheatAmmoLevel(CpuContext* ctx) {
    if (!On(Id::EnableAllAmmo)) {
        func_80274BC0(ctx);
        return;
    }

    const uint32_t plw = ctx->gpr[3], ammo = ctx->gpr[4] & 0xFF, weapon = ctx->gpr[5];
    const uint32_t frameEquip = ctx->gpr[6], barrel = ctx->gpr[7], boosted = ctx->gpr[8];

    func_80274BC0(ctx);

    const uint32_t result = ctx->gpr[3] & 0xFF;
    uint32_t base = 0, barrels = 0;
    bool skill = false;

    {
        GuestFrame frame(ctx, 0x10);
        GuestCall game{ctx};
        base = game.Call<0x8027F06C>({weapon, ammo});

        if (frameEquip != 0 && Memory::Read8(frameEquip) == 12)
            base += game.Call<0x8027F06C>({frameEquip, ammo});

        if (barrel != 0 && Memory::Read8(barrel) == 13)
            barrels = game.Call<0x8027F06C>({barrel, ammo});

        skill = game.Call<PlSkillCk>({plw, 170}) == 1;
    }

    // result = base + barrels + skills (+ 1 for skill 170 unless 0).
    const uint32_t before = skill && result != 0 ? result - 1 : result;
    const uint32_t skills = before - base - barrels;
    ctx->gpr[3] = (1 + barrels + skills + (skill ? 1 : 0)) & 0xFF;

    if (skill)
        Memory::Write8(boosted, 1);
}

REGISTER_NATIVE_FUNCTION_AS(0x80274BC0, MH3_CheatAmmoLevel, "CheatAmmoLevel");

// Pl_frame_check (0x8026A378, plw, ..., f1 frame, f2): 1 once the motion is
// past the frame. Instant sharpen (US 0x8024F564, EU 0x8024F5C8: 260 made 2)
// ends the whetstone's motion early.
extern "C" void func_8026A378(CpuContext* ctx);

extern "C" void MH3_CheatFrameCheck(CpuContext* ctx) {
    if (On(Id::InstantSharpen) && ctx->lr == 0x8024F5D4 && !TwoHunters())
        ctx->fpr[1].d = 2.0;

    func_8026A378(ctx);
}

REGISTER_NATIVE_FUNCTION_AS(0x8026A378, MH3_CheatFrameCheck, "CheatFrameCheck");

// 0x8024AAB8 (plw, ...): grilling meat; the time (+0x5B8) counts up and
// decides the result (+0x5BA, 4 burnt). Instant well-done steak (US 0x8024AE14
// and 0x8024AE3C, EU 0x8024AE78 and 0x8024AEA0) counts 300 at once and makes
// burnt well done (2).
extern "C" void func_8024AAB8(CpuContext* ctx);

extern "C" void MH3_CheatGrill(CpuContext* ctx) {
    if (!On(Id::InstantSteak) || TwoHunters()) {
        func_8024AAB8(ctx);
        return;
    }

    const uint32_t plw = ctx->gpr[3];
    const int16_t time = Read16s(plw + kGrill);
    const uint8_t result = Memory::Read8(plw + kGrill + 2);
    Memory::Write16(plw + kGrill, uint16_t(time + 299));

    func_8024AAB8(ctx);

    const int16_t now = Read16s(plw + kGrill);

    if (now == int16_t(time + 299))
        Memory::Write16(plw + kGrill, uint16_t(time));
    else if (now == int16_t(time + 300) && result != 4 && Memory::Read8(plw + kGrill + 2) == 4)
        Memory::Write8(plw + kGrill + 2, 2);
}

REGISTER_NATIVE_FUNCTION_AS(0x8024AAB8, MH3_CheatGrill, "CheatGrill");

// MHcharSetRate (0x800E16A4, mhchar, f1 rate): the motion's rate (+0x5C, twice
// the given one). Speed up when running (C2 at US 0x800E1664, EU 0x800E16C8)
// makes it 6 while the first hunter's motion speed (+0x354) is 1.25.
extern "C" void func_800E16A4(CpuContext* ctx);

extern "C" void MH3_CheatCharRate(CpuContext* ctx) {
    if (On(Id::RunSpeedUp)) {
        const uint32_t first = Hunter(0);

        if (first != 0 && Memory::Read16(first + kRunSpeed) == 0x3FA0)
            ctx->fpr[1].d = 6.0 / Constant(-30304);
    }

    func_800E16A4(ctx);
}

REGISTER_NATIVE_FUNCTION_AS(0x800E16A4, MH3_CheatCharRate, "CheatCharRate");

// 0x802B7880 (camera): the hunter falling (more than 10 below its last
// ground). Cancel camera zoom-in when falling (US 0x802B7884, EU 0x802B78E8:
// another constant, 300) waits for 300.
extern "C" void func_802B7880(CpuContext* ctx);

extern "C" void MH3_CheatFallCamera(CpuContext* ctx) {
    if (!On(Id::NoFallZoom)) {
        func_802B7880(ctx);
        return;
    }

    // The constant (r2 - 13732) for this call only; its other users are
    // camera scripts this function does not call.
    const uint32_t constant = kR2 - 13732;
    const uint32_t saved = Memory::Read32(constant);
    Memory::WriteFloat32(constant, 300.0);
    func_802B7880(ctx);
    Memory::Write32(constant, saved);
}

REGISTER_NATIVE_FUNCTION_AS(0x802B7880, MH3_CheatFallCamera, "CheatFallCamera");

namespace Cheats {

// The great sword's and the hammer's charges (Doudley's data lines): with
// the action state +0x1D0 7 (a great sword charge) the charge is 0x00460300,
// with 9 (the hammer's) 0x40.
void StepHunterCodes() {
    for (uint32_t i = 0; i < 2; ++i) {
        const uint32_t plw = Hunter(i);

        if (plw == 0 || Memory::Read16(plw) != 0x0101)
            continue;

        const uint8_t state = Memory::Read8(plw + kActionState);

        if (On(Id::GreatSwordCharge) && state == 7)
            Memory::Write32(plw + kCharge, 0x00460300);

        if (On(Id::HammerMaxCharge) && state == 9)
            Memory::Write16(plw + kCharge, 0x40);
    }
}

} // namespace Cheats
