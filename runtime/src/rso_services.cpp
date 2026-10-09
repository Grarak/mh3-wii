#include "abi_bridge.h"
#include "generated/rso_binding.h"
#include <iostream>
#include <stdexcept>

extern "C" void func_8081A8A4(CpuContext*);

// The first home-button module has a deterministic allocation in RMHP08.
// Keep the SDK loader/relocator; reject any image our static code does not match.
// Additional modules and unload/reload still need explicit runtime bindings.
extern "C" void MH3_HbmProlog(CpuContext* cpu) {
    if (cpu->gpr[3] != HbmBinding::Base)
        throw std::runtime_error("hbm_data.rso loaded at an uncompiled address");
    const auto* bytes = Memory::GetPointer(HbmBinding::Text, HbmBinding::Size);
    uint64_t checksum = 0xCBF29CE484222325ULL;
    for (uint32_t i = 0; i < HbmBinding::Size; ++i)
        checksum = (checksum ^ bytes[i]) * 0x100000001B3ULL;
    if (checksum != HbmBinding::Checksum)
        throw std::runtime_error("hbm_data.rso linked text differs from static translation");
    RecompMod::RegisterExecutableRange(HbmBinding::Text, HbmBinding::Text + HbmBinding::Size, "hbm_data.rso");
    std::cerr << "MH3 RSO: verified hbm_data.rso static binding\n";
    func_8081A8A4(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x8081A8A4, MH3_HbmProlog, "MH3_HbmProlog");
