// Cheats, RMHP08: the switches the launcher sets and what their
// implementations share. The codes come from Gecko lists for RMHE08 (the US
// disc); every address below is the EU one, found by matching the US code
// and data against this disc's.
//
// Three kinds of implementation:
//   - writes to the game's data (save data, the player's and the monsters'
//     work), once per logic step from Step, as the Gecko code handler wrote
//     them once per frame;
//   - hooks in native ports of the functions a code patched;
//   - wrappers around translated functions that change their arguments,
//     results or the fields they wrote.
#pragma once

#include "abi_bridge.h"
#include "memory.h"

#include <cstdint>
#include <string_view>

namespace Cheats {

// One per menu toggle, in the launcher's order (the table in cheats.cpp).
enum class Id : uint16_t {
    // Basic
    NoDamage,
    NoStaminaDrain,
    AllItems,
    // Stuff
    GalleryClips,
    TurnItUpTo11,
    LanguageChanger,
    InstantFlex,
    CantSave,
    HighRankArmorUpgrades,
    NoArenaRecords,
    AllMonstersOnMap,
    NoHrpChange,
    BothArmorTypes,
    ArmorPigment,
    CantLeaveArea,
    CantGather,
    FarcasterAtCamp,
    PitfallAtCamp,
    ShockAtCamp,
    BarrelBombAtCamp,
    FelvineUnderwater,
    SmokeUnderwater,
    SteakUnderwater,
    FarcasterPhase2,
    TalismanStartSlot,
    HideRewards,
    ShowRealRaw,
    ShowRealElement,
    ShowRealStatus,
    MapOnHud,
    TranqAttaq,
    InfiniteRetries,
    Autofail,
    Autoclear,
    CursedWeapons,
    NoKnockback,
    MonsterHpMultiplier,
    RageRegen,
    InstantRage,
    InfiniteRage,
    InfiniteMonsterStamina,
    MonsterSpeedModifier,
    MaxHealMonsters,
    FelvineForever,
    MonsterCantSeeYou,
    MonsterActionRepeater,
    // Seth@WiiPlaza
    FreezeUnfreezeMonsters,
    SethWeaponsSharp,
    InfiniteBombsInPouch,
    AltInfiniteHealth,
    VillageSpeedHack,
    VillageSnailSpeed,
    SethUltraSharp,
    SethSpeedGlide,
    SethOxygen,
    HealthNeverChanges,
    // Bully@Wiiplaza
    UnlimitedTraps,
    UnlimitedBarrelBombs,
    SpeedCombosEnabled,
    CombineEnabled,
    LongSwordsMaxSharpness,
    NoDefeats,
    Daredevil,
    CrossbowRapidFire,
    InfiniteItems,
    JoinAnyQuest,
    AlwaysShowHrInfo,
    BuyAnyWeaponAtHr0,
    HunterRankModifier,
    NoQuestBoxJiggle,
    ArenaEquipmentTime,
    NoRequiredMaterials,
    NoPlaytimeIncrease,
    AllBoxes8Pages,
    NoNeedForMoney,
    AllStarQuests,
    AllQuestsCompleted,
    PouchItemsX10,
    BoxItemsX99,
    InfiniteResources,
    ProfileNameModifier,
    SuperSpeed,
    ActionRepeater,
    MoonJump,
    Teleporter,
    FreeFly,
    // Skiller
    AlwaysAbleToSleep,
    GhostWorld,
    MorningOnLoad,
    EveningOnLoad,
    NeverStunned,
    NoReload,
    InfiniteGathering,
    InfiniteCarving,
    GatherItem,
    OneHitKills,
    EnemyOneHp,
    WeaponsSharp,
    Boxes8Pages,
    MaxResourcesOnConverting,
    ResourcesZero,
    ItemsGet10,
    SellUseGet10,
    BuyNewGet10,
    MaxMoney,
    ItemsCost1z,
    DontLoseMoney,
    LongSwordPowerMax,
    SmithyOneItem,
    SmithyMaterial,
    MaxAttackBase,
    MaxSharpness,
    MaxAffinity,
    BossMod,
    BossSize,
    BossLevel,
    SexChange,
    InfiniteHealth,
    InfiniteStamina,
    InfiniteAir,
    DefenseUpgrade,
    DefenseMax,
    AttackUpgrade,
    Add1000Attack,
    RunSpeedUp,
    MaxFire,
    MaxWater,
    MaxThunder,
    MaxDragon,
    MaxIce,
    NeverLifeDrain,
    AddDragon,
    AddThunder,
    AddIce,
    AddFire,
    AddWater,
    NoIceWeakness,
    NoDragonWeakness,
    NoWaterWeakness,
    NoThunderWeakness,
    NoFireWeakness,
    NeverDefenseDown,
    NeverFelvine,
    NeverSnowman,
    NeverMuddy,
    AlwaysWindproof,
    NeverPoison,
    NaturalRecoveryUp,
    PurpleHealthBar,
    NoElementStatus,
    FelyneSkill1,
    FelyneSkill2,
    FelyneSkill3,
    FelyneSkill4,
    PuddyFace,
    HairStyle,
    HairRed,
    HairGreen,
    HairBlue,
    Clothing,
    Voice,
    EyeColor,
    Features,
    ClothingRed,
    ClothingGreen,
    ClothingBlue,
    // Argon
    InfiniteTime,
    SetPlayTime,
    AddToDefense,
    UnlimitedColdDrink,
    UnlimitedHotDrink,
    LoadShots,
    // Doudley
    NoFallZoom,
    EnhancedLeap,
    JumpAtWill,
    JumpVeryHigh,
    FaceForwardStick,
    FaceForwardDpad,
    DoudleyMoonJump,
    Glider,
    AntiGravity,
    RunInMidAir,
    MidAirDpad,
    MidAirStick,
    FasterWeaponUsage,
    EnhancedFocus,
    NoOvercharging,
    GreatSwordCharge,
    SnsInfiniteSlashes,
    HammerMaxCharge,
    LanceChargeSpeed,
    LanceChargeController,
    Battojutsu,
    SwitchAxeSwordMode,
    InfiniteAmmo,
    EnableAllAmmo,
    RapidFire,
    AirAttackGreatSword,
    AirAttackSwordAndShield,
    AirAttackHammer,
    AirAttackLance,
    AirAttackLongSword,
    AirAttackSwitchAxe,
    AirAttackBowgun,
    InstantSharpen,
    InstantSteak,
    ForceGrill,
    AllHuntingFleets,
    CaptainCatch,
    CaptainFourStars,
    RedspearCatch,
    RedspearFourStars,
    BlackspearCatch,
    BlackspearFourStars,
    HalfDayVoyages,
    ChamberlynePrize,
    AllArenaChallenges,
    AllFelyneHelpers,
    MaxFarmStats,
    InstantFarming,
    HuntReportKills,
    ForecastTomorrow1,
    ForecastTomorrow2,
    ForecastDayAfter1,
    ForecastDayAfter2,
    ComboListComplete,
    QuickGestures,
    ForceSheathe,
    ForceUnsheathe,
    ForceEvade,
    SleepAnywhere,
    BackToCamp,
    EnhancedMovement,
    EnhancedDashing,
    EnhancedPanicJump,
    EnhancedEvade,
    EvadeDistanceX2,
    EvadeDistanceX10,
    WeaponReach,
    WalkThroughWalls,
    PassThroughMonsters,
    SlowMo,
    FreezeMonsters,
    SwitchAxeGaugeFull,
    AttackUpForever,
    DefenseUpForever,
    SpeedBurstAlways,
    HelmLevel255,
    VestLevel255,
    GlovesLevel255,
    BeltLevel255,
    PantsLevel255,
    SkinTone,
    BarrelRange,
    HelmSkills,
    ChestSkills,
    ArmsSkills,
    WaistSkills,
    LegsSkills,
    ArmorSkillsOff,
    Count
};

// Set by the launcher (or --cheat) before the game starts.
extern bool g_on[size_t(Id::Count)];
extern int32_t g_value[size_t(Id::Count)];

inline bool On(Id id) {
    return g_on[size_t(id)];
}

inline int32_t Value(Id id) {
    return g_value[size_t(id)];
}

// The switches of the codes that their buttons turn on and off (the Gecko
// "CC" lines): on while the cheat is and its buttons have switched it on,
// or always with the "Always" choice (0).
bool Toggled(Id id);

// The name a cheat with a typed value was given (empty: none).
std::string_view Text(Id id);

// Infinite time's buttons, held this step: they run the quest's time out
// (quest_clock.cpp).
bool QuestEndPressed();

// The position a jump's fall leaves as it is (Jump15 in
// field_player_actions.cpp, for moon jump and free fly): bits 0, 1 and 2 for
// x, y and z.
uint32_t FallKeeps(uint32_t plw);

// Doudley's movement codes (cheat_movement.cpp) in the ports of the functions
// they patched: the values the original's constants take for this player.

// The jump (Jump15) and the climb over a ledge (ClimbOver) in
// field_player_actions.cpp.
struct JumpValues {
    float rise = 18.0f;                 // kind 0's upward speed
    float sideways = 0.0f;              // kind 0's x speed and x and z acceleration
    float floorX = 0.0f, floorZ = 0.0f; // the fall's least x and z speeds
    float reach = 180.0f;               // how far down a ledge is caught (0x80277CF8)
    float climbSpeed = 15.0f;           // the jump off the top of a climb
};

JumpValues Jump(uint32_t plw);

// Run119's step forward (2), and the village dash's motion speed (1.25) and
// first push (5) in village_player_actions.cpp.
float RunStep();
float VillageDashSpeed();
float VillageDashPush();

// Slow-mo is on (its buttons pressed, the stick not moved since) with the
// first hunter in play: motions play at an eighth (the motions' rates in
// cheat_hunter.cpp and cheat_movement.cpp).
bool SlowMotion();

// The data writes, once per logic step before the task pass (MH3_GameFrame).
void Step(CpuContext* ctx);

// Step's parts, by what they write.
void StepSaveData();
void StepEquipment();
void StepItems();
void StepHunterCodes();
void StepPlayer();
void StepMonsters();
void StepMovementCodes();

// The game's data.
constexpr uint32_t kSystem = 0x8065AA60; // system_w

// The character's save data (the userdata), 0 before one is loaded.
inline uint32_t UserData() {
    return Memory::Read32(kSystem + 0x95C);
}

// Move work: the game's lists of players, monsters and other movers, each
// entry starting with u16 0x0101 while it is in use.
enum class Kind : uint32_t {
    Player = 2,
    Monster = 3,
};

constexpr uint32_t kPlayerSize = 0xB20, kMonsterSize = 0xB18;
uint32_t MoveWork(Kind kind, uint32_t index);
uint32_t MoveWorkCount(Kind kind);

// The player work (PLW) of the hunter this console controls, 0 when there is
// none (menus, loading).
uint32_t LocalPlayer();

// The Classic Controller's buttons, as the game keeps them (Psw+0xD6 of
// channel 0): held this step and pressed this step.
enum Button : uint16_t {
    kZL = 0x4,
    kL = 0x8,
    kZR = 0x10,
    kA = 0x20,
    kB = 0x40,
    kR = 0x80,
    kX = 0x100,
    kY = 0x200,
    kRight = 0x400,
    kLeft = 0x800,
    kDown = 0x1000,
    kUp = 0x2000,
    kMinus = 0x4000,
    kPlus = 0x8000,
};

uint16_t Held();
uint16_t Pressed();

// Channel 0's presses this step, taken away: the game's tasks then see none
// (a code that answers a button the game also answers).
void ConsumePresses();

// All of `buttons` held, at least one of them pressed this step.
inline bool Combo(uint16_t buttons) {
    return (Held() & buttons) == buttons && (Pressed() & buttons) != 0;
}

inline bool HeldAll(uint16_t buttons) {
    return (Held() & buttons) == buttons;
}

// The buttons of a code that left them to the user held ("Always", 0, among
// its choices is always held).
inline bool Activated(Id id) {
    const auto buttons = uint16_t(Value(id));
    return buttons == 0 || HeldAll(buttons);
}

} // namespace Cheats
