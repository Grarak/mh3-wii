#include "abi_bridge.h"
#include "generated/rso_binding.h"
#include "generated/rso_overlays/map_village.h"
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
struct Module {
    const char* name;
    uint32_t base, text, size, prolog;
    uint64_t checksum;
    bool active = false;
};
Module modules[]{
    {"hbm_data.rso", HbmBinding::Base, HbmBinding::Text, HbmBinding::Size, 0x8081A8A4, HbmBinding::Checksum},
    {"com_data.rso", ComBinding::Base, ComBinding::Text, ComBinding::Size, 0x8086907C, ComBinding::Checksum},
    {"lobby_data.rso", LobbyBinding::Base, LobbyBinding::Text, LobbyBinding::Size, 0x808C7ED8, LobbyBinding::Checksum},
    {"demo_data.rso", DemoBinding::Base, DemoBinding::Text, DemoBinding::Size, 0x80992FC8, DemoBinding::Checksum},
    {"quest_data.rso", QuestBinding::Base, QuestBinding::Text, QuestBinding::Size, 0x809D60C4, QuestBinding::Checksum},
    {"map_village.rso", MapVillageBinding::Base, MapVillageBinding::Text, MapVillageBinding::Size,
     MapVillageBinding::Prolog, MapVillageBinding::Checksum},
};
uint64_t TextChecksum(uint32_t text, uint32_t size) {
    const auto* bytes = Memory::GetPointer(text, size);
    uint64_t checksum = 0xCBF29CE484222325ULL;
    for (uint32_t i = 0; i < size; ++i)
        checksum = (checksum ^ bytes[i]) * 0x100000001B3ULL;
    return checksum;
}
}

void RecompMod::CheckRsoExecution(uint32_t address) {
    for (const auto& module : modules)
        if (module.active && address >= module.text && address < module.text + module.size) return;
    for (const auto& module : modules) {
        if (address < module.text || address >= module.text + module.size) continue;
        // The prolog verifies the new image before entering any translated code.
        if (address == module.prolog) return;
        throw std::runtime_error(std::string("Execution of unlinked ") + module.name +
                                 " at " + std::to_string(address));
    }
}

// Keep the original SDK loader/relocator and verify each lifetime independently.
static void VerifyModule(CpuContext* cpu, const char* name, uint32_t base,
                         uint32_t text, uint32_t size, uint64_t expected) {
    if (cpu->gpr[3] != base)
        throw std::runtime_error(std::string(name) + " loaded at an uncompiled address");
    if (TextChecksum(text, size) != expected)
        throw std::runtime_error(std::string(name) + " linked text differs from static translation");
    Module* profile = nullptr;
    for (auto& module : modules) {
        if (module.base == base && module.text == text && module.size == size && module.checksum == expected)
            profile = &module;
        else if (module.active && text < module.text + module.size && module.text < text + size)
            throw std::runtime_error("Cannot activate overlapping RSO lifetimes");
    }
    if (!profile) throw std::runtime_error("RSO binding has no lifetime profile");
    RecompMod::RegisterExecutableRange(text, text + size, name);
    profile->active = true;
    std::cerr << "MH3 RSO: verified " << name << " static binding\n";
}

bool RecompMod::TryDispatchRsoOverlay(uint32_t address, CpuContext* cpu) {
    using namespace MapVillageBinding;
    if (address < Text || address >= Text + Size) return false;
    auto& overlay = modules[5];
    if (!overlay.active) {
        // Boot's demo image owns this address range until the SDK unlinks it.
        for (const auto& module : modules)
            if (module.active && Text < module.text + module.size && module.text < Text + Size) return false;
        if (address != Prolog || cpu->gpr[3] != Base) return false;
        VerifyModule(cpu, overlay.name, Base, Text, Size, Checksum);
    }
    for (const auto& record : Records)
        if (record.address == address) return TryDispatchRawCpuTarget(&record, cpu);
    // Never fall through to boot translations for an active overlay's text.
    ReportMissingCpuTarget(address, cpu);
}

void RunRsoOverlaySelfTests(CpuContext* cpu) {
    using namespace MapVillageBinding;
    const auto saved = *cpu;
    auto& demo = modules[3];
    auto& overlay = modules[5];
    if (demo.active || overlay.active) throw std::runtime_error("Overlay tests require an unloaded runtime");
    struct Restore {
        CpuContext* cpu; CpuContext saved; Module& demo; Module& overlay;
        ~Restore() { *cpu = saved; demo.active = false; overlay.active = false; }
    } restore{cpu, saved, demo, overlay};
    cpu->gpr[3] = Base;
    demo.active = true;
    if (RecompMod::TryDispatchRsoOverlay(Prolog, cpu))
        throw std::runtime_error("Village overlay intercepted the active demo image");
    demo.active = false;
    bool rejected = false;
    try { RecompMod::TryDispatchRsoOverlay(Prolog, cpu); }
    catch (const std::runtime_error&) { rejected = true; }
    if (!rejected || overlay.active) throw std::runtime_error("Overlay accepted an unverified image");
    // Exercise the generated register-global-object leaf under an active overlay
    // fixture. Its address overlaps demo code; only the overlay entry may run.
    overlay.active = true;
    constexpr uint32_t chain = 0x8097E2E0, node = 0x80006000;
    const uint32_t oldChain = Memory::Read32(chain);
    Memory::Write32(chain, 0x11223344);
    cpu->gpr[3] = 0xA5; cpu->gpr[4] = 0xB6; cpu->gpr[5] = node;
    const bool dispatched = RecompMod::TryDispatchRsoOverlay(Text, cpu);
    const bool correct = Memory::Read32(chain) == node && Memory::Read32(node) == 0x11223344 &&
        Memory::Read32(node + 4) == 0xB6 && Memory::Read32(node + 8) == 0xA5;
    Memory::Write32(chain, oldChain);
    if (!dispatched || !correct) throw std::runtime_error("Overlay dispatch selected the wrong compiled image");
}

extern "C" void func_804DB614(CpuContext*);
extern "C" void MH3_RSOUnlink(CpuContext* cpu) {
    Module* unloading = nullptr;
    for (auto& module : modules) if (module.active && module.base == cpu->gpr[3]) unloading = &module;
    // The SDK returns false for a module absent from its list. Preserve that
    // behavior without changing the verified lifetime in the host registry.
    if (unloading && (Memory::Read32(unloading->base) || Memory::Read32(unloading->base + 4))) {
        if (!unloading->active || TextChecksum(unloading->text, unloading->size) != unloading->checksum)
            throw std::runtime_error(std::string("Cannot unlink unverified ") + unloading->name);
        unloading->active = false;
        RecompMod::UnregisterExecutableRange(unloading->text, unloading->text + unloading->size);
    } else unloading = nullptr;
    func_804DB614(cpu);
    if (unloading) {
        if (cpu->gpr[3] != 1) throw std::runtime_error("RSO unlink did not complete");
        std::cerr << "MH3 RSO: unlinked " << unloading->name << " static binding\n";
    }
}
REGISTER_NATIVE_FUNCTION_AS(0x804DB614, MH3_RSOUnlink, "MH3_RSOUnlink");

extern "C" void func_8081A8A4(CpuContext*);
extern "C" void MH3_HbmProlog(CpuContext* cpu) {
    using namespace HbmBinding;
    VerifyModule(cpu, "hbm_data.rso", Base, Text, Size, Checksum);
    func_8081A8A4(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x8081A8A4, MH3_HbmProlog, "MH3_HbmProlog");

extern "C" void func_8086907C(CpuContext*);
extern "C" void MH3_ComProlog(CpuContext* cpu) {
    using namespace ComBinding;
    VerifyModule(cpu, "com_data.rso", Base, Text, Size, Checksum);
    func_8086907C(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x8086907C, MH3_ComProlog, "MH3_ComProlog");

extern "C" void func_808C7ED8(CpuContext*);
extern "C" void MH3_LobbyProlog(CpuContext* cpu) {
    using namespace LobbyBinding;
    VerifyModule(cpu, "lobby_data.rso", Base, Text, Size, Checksum);
    func_808C7ED8(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x808C7ED8, MH3_LobbyProlog, "MH3_LobbyProlog");

extern "C" void func_80992FC8(CpuContext*);
extern "C" void MH3_DemoProlog(CpuContext* cpu) {
    using namespace DemoBinding;
    VerifyModule(cpu, "demo_data.rso", Base, Text, Size, Checksum);
    func_80992FC8(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x80992FC8, MH3_DemoProlog, "MH3_DemoProlog");

extern "C" void func_809D60C4(CpuContext*);
extern "C" void MH3_QuestProlog(CpuContext* cpu) {
    using namespace QuestBinding;
    VerifyModule(cpu, "quest_data.rso", Base, Text, Size, Checksum);
    func_809D60C4(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x809D60C4, MH3_QuestProlog, "MH3_QuestProlog");
