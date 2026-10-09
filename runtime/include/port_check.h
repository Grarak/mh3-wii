// MH3_PORT_CHECK=Name,Name... or all (development builds): checks native ports
// against the translated originals they replace. From one RAM and register
// snapshot the original runs, then (restored) the port; every word of
// MEM1/MEM2 that ends up different is reported, once per distinct first
// difference, except the guest stack below the caller's stack pointer (the two
// use different frames) and the LR save word. Only at the stock step
// (GameStep::Ticks() == 1), where a port must match the original. Host-side
// effects of the extra pass (audio, GX) are not undone.
#pragma once

#include "abi_bridge.h"

namespace PortCheck {
bool Enabled(const char* name);
// Runs original(ctx) and port(ctx) from the same state, reports differences,
// and leaves the port's result in place.
void Run(CpuContext* ctx, const char* name, void (*original)(CpuContext*), void (*port)(CpuContext*));

// The port, checked against the original when MH3_PORT_CHECK names it.
inline void Call(CpuContext* ctx, const char* name, void (*original)(CpuContext*), void (*port)(CpuContext*)) {
    if (Enabled(name)) Run(ctx, name, original, port);
    else port(ctx);
}
} // namespace PortCheck
