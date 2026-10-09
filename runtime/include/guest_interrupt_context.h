#pragma once

#include "abi_bridge.h"
#include "ppc_runtime.h"

// Deferred HLE dispatch (VRetrace/alarm callbacks) runs mid-function on an arbitrary translated
// caller; using its live CpuContext would let callback exit state clobber resident locals (once
// wild-read a resident r29 via postVRetrace). This models hardware save/restore: callbacks run on
// a private copy seeded from the interrupted r1/r2/r13/FP mode, made ambient via CpuContextScope.
class GuestInterruptCallbackContext {
public:
    GuestInterruptCallbackContext()
        : registers_(InterruptedRegisters()), scope_(&registers_) {}

    GuestInterruptCallbackContext(const GuestInterruptCallbackContext&) = delete;
    GuestInterruptCallbackContext& operator=(const GuestInterruptCallbackContext&) = delete;

    CpuContext* get() noexcept { return &registers_; }

private:
    static CpuContext InterruptedRegisters() {
        // The ambient context is the interrupted guest thread's file. Without
        // one (host frame loop between fibers) the persistent context is the
        // only meaningful seed for r1/r2/r13.
        const CpuContext* interrupted = TryGetCpuContext();
        return interrupted != nullptr ? *interrupted : GetPersistentCpuContext();
    }

    CpuContext registers_;
    CpuContextScope scope_;
};

