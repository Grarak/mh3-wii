#include "abi_bridge.h"
#include "ios_host.h"

extern "C" void func_80528B90(CpuContext*);
// MH3's keyboard setup spins directly on a stack completion flag after this
// SDK call (0x8056C994), with no scheduler/device-service call in the loop.
// Deliver the real queued open/version/enumeration callbacks at this boundary.
// Keep the original HID initialization and its return value intact.
extern "C" void MH3_HIDInitAsync(CpuContext* cpu) {
    func_80528B90(cpu);
    PumpIosCompletions(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x80528B90, MH3_HIDInitAsync, "MH3_HIDInitAsync");
