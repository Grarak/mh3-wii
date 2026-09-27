#include "abi_bridge.h"
#include <iostream>

// WUDInit: the native controller bridge owns discovery rather than the Wii USB
// Bluetooth stack. Preserve the SDK state queried by WPADGetStatus and setters.
// WPAD's original initialization still creates all four channel control blocks.
extern "C" void MH3_WUDInit(CpuContext* cpu) {
    const auto initialized = cpu->gpr[13] - 0x3700;
    if (Memory::Read32(initialized)) { cpu->gpr[3] = 0; return; }
    constexpr uint32_t state = 0x80760F60;
    for (uint32_t offset : {0u, 4u, 8u, 0x6ECu, 0x6F0u})
        Memory::Write32(state + offset, 0);
    Memory::Write8(state + 0x10, 1);
    Memory::Write8(state + 0x708, 3); // WUD library ready, no connected remotes yet
    Memory::Write8(cpu->gpr[13] - 0x36CC, 0);
    Memory::Write32(initialized, 1);
    cpu->gpr[3] = 1;
    std::cerr << "MH3 WPAD: host discovery initialized; no remote connected\n";
}
REGISTER_NATIVE_FUNCTION(0x804FD450, MH3_WUDInit);
