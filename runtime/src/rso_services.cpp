#include "abi_bridge.h"
#include "generated/rso_binding.h"
#include <iostream>
#include <stdexcept>
#include <string>

// Keep the original SDK loader and relocator. Only verified boot layouts are
// compiled; alternate addresses and unload/reload still require explicit support.
static void VerifyModule(CpuContext* cpu, const char* name, uint32_t base,
                         uint32_t text, uint32_t size, uint64_t expected) {
    if (cpu->gpr[3] != base)
        throw std::runtime_error(std::string(name) + " loaded at an uncompiled address");
    const auto* bytes = Memory::GetPointer(text, size);
    uint64_t checksum = 0xCBF29CE484222325ULL;
    for (uint32_t i = 0; i < size; ++i)
        checksum = (checksum ^ bytes[i]) * 0x100000001B3ULL;
    if (checksum != expected)
        throw std::runtime_error(std::string(name) + " linked text differs from static translation");
    RecompMod::RegisterExecutableRange(text, text + size, name);
    std::cerr << "MH3 RSO: verified " << name << " static binding\n";
}

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
