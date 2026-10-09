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

constexpr Option kPlayTimes[]{
    {"0:00", 0},
    {"10:00", 10 * 3600},
    {"100:00", 100 * 3600},
    {"127:25", 0x6FFFF},
    {"500:00", 500 * 3600},
    {"9999:59", 35999999},
};

constexpr Option kShots[]{
    {"2", 2}, {"5", 5}, {"10", 10}, {"20", 20}, {"50", 50}, {"99", 99},
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
    {Id::MapOnHud, {kStuff, "Map attached to HUD", "The map option follows the HUD's state (Gecko: map option 0, 1 in HUD state 0x3C04)."}},
    {Id::MaxHealMonsters, {kStuff, "Max heal all monsters", "The buttons heal every monster to its full health.", kActivators}},
    {Id::MonsterActionRepeater, {kStuff, "Action repeater for targeted monster", "The buttons restart the action of the monster nearest the hunter.", kActivators}},

    {Id::VillageSpeedHack, {kSeth, "Speed hack in village", "Hold ZL in the village to move very fast."}},
    {Id::VillageSnailSpeed, {kSeth, "Snail speed (village only)", "Hold ZL in the village to move very slowly."}},
    {Id::SethInfiniteHealth, {kSeth, "Infinite health (online code)", "Health and maximum stay at 100 (the online code, on the local hunter)."}},
    {Id::SethUltraSharp, {kSeth, "Weapons ultra sharp (online code)", "Purple sharpness that never wears (the online code, on the local hunter)."}},
    {Id::SethSpeedGlide, {kSeth, "Speed glide/skate (online code)", "Hold ZL while running to glide very fast (the online code, on the local hunter)."}},
    {Id::SethOxygen, {kSeth, "Increase oxygen bar (online code)", "Oxygen gauge of 255 (the online code, on the local hunter)."}},

    {Id::MorningOnLoad, {kSkiller, "Change to morning (upon loading)", "The village and Moga Woods load in the morning."}},
    {Id::EveningOnLoad, {kSkiller, "Change to evening (upon loading)", "The village and Moga Woods load in the evening."}},
    {Id::ResourcesZero, {kSkiller, "Res 0", "Resources (Moga Woods) stay at 0."}},
    {Id::MaxMoney, {kSkiller, "Max/inf money", "Money stays at 9,999,999z."}},
    {Id::SexChange, {kSkiller, "Sex change", "The character's sex (0 male, 1 female); loads with the character.", {}, {0, 1}}},
    {Id::InfiniteHp, {kSkiller, "Inf HP", "Health and recoverable health stay full."}},
    {Id::InfiniteStamina, {kSkiller, "Inf stamina", "Stamina stays full."}},
    {Id::InfiniteAir, {kSkiller, "Inf air", "The oxygen gauge stays full."}},
    {Id::DefenseUpgrade, {kSkiller, "Defense upgrade (255)", "Defense items' boost at its highest."}},
    {Id::DefenseMax, {kSkiller, "Defense max (255)", "Defense 255."}},
    {Id::AttackUpgrade, {kSkiller, "Attack upgrade (255)", "Attack items' boost at its highest."}},
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
    {Id::PuddyFace, {kSkiller, "Puddy face", "Face; loads with the character.", {}, {0, 15}}},
    {Id::HairStyle, {kSkiller, "Hair dresser", "Hair style; loads with the character.", {}, {0, 15}}},
    {Id::HairRed, {kSkiller, "Hair dye (red)", "Hair colour, red.", {}, {0, 255, 15}}},
    {Id::HairGreen, {kSkiller, "Hair dye (green)", "Hair colour, green.", {}, {0, 255, 15}}},
    {Id::HairBlue, {kSkiller, "Hair dye (blue)", "Hair colour, blue.", {}, {0, 255, 15}}},
    {Id::Clothing, {kSkiller, "Clothing change", "Inner clothing; loads with the character.", {}, {0, 15}}},
    {Id::Voice, {kSkiller, "Voice change", "Voice.", {}, {0, 15}}},
    {Id::EyeColor, {kSkiller, "Eye color change", "Eye colour; loads with the character.", {}, {0, 15}}},
    {Id::Features, {kSkiller, "Features", "Facial features; loads with the character.", {}, {0, 15}}},
    {Id::ClothingRed, {kSkiller, "Color changing clothing (red)", "Clothing colour, red.", {}, {0, 255, 15}}},
    {Id::ClothingGreen, {kSkiller, "Color changing clothing (green)", "Clothing colour, green.", {}, {0, 255, 15}}},
    {Id::ClothingBlue, {kSkiller, "Color changing clothing (blue)", "Clothing colour, blue.", {}, {0, 255, 15}}},

    {Id::SetPlayTime, {kArgon, "Set playtime", "The play time stays at this value.", kPlayTimes}},
    {Id::UnlimitedBarrelBombs, {kArgon, "Set unlimited barrel bombs", "Barrel bombs on the field never count against the limit."}},
    {Id::UnlimitedColdDrink, {kArgon, "Unlimited cold drink", "A cold drink's effect never ends."}},
    {Id::UnlimitedHotDrink, {kArgon, "Unlimited hot drink", "A hot drink's effect never ends."}},
    {Id::LoadShots, {kArgon, "Load XX shots", "The bowgun's clip holds this many shots.", kShots, {}, 2}},
    {Id::UnlimitedTraps, {kArgon, "Set unlimited traps", "Traps on the field never count against the limit."}},

    {Id::FaceForwardStick, {kDoudley, "Always face forward when using R stick", "The hunter turns with the camera while the right stick moves."}},
    {Id::FaceForwardDpad, {kDoudley, "Always face forward when using left/right d-pad", "The hunter turns with the camera while d-pad left or right is held."}},
    {Id::AntiGravity, {kDoudley, "Enable anti-gravity", "The hunter floats; ZL pushes up, ZR pulls down."}},
    {Id::RunInMidAir, {kDoudley, "Run and dash in mid air", "Hold L in the air to run, with R to dash."}},
    {Id::MidAirDpad, {kDoudley, "Mid air d-pad version", "With Run and dash in mid air: d-pad up runs upwards, down downwards."}},
    {Id::MidAirStick, {kDoudley, "Mid air R stick version", "With Run and dash in mid air: the right stick up runs upwards, down downwards."}},
    {Id::FasterWeaponUsage, {kDoudley, "Faster weapon usage", "Weapon actions play three times as fast."}},
    {Id::SnsInfiniteSlashes, {kDoudley, "Sword and shield inf slashes", "With the buttons held, X slashes again and again.", kActivatorsOrAlways}},
    {Id::LanceChargeSpeed, {kDoudley, "Lance charge speed increased", "Hold up while a lance charges to charge faster, from then on."}},
    {Id::LanceChargeController, {kDoudley, "Lance charge speed controller", "While charging: stick up goes faster, R faster still, back aims upward."}},
    {Id::Battojutsu, {kDoudley, "Battojutsu mode (long sword quick draw)", "With the buttons held, A draws the long sword into a slash.", kActivatorsOrAlways}},
    {Id::SwitchAxeSwordMode, {kDoudley, "Force switch axe sword mode", "With the buttons held, R morphs into sword mode at once.", kActivatorsOrAlways}},
    {Id::AirAttackGreatSword, {kDoudley, "Air attack (great sword)", "Hold X in the air, then A or A+R to attack."}},
    {Id::AirAttackSwordAndShield, {kDoudley, "Air attack (sword and shield)", "Hold X in the air, then A or A+R to attack."}},
    {Id::AirAttackHammer, {kDoudley, "Air attack (hammer)", "Hold X in the air, then A or A+R to attack."}},
    {Id::AirAttackLance, {kDoudley, "Air attack (lance)", "Hold X in the air to charge; R changes the direction."}},
    {Id::AirAttackLongSword, {kDoudley, "Air attack (long sword)", "Hold X in the air, then A or A+R to attack."}},
    {Id::AirAttackSwitchAxe, {kDoudley, "Air attack (switch axe)", "Hold X in the air, then A or A+R to attack."}},
    {Id::AirAttackBowgun, {kDoudley, "Air attack (bowgun)", "Hold X in the air, then A to shoot."}},
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
    {Id::ShowMonstersOnMap, {kDoudley, "Show all monsters on the map", "Every monster shows on the map."}},
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
    {Id::SkinTone, {kDoudley, "Skin tone change", "Skin tone, darkest to lightest; not saved.", {}, {0, 0x264, 20}}},
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
constexpr uint32_t kPsw = 0x8065B7D0; // channel 0's pad work
constexpr uint32_t kClassicHeld = 0xD6, kClassicPressed = 0xDA;
} // namespace

uint16_t Held() {
    return Memory::Read16(kPsw + kClassicHeld);
}

uint16_t Pressed() {
    return Memory::Read16(kPsw + kClassicPressed);
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

    StepSaveData();
    StepPlayer();
    StepMonsters();
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

    return {};
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
    for (size_t i = 0; i < kList.size(); ++i) {
        if (Key(name) != Key(kList[i].name))
            continue;

        if (!value.empty()) {
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
