#include "abi_bridge.h"
#include "memory.h"
#include <stdexcept>

// RMHP08 OSExceptionInit, verified from main.dol at 0x804CB5A0.
// The original copies the PPC exception vectors and debugger trampoline into
// low memory. Native execution uses host exceptions instead. Preserve the
// guest handler table consumed by __OSSet/GetExceptionHandler, but do not
// install or execute hardware vector code.
extern "C" void MH3_OSExceptionInit(CpuContext*) {
    constexpr uint32_t table = 0x80003000;
    constexpr uint32_t defaultHandler = 0x804CB930;
    Memory::Write32(0x80797798, table); // r13 - 0x3B48
    for (uint32_t i = 0; i < 15; ++i) {
        Memory::Write32(table + i * 4, defaultHandler);
    }
}
REGISTER_NATIVE_FUNCTION(0x804CB5A0, MH3_OSExceptionInit);

extern "C" void MH3_OSDefaultExceptionHandler(CpuContext*) {
    throw std::runtime_error("MH3 invoked its default PowerPC exception handler");
}
REGISTER_NATIVE_FUNCTION(0x804CB930, MH3_OSDefaultExceptionHandler);
