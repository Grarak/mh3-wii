// --village (development builds): straight into the village with File 1's
// first character, without the title's movies and menus.
//
// The title (demo_data.rso, a task in slot 4) runs a state machine in its work
// (*0x809D3568, +0 u8 state, +1 u8 sub-state): 0 sets up, 1 plays the intro,
// 2..9 the logos and the title screen, 10 reads the save files' headers, 11
// and 12 check and read the chosen file, 13 is the file/character select, 20
// and 21 the mode menu (Village/City). The select screen's confirm loads the
// character (LoadCharacter, 0x80442088, from the save manager's buffer) and
// records the selection (system_w+0x89C file, +0x89E character); choosing
// Village (0x80982E40) sets game mode 2 and play mode 5 and starts the village
// task in the title's slot (StartGameModeTask, 0x800419DC).
//
// The boot waits for the title's intro (state 1), reads File 1 into the save
// manager's buffer from the host (after the game's read request), loads its
// first character and chooses Village as the mode menu does.
#include "village_boot.h"

#include "guest_call.h"
#include "memory.h"
#include "save_data.h"
#include "vita_launcher.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <vector>

namespace {

constexpr uint32_t kSystem = 0x8065AA60;
enum : uint32_t {
    kSceneFlag = 0x20,          // u8
    kGameMode = 0x24,           // u8: 2 village
    kPlayMode = 0x25,           // u8: 5 village
    kModeFlag0 = 0x7CE,         // u8, cleared by the mode choice
    kVillageVariant = 0x7D3,    // u8: 0 the village task (StartGameModeTask)
    kModeFlag1 = 0x866,         // u8, cleared by the mode choice
    kOnlineCapable = 0x86B,     // u8, copied to +0x870 by the mode menu
    kOnlineMenu = 0x870,        // u8
    kSelectedFile = 0x89C,      // u8 [2]
    kSelectedCharacter = 0x89E, // u8 [2]
    kMode = 0x8C2,              // u8: the mode menu's choice, 0 Village
    kSystemSaveBuffer = 0x950,  // u32: the save manager's buffer
};
enum : uint8_t { kGameModeVillage = 2, kPlayModeVillage = 5 };
constexpr uint32_t kTitleWork = 0x809D3568; // u32: the title's work, while demo_data.rso is loaded
constexpr uint8_t kTitleIntro = 1;          // its state (+0) while the intro plays
enum : uint32_t {
    SaveReadRequest = 0x80441AD8,
    LoadCharacter = 0x80442088,
    RunExitHook = 0x800408B8,
    StartGameModeTask = 0x800419DC,
    SceneReset = 0x800CF000,
    SceneExit0 = 0x8004D1E0,
    SceneExit1 = 0x8004D1F4,
};

enum class State { Off, Waiting, Complete };
State state = State::Off;
std::vector<uint8_t> file;
unsigned selectedFile = 0, selectedCharacter = 0;

uint32_t Word(std::span<const uint8_t> data, size_t offset) {
    return (uint32_t(data[offset]) << 24) | (uint32_t(data[offset + 1]) << 16) |
           (uint32_t(data[offset + 2]) << 8) | data[offset + 3];
}
bool HasFirstCharacter(std::span<const uint8_t> data) {
    // As LoadCharacter (0x80442088): in use, and the bytes from +8 sum to +4.
    // The first character's record starts at 0x48.
    return data.size() == SaveData::kFileSize && (Word(data, 0) & 1) && Word(data, SaveData::kCharacters) != 0 &&
           std::accumulate(data.begin() + 8, data.end(), uint32_t(0)) == Word(data, 4);
}
#if defined(MH3_DEVELOPMENT) || defined(__vita__)
bool InMainMemory(uint32_t address) {
    return (address >= 0x80000000u && address < 0x81800000u) || (address >= 0x90000000u && address < 0x94000000u);
}
#endif
}

#if defined(MH3_DEVELOPMENT) || defined(__vita__)
void StartVillageBoot(unsigned fileIndex, unsigned character) {
    if (fileIndex >= 6 || character >= 3) throw std::runtime_error("Invalid save selection");
    const char* override = std::getenv("MH3_USER_DIR");
    const auto path = std::filesystem::path(override ? override : "saves") /
        ("nand/title/00010000/524d4850/data/nocopy/data0" + std::to_string(fileIndex));
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input || input.tellg() != SaveData::kFileSize)
        throw std::runtime_error("Quest launch requires the selected existing save: " + path.string());
    file.resize(SaveData::kFileSize);
    input.seekg(0);
    input.read(reinterpret_cast<char*>(file.data()), file.size());
    if (!input || !VitaLauncher::ValidSave(file, character))
        throw std::runtime_error("The selected save character is invalid; create one through normal startup first");
    state = State::Waiting;
    selectedFile = fileIndex; selectedCharacter = character;
    std::cerr << "MH3 village boot: entering File " << selectedFile+1 << " / character " << selectedCharacter+1 << '\n';
}

void StepVillageBoot(CpuContext* ctx) {
    if (state != State::Waiting || Memory::Read8(kSystem + kGameMode) != 0) return;
    const uint32_t title = Memory::Read32(kTitleWork);
    if (!InMainMemory(title) || Memory::Read8(title) != kTitleIntro) return;
    const uint32_t buffer = Memory::Read32(kSystem + kSystemSaveBuffer);
    if (!buffer) throw std::runtime_error("--village: the game has no save buffer at the title");
    const CpuContext saved = *ctx;
    GuestCall game{ctx};
    // File 1's read, as the title's (state 12): the request, then the file.
    if (!game.Call<SaveReadRequest>({selectedFile, uint32_t(-1)})) throw std::runtime_error("Quest launch: the save read request failed");
    std::copy(file.begin(), file.end(), Memory::GetPointer(buffer, SaveData::kFileSize));
    if (!game.Call<LoadCharacter>({selectedCharacter})) throw std::runtime_error("Quest launch: the selected character did not load");
    Memory::Write8(kSystem + kSelectedFile, selectedFile);
    Memory::Write8(kSystem + kSelectedCharacter, selectedCharacter);
    // The mode menu's Village (0x80982E30).
    Memory::Write8(kSystem + kOnlineMenu, Memory::Read8(kSystem + kOnlineCapable) == 1);
    game.Call<RunExitHook>();
    Memory::Write8(kSystem + kMode, 0);
    Memory::Write8(kSystem + kModeFlag0, 0);
    Memory::Write8(kSystem + kModeFlag1, 0);
    game.Call<SceneReset>();
    Memory::Write8(kSystem + kSceneFlag, 0);
    game.Call<SceneExit0>();
    game.Call<SceneExit1>();
    Memory::Write8(kSystem + kGameMode, kGameModeVillage);
    Memory::Write8(kSystem + kPlayMode, kPlayModeVillage);
    Memory::Write8(kSystem + kVillageVariant, 0);
    game.Call<StartGameModeTask>();
    *ctx = saved;
    state = State::Complete;
    std::cerr << "MH3 village boot: loaded File " << selectedFile+1 << " / character " << selectedCharacter+1 << ", starting the village\n";
}
#endif

void FinishVillageBoot() {
    if (state == State::Off) return;
    state = State::Off;
    std::cerr << "MH3 village boot: village module verified\n";
}

ClassicControllerSample ReadGameController() {
    // Recorded input starts in the village.
    const bool booting = state != State::Off;
    HoldInputPlayback(booting);
    const auto input = ReadHostController();
    return booting ? ClassicControllerSample{true} : input;
}

void RunVillageBootSelfTests() {
    auto check = [](bool valid) { if (!valid) throw std::runtime_error("Village boot regression failed"); };
    std::vector<uint8_t> data(SaveData::kFileSize);
    check(!HasFirstCharacter(data));
    data[3] = 1; data[0x4B] = '1'; data[7] = '1';
    check(HasFirstCharacter(data));
    ++data.back(); check(!HasFirstCharacter(data));
    data.resize(16); check(!HasFirstCharacter(data));
}
