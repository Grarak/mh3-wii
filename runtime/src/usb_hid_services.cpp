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

extern "C" void func_80528D40(CpuContext*);
// Keyboard teardown has the matching stack-flag wait at 0x8056CB60.
// Shutdown cancels the outstanding enumeration, then its SDK callback closes
// the IOS handle and invokes the caller's completion. Deliver that chain before
// returning to the tight wait; do not fabricate its completion flag.
extern "C" void MH3_HIDCloseAsync(CpuContext* cpu) {
    func_80528D40(cpu);
    PumpIosCompletions(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x80528D40, MH3_HIDCloseAsync, "MH3_HIDCloseAsync");
