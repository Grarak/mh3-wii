// Cheats in the monsters' functions, RMHP08: health, rage, stamina, speed,
// sight and the quest's large monster entries. The Gecko codes patched
// instructions of these functions (US address, then EU); here each is a
// wrapper around the translated function or a port with the code's change.
#include "cheats.h"

#include "abi_bridge.h"
#include "monster_control.h"
#include "guest_call.h"
#include "ppc_runtime.h"

#include <climits>
#include <cmath>
#include <cstdint>

namespace {

using Cheats::Id;
using Cheats::On;
using Cheats::Toggled;
using Cheats::Value;

// Monster (enemy) work.
enum : uint32_t {
    kInit = 0x4,          // u8: 0 starting, 1 running
    kSpawn = 0x17,        // u8: the spawn state (0 in play)
    kAlpha = 0x1D4,       // f32: the body colour's scale
    kHealth = 0x7A0,      // s32
    kMaxHealth = 0x7A4,   // s32
    kHealthFloor = 0x7B0, // f32: the share of the maximum health cannot go below
    kSpeed = 0x7C0,       // f32: the motion speed's factor
    kLevel = 0x7C8,       // u8
    kOnMap = 0x810,       // s16: shown on the map while above 0
    kHidden1 = 0x818,     // s16 countdowns
    kHidden2 = 0x81A,
    kStamina = 0x8A2,     // u16
    kRage = 0x8A6,        // s16 gauge
    kRageTime = 0x8A8,    // s16
    kRaging = 0x8AA,      // u8: 1 enraged
    kTranquilized = 0x93A, // u16
};

// PPC fctiwz: toward zero, saturating, NaN the most negative.
int32_t Fctiwz(double v) {
    if (std::isnan(v) || v <= double(INT32_MIN))
        return INT32_MIN;

    if (v >= double(INT32_MAX))
        return INT32_MAX;

    return int32_t(v);
}

} // namespace

// 0x8011E6BC (monster, s32 change, s32 max): health += change, within 0 and
// max (at least the health it had), and not below the floor (the maximum
// times +0x7B0). The codes (all US 0x8011E6xx, EU +0x64):
//   1 hit kills and Enemy has 1 HP (0x8011E670: the sum replaced by 0 and
//     1), always or switched on and off by their buttons (Skiller's Enable
//     1 hit kills, Enable 1 HP and Normal enemy HP patched the same sum; his
//     "(Alt)" codes emptied the health first, C2 at 0x8011E65C);
//   Tranq attaq (C2 at 0x8011E674): +0x93A (the tranquilizer gauge) 0x150.
extern "C" void MH3_MonsterHealthAdd(CpuContext* ctx) {
    const uint32_t monster = ctx->gpr[3];
    const int32_t change = int32_t(ctx->gpr[4]);
    int32_t max = int32_t(ctx->gpr[5]);
    const int32_t health = int32_t(Memory::Read32(monster + kHealth));

    if (max < health)
        max = health;

    int32_t total = health + change;

    if (Toggled(Id::OneHitKills))
        total = 0;

    if (Toggled(Id::EnemyOneHp))
        total = 1;

    Memory::Write32(monster + kHealth, uint32_t(total));

    if (On(Id::TranqAttaq))
        Memory::Write16(monster + kTranquilized, 0x150);

    if (total < 0)
        Memory::Write32(monster + kHealth, 0);
    else if (max < total)
        Memory::Write32(monster + kHealth, uint32_t(max));

    const float maxHealth = float(int32_t(Memory::Read32(monster + kMaxHealth)));
    const int32_t floor = Fctiwz(maxHealth * Memory::ReadFloat32(monster + kHealthFloor));

    if (int32_t(Memory::Read32(monster + kHealth)) < floor)
        Memory::Write32(monster + kHealth, uint32_t(floor));
}

REGISTER_NATIVE_FUNCTION_AS(0x8011E6BC, MH3_MonsterHealthAdd, "MonsterHealthAdd");

// 0x80132004 (monster, s32 health): the maximum health, health times the
// quest's factor (0x801260A8), and again for multiplayer (0x803B4F2C,
// 0x803B5094). Monster HP multipler (C2 at US 0x80131FEC, EU 0x80132050)
// multiplies it (here after the multiplayer factor).
extern "C" void func_80132004(CpuContext* ctx);

extern "C" void MH3_CheatMonsterMaxHealth(CpuContext* ctx) {
    const uint32_t monster = ctx->gpr[3];
    func_80132004(ctx);

    if (On(Id::MonsterHpMultiplier)) {
        const int32_t max = int32_t(Memory::Read32(monster + kMaxHealth));
        Memory::Write32(monster + kMaxHealth, uint32_t(max * Value(Id::MonsterHpMultiplier)));
    }
}

REGISTER_NATIVE_FUNCTION_AS(0x80132004, MH3_CheatMonsterMaxHealth, "CheatMonsterMaxHealth");

// 0x8012F4D8 (monster): the motion speed, ... * +0x7BC * +0x7C0. Monster
// speed modifier (C2 at US 0x8012F4D8, EU 0x8012F53C) sets +0x7C0 first.
extern "C" void func_8012F4D8(CpuContext* ctx);

extern "C" void MH3_CheatMonsterSpeed(CpuContext* ctx) {
    if (On(Id::MonsterSpeedModifier))
        Memory::WriteFloat32(ctx->gpr[3] + kSpeed, float(Value(Id::MonsterSpeedModifier)) / 100.0f);

    func_8012F4D8(ctx);
}

REGISTER_NATIVE_FUNCTION_AS(0x8012F4D8, MH3_CheatMonsterSpeed, "CheatMonsterSpeed");

// 0x801308BC (monster, s16 gain): the rage gauge. Its top is the monster
// data's table (0x80126098: +0xA0, then +0x28) entry 0, else 600; the gain is
// scaled by the entry of the health lost (tenths, 0-9). Reaching the top with
// no rage time left and neither 0x8012ECC4 nor 0x8012ECA0 and +0x89F not 1
// enrages (0x8012CEF0 (monster, 4)). One kind (25) does not rage on map kind
// 6 with +0x1E1 1. Instant rage mode (US 0x80130998, EU 0x801309FC: the
// clamp's branch dropped) fills the gauge on any gain.
extern "C" void MH3_RageGaugeAdd(CpuContext* ctx) {
    GuestFrame frame(ctx, 64);
    GuestCall game{ctx};
    const uint32_t monster = frame.Arg(3);
    int32_t gain = int16_t(frame.Arg(4));
    const uint32_t data = game.Call<0x80126098>({monster});

    if (Memory::Read8(monster + 3) == 25 && (game.Call<0x802B06CC>({Memory::Read8(monster + 0x1E0)}) & 0xFF) == 6 && Memory::Read8(monster + 0x1E1) == 1)
        return;

    const uint32_t table = Memory::Read32(Memory::Read32(data + 0xA0) + 0x28);
    int32_t top = 600;

    if (table != 0) {
        top = int16_t(Memory::Read16(table));
        const float health = float(int32_t(Memory::Read32(monster + kHealth)));
        const float max = float(int32_t(Memory::Read32(monster + kMaxHealth)));
        uint32_t lost = uint32_t(Fctiwz(10.0f * (1.0f - health / max))) & 0xFF;

        if (lost > 9)
            lost = 9;

        gain = Fctiwz(float(gain) * Memory::ReadFloat32(table + 16 + 4 * lost));
    }

    const int32_t gauge = int16_t(Memory::Read16(monster + kRage) + gain);
    Memory::Write16(monster + kRage, uint16_t(gauge));

    if (gauge < 0)
        Memory::Write16(monster + kRage, 0);
    else if (gauge > top || On(Id::InstantRage))
        Memory::Write16(monster + kRage, uint16_t(top));

    if (int16_t(Memory::Read16(monster + kRage)) < top)
        return;

    const bool calm = int16_t(Memory::Read16(monster + kRageTime)) <= 0;

    if (calm && game.Call<0x8012ECC4>({monster}) == 0 && game.Call<0x8012ECA0>({monster}) == 0 && Memory::Read8(monster + 0x89F) != 1)
        game.Call<0x8012CEF0>({monster, 4});

    Memory::Write16(monster + kRage, uint16_t(top));
}

REGISTER_NATIVE_FUNCTION_AS(0x801308BC, MH3_RageGaugeAdd, "RageGaugeAdd");

// 0x80130B24 (monster): while enraged, the rage time counts down; at 0 the
// rage ends (0x8012D204, then 0x801308A8). Monsters have regen during rage
// (C2 at US 0x80130AE4, EU 0x80130B48) first adds a third of the level to
// the health every 64 counts while it is below the maximum; Infinite rage
// mode (US 0x80130AF4, EU 0x80130B58) never ends it.
extern "C" void func_80130B24(CpuContext* ctx);

extern "C" void MH3_CheatRageTime(CpuContext* ctx) {
    const uint32_t monster = ctx->gpr[3];

    if (Memory::Read8(monster + kRaging) != 1) {
        func_80130B24(ctx);
        return;
    }

    const int16_t time = int16_t(Memory::Read16(monster + kRageTime));

    if (On(Id::RageRegen) && (time & 0x3F) == 0) {
        const int32_t health = int32_t(Memory::Read32(monster + kHealth));

        if (health < int32_t(Memory::Read32(monster + kMaxHealth)))
            Memory::Write32(monster + kHealth, uint32_t(health + Memory::Read8(monster + kLevel) / 3));
    }

    if (On(Id::InfiniteRage)) {
        Memory::Write16(monster + kRageTime, uint16_t(time - 1));
        return;
    }

    func_80130B24(ctx);
}

REGISTER_NATIVE_FUNCTION_AS(0x80130B24, MH3_CheatRageTime, "CheatRageTime");

// 0x80130BD0 (monster, s16 change, max): the stamina. Infinite monster
// stamina (US 0x80130C64, EU 0x80130CC8: the sum's store dropped) keeps it
// from going down (here none of the drain applies).
extern "C" void func_80130BD0(CpuContext* ctx);

extern "C" void MH3_CheatMonsterStamina(CpuContext* ctx) {
    if (On(Id::InfiniteMonsterStamina) && int16_t(ctx->gpr[4]) < 0)
        return;

    func_80130BD0(ctx);
}

REGISTER_NATIVE_FUNCTION_AS(0x80130BD0, MH3_CheatMonsterStamina, "CheatMonsterStamina");

// 0x80131E24 and 0x80131EF0 (monster): two countdowns (+0x818, +0x81A),
// paused while +0x1E8 or +0x1E9 is 1. Ghost world (US 0x80131DEA/0x80131EB6,
// EU 0x80131E4E/0x80131F1A: the -1 made 0) stops both.
namespace {
void CountDown(uint32_t monster, uint32_t at) {
    if (Memory::Read8(monster + 0x1E8) == 1 || Memory::Read8(monster + 0x1E9) == 1)
        return;

    const int16_t count = int16_t(Memory::Read16(monster + at));

    if (count < 1 || On(Id::GhostWorld))
        return;

    Memory::Write16(monster + at, uint16_t(count - 1));
}
} // namespace

extern "C" void MH3_MonsterCountdown818(CpuContext* ctx) {
    CountDown(ctx->gpr[3], kHidden1);
}

REGISTER_NATIVE_FUNCTION_AS(0x80131E24, MH3_MonsterCountdown818, "MonsterCountdown818");

extern "C" void MH3_MonsterCountdown81A(CpuContext* ctx) {
    CountDown(ctx->gpr[3], kHidden2);
}

REGISTER_NATIVE_FUNCTION_AS(0x80131EF0, MH3_MonsterCountdown81A, "MonsterCountdown81A");

// 0x80137434 (monster): sets the model's material colours, scaled by +0x1D4.
// Ghost world (C2 at US 0x80137478, EU 0x801374DC) makes it 0.125.
extern "C" void func_80137434(CpuContext* ctx);

extern "C" void MH3_CheatMonsterColour(CpuContext* ctx) {
    if (On(Id::GhostWorld))
        Memory::WriteFloat32(ctx->gpr[3] + kAlpha, 0.125f);

    func_80137434(ctx);
}

REGISTER_NATIVE_FUNCTION_AS(0x80137434, MH3_CheatMonsterColour, "CheatMonsterColour");

// 0x8013369C (monster): 1 while it shows on the map (+0x810 above 0). Show
// all monsters on the map (US 0x80133638, EU 0x8013369C) reads 2.
extern "C" void MH3_MonsterOnMap(CpuContext* ctx) {
    const int16_t shown = On(Id::AllMonstersOnMap) ? 2 : int16_t(Memory::Read16(ctx->gpr[3] + kOnMap));
    ctx->gpr[3] = shown > 0;
}

REGISTER_NATIVE_FUNCTION_AS(0x8013369C, MH3_MonsterOnMap, "MonsterOnMap");

// 0x80137980 (monster): a monster's step; while running, after its senses
// (0x801382A0) the spawn state +0x17 picks what follows, 0 (in play) marking
// it active (+0x1DE). Freeze/unfreeze monsters (US 0x801379D8, EU
// 0x80137A3C: the state compared with 1), switched by its buttons, leaves
// monsters in play inactive: their state reads 0xFF meanwhile. The monster
// control's `freeze` holds one monster the same way (monster_control.cpp).
extern "C" void func_80137980(CpuContext* ctx);

extern "C" void MH3_CheatMonsterStep(CpuContext* ctx) {
    const uint32_t monster = ctx->gpr[3];
    const bool freeze = (Toggled(Id::FreezeUnfreezeMonsters) || MonsterControl::Frozen(monster)) && Memory::Read8(monster + kInit) == 1 && Memory::Read8(monster + kSpawn) == 0;

    if (freeze)
        Memory::Write8(monster + kSpawn, 0xFF);

    func_80137980(ctx);

    if (freeze && Memory::Read8(monster + kSpawn) == 0xFF)
        Memory::Write8(monster + kSpawn, 0);
}

REGISTER_NATIVE_FUNCTION_AS(0x80137980, MH3_CheatMonsterStep, "CheatMonsterStep");

// 0x803C4878: 1 while an event scene (state 4) plays. 0x801382A0 skips the
// monster's sight, awareness and target updates then (its call returns to
// 0x80138720); Monster can't see you (US 0x801386BC, EU 0x80138720: the
// branch taken) always skips them.
extern "C" void func_803C4878(CpuContext* ctx);

extern "C" void MH3_CheatEventDemoState4(CpuContext* ctx) {
    if (On(Id::MonsterCantSeeYou) && ctx->lr == 0x80138720) {
        ctx->gpr[3] = 1;
        return;
    }

    func_803C4878(ctx);
}

REGISTER_NATIVE_FUNCTION_AS(0x803C4878, MH3_CheatEventDemoState4, "CheatEventDemoState4");

// 0x803AEBE0 (to, from): copies a quest's large monster entry (8 bytes: u8
// kind, ..., +6 size, +7 level). Boss/s mod, Set boss/s size to and Set
// boss/s level to (US 0x803AEB7C/BAC/BB4, EU 0x803AEBE0/C10/C18: the loads
// replaced by the value) set them.
extern "C" void func_803AEBE0(CpuContext* ctx);

extern "C" void MH3_CheatBossEntry(CpuContext* ctx) {
    const uint32_t to = ctx->gpr[3];
    func_803AEBE0(ctx);

    if (On(Id::BossMod))
        Memory::Write8(to, uint8_t(Value(Id::BossMod)));

    if (On(Id::BossSize))
        Memory::Write8(to + 6, uint8_t(Value(Id::BossSize)));

    if (On(Id::BossLevel))
        Memory::Write8(to + 7, uint8_t(Value(Id::BossLevel)));
}

REGISTER_NATIVE_FUNCTION_AS(0x803AEBE0, MH3_CheatBossEntry, "CheatBossEntry");
