// Cheats that write the game's data, RMHP08: the Gecko codes made of RAM
// writes and conditions (types 00-08, 28-3A, 42/4A), as C, once per logic
// step (the code handler ran them once per frame). Addresses are EU ones:
// heap data (the player and monster work, the save data) sits where the US
// disc has it; static data moved with the binary (system_w and the Psw by
// 0x2480, .sdata by 0x24C0).
//
// The codes read the buttons in three places: the pad work (Psw, here the
// game's Classic bits, Held/Pressed), and the player work's copies: +0xB8,
// +0xC0 held, +0xC4 pressed and +0xC8 the left stick's directions, all with
// the Classic bits.
#include "cheats.h"

#include <bit>

namespace Cheats {

namespace {

// Save data (userdata).
enum : uint32_t {
    kSex = 0x0,           // u8
    kFace = 0x1,          // u8
    kHair = 0x2,          // u8
    kHairColor = 0x14,    // u8 red, green, blue
    kMoney = 0x18,        // u32
    kClothing = 0x9E,     // u8
    kVoice = 0x9F,        // u8
    kClothingColor = 0x39A0, // u8 red, green, blue
    kCombos = 0x3960,     // u32[4] known combinations, then u8 at +0x10
    kEyeColor = 0x3DE6,   // u8
    kFeatures = 0x3DE7,   // u8
    kTimeOfDay = 0x3E00,  // u8: 0 morning, 1 evening (read when an area loads)
    kFleets = 0x3E02,     // u8: hunting fleet ships available
    kResources = 0x3F04,  // u32 Moga Woods resources
    kFleet = 0x3F38,      // three ships, 0xC each: u8 catch, +2 u8 stars, +4 u8 voyage length
    kFarm = 0x4654,       // u8[4] field levels
    kFarmWork = 0x465C,   // three helpers, 8 each: u8 work left
    kFelyneHelpers = 0x4670, // u8
    kForecast = 0x4850,   // u8 tomorrow's two large monsters, the day after's at +8
    kHuntKills = 0x48DA,  // u16[36]
    kArenaChallenges = 0x52E2, // u16 bits
    kChamberlyne = 0x53A4, // u32 prize money
};

// Player work (PLW).
enum : uint32_t {
    kState = 0x5,          // u8: the action's state
    kAction8 = 0x8,        // u8 (u32 at +8: 0 when idle)
    kAction9 = 0x9,        // u8
    kActionGroup = 0xA,    // u8: 0 field, 2 air, 4 weapon, 8 down
    kActionB = 0xB,        // u8
    kActionId = 0xD,       // u8
    kActionF = 0xF,        // u8
    kDrawn = 0x18,         // u8: 1 weapon drawn
    kLift = 0x88,          // f32 vertical speed
    kFacing = 0xAA,        // u16 angle
    kPitch = 0xAE,         // u16 angle
    kButtons = 0xB8,       // u16 held, Classic bits
    kHeld = 0xC0,          // u16
    kPressed = 0xC4,       // u16
    kStick = 0xC8,         // u16 left stick directions
    kCameraYaw = 0xD8,     // u16 angle
    kVestLevel = 0x141,    // u8 per armor piece, 0xC apart: vest, gloves, belt, pants, helm
    kWeapon = 0x1D0,       // u8 weapon type: 7 great sword, 8 sword and shield, 9 hammer,
                           //   0xA lance, 0xB bowgun, 0xE long sword, 0xF switch axe
    kClip = 0x269,         // u8 shots loaded, u8 at +1
    kTrapsOut = 0x30C,     // u8
    kBarrelsOut = 0x30E,   // u8
    kMotionSpeed = 0x354,  // f32
    kGauge = 0x36C,        // u16 switch axe phial gauge
    kHealth = 0x370,       // s16, maximum at +2, recoverable at +6
    kStamina = 0x378,      // s16, maximum at +2
    kAir = 0x37F,          // u8, maximum at +2 (u16 0x37E, 0x380)
    kOnline = 0x340,       // u16 0x0101 in the online player work layout
    kLifeDrain = 0x3AC,    // u16
    kDefense = 0x3C0,      // f32
    kElementStatus = 0x3DE, // u16 timers from here on
    kPoison = 0x3EA,
    kStun = 0x3FC,
    kPurple = 0x404,
    kFelvine = 0x410,
    kSpeedBurst = 0x412,
    kSnowman = 0x416,
    kMuddy = 0x41C,
    kDefenseDown = 0x422,
    kFireDown = 0x42E,     // u16 each: fire, water, thunder, ice, dragon
    kFireUp = 0x438,       // u16 each: fire, water, thunder, ice; dragon at 0x442
    kDragonUp = 0x442,
    kAttackUp = 0x448,     // u8 levels, u16 time at +2
    kDefenseUp = 0x44C,    // u8 levels, u16 time at +2
    kWindproof = 0x454,
    kNaturalRecovery = 0x45E,
    kColdDrink = 0x464,    // u16 timers
    kHotDrink = 0x466,
    kSharpness = 0x568,    // u8 level, u8, u16
    kGesture = 0x5C8,      // u8
    kFelyneSkills = 0x612, // u16[4]
    kSkinTone = 0xB1C,     // u16
};

constexpr uint32_t kArmorStride = 0xC;
enum : uint32_t {
    kVest,
    kGloves,
    kBelt,
    kPants,
    kHelm
};

// Monster work.
enum : uint32_t {
    kMonsterState = 0x5,   // u8: the action's state
    kMonsterHealth = 0x7A0, // s32, maximum at +4
    kMonsterMap = 0x811,   // u8: 2 shown on the map
    kMonsterX = 0x28,      // f32 position, z at +8
};

constexpr uint32_t kPlayerX = 0x3C; // PLW f32 position, z at +8

// Data outside the work areas.
enum : uint32_t {
    kGallery = kSystem + 0x888,  // u8[8] unlocked movies
    kPlaySeconds = kSystem + 0x890, // u32 (PlayTimeTick 0x800D2F80)
    kMapOption = 0x8065B51E,     // u8 game option 14 (options at 0x8065B510)
    kHudState = 0x806C0281,      // u16 (unaligned), menu state
    kMusicVolume = 0x80796E80,   // f32 (sound code at 0x800E4800)
    kEnemySpeed = 0x80799110,    // f32 1.0: enemy motion rate (0x80125xxx)
    kLanceChargeRate = 0x8079D874, // f32 1.2
    kRightStickDirs = 0x8065B7D0 + 0xEC, // Psw channel 0
    kVillagePlayer = 0x901B1F80, // the village's PLW
    kVillageSpeed = 0x901B2F14,  // f32
    kClassicDirs = 0x8065B7D0 + 0xDE,
};

void WriteF32(uint32_t address, float value) {
    Memory::Write32(address, std::bit_cast<uint32_t>(value));
}

uint16_t ReadUnaligned16(uint32_t address) {
    return uint16_t(Memory::Read8(address) << 8 | Memory::Read8(address + 1));
}

// The Gecko codes' "16-bit if" on a PLW halfword: (value & ~mask) == compare.
bool Is(uint32_t plw, uint32_t offset, uint16_t compare, uint16_t mask = 0) {
    return (Memory::Read16(plw + offset) & ~mask) == compare;
}

bool ButtonsIn(uint32_t plw, uint32_t offset, uint16_t buttons) {
    return (Memory::Read16(plw + offset) & buttons) == buttons;
}

uint8_t Weapon(uint32_t plw) {
    return Memory::Read8(plw + kWeapon);
}

// An activator with "Always" (0) among its choices.
bool Activated(Id id) {
    const auto buttons = uint16_t(Value(id));
    return buttons == 0 || HeldAll(buttons);
}

void TurnWithCamera(uint32_t plw) {
    Memory::Write16(plw + kFacing, Memory::Read16(plw + kCameraYaw));
}

// Doudley's attack injections: from an action that allows it, restart the
// action state machine in the weapon group (4) on `id`, or on `alternative`
// when the player work's +9 is 3.
void Inject(uint32_t plw, uint8_t id, uint8_t alternative) {
    Memory::Write8(plw + kState, 0);
    Memory::Write8(plw + kActionGroup, 4);
    Memory::Write8(plw + kActionId, id);

    if (Memory::Read8(plw + kAction9) == 3)
        Memory::Write8(plw + kActionId, alternative);
}

// Doudley's air attacks: in the air (group 2) with X held, the weapon comes
// out; A starts `attack` (with R, `alternative`).
void AirAttack(uint32_t plw, uint8_t weapon, uint8_t attack, uint8_t alternative) {
    if (!Is(plw, kActionGroup, 0x0200) || Weapon(plw) != weapon || !ButtonsIn(plw, kButtons, kX))
        return;

    Memory::Write8(plw + kState, 1);
    Memory::Write8(plw + kDrawn, 1);

    if (!ButtonsIn(plw, kButtons, kA))
        return;

    Memory::Write8(plw + kAction9, 0);
    Memory::Write8(plw + kActionGroup, 4);
    Memory::Write8(plw + kActionId, attack);

    if (ButtonsIn(plw, kButtons, kR))
        Memory::Write8(plw + kActionId, alternative);
}

void StepAirAttacks(uint32_t plw) {
    if (On(Id::AirAttackGreatSword))
        AirAttack(plw, 0x7, 0x28, 0x07);

    if (On(Id::AirAttackSwordAndShield) && Is(plw, kActionGroup, 0x0200) && Weapon(plw) == 0x8 && ButtonsIn(plw, kButtons, kX)) {
        Memory::Write16(plw + kPitch, 0);
        AirAttack(plw, 0x8, 0x15, 0x15);

        if (ButtonsIn(plw, kButtons, kA) && ButtonsIn(plw, kButtons, kR))
            Memory::Write16(plw + kPitch, 0x1000);
    }

    if (On(Id::AirAttackHammer))
        AirAttack(plw, 0x9, 0x1C, 0x1A);

    if (On(Id::AirAttackLance) && Is(plw, kActionGroup, 0x0200) && Weapon(plw) == 0xA && ButtonsIn(plw, kButtons, kX)) {
        Memory::Write8(plw + kState, 1);
        Memory::Write8(plw + kAction9, 0);
        Memory::Write8(plw + kActionGroup, 4);
        Memory::Write8(plw + kActionId, 0x1B);
        Memory::Write8(plw + kDrawn, 1);
        Memory::Write16(plw + kPitch, ButtonsIn(plw, kButtons, kR) ? 0xC000 : 0x4000);
    }

    if (On(Id::AirAttackLongSword))
        AirAttack(plw, 0xE, 0x31, 0x2B);

    if (On(Id::AirAttackSwitchAxe))
        AirAttack(plw, 0xF, 0x3A, 0x26);

    if (On(Id::AirAttackBowgun) && Is(plw, kActionGroup, 0x0200) && Weapon(plw) == 0xB && ButtonsIn(plw, kButtons, kX)) {
        Memory::Write8(plw + kState, 1);
        Memory::Write8(plw + kDrawn, 1);
        Memory::Write16(plw + kPitch, 0x3000);

        if (ButtonsIn(plw, kButtons, kA)) {
            Memory::Write8(plw + kAction9, 0);
            Memory::Write8(plw + kActionGroup, 4);
            Memory::Write8(plw + kActionId, 0x05);
            Memory::Write8(plw + kState, 2);
        }
    }
}

// Doudley's movement codes.
void StepMovement(uint32_t plw) {
    if (On(Id::FaceForwardStick) && Memory::Read16(kRightStickDirs) != 0)
        TurnWithCamera(plw);

    if (On(Id::FaceForwardDpad) && (ButtonsIn(plw, kHeld, kLeft) || ButtonsIn(plw, kHeld, kRight)))
        TurnWithCamera(plw);

    // No button: hover; ZL: rise; ZR: sink (the code read the raw Classic
    // report, these are the same buttons).
    if (On(Id::AntiGravity)) {
        if (Held() == 0)
            Memory::Write32(plw + kLift, 0xB0000000);

        if (HeldAll(kZL))
            WriteF32(plw + kLift, 1.25f);

        if (HeldAll(kZR))
            WriteF32(plw + kLift, -4.0f);
    }

    // In the air (+8 u16 2), L turns the fall into the run (u32 +8 0x101),
    // R into the dash (+0xD 2).
    if (On(Id::RunInMidAir)) {
        if (!Is(plw, kAction8, 0x0002))
            Memory::Write8(plw + kActionB, 0);

        if (ButtonsIn(plw, kHeld, kL) && Is(plw, kAction8, 0x0002)) {
            Memory::Write32(plw + kAction8, 0x00000101);

            if (ButtonsIn(plw, kHeld, kR))
                Memory::Write8(plw + kActionId, 2);
        }

        if (Is(plw, kActionGroup, 0x0101))
            TurnWithCamera(plw);
    }

    if (On(Id::MidAirDpad) && Is(plw, kActionGroup, 0x0101)) {
        if ((Memory::Read16(plw + kHeld) & (kUp | kDown)) == kUp)
            Memory::Write16(plw + kPitch, 0xE000);

        if (ButtonsIn(plw, kHeld, kDown))
            Memory::Write16(plw + kPitch, 0x1000);
    }

    if (On(Id::MidAirStick) && Is(plw, kActionGroup, 0x0101)) {
        if (Memory::Read16(kClassicDirs) & 0x20)
            Memory::Write16(plw + kPitch, 0xE000);

        if (Memory::Read16(kClassicDirs) & 0x10)
            Memory::Write16(plw + kPitch, 0x1000);
    }
}

// Doudley's weapon codes.
void StepWeapons(uint32_t plw) {
    if (On(Id::FasterWeaponUsage) && Memory::Read8(plw + kActionGroup) == 4)
        WriteF32(plw + kMotionSpeed, 3.0f);

    if (On(Id::SnsInfiniteSlashes) && Activated(Id::SnsInfiniteSlashes) && Weapon(plw) == 0x8 && Memory::Read8(plw + kDrawn) == 1) {
        const uint8_t id = Memory::Read8(plw + kActionId);

        if (id != 0x03 && id != 0x11 && ButtonsIn(plw, kPressed, kX))
            Inject(plw, 0x03, 0x11);
    }

    // Held up while charging with a lance: the charge's motion rate (1.2)
    // becomes 4.0, and stays.
    if (On(Id::LanceChargeSpeed) && !Is(plw, kOnline, 0x0101) && Weapon(plw) == 0xA && (Memory::Read16(0x8065B7D0 + 0x2C8) & kUp))
        WriteF32(kLanceChargeRate, 4.0f);

    if (On(Id::LanceChargeController) && Weapon(plw) == 0xA) {
        const uint8_t id = Memory::Read8(plw + kActionId);

        if (id == 0x03) {
            WriteF32(plw + kMotionSpeed, 1.2f);
            Memory::Write16(plw + kPitch, 0);

            if (ButtonsIn(plw, kStick, kDown))
                Memory::Write16(plw + kPitch, 0xE000);

            if (ButtonsIn(plw, kStick, kUp)) {
                WriteF32(plw + kMotionSpeed, 3.0f);

                if (ButtonsIn(plw, kHeld, kR))
                    WriteF32(plw + kMotionSpeed, 8.0f);
            }
        }

        if (id == 0x1B) {
            WriteF32(plw + kMotionSpeed, 1.2f);

            if (ButtonsIn(plw, kStick, kUp))
                WriteF32(plw + kMotionSpeed, 4.0f);
        }
    }

    if (On(Id::Battojutsu) && Weapon(plw) == 0xE) {
        const uint8_t id = Memory::Read8(plw + kActionId);

        if (Activated(Id::Battojutsu) && Memory::Read8(plw + kDrawn) == 0 && id != 0x2F && id != 0x3A && ButtonsIn(plw, kPressed, kA))
            Inject(plw, 0x2F, 0x3A);

        if (Memory::Read8(plw + kAction9) == 2 && Memory::Read8(plw + kActionF) == 4)
            Memory::Write8(plw + kDrawn, 0);
    }

    if (On(Id::SwitchAxeSwordMode) && Activated(Id::SwitchAxeSwordMode) && Weapon(plw) == 0xF && Memory::Read8(plw + kDrawn) == 1) {
        const uint8_t id = Memory::Read8(plw + kActionId);

        if (id != 0x01 && id != 0x2D && ButtonsIn(plw, kPressed, kR))
            Inject(plw, 0x01, 0x2D);
    }

    StepAirAttacks(plw);

    if (On(Id::SwitchAxeGaugeFull))
        Memory::Write16(plw + kGauge, 100);
}

// Doudley's action codes.
void StepActions(uint32_t plw) {
    const bool idle = Memory::Read32(plw + kAction8) == 0;

    // With the quest menu open: the player work then sees no buttons.
    if (On(Id::ForceGrill) && Combo(uint16_t(Value(Id::ForceGrill))) && idle && Memory::Read16(plw + kHeld) == 0)
        Memory::Write8(plw + kActionId, 0x22);

    if (On(Id::QuickGestures) && Combo(kL | kR | kUp))
        Memory::Write8(plw + kGesture, uint8_t(Value(Id::QuickGestures)));

    if (On(Id::ForceSheathe) && Combo(uint16_t(Value(Id::ForceSheathe))))
        Memory::Write8(plw + kDrawn, 0);

    if (On(Id::ForceUnsheathe) && Combo(uint16_t(Value(Id::ForceUnsheathe))))
        Memory::Write8(plw + kDrawn, 1);

    if (On(Id::ActionRepeater) && (Pressed() & kPlus))
        Memory::Write8(plw + kState, 0);

    if (On(Id::ForceEvade) && Activated(Id::ForceEvade) && Memory::Read8(plw + kAction9) != 2 && Memory::Read8(plw + kActionGroup) != 2 &&
        ButtonsIn(plw, kPressed, kB)) {
        Memory::Write8(plw + kState, 0);
        Memory::Write8(plw + kActionGroup, 0);
        Memory::Write8(plw + kActionId, 0x08);

        if (Memory::Read8(plw + kAction9) == 3)
            Memory::Write8(plw + kActionId, 0x1B);
    }

    // The code wrote 0x46, then 0x54.
    if (On(Id::SleepAnywhere) && Pressed() == (kMinus | kB) && idle)
        Memory::Write8(plw + kActionId, 0x54);

    // Group 8 (down): fainted, back to camp.
    if (On(Id::BackToCamp) && Pressed() == (kMinus | kR) && idle) {
        Memory::Write8(plw + kAction9, 0);
        Memory::Write8(plw + kActionGroup, 8);
    }
}

void StepStatus(uint32_t plw) {
    struct Write {
        Id id;
        uint32_t offset;
        uint16_t value;
    };

    // Skiller's and Doudley's status codes: u16 timers and bonuses held at
    // a value.
    constexpr Write kWrites[]{
        {Id::NeverLifeDrain, kLifeDrain, 0},
        {Id::AddDragon, kDragonUp, 0x3000},
        {Id::AddThunder, kFireUp + 4, 0x300},
        {Id::AddIce, kFireUp + 6, 0x300},
        {Id::AddFire, kFireUp, 0x300},
        {Id::AddWater, kFireUp + 2, 0x300},
        {Id::NoIceWeakness, kFireDown + 6, 0},
        {Id::NoDragonWeakness, kFireDown + 8, 0},
        {Id::NoWaterWeakness, kFireDown + 2, 0},
        {Id::NoThunderWeakness, kFireDown + 4, 0},
        {Id::NoFireWeakness, kFireDown, 0},
        {Id::NeverDefenseDown, kDefenseDown, 0},
        {Id::NeverFelvine, kFelvine, 0},
        {Id::NeverSnowman, kSnowman, 0},
        {Id::NeverMuddy, kMuddy, 0},
        {Id::AlwaysWindproof, kWindproof, 0x300},
        {Id::NeverStun, kStun, 0},
        {Id::NeverPoison, kPoison, 0},
        {Id::NaturalRecoveryUp, kNaturalRecovery, 0x300},
        {Id::PurpleHealthBar, kPurple, 0x3000},
        {Id::NoElementStatus, kElementStatus, 0},
        {Id::UnlimitedColdDrink, kColdDrink, 0x464A},
        {Id::UnlimitedHotDrink, kHotDrink, 0x464A},
        {Id::AttackUpForever, kAttackUp + 2, 0xFFFF},
        {Id::DefenseUpForever, kDefenseUp + 2, 0xFFFF},
        {Id::SpeedBurstAlways, kSpeedBurst, 0x7FFF},
    };

    for (const Write& write : kWrites)
        if (On(write.id))
            Memory::Write16(plw + write.offset, write.value);

    if (On(Id::InfiniteHp)) {
        const uint16_t maximum = Memory::Read16(plw + kHealth + 2);
        Memory::Write16(plw + kHealth, maximum);
        Memory::Write16(plw + kHealth + 6, maximum);
    }

    if (On(Id::InfiniteStamina))
        Memory::Write16(plw + kStamina, Memory::Read16(plw + kStamina + 2));

    if (On(Id::InfiniteAir)) {
        Memory::Write8(plw + kAir, 100);
        Memory::Write8(plw + kAir + 2, 100);
    }

    if (On(Id::DefenseUpgrade)) {
        Memory::Write8(plw + kDefenseUp, 0x7F);
        Memory::Write8(plw + kDefenseUp + 1, 0x7F);
    }

    if (On(Id::AttackUpgrade)) {
        Memory::Write8(plw + kAttackUp, 0x7F);
        Memory::Write8(plw + kAttackUp + 1, 0x7F);
    }

    if (On(Id::DefenseMax))
        WriteF32(plw + kDefense, 255.0f);

    // Seth's online codes wrote the online layout's player work
    // (0x9014AE80); here the local player's, wherever it is.
    if (On(Id::SethInfiniteHealth))
        Memory::Write32(plw + kHealth, 0x00640064);

    if (On(Id::SethUltraSharp))
        Memory::Write32(plw + kSharpness, 0x06400404);

    if (On(Id::SethSpeedGlide) && HeldAll(kZL))
        WriteF32(plw + kMotionSpeed, 32.0f);

    // u16 maximum oxygen 255, then u16 0.
    if (On(Id::SethOxygen))
        Memory::Write32(plw + kAir + 1, 0x00FF0000);
}

void StepItems(uint32_t plw) {
    if (On(Id::UnlimitedBarrelBombs))
        Memory::Write8(plw + kBarrelsOut, 0);

    if (On(Id::UnlimitedTraps))
        Memory::Write8(plw + kTrapsOut, 0);

    if (On(Id::LoadShots)) {
        Memory::Write8(plw + kClip, uint8_t(Value(Id::LoadShots)));
        Memory::Write8(plw + kClip + 1, uint8_t(Value(Id::LoadShots)));
    }

    constexpr struct {
        Id id;
        uint16_t buttons;
    } kFelyneSkillButtons[]{
        {Id::FelyneSkill1, kL | kR | kUp},
        {Id::FelyneSkill2, kL | kR | kDown},
        {Id::FelyneSkill3, kL | kR | kLeft},
        {Id::FelyneSkill4, kL | kR | kRight},
    };

    for (uint32_t i = 0; i < 4; ++i)
        if (On(kFelyneSkillButtons[i].id) && Combo(kFelyneSkillButtons[i].buttons))
            Memory::Write16(plw + kFelyneSkills + 2 * i, uint16_t(Value(kFelyneSkillButtons[i].id)));

    constexpr struct {
        Id id;
        uint32_t piece;
    } kArmorLevels[]{
        {Id::HelmLevel255, kHelm},
        {Id::VestLevel255, kVest},
        {Id::GlovesLevel255, kGloves},
        {Id::BeltLevel255, kBelt},
        {Id::PantsLevel255, kPants},
    };

    for (const auto& armor : kArmorLevels)
        if (On(armor.id))
            Memory::Write8(plw + kVestLevel + armor.piece * kArmorStride, 0xFE);

    if (On(Id::SkinTone))
        Memory::Write16(plw + kSkinTone, uint16_t(Value(Id::SkinTone)));
}

} // namespace

void StepSaveData() {
    if (On(Id::GalleryClips)) {
        constexpr uint8_t kAll[8]{0xDF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x7F};

        for (uint32_t i = 0; i < 8; ++i)
            Memory::Write8(kGallery + i, Memory::Read8(kGallery + i) | kAll[i]);
    }

    if (On(Id::TurnItUpTo11))
        Memory::Write32(kMusicVolume, 0x40112244);

    if (On(Id::MapOnHud))
        Memory::Write8(kMapOption, ReadUnaligned16(kHudState) == 0x3C04 ? 1 : 0);

    if (On(Id::SetPlayTime))
        Memory::Write32(kPlaySeconds, uint32_t(Value(Id::SetPlayTime)));

    const uint32_t user = UserData();

    if (user == 0)
        return;

    struct Write {
        Id id;
        uint32_t offset;
        uint8_t value;
    };

    constexpr Write kBytes[]{
        {Id::MorningOnLoad, kTimeOfDay, 0},
        {Id::EveningOnLoad, kTimeOfDay, 1},
        {Id::AllHuntingFleets, kFleets, 3},
        {Id::CaptainCatch, kFleet, 1},
        {Id::CaptainFourStars, kFleet + 2, 3},
        {Id::RedspearCatch, kFleet + 0xC, 1},
        {Id::RedspearFourStars, kFleet + 0xE, 3},
        {Id::BlackspearCatch, kFleet + 0x18, 1},
        {Id::BlackspearFourStars, kFleet + 0x1A, 3},
        {Id::HalfDayVoyages, kFleet + 4, 1},
        {Id::HalfDayVoyages, kFleet + 0x10, 1},
        {Id::HalfDayVoyages, kFleet + 0x1C, 1},
        {Id::AllFelyneHelpers, kFelyneHelpers, 3},
        {Id::InstantFarming, kFarmWork, 0},
        {Id::InstantFarming, kFarmWork + 8, 0},
        {Id::InstantFarming, kFarmWork + 0x10, 0},
    };

    for (const Write& write : kBytes)
        if (On(write.id))
            Memory::Write8(user + write.offset, write.value);

    // Picked values: the character's looks and the forecast.
    constexpr struct {
        Id id;
        uint32_t offset;
    } kPicked[]{
        {Id::SexChange, kSex},
        {Id::PuddyFace, kFace},
        {Id::HairStyle, kHair},
        {Id::HairRed, kHairColor},
        {Id::HairGreen, kHairColor + 1},
        {Id::HairBlue, kHairColor + 2},
        {Id::Clothing, kClothing},
        {Id::Voice, kVoice},
        {Id::EyeColor, kEyeColor},
        {Id::Features, kFeatures},
        {Id::ClothingRed, kClothingColor},
        {Id::ClothingGreen, kClothingColor + 1},
        {Id::ClothingBlue, kClothingColor + 2},
        {Id::ForecastTomorrow1, kForecast},
        {Id::ForecastTomorrow2, kForecast + 1},
        {Id::ForecastDayAfter1, kForecast + 8},
        {Id::ForecastDayAfter2, kForecast + 9},
    };

    for (const auto& picked : kPicked)
        if (On(picked.id))
            Memory::Write8(user + picked.offset, uint8_t(Value(picked.id)));

    if (On(Id::ResourcesZero))
        Memory::Write32(user + kResources, 0);

    if (On(Id::MaxMoney))
        Memory::Write32(user + kMoney, 9999999);

    if (On(Id::ChamberlynePrize))
        Memory::Write32(user + kChamberlyne, 1000000);

    if (On(Id::AllArenaChallenges))
        Memory::Write16(user + kArenaChallenges, 0x3FF);

    if (On(Id::MaxFarmStats))
        Memory::Write32(user + kFarm, 0x03030303);

    // The code set every count to 1; here at least 1.
    if (On(Id::HuntReportKills))
        for (uint32_t i = 0; i < 36; ++i)
            if (Memory::Read16(user + kHuntKills + 2 * i) == 0)
                Memory::Write16(user + kHuntKills + 2 * i, 1);

    if (On(Id::ComboListComplete)) {
        for (uint32_t i = 0; i < 4; ++i)
            Memory::Write32(user + kCombos + 4 * i, 0xFFFFFFFF);

        Memory::Write8(user + kCombos + 0x10, 0x0F);
    }
}

void StepPlayer() {
    const uint32_t plw = LocalPlayer();

    if (plw == 0)
        return;

    StepStatus(plw);
    StepItems(plw);
    StepMovement(plw);
    StepWeapons(plw);
    StepActions(plw);

    // Seth's village codes: with Z held (here ZL), a float of the village's
    // move work (0x901B2F14, the village heap as the US disc has it) huge or
    // nearly 0. Only while the local player is the village's.
    if (plw == kVillagePlayer && HeldAll(kZL)) {
        if (On(Id::VillageSpeedHack))
            Memory::Write32(kVillageSpeed, 0x60000000);

        if (On(Id::VillageSnailSpeed))
            Memory::Write32(kVillageSpeed, 0x00070000);
    }
}

void StepMonsters() {
    // Doudley's freeze: the enemy motion rate (1.0) nearly 0 while the
    // buttons are held, with a hunter on the field.
    if (On(Id::FreezeMonsters))
        WriteF32(kEnemySpeed, LocalPlayer() && HeldAll(uint16_t(Value(Id::FreezeMonsters))) ? std::bit_cast<float>(0x30000000u) : 1.0f);

    const uint32_t count = MoveWorkCount(Kind::Monster);

    if (On(Id::MaxHealMonsters) && Combo(uint16_t(Value(Id::MaxHealMonsters))))
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t monster = MoveWork(Kind::Monster, i);

            if (monster && Memory::Read16(monster) == 0x0101)
                Memory::Write32(monster + kMonsterHealth, Memory::Read32(monster + kMonsterHealth + 4));
        }

    // The original took the monster from another code ("MID"); here the one
    // nearest the hunter.
    if (On(Id::MonsterActionRepeater) && Combo(uint16_t(Value(Id::MonsterActionRepeater)))) {
        const uint32_t plw = LocalPlayer();
        uint32_t nearest = 0;
        float best = 0;

        for (uint32_t i = 0; plw && i < count; ++i) {
            const uint32_t monster = MoveWork(Kind::Monster, i);

            if (!monster || Memory::Read16(monster) != 0x0101)
                continue;

            const float dx = std::bit_cast<float>(Memory::Read32(monster + kMonsterX)) - std::bit_cast<float>(Memory::Read32(plw + kPlayerX));
            const float dz = std::bit_cast<float>(Memory::Read32(monster + kMonsterX + 8)) - std::bit_cast<float>(Memory::Read32(plw + kPlayerX + 8));
            const float distance = dx * dx + dz * dz;

            if (!nearest || distance < best) {
                nearest = monster;
                best = distance;
            }
        }

        if (nearest)
            Memory::Write8(nearest + kMonsterState, 0);
    }

    // The first 21 monsters, while the first is in use.
    if (On(Id::ShowMonstersOnMap)) {
        const uint32_t first = MoveWork(Kind::Monster, 0);

        if (first && Memory::Read16(first) == 0x0101)
            for (uint32_t i = 0; i < 21 && i < count; ++i)
                Memory::Write8(MoveWork(Kind::Monster, i) + kMonsterMap, 2);
    }
}

} // namespace Cheats
