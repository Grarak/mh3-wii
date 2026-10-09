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

namespace Cheats {

// One per menu toggle, in the launcher's order (the table in cheats.cpp).
enum class Id : uint16_t {
    // Basic
    NoDamage,
    NoStaminaDrain,
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
    AutofailAtWill,
    AutoclearAtWill,
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
    SethInfiniteHealth,
    SethUltraSharp,
    SethSpeedGlide,
    SethOxygen,
    ToggleInfiniteHealth,
    // Bully@Wiiplaza
    BullyUnlimitedTraps,
    BullyUnlimitedBarrelBombs,
    SpeedCombosEnabled,
    CombineEnabled,
    LongSwordsMaxSharpness,
    NoDefeats,
    ThunderAlwaysCharged,
    BullyInfiniteHealth,
    BullyInfiniteStamina,
    HotDrinkForever,
    ColdDrinkForever,
    Daredevil,
    BullyWeaponsSharp,
    BullyInfiniteAmmo,
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
    InfiniteFunds,
    SuperSpeed,
    BullyActionRepeater,
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
    AltOneHitKillsToggle,
    AltOneHitKills,
    OneHitKills,
    EnemyOneHp,
    EnableOneHitKills,
    EnableOneHp,
    NormalEnemyHp,
    SkillerWeaponSharp,
    Boxes8Pages,
    MaxResourcesOnConverting,
    ResourcesZero,
    ItemsGet10,
    ItemsGet10Toggle,
    SellUseGet10,
    BuyNewGet10,
    MoneyChangeMax,
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
    SkillerInfiniteTime,
    SexChange,
    InfiniteHp,
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
    NeverStun,
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
    ArgonInfiniteTime,
    SetPlayTime,
    AddToDefense,
    UnlimitedBarrelBombs,
    UnlimitedColdDrink,
    UnlimitedHotDrink,
    LoadShots,
    UnlimitedTraps,
    // Doudley
    NoFallZoom,
    FaceForwardStick,
    FaceForwardDpad,
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
    DoudleyInfiniteAmmo,
    EnableAllAmmo,
    RapidFireHold,
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
    ActionRepeater,
    ForceEvade,
    SleepAnywhere,
    BackToCamp,
    EnhancedMovement,
    UnknockablePlayers,
    InvinciblePlayers,
    ShowMonstersOnMap,
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
// "CC" lines): on while the cheat is and its buttons have switched it on.
bool Toggled(Id id);

// The infinite time codes' buttons, held this step: they run the quest's
// time out (quest_clock.cpp).
bool QuestEndPressed();

// The position a jump's fall leaves as it is (Jump15 in
// field_player_actions.cpp, for moon jump and free fly): bits 0, 1 and 2 for
// x, y and z.
uint32_t FallKeeps(uint32_t plw);

// The data writes, once per logic step before the task pass (MH3_GameFrame).
void Step(CpuContext* ctx);

// Step's parts, by what they write.
void StepSaveData();
void StepItems();
void StepMonsterCodes();
void StepHunterCodes();
void StepPlayer();
void StepMonsters();

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
