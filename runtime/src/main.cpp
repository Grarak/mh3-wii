#include "abi_bridge.h"
#include "fatal.h"
#include <aurora/fatal.hpp>
#include "generated/RuntimeConfig.h"
#include "disc_image.h"
#include "audio_dma.h"
#include "village_boot.h"
#include "gx_host.h"
#include "gx_panic.h"
#include "field_jump.h"
#include "quest_placement.h"
#include "quest_catalog.h"
#include "launcher_cheats.h"
#include "guest_worker.h"
#include "camera_mouse.h"
#include "net/online.h"
#include <algorithm>
#include <chrono>
#include <thread>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <strings.h>
#ifdef __vita__
#include "../vita/platform.h"
#include "vita_launcher.h"
#include <psp2/kernel/clib.h>
#else
#include "vita_launcher.h"
#include "launcher_screen.h"
#ifndef __EMSCRIPTEN__
#include <execinfo.h>
#endif
#endif
#include <iostream>
#include <string_view>
#ifdef MH3_DEVELOPMENT
#include "dev_options.h"
#endif

extern "C" void InitializeDataSections();
extern "C" void MH3_OSExceptionInit(CpuContext*);
extern "C" void MH3_OSInterruptInit(CpuContext*);
void RunDeviceSelfTests(CpuContext*);
void RunAudioSelfTests(CpuContext*);
#ifdef MH3_ENABLE_COLLISION_CHECK
int RunCollisionCheck(CpuContext*);
#endif

void SystemBridge::DumpCpuState(std::ostream& out, const CpuContext* cpu) {
    if (!cpu)
        return;

    out << std::hex << "guest pc=" << cpu->pc << " lr=" << cpu->lr << " ctr=" << cpu->ctr << '\n';

    for (unsigned i = 0; i < 32; ++i)
        out << "r" << std::dec << i << "=" << std::hex << cpu->gpr[i] << ((i % 4 == 3) ? '\n' : ' ');

    out << std::dec;
}

void SystemBridge::DumpCpuState(const CpuContext* cpu) {
    DumpCpuState(std::cerr, cpu);
}

void DumpHostStackTraceForRuntimeHelper() {
#if !defined(__vita__) && !defined(__EMSCRIPTEN__) && !defined(MH3_NO_UNWIND_TABLES)
    void* frames[40];
    int count = backtrace(frames, 40);
    backtrace_symbols_fd(frames, count, 2);
#endif
}

void ShowRuntimeFatalPopup(std::string_view category, std::string_view details) noexcept {
    std::cerr << category << ": " << details << '\n';
}

void MarkFatalErrorReported() {}
void SetRuntimeExitCode(int) {}
namespace RuntimeCrash {
// Every report that stops the game, with the guest CPU state, also goes to
// crash.txt (appended) and, on the Vita, through sceClibPrintf, which the
// console shows in any build: ux0:/data/mh3-wii/crash.txt there, beside the
// saves elsewhere (MH3_USER_DIR, default saves/). Host crashes that report
// nothing (a data abort) leave the system's core dump instead.
void AppendCrashLog(std::string_view what, const CpuContext* cpu) noexcept {
    std::ostringstream report;
    report << "MH3 runtime stopped: " << what << '\n';
    SystemBridge::DumpCpuState(report, cpu);
    const std::string text = report.str();
#ifdef __vita__
    for (size_t at = 0; at < text.size();) {
        const size_t end = std::min(text.find('\n', at), text.size());
        sceClibPrintf("%.*s\n", int(end - at), text.data() + at);
        at = end + 1;
    }

    const std::string path = "ux0:/data/mh3-wii/crash.txt";
#else
    const char* user = std::getenv("MH3_USER_DIR");
    const std::string path = std::string(user ? user : "saves") + "/crash.txt";
#endif
    std::ofstream out(path, std::ios::app);

    if (out)
        out << text << '\n';
}

void WriteCrashArtifacts(std::string_view reason, std::string_view details, const uint32_t*) noexcept {
    std::cerr << reason << ": " << details << '\n';
    SystemBridge::DumpCpuState(TryGetCpuContext());
    AppendCrashLog(std::string(reason) + ": " + std::string(details), TryGetCpuContext());
}

[[noreturn]] void FatalMissingGuestTarget(uint32_t target, CpuContext* cpu) noexcept {
    char what[64];
    std::snprintf(what, sizeof what, "Missing translated target 0x%08x", target);
    std::cerr << what << '\n';
    SystemBridge::DumpCpuState(cpu);
    AppendCrashLog(what, cpu);
    DumpHostStackTraceForRuntimeHelper();
    std::exit(1);
}
} // namespace RuntimeCrash

#ifdef MH3_HAS_AURORA
int RunGraphicsCheck(int argc, char** argv);
int RunEfbCheck(int argc, char** argv);
#endif

constexpr const char* kUsage = "Unknown option. Options:\n"
                               "  --village                      start File 1 / character 1 in the village\n"
                               "  --city                         start File 1 / character 1 in the City (online play)\n"
                               "  --list quests|online-quests|arena-quests\n"
                               "                                 list a quest set (id, stars, map, name, monsters, goal)\n"
                               "  --quest <id>|free|custom:<file> [--area <n>|camp] [--spawn <x,y,z>|exit:<n>[,<m>...]|exit:tour]\n"
                               "                                 start a quest (village, arena or online) or the\n"
                               "                                 Moga Woods free roam from the village, in the area\n"
                               "                                 numbered n on the map; --spawn moves the hunter\n"
                               "                                 there (arena pit: 0,300,0), or into the area's exit\n"
                               "                                 to area n; custom:<file> plays a custom quest\n"
                               "                                 (USER/custom-quests/<name>.mh3quest)\n"
                               "  --area-index <n>               (with --quest) start in the game's area n instead\n"
                               "                                 (its own order, also the areas without a number)\n"
                               "  --quest-monster <name|id>      (with --quest) replace the quest's large monster\n"
                               "                                 (any monster in the arena pit: --quest 9004 --spawn 0,300,0)\n"
                               "  --quest-small <name|id>        (with --quest) replace the quest's small monsters\n"
                               "                                 (a field area full: --quest 1028 --area 1)\n"
                               "  --quest-patch <off>=<hex>[,...] (with --quest) replace bytes of the quest's entry\n"
                               "                                 (hex offset into the entry, then the bytes)\n"
                               "  --no-damage                    (with any of the above) the hunter takes no damage\n"
                               "  --weapon <kind>[:<id>]         (with any of the above) the hunter wears that weapon: great-sword,\n"
                               "                                 sword, hammer, lance, long-sword, switch-axe (id 1 by default) or\n"
                               "                                 bowgun[:<frame>[:<barrel>:<stock>]] (frames 17-19 bring their own)\n"
                               "  --cheat <name>[=<value>]       (with any of the above, repeatable) turn on a cheat of\n"
                               "                                 the launcher's list by its name (\"InfHP\"; with its\n"
                               "                                 group when lists share it, \"Bully:ActionRepeater\"),\n"
                               "                                 with a value by label or number\n"
                               "  --rom <image>                  (with any of the above) play from the European disc\n"
                               "                                 (RVZ, WIA or ISO); remembered in disc/ next to the executable\n";

#ifndef __vita__
// The disc metadata and converted-asset cache live in disc/ next to the
// executable (MH3_DISC_DIR overrides it); the disc image is read in place.
// Every launch checks the image recorded there; --rom records a new one.
void PrepareGameData(const char* rom) {
    if (std::getenv("MH3_DISC_DIR")) {
        if (rom)
            std::cerr << "MH3: MH3_DISC_DIR is set; --rom is ignored\n";

        return;
    }

    const auto data = std::filesystem::read_symlink("/proc/self/exe").parent_path() / "disc";

    if (rom || !VitaLauncher::DiscReady(data)) {
        if (!rom)
            MH3_THROW(std::runtime_error("Game data in " + data.string() + " is missing, or its disc image moved: pass --rom <path to the European Monster Hunter Tri RVZ/WIA/ISO>"));

        std::string phase;
        unsigned shown = 101;
        CheckStatus(VitaLauncher::ProcessDisc(rom, data, [&](const std::string& stage, uint64_t done, uint64_t total) {
            const unsigned percent = total ? unsigned(done * 100 / total) : 100;

            if (stage == phase && percent == shown)
                return;

            phase = stage;
            shown = percent;
            std::cerr << "MH3 prepare: " << stage << " " << percent << "%\n";
        }));
    }

    setenv("MH3_DISC_DIR", data.c_str(), 1);
}
#endif

// --quest <id>|free [--area <n>|camp] [--area-index <n>] [--spawn <x,y,z>|exit:<n>[,<m>...]|exit:tour].
// The quest sets' ids don't overlap (village 1000.., arena 9000.., online
// 10000..): the id picks the set. --area and exit:<n> take the number the
// quest's map shows for the area (QuestCatalog::AreaIndex), --area-index the
// game's index.
void StartQuestBoot(int argc, char** argv) {
    using QuestCatalog::Set;
    const std::string_view id = argv[2];
    int area = -1, number = -1;
    std::vector<int> exitNumbers;
    std::string monster, small, patch;

    for (int i = 3; i < argc; ++i) {
        const std::string_view option = argv[i];
        float x, y, z;

        if (option == "--area" && i + 1 < argc) {
            const std::string_view value = argv[++i];
            number = value == "camp" ? 0 : std::stoi(argv[i]);
        } else if (option == "--area-index" && i + 1 < argc)
            area = std::stoi(argv[++i]);
        else if (option == "--quest-monster" && i + 1 < argc)
            monster = argv[++i];
        else if (option == "--quest-small" && i + 1 < argc)
            small = argv[++i];
        else if (option == "--quest-patch" && i + 1 < argc)
            patch = argv[++i];
        else if (option == "--spawn" && i + 1 < argc && std::string_view(argv[i + 1]) == "exit:tour")
            ++i, RequestQuestTour();
        else if (option == "--spawn" && i + 1 < argc && std::string_view(argv[i + 1]).starts_with("exit:"))
            for (std::string_view numbers = argv[++i] + 5; !numbers.empty();) {
                const size_t comma = std::min(numbers.find(','), numbers.size());
                exitNumbers.push_back(std::stoi(std::string(numbers.substr(0, comma))));
                numbers.remove_prefix(std::min(comma + 1, numbers.size()));
            }
        else if (option == "--spawn" && i + 1 < argc && std::sscanf(argv[++i], "%f,%f,%f", &x, &y, &z) == 3)
            RequestQuestSpawn(x, y, z);
        else
            MH3_THROW(std::runtime_error(kUsage));
    }

    Set set = Set::Village;
    uint32_t quest = 0;
    uint8_t map = 1; // the free roam's

    // custom:PATH: a custom quest's record (USER/custom-quests/NAME.mh3quest).
    if (id.starts_with("custom:")) {
        const std::filesystem::path path(id.substr(7));
        std::vector<VitaLauncher::CustomQuest> custom;
        CheckStatus(VitaLauncher::LoadCustomQuests(path.parent_path().parent_path(), custom));
        const auto it = std::find_if(custom.begin(), custom.end(), [&](const auto& q) { return q.storageKey == path.stem().string(); });

        if (it == custom.end())
            MH3_THROW(std::runtime_error("No valid custom quest " + path.string()));

        CheckStatus(VitaLauncher::ActivateCustomQuest(*it));
        set = it->set;
        quest = it->base;
        map = it->map;

        // A template quest is on its base quest's map.
        for (const auto& q : map ? std::vector<QuestCatalog::Quest>{} : QuestCatalog::Load(set))
            if (q.id == quest)
                map = q.map;

        std::cerr << "MH3 quest: custom " << it->name << " over " << quest << " (" << QuestCatalog::MapName(map) << ")\n";
    } else if (id != "free") {
        quest = uint32_t(std::stoul(std::string(id)));
        bool found = false;

        for (const Set candidate : {Set::Village, Set::Arena, Set::Online}) {
            const auto quests = QuestCatalog::Load(candidate);
            const auto it = std::find_if(quests.begin(), quests.end(), [&](const auto& q) { return q.id == quest; });

            if (it == quests.end())
                continue;

            set = candidate;
            map = it->map;
            found = true;
            std::cerr << "MH3 quest: " << it->id << " " << it->name << " (" << QuestCatalog::MapName(it->map) << ")\n";
            break;
        }

        if (!found)
            MH3_THROW(std::runtime_error("No quest " + std::string(id) + "; see --list"));

        // --quest-monster NAME|ID: the quest's large monster replaced (any
        // monster in the arena pit: --quest 9004 --spawn 0,300,0);
        // --quest-small NAME|ID: its small monsters (a field area full:
        // --quest 1028 --area 1).
        const auto monsterId = [](const std::string& name) {
            const auto names = QuestCatalog::MonsterNames();
            uint32_t chosen = 0;

            for (uint32_t m = 1; m < names.size() && !chosen; ++m)
                if (strcasecmp(names[m].c_str(), name.c_str()) == 0)
                    chosen = m;

            if (!chosen && std::isdigit(static_cast<unsigned char>(name[0])))
                chosen = uint32_t(std::stoul(name));

            if (!chosen || chosen > 255)
                MH3_THROW(std::runtime_error("No monster " + name));

            return uint8_t(chosen);
        };

        if (!monster.empty())
            CheckStatus(VitaLauncher::ReplaceQuestMonster(set, uint16_t(quest), monsterId(monster)));

        if (!small.empty())
            CheckStatus(VitaLauncher::ReplaceQuestSmallMonsters(set, uint16_t(quest), monsterId(small)));

        // --quest-patch OFFSET=HEX,...: bytes of the quest's entry replaced
        // (development: flags, monster slots, objectives).
        if (!patch.empty())
            CheckStatus(VitaLauncher::PatchQuest(set, uint16_t(quest), patch));
    }

    const auto index = [&](int shown) {
        const int i = QuestCatalog::AreaIndex(map, shown);

        if (i < 0)
            MH3_THROW(std::runtime_error(QuestCatalog::MapName(map) + " has no area " + std::to_string(shown) + " on its map (areas: " + QuestCatalog::AreaNumbersText(map) +
                                         "; --area-index takes the game's own index)"));

        return i;
    };

    if (number >= 0) {
        area = index(number);
        std::cerr << "MH3 quest: " << QuestCatalog::AreaName(map, area) << "\n";
    }

    if (!exitNumbers.empty()) {
        std::vector<int> exits;

        for (const int n : exitNumbers)
            exits.push_back(index(n));

        RequestQuestExit(std::move(exits));
    }

    RequestQuestJump(quest, set, area);
    StartVillageBoot();
}

#if defined(__vita__) && defined(MH3_DEVELOPMENT)
void PrepareVitaGameData() {
    const char* dir = std::getenv("MH3_DISC_DIR");

    if (!dir)
        return;

    const std::filesystem::path data = dir;

    if (VitaLauncher::DiscReady(data))
        return;

    std::filesystem::path image;

    for (const auto& entry : std::filesystem::directory_iterator(data.parent_path()))
        if (entry.is_regular_file() && (entry.path().extension() == ".iso" || entry.path().extension() == ".rvz" || entry.path().extension() == ".wia")) {
            if (!image.empty())
                MH3_THROW(std::runtime_error("More than one disc image in " + data.parent_path().string()));

            image = entry.path();
        }
    if (image.empty())
        MH3_THROW(std::runtime_error("No game data in " + data.string() + " and no disc image beside it"));

    CheckStatus(VitaLauncher::ProcessDisc(image, data, [](const std::string& stage, uint64_t, uint64_t) { std::fprintf(stderr, "MH3 prepare: %s\n", stage.c_str()); }));
}
#endif

#ifdef __EMSCRIPTEN__
void MountWebFiles();
#endif
int main(int argc, char** argv) {
#ifdef __EMSCRIPTEN__
    MountWebFiles();
#endif
    // A failure that stops the game reports the guest CPU state.
    fatalReportHook = [](const char* what) {
        SystemBridge::DumpCpuState(&GetPersistentCpuContext());
        RuntimeCrash::AppendCrashLog(what, &GetPersistentCpuContext());
    };

    aurora::fatal_handler = [](const char* what) { MH3Fatal(what); };
    InstallLegacyGxPanicHandler();
#ifdef __vita__
    InitializeVitaPlatform(argc, argv);
#endif
    // Ctrl-C ends the process at once (the default action); SDL would turn it
    // into a quit event the game only sees at its next event check.
    setenv("SDL_NO_SIGNAL_HANDLERS", "1", 0);

    // With exceptions (development builds) a failure on the game thread comes
    // to the report below; without them (release builds) MH3Fatal makes the
    // same report where it happens.
#if defined(__cpp_exceptions)
    try {
#else
    {
#endif
#ifdef __vita__
        if (argc == 1 && !VitaLauncher::Run())
            return 0;
#endif
#if defined(__vita__) && defined(MH3_DEVELOPMENT)
        // Development launches skip the launcher: prepare MH3_DISC_DIR from
        // the one disc image in its parent directory.
        if (argc > 1)
            PrepareVitaGameData();
#endif
#ifndef __vita__
        // --rom <image> goes with any other option.
        const char* rom = nullptr;

        for (int i = 1; i + 1 < argc; ++i)
            if (std::string_view(argv[i]) == "--rom") {
                rom = argv[i + 1];
                std::copy(argv + i + 2, argv + argc, argv + i);
                argc -= 2;
                break;
            }
#endif
        // --no-damage and --cheat go with any other option.
        const auto noDamage = std::remove_if(argv + 1, argv + argc, [](const char* a) { return std::string_view(a) == "--no-damage"; });

        if (noDamage != argv + argc) {
            LauncherCheats::EnableByName("No damage", "");
            std::cerr << "MH3: no damage\n";
            argc = int(noDamage - argv);
        }

        for (int i = 1; i + 1 < argc;) {
            if (std::string_view(argv[i]) != "--cheat") {
                ++i;
                continue;
            }

            const std::string_view cheat = argv[i + 1];
            const size_t equals = cheat.find('=');
            const std::string_view name = cheat.substr(0, equals);
            const std::string_view value = equals == std::string_view::npos ? std::string_view() : cheat.substr(equals + 1);

            if (!LauncherCheats::EnableByName(name, value))
                MH3_THROW(std::runtime_error("Unknown cheat or value: " + std::string(cheat)));

            std::cerr << "MH3: cheat " << cheat << "\n";
            std::copy(argv + i + 2, argv + argc, argv + i);
            argc -= 2;
        }

        // --weapon goes with any other option too.
        for (int i = 1; i + 1 < argc;) {
            if (std::string_view(argv[i]) != "--weapon") {
                ++i;
                continue;
            }

            if (!LauncherCheats::RequestWeapon(argv[i + 1]))
                MH3_THROW(std::runtime_error("Unknown weapon: " + std::string(argv[i + 1])));

            std::cerr << "MH3: weapon " << argv[i + 1] << "\n";
            std::copy(argv + i + 2, argv + argc, argv + i);
            argc -= 2;
        }
#ifdef MH3_HAS_AURORA
        if (argc == 2 && std::string_view(argv[1]) == "--efb-check")
            return RunEfbCheck(argc, argv);

        if (argc == 2 && std::string_view(argv[1]) == "--graphics-check")
            return RunGraphicsCheck(argc, argv);
#if defined(MH3_DEVELOPMENT) && !defined(__vita__)
        // The Vita launcher's renderer use and hand-off to the game, on the host.
        if (argc == 2 && std::string_view(argv[1]) == "--launcher-screen-check")
            return VitaLauncher::RunLauncherScreenCheck();
#endif
#endif
        if (argc == 6 && std::string_view(argv[1]) == "--disc-read") {
            DiscImage disc(argv[2]);
            const auto size = std::stoull(argv[4], nullptr, 0);

            if (size > 64 * 1024 * 1024)
                MH3_THROW(std::runtime_error("Disc read too large"));

            auto bytes = disc.Read(std::stoull(argv[3], nullptr, 0), static_cast<uint32_t>(size));
            std::ofstream output(argv[5], std::ios::binary);
            output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());

            if (!output)
                MH3_THROW(std::runtime_error("Disc read output failed"));

            return 0;
        }
#ifndef __vita__
        PrepareGameData(rom);
#if (defined(MH3_DEVELOPMENT) || defined(__EMSCRIPTEN__)) && defined(MH3_HAS_AURORA)
        // The Vita launcher's menus, then the game as they start it. Its disc
        // directory is MH3_DISC_DIR as on the Vita: prepared game data, or a
        // directory with one disc image (prepared into its disc/).
        if (argc == 2 && std::string_view(argv[1]) == "--launcher") {
            if (!VitaLauncher::Run())
                return 0;

            argc = 1;
        }
#endif
#endif
        if (argc == 3 && std::string_view(argv[1]) == "--list") {
            const std::string_view what = argv[2];

            if (what == "quests")
                QuestCatalog::Print(QuestCatalog::Set::Village);
            else if (what == "online-quests")
                QuestCatalog::Print(QuestCatalog::Set::Online);
            else if (what == "arena-quests")
                QuestCatalog::Print(QuestCatalog::Set::Arena);
            else
                MH3_THROW(std::runtime_error(kUsage));

            return 0;
        }

        Memory::Init(24 * 1024 * 1024);
        InitializePersistentCpuContext();
        auto& cpu = GetPersistentCpuContext();
        CpuContextScope scope(&cpu);

        if (argc == 2 && std::string_view(argv[1]) == "--self-test") {
            Memory::Write32(0x80001000, 0x12345678);

            if (Memory::Read32(0xC0001000) != 0x12345678 || Memory::Read16(0xC0001000) != 0x1234)
                MH3_THROW(std::runtime_error("RAM alias or big-endian access failed"));

            MemoryInline::FlatWriteRam32(0x80001004, 0xABCDEF01);

            if (MemoryInline::FlatLoad<uint32_t>(0xC0001004) != 0xABCDEF01)
                MH3_THROW(std::runtime_error("Translated stack-store mapping failed"));

            // GX MMIO now terminates with a backtrace; an unrelated unclaimed
            // device address still reports an ordinary access violation.
            uint32_t unclaimed;

            if (Memory::TryRead32(0xCC009000, unclaimed))
                MH3_THROW(std::runtime_error("MMIO must not silently read as RAM"));

            MH3_OSExceptionInit(&cpu);

            if (Memory::Read32(0x80797798) != 0x80003000 || Memory::Read32(0x80003038) != 0x804CB930)
                MH3_THROW(std::runtime_error("MH3 exception table initialization failed"));

            Memory::Write32(0x800030BC, 0xFFFFFFFF);
            MH3_OSInterruptInit(&cpu);

            if (Memory::Read32(0x80797828) != 0x80003040 || Memory::Read32(0x800030BC) != 0 || Memory::Read32(0x800000C4) != 0xFFFFFFF0 || Memory::Read32(0x80003010) != 0x804D1A70)
                MH3_THROW(std::runtime_error("MH3 interrupt initialization failed"));

            RunDeviceSelfTests(&cpu);
            std::cout << "MH3 runtime self-test passed\n";
            return 0;
        }

        InitializeDataSections();
        ApplyOnlinePatches();
#ifdef MH3_ENABLE_COLLISION_CHECK
        if (argc == 2 && std::string_view(argv[1]) == "--collision-check")
            return RunCollisionCheck(&cpu);
#endif
#ifdef MH3_HAS_AURORA
        if (argc == 2 && std::string_view(argv[1]) == "--audio-output-check") {
            RunAudioSelfTests(&cpu);

            // Check the real playback device with silence, without requiring
            // game boot or claiming that an in-game mix has been rendered.
            const std::vector<uint8_t> silence(12800, 0);
            QueueHostPcm(silence.data(), silence.size(), 32000);
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            std::cout << "MH3 host audio playback check passed\n";
            return 0;
        }
#endif
        if (argc == 2 && std::string_view(argv[1]) == "--audio-self-test") {
            RunAudioSelfTests(&cpu);
            return 0;
        }

        if (argc == 2 && std::string_view(argv[1]) == "--village")
            StartVillageBoot();
        else if (argc == 2 && std::string_view(argv[1]) == "--city")
            StartVillageBoot(0, 0, true);
        else if (argc >= 3 && std::string_view(argv[1]) == "--quest")
            StartQuestBoot(argc, argv);
        else if (argc != 1)
            MH3_THROW(std::runtime_error(kUsage));

        RecompMod::RegisterExecutableRange(0x80004000, 0x800066E0, ".init");
        RecompMod::RegisterExecutableRange(0x8003F260, 0x8056FAA0, ".text");

        // The apploader normally installs the FST in reserved high MEM1.
        // The SDK's DVDOpen walks this guest table, independently of the host
        // partition reader. Keep it outside both advertised MEM1 arenas.
        const char* discOverride = std::getenv("MH3_DISC_DIR");
        const auto fstPath = std::filesystem::path(discOverride ? discOverride : "Assets/DATA") / "sys/fst.bin";
        std::ifstream fst(fstPath, std::ios::binary | std::ios::ate);
        const auto fstLength = fst ? fst.tellg() : std::streampos(-1);

        if (fstLength < 12 || fstLength > 1024 * 1024)
            MH3_THROW(std::runtime_error("Invalid or missing boot FST"));

        const uint32_t fstSize = static_cast<uint32_t>(fstLength);
        const uint32_t arenaHi = (0x81800000u - fstSize) & ~31u;
        fst.seekg(0);
        fst.read(reinterpret_cast<char*>(Memory::GetPointer(arenaHi, fstSize)), fstSize);

        if (!fst || Memory::Read8(arenaHi) != 1 || Memory::Read32(arenaHi + 8) < 1 || uint64_t(Memory::Read32(arenaHi + 8)) * 12 > fstSize)
            MH3_THROW(std::runtime_error("Invalid boot FST root"));

        Memory::Write32(0x80000038, arenaHi);
        Memory::Write32(0x8000003C, fstSize);

        // The guest worker's stack sits below the FST, the draw thread's below
        // it; the game's arena ends under them.
        const uint32_t workerStack = arenaHi - GuestWorker::kGuestStackBytes;
        GuestWorker::ReserveGuestStack(workerStack);
        const uint32_t gameArenaHi = workerStack - DrawThread::kGuestStackBytes;
        DrawThread::Thread().ReserveGuestStack(gameArenaHi, DrawThread::kGuestStackBytes);

        // Standard Wii boot information, with MH3's own arena boundary.
        Memory::Write32(0x80000000, 0x524D4850); // RMHP
        Memory::Write16(0x80000004, 0x3038);     // 08
        Memory::Write32(0x80000020, 0x0D15EA5E);
        Memory::Write32(0x80000028, 24 * 1024 * 1024);
        Memory::Write32(0x8000002C, 0x23);
        Memory::Write32(0x80000030, 0x807B1CC0); // __ArenaLo, from OSInit
        Memory::Write32(0x80000034, gameArenaHi);

        // TV mode EuRGB60 (PAL console set to 60 Hz). MH3's logic assumes 60 Hz
        // retraces in every region (RenderInit's frame rate is 60.0 / vcnt), and
        // VIInit reports format 5 for this value, so RenderInit picks the 480-line
        // 60 Hz mode instead of the 574-line PAL50 one.
        Memory::Write32(0x800000CC, 5);
        Memory::Write32(0x800000F8, 243000000);
        Memory::Write32(0x800000FC, 729000000);

        // This disc's TMD requests IOS55 (system title 00000001-00000037).
        // IOS metadata and arena limits follow Dolphin's IOS/VersionInfo.cpp.
        // https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/Core/IOS/VersionInfo.cpp
        Memory::Write32(0x800000F0, 0x01800000);
        Memory::Write32(0x80003100, 0x01800000);
        Memory::Write32(0x80003104, 0x01800000);
        Memory::Write32(0x80003108, 0x81800000);
        Memory::Write32(0x8000310C, 0x807B1CC0);
        Memory::Write32(0x80003110, gameArenaHi);
        Memory::Write32(0x80003118, 0x04000000);
        Memory::Write32(0x8000311C, 0x04000000);
        Memory::Write32(0x80003120, 0x93600000);
        Memory::Write32(0x80003124, 0x90000800);

        // The guest worker's and the draw thread's copies of the g3d scratch
        // end the MEM2 arena.
        const uint32_t workerArea = 0x935E0000 - GuestWorker::kWorkAreaBytes;
        GuestWorker::ReserveWorkArea(workerArea);
        const uint32_t drawArea = workerArea - GuestWorker::kWorkAreaBytes;
        DrawThread::Thread().ReserveWorkArea(drawArea);

        // Then the field camera's blended distance row (mouse look), and the
        // spawn lists of monsters a quest puts on maps they have no placement
        // for (custom quests, --quest-monster).
        const uint32_t cameraRow = drawArea - CameraMouse::kRowBytes;
        CameraMouse::ReserveRow(cameraRow);
        const uint32_t mem2ArenaHi = cameraRow - QuestPlacement::kBytes;
        QuestPlacement::Apply(mem2ArenaHi);
        Memory::Write32(0x80003128, mem2ArenaHi);
        Memory::Write32(0x80003130, 0x935E0000);
        Memory::Write32(0x80003134, 0x93600000);
        Memory::Write32(0x80003138, 0x11);
        Memory::Write32(0x8000313C, 0xFF01);
        Memory::Write32(0x80003140, 0x0037161F);
        Memory::Write32(0x80003144, 0x00030110);
        Memory::Write32(0x80003148, 0x93600000);
        Memory::Write32(0x8000314C, 0x93620000);
        Memory::Write32(0x80003180, 0x524D4850);
        Memory::Write32(0x80003188, 0x0037161F);
        Memory::Write32(0x80003194, 0x524D4850);
        cpu.gpr[1] = 0x807AFCB8;
        cpu.msr = 0x8000;
        TranslatedFunctionRegistry::Finalize();
        std::cout << "Starting statically recompiled MH3 at 0x80006310\n";
        InvokeIndirectCpu(0x80006310, &cpu);
        std::cout << "MH3 entry returned\n";
        return 0;
    }
#if defined(__cpp_exceptions)
    catch (const std::exception& error) {
        std::cerr << "MH3 runtime stopped: " << error.what() << '\n';
        SystemBridge::DumpCpuState(&GetPersistentCpuContext());
        RuntimeCrash::AppendCrashLog(error.what(), &GetPersistentCpuContext());
        return 1;
    }
#endif
}
