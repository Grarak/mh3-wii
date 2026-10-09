#pragma once
#include "quest_catalog.h"
#include "status.h"
#include <filesystem>
#include <functional>
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
};

// Quest editing. A failure (a template missing from the disc, settings the
// template cannot take, a file error) says why.
Status QuestCompositionOptions(std::span<const uint8_t> source, uint16_t base, std::vector<uint16_t>& options);
Status CompositionOptions(const CustomQuest& quest, std::vector<uint16_t>& options);
Status CompositionMaps(const CustomQuest& quest, std::vector<uint16_t>& maps);
Status EditQuestFile(std::span<const uint8_t> source, const CustomQuest& quest, std::vector<uint8_t>& edited);

// Development: quest `id`'s first large monster (and its objectives) replaced
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

// Whole-disc conversion (development tools): native-textures.bin and its tag map.
void PrepareTextures(const std::filesystem::path& root, const Progress& progress);

// Whole-disc conversion (development tools): native-models/ packs and index.
bool ModelsReady(const std::filesystem::path& root);
void PrepareModels(const std::filesystem::path& root, const Progress& progress);
#if defined(__vita__) || defined(MH3_DEVELOPMENT)
// The launcher's menus (runtime/vita/launcher.cpp); development hosts run them
// with MH3 --launcher. Returns false when startup cannot continue or the user
// exits.
bool Run();
#endif
} // namespace VitaLauncher
