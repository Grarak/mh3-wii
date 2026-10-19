#pragma once
#include "quest_catalog.h"
#include "status.h"
#include <array>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace VitaLauncher {
struct SaveSlot {
    unsigned file, character;
};

std::vector<SaveSlot> ProbeSaves(const std::filesystem::path& user);
bool ValidSave(std::span<const uint8_t> bytes, unsigned character);
struct CustomQuest {
    std::string name;
    QuestCatalog::Set set = QuestCatalog::Set::Village;
    uint16_t base = 0;
    uint16_t count = 1;
    bool capture = false;
    uint8_t stars = 1;
    std::string storageKey; // Assigned by the read-only catalog, empty for new quests.
    uint16_t encounter = 0; // Authored monster placement on the same ordinary map.
    uint16_t minutes = 0;   // Zero preserves the original time limit.

    // A quest from scratch (map not 0): its map (the quest map number), one
    // to three large monsters at once (0 for none, after the first), their
    // size in percent and a rank (0 village, 1 low, 2 high) whose quests give
    // the monsters' strength and rewards. set and base name the disc quest
    // whose entry it takes (SetScratchCarrier); count and encounter are
    // unused. Map 0: a quest edited from a template.
    uint8_t map = 0;
    std::array<uint8_t, 3> monsters{};
    uint8_t rank = 0;
    uint8_t size = 100;
};

// Quest editing. A failure (a template missing from the disc, settings the
// template cannot take, a file error) says why.
Status QuestCompositionOptions(std::span<const uint8_t> source, uint16_t base, std::vector<uint16_t>& options);
Status CompositionOptions(const CustomQuest& quest, std::vector<uint16_t>& options);
Status CompositionMaps(const CustomQuest& quest, std::vector<uint16_t>& maps);
Status EditQuestFile(std::span<const uint8_t> source, const CustomQuest& quest, std::vector<uint8_t>& edited);

// Quests from scratch. The maps, and the large monsters a map takes: any but
// Ceadeus and Jhen Mohran, who stay on their own maps (which take nobody
// else), and in the water arena only the swimmers.
std::vector<uint8_t> ScratchMaps();
std::vector<uint8_t> ScratchMonsters(uint8_t map);

// Ceadeus, Alatreon and Jhen Mohran: only alone (their heap slot and effects
// leave no room for another large monster).
bool ScratchAlone(uint8_t monster);

// The monster heap layout the quest needs (the six slots' memory classes;
// its +0x390 then 4) when no disc layout fits: three different monsters at
// once. None for one or two.
std::optional<std::array<int8_t, 6>> ScratchLayout(const CustomQuest& quest);

// Whether the quest's monsters can be captured there: an authored quest
// captures each, on a map with hunt quests.
Status ScratchCapture(const CustomQuest& quest, bool& supported);

// The quest's set and base: the disc quest on its map (by rank where there
// are two) whose entry it is built over.
void SetScratchCarrier(CustomQuest& quest);

// A template quest's settings as a quest from scratch: its map, monster,
// objective, time, stars and name.
Status ScratchFromTemplate(CustomQuest& quest);

// The quest file of quest.set (sets: the disc's three quest files, by set)
// with quest's entry built: its name, goal, monsters (each with the strength,
// placement bytes and rewards of the authored quest nearest its rank and
// stars), objectives and time over the carrier's map, supplies, small
// monsters and fees.
Status ScratchQuestFile(const std::array<std::vector<uint8_t>, 3>& sets, const CustomQuest& quest, std::vector<uint8_t>& edited);

// Quest `id`'s first large monster (and its objectives) replaced
// by `monster`, a monster list id (--quest-monster).
Status ReplaceQuestMonster(QuestCatalog::Set set, uint16_t id, uint8_t monster);
Status ReplaceQuestSmallMonsters(QuestCatalog::Set set, uint16_t id, uint8_t monster);
Status SaveCustomQuest(const std::filesystem::path& user, const CustomQuest& quest);

// The valid custom quests of user, by name; invalid records are skipped.
Status LoadCustomQuests(const std::filesystem::path& user, std::vector<CustomQuest>& quests);
Status SupportsCapture(const CustomQuest& quest, bool& supported);
Status ActivateCustomQuest(const CustomQuest& quest);
using Progress = std::function<void(const std::string&, uint64_t, uint64_t)>;

// Extracts the disc metadata (sys/, ticket, TMD, certificates) of an ISO/RVZ
// and records the image in rom.txt; the game partition is read from the image
// in place, and textures and models are converted when the game loads them.
// The input is always read-only. Publication happens only after validation;
// a failure says why.
Status ProcessDisc(const std::filesystem::path& iso, const std::filesystem::path& output, const Progress& progress);

// The metadata is present and matches partition.bin or the recorded image.
bool DiscReady(const std::filesystem::path& output);

// output's rom.txt names iso (only the record is read, nothing validated).
bool DiscPreparedFrom(const std::filesystem::path& output, const std::filesystem::path& iso);

// Whole-disc conversion (development tools): native-textures.bin and its tag map.
void PrepareTextures(const std::filesystem::path& root, const Progress& progress);

// Whole-disc conversion (development tools): native-models/ packs and index.
bool ModelsReady(const std::filesystem::path& root);
void PrepareModels(const std::filesystem::path& root, const Progress& progress);
#if defined(__vita__) || defined(MH3_DEVELOPMENT) || defined(__EMSCRIPTEN__)
// The launcher's menus (runtime/vita/launcher.cpp); development hosts and the
// web page run them with MH3 --launcher. Returns false when startup cannot
// continue or the user exits.
bool Run();
#endif
} // namespace VitaLauncher
