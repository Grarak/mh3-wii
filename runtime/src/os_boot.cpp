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

// __OSInterruptInit (0x804D1390). The guest table, masks, and external
// interrupt-vector binding are observable to the translated OS. Hollywood
// mask-register writes are replaced by host-service scheduling.
extern "C" void MH3_OSInterruptInit(CpuContext*) {
    constexpr uint32_t table = 0x80003040;
    Memory::Write32(0x80797828, table); // r13 - 0x3AB8
    for (uint32_t i = 0; i < 32; ++i)
        Memory::Write32(table + i * 4, 0);
    Memory::Write32(0x800000C4, 0xFFFFFFF0);
    Memory::Write32(0x800000C8, 0);
    Memory::Write32(0x80003010, 0x804D1A70); // exception 4: __OSDispatchInterrupt
}
REGISTER_NATIVE_FUNCTION(0x804D1390, MH3_OSInterruptInit);

// SetInterruptMask receives the remaining hardware groups in r3 and effective
// mask in r4. The translated callers already store the OS's mask in low RAM.
// Host services consult that state; there are no physical masks to program.
// Returning no remaining groups completes the callers' programming loop.
extern "C" void MH3_SetInterruptMask(CpuContext* ctx) {
    ctx->gpr[3] = 0;
}
REGISTER_NATIVE_FUNCTION(0x804D1460, MH3_SetInterruptMask);

// __OSInitMemoryProtection configures MI registers and PPC BAT mappings.
// Host RAM access is already checked by memory.cpp. Retain the guest interrupt
// bindings and shutdown-list registration, which are independently observable.
extern "C" void MH3_OSInitMemoryProtection(CpuContext* ctx) {
    if (Memory::Read32(0x80797830))
        return;
    Memory::Write32(0x800000C4, Memory::Read32(0x800000C4) | 0xF0000000);
    for (uint32_t i = 0; i < 5; ++i)
        Memory::Write32(0x80003040 + i * 4, 0x804D1E30);
    ctx->gpr[3] = 0x8061DCC0;
    InvokeIndirectCpu(0x804D2870, ctx); // OSRegisterShutdownFunction
    Memory::Write32(0x800000C4, Memory::Read32(0x800000C4) & ~0x08000000u);
    Memory::Write32(0x80797830, 1);
}
REGISTER_NATIVE_FUNCTION(0x804D2490, MH3_OSInitMemoryProtection);

// OSGetResetCode: retain the SDK's saved restart code, otherwise report a
// cold host launch instead of reading the physical PI reset-code register.
extern "C" void MH3_OSGetResetCode(CpuContext* ctx) {
    ctx->gpr[3] = Memory::Read32(0x8074F740) ? Memory::Read32(0x8074F744) | 0x80000000u : 0;
}
REGISTER_NATIVE_FUNCTION(0x804D31C0, MH3_OSGetResetCode);
