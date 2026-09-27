#include "abi_bridge.h"
#include "generated/rso_binding.h"
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
    for (const auto& module : modules) {
        if (address < module.text || address >= module.text + module.size) continue;
        // The prolog verifies the new image before entering any translated code.
        if (module.active || address == module.prolog) return;
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
    RecompMod::RegisterExecutableRange(text, text + size, name);
    for (auto& module : modules) if (module.base == base) module.active = true;
    std::cerr << "MH3 RSO: verified " << name << " static binding\n";
}

extern "C" void func_804DB614(CpuContext*);
extern "C" void MH3_RSOUnlink(CpuContext* cpu) {
    Module* unloading = nullptr;
    for (auto& module : modules) if (module.base == cpu->gpr[3]) unloading = &module;
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
