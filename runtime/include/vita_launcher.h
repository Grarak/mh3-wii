#pragma once
#include "quest_catalog.h"
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace VitaLauncher {
struct SaveSlot { unsigned file, character; };
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
    uint16_t minutes = 0; // Zero preserves the original time limit.
};
std::vector<uint16_t> QuestCompositionOptions(std::span<const uint8_t> source, uint16_t base);
std::vector<uint16_t> CompositionOptions(const CustomQuest& quest);
std::vector<uint16_t> CompositionMaps(const CustomQuest& quest);
std::vector<uint8_t> EditQuestFile(std::span<const uint8_t> source, const CustomQuest& quest);
void SaveCustomQuest(const std::filesystem::path& user, const CustomQuest& quest);
std::vector<CustomQuest> LoadCustomQuests(const std::filesystem::path& user);
bool SupportsCapture(const CustomQuest& quest);
void ActivateCustomQuest(const CustomQuest& quest);
using Progress = std::function<void(const std::string&, uint64_t, uint64_t)>;
// Converts ISO/RVZ with the native nod decoder, preserving partition padding.
// The input is always read-only. Publication happens only after validation.
void ProcessDisc(const std::filesystem::path& iso, const std::filesystem::path& output,
                 const Progress& progress);
bool DiscReady(const std::filesystem::path& output);
void PrepareTextures(const std::filesystem::path& root, const Progress& progress);
#ifdef __vita__
// Returns false when startup cannot continue or the user exits.
bool Run();
#endif
}
