#include "gx_panic.h"
#include "abi_bridge.h"

#include <cstdio>
#include <cstdlib>
#include <unwind.h>
#ifdef __vita__
#include <psp2/kernel/processmgr.h>
#else
#include <dlfcn.h>
#endif
#ifdef MH3_HAS_AURORA
#include <aurora/legacy_gx.h>
#endif

namespace {
void GuestLocation(const char* label, uint32_t address) {
    std::fprintf(stderr, "  %s 0x%08x", label, address);

    if (const auto info = TranslatedFunctionRegistry::FindNearestByAddress(address); info && address - info->address < 0x10000) {
        std::fprintf(stderr, " %s + 0x%x", info->name, address - info->address);
    }

    std::fputc('\n', stderr);
}

#if !defined(MH3_NO_UNWIND_TABLES) && !defined(__EMSCRIPTEN__)
struct NativeTrace {
    unsigned count = 0;
};

_Unwind_Reason_Code NativeFrame(_Unwind_Context* context, void* value) {
    auto& trace = *static_cast<NativeTrace*>(value);
    const uintptr_t ip = _Unwind_GetIP(context);

    if (!ip || trace.count == 64)
        return _URC_END_OF_STACK;

    std::fprintf(stderr, "  #%u %p", trace.count++, reinterpret_cast<void*>(ip));
#ifndef __vita__
    Dl_info symbol{};

    // Unwind IPs are return addresses. A noreturn stub's return address can
    // equal the next function's start; symbolize the instruction before it.
    if (dladdr(reinterpret_cast<void*>(ip - 1), &symbol) && symbol.dli_sname) {
        std::fprintf(stderr, " %s + 0x%lx", symbol.dli_sname, static_cast<unsigned long>(ip - reinterpret_cast<uintptr_t>(symbol.dli_saddr)));
    }

    if (symbol.dli_fbase)
        std::fprintf(stderr, " (ELF + 0x%lx)", static_cast<unsigned long>(ip - reinterpret_cast<uintptr_t>(symbol.dli_fbase)));
#endif
    std::fputc('\n', stderr);
    return _URC_NO_REASON;
}
#endif
} // namespace

[[noreturn]] void PanicLegacyGx(const char* entry, uint32_t guestAddress, const CpuContext* cpu) noexcept {
    std::fprintf(stderr, "\nMH3 GX PANIC: %s", entry);

    if (guestAddress)
        std::fprintf(stderr, " (guest address 0x%08x)", guestAddress);

    std::fputc('\n', stderr);

    if (!cpu)
        cpu = TryGetCpuContext();

    std::fprintf(stderr, "Guest backtrace (LR and saved caller links):\n");

    // Each frame is read only once Memory::Contains holds: nothing here fails.
    if (cpu) {
        GuestLocation("LR", cpu->lr);
        GuestLocation("PC", cpu->pc);
        std::fprintf(stderr, "  SP 0x%08x, r3..r10", cpu->gpr[1]);

        for (unsigned i = 3; i <= 10; ++i)
            std::fprintf(stderr, " %08x", cpu->gpr[i]);

        std::fputc('\n', stderr);
        uint32_t stack = cpu->gpr[1];

        for (unsigned depth = 0; depth < 48; ++depth) {
            if ((stack & 3) || !Memory::Contains(stack, 4))
                break;

            const uint32_t parent = Memory::Read32(stack);

            if ((parent & 3) || parent <= stack || parent - stack > 0x100000 || !Memory::Contains(parent, 8))
                break;
            // PPC saves a function's incoming LR in its caller's linkage
            // area, above the backchain, rather than at current SP + 4.
            const uint32_t link = Memory::Read32(parent + 4);

            if (!link)
                break;

            GuestLocation("caller", link);
            stack = parent;
        }
    } else {
        std::fprintf(stderr, "  No active guest CPU context\n");
    }

#if !defined(MH3_NO_UNWIND_TABLES) && !defined(__EMSCRIPTEN__)
    std::fprintf(stderr, "Native backtrace (symbolize addresses against this build's ELF):\n");
    NativeTrace trace;
    _Unwind_Backtrace(NativeFrame, &trace);
#endif
    std::fprintf(stderr, "GX calls and FIFO decoding must be replaced with native submissions.\n");
    std::fflush(nullptr);
#ifdef __vita__
    sceKernelExitProcess(1);
#endif
    std::_Exit(EXIT_FAILURE);
}

void InstallLegacyGxPanicHandler() noexcept {
#ifdef MH3_HAS_AURORA
    aurora_set_legacy_gx_panic_handler([](const char* entry) { PanicLegacyGx(entry); });
#endif
}
