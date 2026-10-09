// Cheats, RMHP08: the launcher's cheat table, the switches and the per-step
// pass. The entries come from the Gecko code lists for RMHE08 (cheats/ in the
// development checkout, by author tag), in their order; codes that only work
// online (city, lobby, chat, friends, servers) are not in the table.
#include "cheats.h"

#include "launcher_cheats.h"
#include "player_health.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <climits>
#include <iterator>
#include <string>
#include <vector>

namespace Cheats {

bool g_on[size_t(Id::Count)];
int32_t g_value[size_t(Id::Count)];

namespace {

using LauncherCheats::Cheat;
using LauncherCheats::Option;
using LauncherCheats::Range;

// Buttons for the codes that left them to the user (their "28?????? MMMMXXXX"
// line): L and R or SELECT with a d-pad direction.
constexpr Option kActivators[]{
    {"L+R+Up", kL | kR | kUp},
    {"L+R+Down", kL | kR | kDown},
    {"L+R+Left", kL | kR | kLeft},
    {"L+R+Right", kL | kR | kRight},
    {"SELECT+Up", kMinus | kUp},
    {"SELECT+Down", kMinus | kDown},
    {"SELECT+Left", kMinus | kLeft},
    {"SELECT+Right", kMinus | kRight},
};

// The same, for codes that work without a button as well.
constexpr Option kActivatorsOrAlways[]{
    {"Always", 0},
    {"L+R+Up", kL | kR | kUp},
    {"L+R+Down", kL | kR | kDown},
    {"L+R+Left", kL | kR | kLeft},
    {"L+R+Right", kL | kR | kRight},
    {"SELECT+Up", kMinus | kUp},
    {"SELECT+Down", kMinus | kDown},
    {"SELECT+Left", kMinus | kLeft},
    {"SELECT+Right", kMinus | kRight},
};

// The teleporter's buttons, with d-pad down (save) or up (teleport).
constexpr Option kModifiers[]{
    {"L+R", kL | kR},
    {"ZL+ZR", kZL | kZR},
    {"SELECT", kMinus},
};

constexpr Option kPlayTimes[]{
    {"0:00", 0},
    {"10:00", 10 * 3600},
    {"100:00", 100 * 3600},
    {"127:25", 0x6FFFF},
    {"500:00", 500 * 3600},
    {"9999:59", 35999999},
};

// The game's languages (system_w+9), as the US code's menu named them.
constexpr Option kLanguages[]{
    {"English", 1}, {"French", 2}, {"German", 3}, {"Italian", 4}, {"Spanish", 5},
};

constexpr Option kHunterRanks[]{
    {"1", 1}, {"2", 2}, {"9", 9}, {"17", 17}, {"30", 30}, {"31", 31}, {"50", 50},
    {"100", 100}, {"200", 200}, {"500", 500}, {"999", 999},
};

constexpr Option kMultipliers[]{
    {"x2", 2}, {"x3", 3}, {"x4", 4}, {"x5", 5}, {"x10", 10},
};

// Monster speed factors in percent (the code's float's upper half: 3F00 0.5,
// 3F80 normal, 3FC0 faster, 4040 three times).
constexpr Option kMonsterSpeeds[]{
    {"x0.25", 25}, {"x0.5", 50}, {"x0.75", 75}, {"x1 (normal)", 100}, {"x1.5", 150}, {"x2", 200}, {"x3", 300},
};

constexpr Option kShots[]{
    {"2", 2}, {"5", 5}, {"10", 10}, {"20", 20}, {"50", 50}, {"99", 99},
};

// Add to defense's values (the code's XXXX: 0FA0, 2328 in its notes).
constexpr Option kDefenseBonuses[]{
    {"+100", 100}, {"+500", 500}, {"+1000", 1000}, {"+4000", 4000}, {"+9000", 9000},
};

// Skin tones, darkest to lightest: the u16 is the tone's base (0 to 2) and
// its shade towards the next (0 to 100), as character creation sets them
// (0x80047BBC). The code's notes gave 000-264 as one hex range; the values
// between hold shades the game never makes.
constexpr Option kSkinTones[]{
    {"000", 0x000}, {"014", 0x014}, {"028", 0x028}, {"03C", 0x03C}, {"050", 0x050},
    {"100", 0x100}, {"114", 0x114}, {"128", 0x128}, {"13C", 0x13C}, {"150", 0x150},
    {"200", 0x200}, {"214", 0x214}, {"228", 0x228}, {"23C", 0x23C}, {"250", 0x250}, {"264", 0x264},
};

// Bowgun barrel ranges, as the code's notes listed them (hundredths).
constexpr Option kBarrelRanges[]{
    {"0.30", 30}, {"0.50", 50}, {"0.70", 70}, {"1.00", 100}, {"3.00", 300}, {"4.00", 400},
};

struct Entry {
    Id id;
    Cheat cheat;
};

constexpr const char* kBasic = "Basic";
constexpr const char* kStuff = "Stuff";
constexpr const char* kSeth = "Seth@WiiPlaza";
constexpr const char* kBully = "Bully@Wiiplaza";
constexpr const char* kSkiller = "Skiller";
constexpr const char* kArgon = "Argon";
constexpr const char* kDoudley = "Doudley";
constexpr const char* kGroups[]{kBasic, kStuff, kSeth, kBully, kSkiller, kArgon, kDoudley};

constexpr Entry kEntries[]{
    {Id::NoDamage, {kBasic, "No damage", "The hunter takes no damage."}},
    {Id::NoStaminaDrain, {kBasic, "No action stamina drain", "Running, guarding and other continuous actions cost no stamina."}},

    {Id::GalleryClips, {kStuff, "Unlock all gallery clips", "Every movie of the gallery can be watched; saved with the game."}},
    {Id::TurnItUpTo11, {kStuff, "Turn it up to 11", "Music volume past the option's maximum."}},
    {Id::LanguageChanger, {kStuff, "Language changer", "The game's language (text and menus loaded after the change).", kLanguages}},
    {Id::InstantFlex, {kStuff, "Instant flex", "The quest fails once less than a minute of its time is left."}},
    {Id::CantSave, {kStuff, "Can't save", "Saving writes back the game as it was loaded (progress is not saved)."}},
    {Id::HighRankArmorUpgrades, {kStuff, "Armor is upgradeable to high rank even at low rank", "Armor upgrades act as if the hunter rank were 999."}},
    {Id::NoArenaRecords, {kStuff, "Don't save new arena records", "Arena and challenge quest times are not recorded."}},
    {Id::AllMonstersOnMap, {kStuff, "Show all monsters on the map", "Every monster counts as marked on the map."}},
    {Id::NoHrpChange, {kStuff, "No HRP gain/loss", "Hunter rank points and the hunter rank stay as they are."}},
    {Id::BothArmorTypes, {kStuff, "Equip blade/gunner armor", "Blade and gunner armor can be equipped with either weapon class."}},
    {Id::ArmorPigment, {kStuff, "Enable armor pigment for all armor", "The pigment of every armor piece can be changed."}},
    {Id::CantLeaveArea, {kStuff, "Can't leave area (on/off)", "The buttons switch it on and off: while on, the hunter cannot leave the area.", kActivators}},
    {Id::CantGather, {kStuff, "Can't carve/gather (on/off)", "The buttons switch it on and off: while on, nothing can be carved or gathered.", kActivators}},
    {Id::FarcasterAtCamp, {kStuff, "Farcaster at camp", "Farcasters work in the camp's area too."}},
    {Id::PitfallAtCamp, {kStuff, "Pitfall at camp", "Pitfall traps can be set anywhere."}},
    {Id::ShockAtCamp, {kStuff, "Shock at camp", "Shock traps can be set anywhere."}},
    {Id::BarrelBombAtCamp, {kStuff, "LBB at camp", "Large barrel bombs can be set in the camp's area."}},
    {Id::FelvineUnderwater, {kStuff, "Felvine underwater", "Felvine bombs can be used underwater."}},
    {Id::SmokeUnderwater, {kStuff, "Smoke underwater", "Smoke bombs can be used underwater."}},
    {Id::SteakUnderwater, {kStuff, "Steak underwater", "Steaks and other meat can be eaten underwater."}},
    {Id::FarcasterPhase2, {kStuff, "Farcaster in phase 2", "Farcasters work in the second phase of the final fights too."}},
    {Id::TalismanStartSlot, {kStuff, "Talismans sent to box start at slot", "New talismans go to the first free box slot from this one.", {}, {0, 750, 50}}},
    {Id::HideRewards, {kStuff, "Hold to hide rewards", "While the buttons are held, the quest result screen stands still (for screenshots).", kActivators}},
    {Id::ShowRealRaw, {kStuff, "Show real raw", "Weapon attack is shown without the weapon type's factor."}},
    {Id::ShowRealElement, {kStuff, "Show real element", "Weapon element is shown without the times ten."}},
    {Id::ShowRealStatus, {kStuff, "Show real status", "Weapon status is shown without the times ten."}},
    {Id::MapOnHud, {kStuff, "Map attached to HUD", "The map option follows the HUD's state (Gecko: map option 0, 1 in HUD state 0x3C04)."}},
    {Id::TranqAttaq, {kStuff, "Tranq attaq", "Every hit fills the monster's tranquilizer gauge (more than two tranq bombs)."}},
    {Id::InfiniteRetries, {kStuff, "Inf retries + reward increased instead", "Fainting adds the penalty to the reward instead of taking it, so the quest never fails by faints."}},
    {Id::Autofail, {kStuff, "Autofail", "The quest fails at once."}},
    {Id::Autoclear, {kStuff, "Autoclear (main and subs)", "Every started objective counts as met: the quest clears. Not with autofail."}},
    {Id::AutofailAtWill, {kStuff, "Autofail at will", "The quest fails while the buttons are held.", kActivators}},
    {Id::AutoclearAtWill, {kStuff, "Autoclear at will", "The objectives count as met while the buttons are held.", kActivators}},
    {Id::CursedWeapons, {kStuff, "Cursed weapons", "While the weapon is drawn, health drains by 1 every 32 steps."}},
    {Id::NoKnockback, {kStuff, "No knockback", "Hits do not knock the hunter back or down."}},
    {Id::MonsterHpMultiplier, {kStuff, "Monster HP multipler", "Large and small monsters' maximum health times this.", kMultipliers}},
    {Id::RageRegen, {kStuff, "Monsters have regen during rage", "While enraged, monsters regain a third of their level in health every 64 steps."}},
    {Id::InstantRage, {kStuff, "Instant rage mode", "Any rage gain fills the gauge: monsters enrage at once."}},
    {Id::InfiniteRage, {kStuff, "Infinite rage mode", "Rage never ends."}},
    {Id::InfiniteMonsterStamina, {kStuff, "Infinite monster stamina", "Monsters' stamina never goes down (they never tire)."}},
    {Id::MonsterSpeedModifier, {kStuff, "Monster speed modifier", "Monsters' motions play at this speed.", kMonsterSpeeds, {}, 3}},
    {Id::MaxHealMonsters, {kStuff, "Max heal all monsters", "The buttons heal every monster to its full health.", kActivators}},
    {Id::FelvineForever, {kStuff, "Felvine lasts forever on monster", "A monster's felvine countdown never runs out."}},
    {Id::MonsterCantSeeYou, {kStuff, "Monster can't see you", "Monsters' sight and awareness never update."}},
    {Id::MonsterActionRepeater, {kStuff, "Action repeater for targeted monster", "The buttons restart the action of the monster nearest the hunter.", kActivators}},

    {Id::FreezeUnfreezeMonsters, {kSeth, "Freeze/unfreeze monsters", "The buttons switch it on and off: while on, monsters in play stand still.", kActivators}},
    {Id::SethWeaponsSharp, {kSeth, "Weapons always sharp (best version)", "The sharpness levels set when a weapon is drawn stay as they are."}},
    {Id::InfiniteBombsInPouch, {kSeth, "Infinite bombs in pouch", "Items whose count changes on an existing pouch stack keep their count."}},
    {Id::AltInfiniteHealth, {kSeth, "Alternate infinite health", "Damage leaves health as it is, unless a hit would empty it."}},
    {Id::VillageSpeedHack, {kSeth, "Speed hack in village", "Hold ZL in the village: the hunter jumps ahead to the nearest wall."}},
    {Id::VillageSnailSpeed, {kSeth, "Snail speed (village only)", "Hold ZL in the village: the hunter barely moves."}},
    {Id::SethInfiniteHealth, {kSeth, "Infinite health (online code)", "Health and maximum stay at 100 (the online code, on the local hunter)."}},
    {Id::SethUltraSharp, {kSeth, "Weapons ultra sharp (online code)", "Purple sharpness that never wears (the online code, on the local hunter)."}},
    {Id::SethSpeedGlide, {kSeth, "Speed glide/skate (online code)", "Hold ZL while running to glide very fast (the online code, on the local hunter)."}},
    {Id::SethOxygen, {kSeth, "Increase oxygen bar (online code)", "Oxygen gauge of 255 (the online code, on the local hunter)."}},
    {Id::ToggleInfiniteHealth, {kSeth, "Toggle infinite health", "The buttons switch it on and off: while on, health never changes.", kActivators}},

    {Id::BullyUnlimitedTraps, {kBully, "Set unlimited traps", "Traps can be set while another is set."}},
    {Id::BullyUnlimitedBarrelBombs, {kBully, "Set unlimited barrel bombs", "Any number of large barrel bombs can be set."}},
    {Id::SpeedCombosEnabled, {kBully, "Speed combos never disabled", "The item menu's speed combo entry is always enabled."}},
    {Id::CombineEnabled, {kBully, "Combine never disabled", "The item menu's combine entry is always enabled."}},
    {Id::LongSwordsMaxSharpness, {kBully, "Long swords max sharpness", "The code as written holds the long sword's spirit gauge at 0."}},
    {Id::NoDefeats, {kBully, "Defeats counter always 0", "Faints are not counted."}},
    {Id::ThunderAlwaysCharged, {kBully, "Thunder weapons always charged", "The gauge at +0x36C of the hunter stays full."}},
    {Id::BullyInfiniteHealth, {kBully, "Infinite health", "Health never changes; the maximum drops to the current health."}},
    {Id::BullyInfiniteStamina, {kBully, "Infinite stamina", "Any stamina change sets stamina and its maximum to 605."}},
    {Id::HotDrinkForever, {kBully, "Hot drink lasts forever", "A hot drink no longer runs out outside the cold areas (in them it still does)."}},
    {Id::ColdDrinkForever, {kBully, "Cold drink lasts forever", "A cold drink no longer runs out outside the hot areas (in them it still does)."}},
    {Id::Daredevil, {kBully, "Daredevil mode", "Any change to health empties it."}},
    {Id::BullyWeaponsSharp, {kBully, "Weapons always sharp", "Any sharpness change sets it to the maximum."}},
    {Id::BullyInfiniteAmmo, {kBully, "Infinite ammo", "Each shot reloads the bowgun's clip."}},
    {Id::CrossbowRapidFire, {kBully, "Crossbow rapid fire", "A new action keeps the bowgun's firing state (+0x318)."}},
    {Id::InfiniteItems, {kBully, "Infinite items", "Using an item (potions, whetstones, food, and so on) does not use it up."}},
    {Id::JoinAnyQuest, {kBully, "Join/host any quest", "The hunter rank counts as 999 for quests and the status screen."}},
    {Id::AlwaysShowHrInfo, {kBully, "Always display HR infos on status", "The status screen shows the hunter rank details at HR 0 too."}},
    {Id::BuyAnyWeaponAtHr0, {kBully, "Purchase any weapon with HR0", "Weapons that need a hunter rank can be bought at HR 0."}},
    {Id::HunterRankModifier, {kBully, "Hunter ranking modifier", "The hunter rank (and its points) is set to this when the status is read.", kHunterRanks}},
    {Id::NoQuestBoxJiggle, {kBully, "No quest box jiggle", "The selected quest box stands still (when selecting the level)."}},
    {Id::ArenaEquipmentTime, {kBully, "Infinite time for choosing your equipment", "The arena's equipment choice has no time limit."}},
    {Id::NoRequiredMaterials, {kBully, "No need to have required materials", "Every required material counts as 999 in the box."}},
    {Id::NoPlaytimeIncrease, {kBully, "No playtime increase", "The play time clock stops."}},
    {Id::AllBoxes8Pages, {kBully, "All boxes have 8 pages", "Item pouch, gunner pouch, item box and equipment box show 8 pages."}},
    {Id::NoNeedForMoney, {kBully, "No need for money", "Anything can be bought or upgraded with too little money (the money then runs out)."}},
    {Id::AllStarQuests, {kBully, "Unlock all star quests", "Every star quest is unlocked; saved with the game."}},
    {Id::AllQuestsCompleted, {kBully, "All quests completed", "Every quest counts as cleared; saved with the game."}},
    {Id::PouchItemsX10, {kBully, "Item pouch items x10", "Items shown in the item lists get a count of 10."}},
    {Id::BoxItemsX99, {kBully, "All item box items x99", "Item box stacks get a count of 99 when the box is listed."}},
    {Id::InfiniteResources, {kBully, "Infinite ressources", "Moga Woods resources become 99,999,999 when shown."}},
    {Id::InfiniteFunds, {kBully, "Infinite funds", "Money becomes 99,999,999 when shown."}},
    {Id::ProfileNameModifier, {kBully, "Profile name modifier", "The character gets the typed name as it loads; saving keeps it (16 characters at most).", {}, {}, 0, 16}},
    {Id::SuperSpeed, {kBully, "Super speed", "The buttons switch it on and off: the hunter's motions play five times as fast.", kActivators}},
    {Id::BullyActionRepeater, {kBully, "Action repeater", "The buttons restart the hunter's current action.", kActivators}},
    {Id::MoonJump, {kBully, "Moon jump", "Hold A+B to rise (not with Free fly)."}},
    {Id::Teleporter, {kBully, "Teleporter", "These buttons with d-pad down save the position, with d-pad up teleport there.", kModifiers}},
    {Id::FreeFly, {kBully, "Free fly", "Hold L with the d-pad to move, A+B to rise; the buttons switch falling off and on.", kActivators}},

    {Id::AlwaysAbleToSleep, {kSkiller, "Always able to sleep", "The bed can always be used."}},
    {Id::GhostWorld, {kSkiller, "Ghost world", "Monsters are drawn faint, and two of their countdowns stop."}},
    {Id::MorningOnLoad, {kSkiller, "Change to morning (upon loading)", "The village and Moga Woods load in the morning."}},
    {Id::EveningOnLoad, {kSkiller, "Change to evening (upon loading)", "The village and Moga Woods load in the evening."}},
    {Id::NeverStunned, {kSkiller, "Never stund", "The stun gauge never fills."}},
    {Id::NoReload, {kSkiller, "No reload", "Shots take no round from the loaded ones."}},
    {Id::InfiniteGathering, {kSkiller, "Inf gathering", "Gathering spots never run out."}},
    {Id::InfiniteCarving, {kSkiller, "Inf carving", "Gathering never uses up the spot's second count (no tool breaking)."}},
    {Id::GatherItem, {kSkiller, "Gathering/carving mod", "Every gather and carve gives this item (by number).", {}, {1, 999}}},
    {Id::AltOneHitKillsToggle, {kSkiller, "(Alt) 1 hit kills (buttons on/off)", "The buttons switch it on and off: while on, any hit kills a monster.", kActivators}},
    {Id::AltOneHitKills, {kSkiller, "(Alt) 1 hit kills enemys", "Any hit kills a monster."}},
    {Id::OneHitKills, {kSkiller, "1 hit kills enemy", "Any change to a monster's health empties it."}},
    {Id::EnemyOneHp, {kSkiller, "Enemy has 1 HP (good for capture/cutting parts off)", "Any change to a monster's health leaves 1."}},
    {Id::EnableOneHitKills, {kSkiller, "Enable 1 hit kills (enemy)", "The buttons switch monsters to 1 hit kills.", kActivators}},
    {Id::EnableOneHp, {kSkiller, "Enable 1 HP (enemy capture/cutting parts mode)", "The buttons switch monsters to 1 HP.", kActivators, {}, 1}},
    {Id::NormalEnemyHp, {kSkiller, "Normal enemy HP (off code for 1 HP and 1 hit kills)", "The buttons switch the two codes above off.", kActivators, {}, 2}},
    {Id::SkillerWeaponSharp, {kSkiller, "Weapon always sharp", "Any sharpness change sets it to the maximum."}},
    {Id::Boxes8Pages, {kSkiller, "Item/equip boxes have 8 pages", "The item and equipment boxes start with 8 pages."}},
    {Id::MaxResourcesOnConverting, {kSkiller, "Max res on converting", "Any change to the resources sets them to 9,999,999."}},
    {Id::ResourcesZero, {kSkiller, "Res 0", "Resources (Moga Woods) stay at 0."}},
    {Id::ItemsGet10, {kSkiller, "Gain/use items/ammo get 10", "An item stack you already have becomes 10 when its count changes."}},
    {Id::ItemsGet10Toggle, {kSkiller, "Gain/use items/ammo get 10 (buttons on/off)", "As above; the buttons switch it on and off.", kActivators}},
    {Id::SellUseGet10, {kSkiller, "Sell/use item get 10", "A stack you sell from or use becomes 10."}},
    {Id::BuyNewGet10, {kSkiller, "Buy new item get 10", "A new item stack starts at 10."}},
    {Id::MoneyChangeMax, {kSkiller, "Gain/use money > max money", "Any change to the money sets it to 9,999,999z."}},
    {Id::MaxMoney, {kSkiller, "Max/inf money", "Money stays at 9,999,999z."}},
    {Id::ItemsCost1z, {kSkiller, "All items cost 1z in item stores", "Item prices become 0 (1z at the store)."}},
    {Id::DontLoseMoney, {kSkiller, "Dont lose money buying items", "Money never goes down."}},
    {Id::LongSwordPowerMax, {kSkiller, "On hit long sword power max (charge bar)", "When the long sword's spirit gauge would go down, it fills."}},
    {Id::SmithyOneItem, {kSkiller, "Smithy forge/upgrade with 1 item", "The smithy needs no materials."}},
    {Id::SmithyMaterial, {kSkiller, "Set smithy req materials to", "Every smithy material slot asks for this item (reopen the menu).", {}, {1, 999}}},
    {Id::MaxAttackBase, {kSkiller, "Weapon max attack base (press buttons when on weapon)", "With the buttons held while a weapon's stats show, its attack becomes 32767.", kActivators}},
    {Id::MaxSharpness, {kSkiller, "Max sharp (press buttons when on weapon)", "With the buttons held while a weapon's sharpness shows, it becomes the highest.", kActivators}},
    {Id::MaxAffinity, {kSkiller, "Max affinity (press buttons when on weapon)", "With the buttons held while a weapon's affinity shows, it becomes 127.", kActivators}},
    {Id::BossMod, {kSkiller, "Boss/s mod", "The quest's large monsters are this kind (quest data read after the change).", {}, {1, 63}}},
    {Id::BossSize, {kSkiller, "Set boss/s size to", "The quest's large monsters' size byte.", {}, {0, 255, 5}}},
    {Id::BossLevel, {kSkiller, "Set boss/s level to", "The quest's large monsters' level byte.", {}, {0, 255}}},
    {Id::SkillerInfiniteTime, {kSkiller, "Inf time arena/quests (finish mission press __)", "The quest clock stops; the buttons run the time out.", kActivators}},
    {Id::SexChange, {kSkiller, "Sex change", "The character's sex (0 male, 1 female); loads with the character.", {}, {0, 1}}},
    {Id::NameChange, {kSkiller, "Name change", "The character's name is the typed one (7 characters at most).", {}, {}, 0, 7}},
    {Id::InfiniteHp, {kSkiller, "Inf HP", "Health and recoverable health stay full."}},
    {Id::InfiniteStamina, {kSkiller, "Inf stamina", "Stamina stays full."}},
    {Id::InfiniteAir, {kSkiller, "Inf air", "The oxygen gauge stays full."}},
    {Id::DefenseUpgrade, {kSkiller, "Defense upgrade (255)", "Defense items' boost at its highest."}},
    {Id::DefenseMax, {kSkiller, "Defense max (255)", "Defense 255."}},
    {Id::AttackUpgrade, {kSkiller, "Attack upgrade (255)", "Attack items' boost at its highest."}},
    {Id::Add1000Attack, {kSkiller, "Add 1000 to attack", "The hunter's attack is 1000."}},
    {Id::RunSpeedUp, {kSkiller, "Speed up when running", "While the hunter runs, motions set then play at rate 6."}},
    {Id::MaxFire, {kSkiller, "Max fire", "Fire resistance at its maximum."}},
    {Id::MaxWater, {kSkiller, "Max water", "Water resistance at its maximum."}},
    {Id::MaxThunder, {kSkiller, "Max thunder", "Thunder resistance at its maximum."}},
    {Id::MaxDragon, {kSkiller, "Max dragon", "Dragon resistance at its maximum."}},
    {Id::MaxIce, {kSkiller, "Max ice", "Ice resistance at its maximum."}},
    {Id::NeverLifeDrain, {kSkiller, "Never life drain", "Health is never drained."}},
    {Id::AddDragon, {kSkiller, "Add 5 to dragon", "Dragon resistance bonus."}},
    {Id::AddThunder, {kSkiller, "Add 5 to thunder", "Thunder resistance bonus."}},
    {Id::AddIce, {kSkiller, "Add 5 to ice", "Ice resistance bonus."}},
    {Id::AddFire, {kSkiller, "Add 5 to fire (effect)", "Fire resistance bonus."}},
    {Id::AddWater, {kSkiller, "Adds 5 to water", "Water resistance bonus."}},
    {Id::NoIceWeakness, {kSkiller, "No negative ice stats effect", "No ice resistance down."}},
    {Id::NoDragonWeakness, {kSkiller, "No negative dragon stats effect", "No dragon resistance down."}},
    {Id::NoWaterWeakness, {kSkiller, "No negative water stats effect", "No water resistance down."}},
    {Id::NoThunderWeakness, {kSkiller, "No negative thunder stats effect", "No thunder resistance down."}},
    {Id::NoFireWeakness, {kSkiller, "No negative fire stats", "No fire resistance down."}},
    {Id::NeverDefenseDown, {kSkiller, "Never get defense down", "Defense down never lasts."}},
    {Id::NeverFelvine, {kSkiller, "Never get felvine-scented", "Felvine scent never lasts."}},
    {Id::NeverSnowman, {kSkiller, "Never get snowman", "Snowman never lasts."}},
    {Id::NeverMuddy, {kSkiller, "Never get muddy", "Mud never lasts."}},
    {Id::AlwaysWindproof, {kSkiller, "Always windproof", "Windproof always on."}},
    {Id::NeverStun, {kSkiller, "Never stun", "Stun never lasts."}},
    {Id::NeverPoison, {kSkiller, "Never poison", "Poison never lasts."}},
    {Id::NaturalRecoveryUp, {kSkiller, "Always have natural rec up", "Natural recovery up always on."}},
    {Id::PurpleHealthBar, {kSkiller, "Health bar flash purple", "The health bar flashes purple."}},
    {Id::NoElementStatus, {kSkiller, "No element", "No elemental status (fire, and so on)."}},
    {Id::FelyneSkill1, {kSkiller, "Felyne skill 1 button activation", "L+R+Up sets the first Felyne skill.", {}, {0, 63}}},
    {Id::FelyneSkill2, {kSkiller, "Felyne skill 2 button activation", "L+R+Down sets the second Felyne skill.", {}, {0, 63}}},
    {Id::FelyneSkill3, {kSkiller, "Felyne skill 3 button activation", "L+R+Left sets the third Felyne skill.", {}, {0, 63}}},
    {Id::FelyneSkill4, {kSkiller, "Felyne skill 4 button activation", "L+R+Right sets the fourth Felyne skill.", {}, {0, 63}}},
    {Id::PuddyFace, {kSkiller, "Puddy face", "Face (character creation's 9); loads with the character.", {}, {0, 8}}},
    {Id::HairStyle, {kSkiller, "Hair dresser", "Hair style (character creation's 11); loads with the character.", {}, {0, 10}}},
    {Id::HairRed, {kSkiller, "Hair dye (red)", "Hair colour, red.", {}, {0, 255, 15}}},
    {Id::HairGreen, {kSkiller, "Hair dye (green)", "Hair colour, green.", {}, {0, 255, 15}}},
    {Id::HairBlue, {kSkiller, "Hair dye (blue)", "Hair colour, blue.", {}, {0, 255, 15}}},
    {Id::Clothing, {kSkiller, "Clothing change", "Inner clothing (character creation's 4); loads with the character.", {}, {0, 3}}},
    {Id::Voice, {kSkiller, "Voice change", "Voice (character creation's 20).", {}, {0, 19}}},
    {Id::EyeColor, {kSkiller, "Eye color change", "Eye colour (character creation's 10); loads with the character.", {}, {0, 9}}},
    {Id::Features, {kSkiller, "Features", "Facial features (character creation's 13); loads with the character.", {}, {0, 12}}},
    {Id::ClothingRed, {kSkiller, "Color changing clothing (red)", "Inner clothing colour, red; loads with the character.", {}, {0, 255, 15}}},
    {Id::ClothingGreen, {kSkiller, "Color changing clothing (green)", "Inner clothing colour, green; loads with the character.", {}, {0, 255, 15}}},
    {Id::ClothingBlue, {kSkiller, "Color changing clothing (blue)", "Inner clothing colour, blue; loads with the character.", {}, {0, 255, 15}}},

    {Id::ArgonInfiniteTime, {kArgon, "Infinite time arena and quests", "The quest clock stops; the buttons run the time out.", kActivators}},
    {Id::SetPlayTime, {kArgon, "Set playtime", "The play time stays at this value.", kPlayTimes}},
    {Id::AddToDefense, {kArgon, "Add to defense", "This is added to the hunter's defense.", kDefenseBonuses}},
    {Id::UnlimitedBarrelBombs, {kArgon, "Set unlimited barrel bombs", "Barrel bombs on the field never count against the limit."}},
    {Id::UnlimitedColdDrink, {kArgon, "Unlimited cold drink", "A cold drink's effect never ends."}},
    {Id::UnlimitedHotDrink, {kArgon, "Unlimited hot drink", "A hot drink's effect never ends."}},
    {Id::LoadShots, {kArgon, "Load XX shots", "The bowgun's clip holds this many shots.", kShots, {}, 2}},
    {Id::UnlimitedTraps, {kArgon, "Set unlimited traps", "Traps on the field never count against the limit."}},

    {Id::NoFallZoom, {kDoudley, "Cancel camera zoom-in when falling", "The camera does not zoom in on falls under 300 units."}},
    {Id::EnhancedLeap, {kDoudley, "Enhanced leap at will (press dash and action button)", "While dashing, A leaps off flat ground; A in the air carries the leap forward."}},
    {Id::JumpAtWill, {kDoudley, "Jump at will (press the action button to jump)", "A jumps from anywhere: a tap jumps low, holding it longer jumps higher."}},
    {Id::JumpVeryHigh, {kDoudley, "Jump very high (press dash and action button)", "With Jump at will: R and A together jump much higher and fall slowly."}},
    {Id::FaceForwardStick, {kDoudley, "Always face forward when using R stick", "The hunter turns with the camera while the right stick moves."}},
    {Id::FaceForwardDpad, {kDoudley, "Always face forward when using left/right d-pad", "The hunter turns with the camera while d-pad left or right is held."}},
    {Id::DoudleyMoonJump, {kDoudley, "Moonjump (A button)", "While dashing, A jumps high and floats down slowly."}},
    {Id::Glider, {kDoudley, "Glider (single play)", "Holding A as a jump starts glides; the stick steers in the air."}},
    {Id::AntiGravity, {kDoudley, "Enable anti-gravity", "The hunter floats; ZL pushes up, ZR pulls down."}},
    {Id::RunInMidAir, {kDoudley, "Run and dash in mid air", "Hold L in the air to run, with R to dash."}},
    {Id::MidAirDpad, {kDoudley, "Mid air d-pad version", "With Run and dash in mid air: d-pad up runs upwards, down downwards."}},
    {Id::MidAirStick, {kDoudley, "Mid air R stick version", "With Run and dash in mid air: the right stick up runs upwards, down downwards."}},
    {Id::FasterWeaponUsage, {kDoudley, "Faster weapon usage", "Weapon actions play three times as fast."}},
    {Id::EnhancedFocus, {kDoudley, "Enhanced focus (improved weapon charge for GS, HA, LS, SW)", "Focus always, five times as strong (the great sword's charge almost at once)."}},
    {Id::NoOvercharging, {kDoudley, "GreatSword no overcharging", "Charges stay at level 3 instead of overcharging."}},
    {Id::GreatSwordCharge, {kDoudley, "GreatSword inf charge and Lv3 charge always", "Charge times are endless and the great sword's charge is set to level 3."}},
    {Id::SnsInfiniteSlashes, {kDoudley, "Sword and shield inf slashes", "With the buttons held, X slashes again and again.", kActivatorsOrAlways}},
    {Id::HammerMaxCharge, {kDoudley, "Hammer max charge always", "The hammer's charge is full at once."}},
    {Id::LanceChargeSpeed, {kDoudley, "Lance charge speed increased", "Hold up while a lance charges to charge faster, from then on."}},
    {Id::LanceChargeController, {kDoudley, "Lance charge speed controller", "While charging: stick up goes faster, R faster still, back aims upward."}},
    {Id::Battojutsu, {kDoudley, "Battojutsu mode (long sword quick draw)", "With the buttons held, A draws the long sword into a slash.", kActivatorsOrAlways}},
    {Id::SwitchAxeSwordMode, {kDoudley, "Force switch axe sword mode", "With the buttons held, R morphs into sword mode at once.", kActivatorsOrAlways}},
    {Id::DoudleyInfiniteAmmo, {kDoudley, "Inf ammo", "Firing the bowgun uses no ammo."}},
    {Id::EnableAllAmmo, {kDoudley, "Enable all ammo v2", "Every ammo can be loaded (at level 1 and the barrel's and skills' levels)."}},
    {Id::RapidFireHold, {kDoudley, "Rapidfire all ammo (hold shoot button while firing)", "With A held, a shot fires six more rounds and uses no ammo."}},
    {Id::RapidFire, {kDoudley, "Rapidfire all ammo (always active)", "A shot fires six more rounds and uses no ammo."}},
    {Id::AirAttackGreatSword, {kDoudley, "Air attack (great sword)", "Hold X in the air, then A or A+R to attack."}},
    {Id::AirAttackSwordAndShield, {kDoudley, "Air attack (sword and shield)", "Hold X in the air, then A or A+R to attack."}},
    {Id::AirAttackHammer, {kDoudley, "Air attack (hammer)", "Hold X in the air, then A or A+R to attack."}},
    {Id::AirAttackLance, {kDoudley, "Air attack (lance)", "Hold X in the air to charge; R changes the direction."}},
    {Id::AirAttackLongSword, {kDoudley, "Air attack (long sword)", "Hold X in the air, then A or A+R to attack."}},
    {Id::AirAttackSwitchAxe, {kDoudley, "Air attack (switch axe)", "Hold X in the air, then A or A+R to attack."}},
    {Id::AirAttackBowgun, {kDoudley, "Air attack (bowgun)", "Hold X in the air, then A to shoot."}},
    {Id::InstantSharpen, {kDoudley, "Instant sharpen", "The whetstone's motion ends at once."}},
    {Id::InstantSteak, {kDoudley, "Instant well-done steak", "Grilled meat is well done at once."}},
    {Id::ForceGrill, {kDoudley, "Force grill", "With the quest menu open, the buttons start grilling meat.", kActivators}},
    {Id::AllHuntingFleets, {kDoudley, "Enable all hunting fleets", "Every hunting fleet ship is available."}},
    {Id::CaptainCatch, {kDoudley, "Hunting fleet captain always has catch", "The captain's ship always returns with a catch."}},
    {Id::CaptainFourStars, {kDoudley, "Hunting fleet captain always 4 stars", "The captain's ship at 4 stars."}},
    {Id::RedspearCatch, {kDoudley, "Hunting fleet Redspear always has catch", "Redspear always returns with a catch."}},
    {Id::RedspearFourStars, {kDoudley, "Hunting fleet Redspear always 4 stars", "Redspear at 4 stars."}},
    {Id::BlackspearCatch, {kDoudley, "Hunting fleet Blackspear always has catch", "Blackspear always returns with a catch."}},
    {Id::BlackspearFourStars, {kDoudley, "Hunting fleet Blackspear always 4 stars", "Blackspear at 4 stars."}},
    {Id::HalfDayVoyages, {kDoudley, "All hunting fleets: half a day voyages", "Every voyage takes half a day."}},
    {Id::ChamberlynePrize, {kDoudley, "1 million zenny prize from Chamberlyne", "Chamberlyne's prize money is 1,000,000z."}},
    {Id::AllArenaChallenges, {kDoudley, "Enable all challenges in offline arena", "Every arena challenge is open (save in the village first)."}},
    {Id::AllFelyneHelpers, {kDoudley, "Enable all Felyne helpers", "Every farm Felyne helper is hired."}},
    {Id::MaxFarmStats, {kDoudley, "Max out all farm stats", "Every farm field at its highest level."}},
    {Id::InstantFarming, {kDoudley, "Instant farming for all Felyne helpers", "The Felyne helpers' work is done at once."}},
    {Id::HuntReportKills, {kDoudley, "Each monster kill at hunt report", "Every monster has at least one kill in the hunt report."}},
    {Id::ForecastTomorrow1, {kDoudley, "Boss 1 Moga forecast tomorrow", "The first large monster forecast for tomorrow.", {}, {0, 63}}},
    {Id::ForecastTomorrow2, {kDoudley, "Boss 2 Moga forecast tomorrow", "The second large monster forecast for tomorrow.", {}, {0, 63}}},
    {Id::ForecastDayAfter1, {kDoudley, "Boss 1 Moga forecast day after", "The first large monster forecast for the day after.", {}, {0, 63}}},
    {Id::ForecastDayAfter2, {kDoudley, "Boss 2 Moga forecast day after", "The second large monster forecast for the day after.", {}, {0, 63}}},
    {Id::ComboListComplete, {kDoudley, "Combo list complete", "Every combination is known (and the Scroll of the Sage award)."}},
    {Id::QuickGestures, {kDoudley, "Quick gestures", "L+R+Up plays this gesture.", {}, {0, 31}}},
    {Id::ForceSheathe, {kDoudley, "Force sheathe weapon", "The buttons sheathe the weapon at once.", kActivators}},
    {Id::ForceUnsheathe, {kDoudley, "Force unsheathe weapon", "The buttons draw the weapon at once.", kActivators}},
    {Id::ActionRepeater, {kDoudley, "Action repeater", "START repeats the current action."}},
    {Id::ForceEvade, {kDoudley, "Force evade (quick recover)", "With the buttons held, B evades out of any action.", kActivatorsOrAlways}},
    {Id::SleepAnywhere, {kDoudley, "Sleep anywhere", "SELECT and B together lie the hunter down to sleep (standing on the ground)."}},
    {Id::BackToCamp, {kDoudley, "Thrown back to camp", "SELECT and R together faint the hunter back to camp."}},
    {Id::EnhancedMovement, {kDoudley, "Enhanced movement speed (quest/village)", "Actions play twice as fast and walking is faster."}},
    {Id::EnhancedDashing, {kDoudley, "Enhanced dashing (quest/village)", "Dashes start faster and cover far more ground."}},
    {Id::EnhancedPanicJump, {kDoudley, "Enhanced panic jump", "The panic dive goes farther and higher."}},
    {Id::EnhancedEvade, {kDoudley, "Enhanced evade distances", "Evade+ always: every evade is the skill's longer one."}},
    {Id::EvadeDistanceX2, {kDoudley, "Evade distance x2 (needs Enhanced evade distances)", "Evades go about twice as far."}},
    {Id::EvadeDistanceX10, {kDoudley, "Evade distance x10 (needs Enhanced evade distances)", "Evades go about ten times as far."}},
    {Id::WeaponReach, {kDoudley, "Exceed weapon size on attack", "While the first hunter attacks, hits reach twice as far."}},
    {Id::UnknockablePlayers, {kDoudley, "Unknockable players", "Hits do not knock hunters back or down."}},
    {Id::InvinciblePlayers, {kDoudley, "Invincible players", "Hunters take no damage."}},
    {Id::WalkThroughWalls, {kDoudley, "Walk through walls (on/off)", "The buttons switch it on and off: while on, walls do not stop the hunter.", kActivators}},
    {Id::PassThroughMonsters, {kDoudley, "Pass through monsters (on/off)", "The buttons switch it on and off: while on, monsters do not push the hunter.", kActivators}},
    {Id::ShowMonstersOnMap, {kDoudley, "Show all monsters on the map", "Every monster shows on the map."}},
    {Id::SlowMo, {kDoudley, "Slow-mo mode", "The buttons slow every motion to an eighth; moving the stick ends it.", kActivators}},
    {Id::FreezeMonsters, {kDoudley, "Freeze monsters", "The buttons freeze monsters before their next action.", kActivators}},
    {Id::SwitchAxeGaugeFull, {kDoudley, "Switch axe power gauge always full", "The switch axe's phial gauge stays full."}},
    {Id::AttackUpForever, {kDoudley, "Atk up durations have no limit", "Attack items' boost never runs out."}},
    {Id::DefenseUpForever, {kDoudley, "Def up durations have no limit", "Defense items' boost never runs out."}},
    {Id::SpeedBurstAlways, {kDoudley, "Speed burst always", "Speed burst always on."}},
    {Id::HelmLevel255, {kDoudley, "Equipped helm level 255", "The equipped helm at level 255."}},
    {Id::VestLevel255, {kDoudley, "Equipped vest level 255", "The equipped vest at level 255."}},
    {Id::GlovesLevel255, {kDoudley, "Equipped gloves level 255 (quest only)", "The equipped gloves at level 255."}},
    {Id::BeltLevel255, {kDoudley, "Equipped belt level 255 (quest only)", "The equipped belt at level 255."}},
    {Id::PantsLevel255, {kDoudley, "Equipped pants level 255 (quest only)", "The equipped pants at level 255."}},
    {Id::SkinTone, {kDoudley, "Skin tone change", "Skin tone, darkest to lightest; not saved.", kSkinTones}},
    {Id::DoudleyBothArmorTypes, {kDoudley, "Enable both armor type for all armors", "Every chest, arms, waist and legs armor counts as both types (blademaster and gunner)."}},
    {Id::DoudleyArmorPigment, {kDoudley, "Enable armour pigment for all armors", "Every armor piece can be dyed."}},
    {Id::BarrelRange300, {kDoudley, "Range 3.00 for all bowgun barrels", "Every bowgun barrel has range 3.00."}},
    {Id::BarrelRange, {kDoudley, "Range N.NN for all bowgun barrels", "Every bowgun barrel has the picked range (from the start: the code's buttons are not needed).", kBarrelRanges, {}, 4}},
    {Id::HelmSkills, {kDoudley, "Activate armor skills for all helmet", "Every helm gives 250 points of the picked skill (its number); on before the quest starts.", {}, {1, 127}}},
    {Id::ChestSkills, {kDoudley, "Activate armor skills for all chest", "Every chest armor gives 250 points of the picked skill (its number); on before the quest starts.", {}, {1, 127}}},
    {Id::ArmsSkills, {kDoudley, "Activate armor skills for all arms", "Every arms armor gives 250 points of the picked skill (its number); on before the quest starts.", {}, {1, 127}}},
    {Id::WaistSkills, {kDoudley, "Activate armor skills for all waist", "Every waist armor gives 250 points of the picked skill (its number); on before the quest starts.", {}, {1, 127}}},
    {Id::LegsSkills, {kDoudley, "Activate armor skills for all legs", "Every legs armor gives 250 points of the picked skill (its number); on before the quest starts.", {}, {1, 127}}},
    {Id::ArmorSkillsOff, {kDoudley, "Deactivate armor skills codes (turn off)", "The buttons switch the five codes above off (the armor's own skills back) and on again.", kActivators}},
};

static_assert(std::size(kEntries) == size_t(Id::Count));

constexpr bool InIdOrder() {
    for (size_t i = 0; i < std::size(kEntries); ++i)
        if (size_t(kEntries[i].id) != i)
            return false;
    return true;
}

static_assert(InIdOrder(), "kEntries must follow Cheats::Id");

const std::array<Cheat, size_t(Id::Count)> kList = [] {
    std::array<Cheat, size_t(Id::Count)> list;

    for (size_t i = 0; i < list.size(); ++i)
        list[i] = kEntries[i].cheat;

    return list;
}();

size_t g_selected[size_t(Id::Count)];

// The typed names (the cheats with a textLength).
std::string g_text[size_t(Id::Count)];

bool HasRange(const Cheat& cheat) {
    return cheat.options.empty() && cheat.range.max > cheat.range.min;
}

int32_t ValueOf(size_t index, size_t choice) {
    const Cheat& cheat = kList[index];

    if (!cheat.options.empty())
        return cheat.options[choice].value;

    if (HasRange(cheat))
        return std::min(cheat.range.min + int32_t(choice) * cheat.range.step, cheat.range.max);

    return 0;
}

// The launcher's two toggles also switch their ports' flags.
void Apply(size_t index) {
    if (Id(index) == Id::NoDamage)
        PlayerHealth::SetNoDamage(g_on[index]);

    if (Id(index) == Id::NoStaminaDrain)
        LauncherCheats::SetNoStaminaDrain(g_on[index]);
}

struct Defaults {
    Defaults() {
        for (size_t i = 0; i < kList.size(); ++i) {
            g_selected[i] = kList[i].defaultOption;
            g_value[i] = ValueOf(i, g_selected[i]);
        }
    }
} defaults;

} // namespace

uint32_t MoveWorkCount(Kind kind) {
    const uint32_t table = Memory::Read32(kSystem + 0xA4);

    if (table == 0)
        return 0;

    return Memory::Read16(table + 2 * uint32_t(kind));
}

uint32_t MoveWork(Kind kind, uint32_t index) {
    const uint32_t table = Memory::Read32(kSystem + 0xA4);

    if (table == 0 || index >= Memory::Read16(table + 2 * uint32_t(kind)))
        return 0;

    const uint32_t base = Memory::Read32(table + 0x10 + 4 * uint32_t(kind));

    if (base == 0)
        return 0;

    return base + index * (kind == Kind::Player ? kPlayerSize : kMonsterSize);
}

uint32_t LocalPlayer() {
    // MyPlayerIndex (0x800CF3E8) reads system_w+0x27.
    const uint32_t plw = MoveWork(Kind::Player, Memory::Read8(kSystem + 0x27));

    if (plw == 0 || Memory::Read16(plw) != 0x0101)
        return 0;

    return plw;
}

namespace {
bool g_toggled[size_t(Id::Count)];

// The cheats switched by their buttons.
constexpr Id kToggles[]{Id::ToggleInfiniteHealth, Id::SuperSpeed, Id::FreeFly, Id::ItemsGet10Toggle, Id::FreezeUnfreezeMonsters, Id::AltOneHitKillsToggle, Id::CantGather, Id::CantLeaveArea, Id::WalkThroughWalls, Id::PassThroughMonsters, Id::ArmorSkillsOff};
} // namespace

bool Toggled(Id id) {
    return On(id) && g_toggled[size_t(id)];
}

std::string_view Text(Id id) {
    return g_text[size_t(id)];
}

bool QuestEndPressed() {
    for (const Id id : {Id::ArgonInfiniteTime, Id::SkillerInfiniteTime})
        if (On(id) && HeldAll(uint16_t(Value(id))))
            return true;

    return false;
}

namespace {
constexpr uint32_t kPsw = 0x8065B7D0; // channel 0's pad work
constexpr uint32_t kClassicHeld = 0xD6, kClassicPressed = 0xDA;

// The pressed words of the Wii Remote's, the Classic Controller's, the
// Nunchuk's and the game's key sets (pad_input.cpp).
constexpr uint32_t kPressedWords[]{0xCA + 4, 0xD6 + 4, 0xFC + 4, 0x2C0 + 4};
} // namespace

uint16_t Held() {
    return Memory::Read16(kPsw + kClassicHeld);
}

uint16_t Pressed() {
    return Memory::Read16(kPsw + kClassicPressed);
}

void ConsumePresses() {
    for (const uint32_t offset : kPressedWords)
        Memory::Write16(kPsw + offset, 0);
}

void Step(CpuContext*) {
    static const bool any = [] {
        for (size_t i = 0; i < size_t(Id::Count); ++i)
            if (g_on[i] && Id(i) != Id::NoDamage && Id(i) != Id::NoStaminaDrain)
                return true;
        return false;
    }();

    if (!any)
        return;

    for (const Id id : kToggles)
        if (On(id) && Combo(uint16_t(Value(id))))
            g_toggled[size_t(id)] = !g_toggled[size_t(id)];

    StepSaveData();
    StepEquipment();
    StepItems();
    StepMonsterCodes();
    StepHunterCodes();
    StepPlayer();
    StepMonsters();
    StepMovementCodes();
}

} // namespace Cheats

namespace LauncherCheats {

using namespace Cheats;

std::span<const Cheat> List() {
    return kList;
}

std::span<const char* const> Groups() {
    // The groups that have entries.
    static const std::vector<const char*> groups = [] {
        std::vector<const char*> list;

        for (const char* group : kGroups)
            if (std::any_of(kList.begin(), kList.end(), [&](const Cheat& cheat) { return cheat.group == group; }))
                list.push_back(group);
        return list;
    }();
    return groups;
}

bool Enabled(size_t cheat) {
    return g_on[cheat];
}

void Enable(size_t cheat, bool enabled) {
    g_on[cheat] = enabled;
    Apply(cheat);
}

size_t Choices(size_t cheat) {
    const Cheat& c = kList[cheat];

    if (!c.options.empty())
        return c.options.size();

    if (HasRange(c))
        return size_t((c.range.max - c.range.min + c.range.step - 1) / c.range.step) + 1;

    return 0;
}

size_t Selected(size_t cheat) {
    return g_selected[cheat];
}

void Select(size_t cheat, size_t choice) {
    if (choice >= Choices(cheat))
        return;

    g_selected[cheat] = choice;
    g_value[cheat] = ValueOf(cheat, choice);
}

std::string ValueLabel(size_t cheat) {
    const Cheat& c = kList[cheat];

    if (!c.options.empty())
        return c.options[g_selected[cheat]].label;

    if (HasRange(c))
        return std::to_string(g_value[cheat]);

    if (c.textLength)
        return g_text[cheat].empty() ? "none typed" : g_text[cheat];

    return {};
}

size_t TextLength(size_t cheat) {
    return kList[cheat].textLength;
}

std::string Text(size_t cheat) {
    return g_text[cheat];
}

bool SetText(size_t cheat, std::string_view text) {
    if (text.size() > TextLength(cheat))
        return false;

    for (char c : text)
        if (c < 32 || c > 126)
            return false;

    g_text[cheat] = text;
    return true;
}

size_t EnabledCount() {
    size_t count = 0;

    for (bool on : g_on)
        count += on;

    return count;
}

namespace {
// A name's letters and digits, lower case: "inf-hp" and "InfHP" name "Inf HP".
std::string Key(std::string_view name) {
    std::string key;

    for (char c : name)
        if (std::isalnum(static_cast<unsigned char>(c)))
            key += char(std::tolower(static_cast<unsigned char>(c)));
    return key;
}

// A decimal or 0x-prefixed hexadecimal number, else INT32_MIN.
int32_t Number(std::string_view text) {
    int base = 10;

    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        text.remove_prefix(2);
        base = 16;
    }

    int32_t number = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), number, base);

    if (error != std::errc() || end != text.data() + text.size())
        return INT32_MIN;

    return number;
}
} // namespace

bool EnableByName(std::string_view name, std::string_view value) {
    // "Group:name" picks among cheats of the same name in several lists (the
    // group by the start of its name: "bully:actionrepeater"); a name with a
    // colon of its own ("All hunting fleets: half a day voyages") also
    // matches whole.
    std::string_view group;
    std::string_view shortName = name;

    if (const size_t colon = name.find(':'); colon != std::string_view::npos) {
        group = name.substr(0, colon);
        shortName = name.substr(colon + 1);
    }

    for (size_t i = 0; i < kList.size(); ++i) {
        const bool inGroup = Key(shortName) == Key(kList[i].name) && Key(kList[i].group).starts_with(Key(group));
        const bool whole = Key(name) == Key(kList[i].name);

        if (!inGroup && !whole)
            continue;

        if (TextLength(i)) {
            if (!SetText(i, value))
                return false;
        } else if (!value.empty()) {
            const size_t choices = Choices(i);
            size_t choice = choices;

            for (size_t k = 0; k < choices; ++k) {
                const Cheat& c = kList[i];
                const bool byLabel = !c.options.empty() && value == c.options[k].label;
                const bool byNumber = Number(value) == ValueOf(i, k);

                if (byLabel || byNumber) {
                    choice = k;
                    break;
                }
            }

            if (choice == choices)
                return false;

            Select(i, choice);
        }

        Enable(i, true);
        return true;
    }

    return false;
}

} // namespace LauncherCheats
