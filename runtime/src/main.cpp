#include "abi_bridge.h"
#include "generated/RuntimeConfig.h"
#include "disc_image.h"
#include "audio_dma.h"
#include "village_boot.h"
#include "gx_host.h"
#include "field_jump.h"
#include "quest_catalog.h"
#include "player_health.h"
#include <algorithm>
#include <chrono>
#include <thread>
#include <fstream>
#include <filesystem>
#include <cstdio>
#include <cstdlib>
#ifdef __vita__
#include "../vita/platform.h"
#else
#include <execinfo.h>
#endif
#include <iostream>
#include <string_view>

extern "C" void InitializeDataSections();
extern "C" void MH3_OSExceptionInit(CpuContext*);
extern "C" void MH3_OSInterruptInit(CpuContext*);
void RunDeviceSelfTests(CpuContext*);
void RunAudioSelfTests(CpuContext*);
#ifdef MH3_ENABLE_COLLISION_CHECK
int RunCollisionCheck(CpuContext*);
#endif

void SystemBridge::DumpCpuState(std::ostream& out, const CpuContext* cpu) {
    if (!cpu) return;
    out << std::hex << "guest pc=" << cpu->pc << " lr=" << cpu->lr << " ctr=" << cpu->ctr << '\n';
    for (unsigned i = 0; i < 32; ++i) out << "r" << std::dec << i << "=" << std::hex << cpu->gpr[i] << ((i % 4 == 3) ? '\n' : ' ');
    out << std::dec;
}
void SystemBridge::DumpCpuState(const CpuContext* cpu) { DumpCpuState(std::cerr, cpu); }
void DumpHostStackTraceForRuntimeHelper() {
#ifndef __vita__
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
void WriteCrashArtifacts(std::string_view reason, std::string_view details, const uint32_t*) noexcept {
    std::cerr << reason << ": " << details << '\n';
    SystemBridge::DumpCpuState(TryGetCpuContext());
}
[[noreturn]] void FatalMissingGuestTarget(uint32_t target, CpuContext* cpu) noexcept {
    std::cerr << "Missing translated target 0x" << std::hex << target << std::dec << '\n';
    SystemBridge::DumpCpuState(cpu);
    // Optional diagnostic input for offline module translation. Never generated
    // as part of a normal run, and never used as executable host code.
    if (const char* path = std::getenv("MH3_MISSING_TARGET_MEM1_DUMP")) {
        try {
            std::ofstream dump(path, std::ios::binary | std::ios::trunc);
            dump.write(reinterpret_cast<const char*>(Memory::GetPointer(0x80000000, 0x1800000)), 0x1800000);
            if (!dump) throw std::runtime_error("write failed");
            std::cerr << "MEM1 diagnostic snapshot: " << path << '\n';
        } catch (const std::exception& error) {
            std::cerr << "Cannot write MEM1 diagnostic snapshot: " << error.what() << '\n';
        }
    }
    DumpHostStackTraceForRuntimeHelper();
    std::exit(1);
}
}

#ifdef MH3_HAS_AURORA
int RunGraphicsCheck(int argc, char** argv);
int RunEfbCheck(int argc, char** argv);
#endif

constexpr const char* kUsage =
#ifdef MH3_DEVELOPMENT
    "Unknown option. Options:\n"
    "  --village                      start File 1 / character 1 in the village\n"
    "  --list quests|online-quests|arena-quests\n"
    "                                 list a quest set (id, stars, map, name, monsters, goal)\n"
    "  --quest <id>|free [--area <n>] [--spawn <x,y,z>]\n"
    "                                 start a quest (village, arena or online) or the\n"
    "                                 Moga Woods free roam from the village; --spawn\n"
    "                                 moves the hunter there (arena pit: 0,300,0)\n"
    "  --no-damage                    (with any of the above) the hunter takes no damage\n"
#else
    "Unknown option (--village, --quest and --list are in development builds)"
#endif
    ;

#ifdef MH3_DEVELOPMENT
// --quest <id>|free [--area <n>] [--spawn <x,y,z>]. The quest sets' ids don't overlap (village
// 1000.., arena 9000.., online 10000..): the id picks the set.
void StartQuestBoot(int argc, char** argv) {
    using QuestCatalog::Set;
    const std::string_view id = argv[2];
    int area = -1;
    for (int i = 3; i < argc; ++i) {
        const std::string_view option = argv[i];
        float x, y, z;
        if (option == "--area" && i + 1 < argc) area = std::stoi(argv[++i]);
        else if (option == "--spawn" && i + 1 < argc && std::sscanf(argv[++i], "%f,%f,%f", &x, &y, &z) == 3)
            RequestQuestSpawn(x, y, z);
        else throw std::runtime_error(kUsage);
    }
    Set set = Set::Village;
    uint32_t quest = 0;
    if (id != "free") {
        quest = uint32_t(std::stoul(std::string(id)));
        bool found = false;
        for (const Set candidate : {Set::Village, Set::Arena, Set::Online}) {
            const auto quests = QuestCatalog::Load(candidate);
            const auto it = std::find_if(quests.begin(), quests.end(), [&](const auto& q) { return q.id == quest; });
            if (it == quests.end()) continue;
            set = candidate;
            found = true;
            std::cerr << "MH3 quest: " << it->id << " " << it->name << " (" << QuestCatalog::MapName(it->map) << ")\n";
            break;
        }
        if (!found) throw std::runtime_error("No quest " + std::string(id) + "; see --list");
    }
    RequestQuestJump(quest, set, area);
    StartVillageBoot();
}
#endif

int main(int argc, char** argv) {
#ifdef __vita__
    InitializeVitaPlatform(argc, argv);
#endif
    // Ctrl-C ends the process at once (the default action); SDL would turn it
    // into a quit event the game only sees at its next event check.
    setenv("SDL_NO_SIGNAL_HANDLERS", "1", 0);
    try {
#ifdef MH3_DEVELOPMENT
        // --no-damage goes with any other option.
        const auto noDamage = std::remove_if(argv + 1, argv + argc, [](const char* a) { return std::string_view(a) == "--no-damage"; });
        if (noDamage != argv + argc) {
            PlayerHealth::SetNoDamage(true);
            std::cerr << "MH3: no damage\n";
            argc = int(noDamage - argv);
        }
#endif
#ifdef MH3_HAS_AURORA
        if (argc == 2 && std::string_view(argv[1]) == "--efb-check")
            return RunEfbCheck(argc, argv);
        if (argc == 2 && std::string_view(argv[1]) == "--graphics-check")
            return RunGraphicsCheck(argc, argv);
#endif
        if (argc == 6 && std::string_view(argv[1]) == "--disc-read") {
            DiscImage disc(argv[2]);
            const auto size = std::stoull(argv[4], nullptr, 0);
            if (size > 64 * 1024 * 1024) throw std::runtime_error("Disc read too large");
            auto bytes = disc.Read(std::stoull(argv[3], nullptr, 0), static_cast<uint32_t>(size));
            std::ofstream output(argv[5], std::ios::binary);
            output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            if (!output) throw std::runtime_error("Disc read output failed");
            return 0;
        }
#ifdef MH3_DEVELOPMENT
        if (argc == 3 && std::string_view(argv[1]) == "--list") {
            const std::string_view what = argv[2];
            if (what == "quests") QuestCatalog::Print(QuestCatalog::Set::Village);
            else if (what == "online-quests") QuestCatalog::Print(QuestCatalog::Set::Online);
            else if (what == "arena-quests") QuestCatalog::Print(QuestCatalog::Set::Arena);
            else throw std::runtime_error(kUsage);
            return 0;
        }
#endif
        Memory::Init(24 * 1024 * 1024);
        InitializePersistentCpuContext();
        auto& cpu = GetPersistentCpuContext();
        CpuContextScope scope(&cpu);
        if (argc == 2 && std::string_view(argv[1]) == "--self-test") {
            Memory::Write32(0x80001000, 0x12345678);
            if (Memory::Read32(0x1000) != 0x12345678 || Memory::Read16(0xC0001000) != 0x1234)
                throw std::runtime_error("RAM alias or big-endian access failed");
            MemoryInline::FlatWriteRam32(0x80001004, 0xABCDEF01);
            if (MemoryInline::FlatLoad<uint32_t>(0xC0001004) != 0xABCDEF01)
                throw std::runtime_error("Translated stack-store mapping failed");
            bool caught = false;
            try { Memory::Read32(0xCC000000); } catch (const Memory::AccessViolation&) { caught = true; }
            if (!caught) throw std::runtime_error("MMIO must not silently read as RAM");
            MH3_OSExceptionInit(&cpu);
            if (Memory::Read32(0x80797798) != 0x80003000 || Memory::Read32(0x80003038) != 0x804CB930)
                throw std::runtime_error("MH3 exception table initialization failed");
            Memory::Write32(0x800030BC, 0xFFFFFFFF);
            MH3_OSInterruptInit(&cpu);
            if (Memory::Read32(0x80797828) != 0x80003040 || Memory::Read32(0x800030BC) != 0 ||
                Memory::Read32(0x800000C4) != 0xFFFFFFF0 || Memory::Read32(0x80003010) != 0x804D1A70)
                throw std::runtime_error("MH3 interrupt initialization failed");
            RunDeviceSelfTests(&cpu);
            std::cout << "MH3 runtime self-test passed\n";
            return 0;
        }
        InitializeDataSections();
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
#ifdef MH3_DEVELOPMENT
        if (argc == 2 && std::string_view(argv[1]) == "--village") StartVillageBoot();
        else if (argc >= 3 && std::string_view(argv[1]) == "--quest")
            StartQuestBoot(argc, argv);
        else
#endif
        if (argc != 1) throw std::runtime_error(kUsage);
        RecompMod::RegisterExecutableRange(0x80004000, 0x800066E0, ".init");
        RecompMod::RegisterExecutableRange(0x8003F260, 0x8056FAA0, ".text");
        // The apploader normally installs the FST in reserved high MEM1.
        // The SDK's DVDOpen walks this guest table, independently of the host
        // partition reader. Keep it outside both advertised MEM1 arenas.
        const char* discOverride = std::getenv("MH3_DISC_DIR");
        const auto fstPath = std::filesystem::path(discOverride ? discOverride : "Assets/DATA") / "sys/fst.bin";
        std::ifstream fst(fstPath, std::ios::binary | std::ios::ate);
        const auto fstLength = fst ? fst.tellg() : std::streampos(-1);
        if (fstLength < 12 || fstLength > 1024 * 1024) throw std::runtime_error("Invalid or missing boot FST");
        const uint32_t fstSize = static_cast<uint32_t>(fstLength);
        const uint32_t arenaHi = (0x81800000u - fstSize) & ~31u;
        fst.seekg(0);
        fst.read(reinterpret_cast<char*>(Memory::GetPointer(arenaHi, fstSize)), fstSize);
        if (!fst || Memory::Read8(arenaHi) != 1 || Memory::Read32(arenaHi + 8) < 1 ||
            uint64_t(Memory::Read32(arenaHi + 8)) * 12 > fstSize)
            throw std::runtime_error("Invalid boot FST root");
        Memory::Write32(0x80000038, arenaHi);
        Memory::Write32(0x8000003C, fstSize);
        // Standard Wii boot information, with MH3's own arena boundary.
        Memory::Write32(0x80000000, 0x524D4850); // RMHP
        Memory::Write16(0x80000004, 0x3038); // 08
        Memory::Write32(0x80000020, 0x0D15EA5E);
        Memory::Write32(0x80000028, 24 * 1024 * 1024);
        Memory::Write32(0x8000002C, 0x23);
        Memory::Write32(0x80000030, 0x807B1CC0); // __ArenaLo, from OSInit
        Memory::Write32(0x80000034, arenaHi);
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
        Memory::Write32(0x80003110, arenaHi);
        Memory::Write32(0x80003118, 0x04000000);
        Memory::Write32(0x8000311C, 0x04000000);
        Memory::Write32(0x80003120, 0x93600000);
        Memory::Write32(0x80003124, 0x90000800);
        Memory::Write32(0x80003128, 0x935E0000);
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
    } catch (const HostQuit&) {
        // Out at once: the audio and render threads are still running, so no
        // static destructors.
        std::cerr << "MH3: window closed\n";
        std::fflush(nullptr);
        std::_Exit(0);
    } catch (const std::exception& error) {
        std::cerr << "MH3 runtime stopped: " << error.what() << '\n';
        SystemBridge::DumpCpuState(&GetPersistentCpuContext());
        return 1;
    }
}
