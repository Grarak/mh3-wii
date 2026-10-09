#include <aurora/legacy_gx.h>
#include <cstdio>
#include <cstdlib>

namespace {
AuroraLegacyGxPanicHandler panicHandler = nullptr;
}

void aurora_set_legacy_gx_panic_handler(AuroraLegacyGxPanicHandler handler) {
    panicHandler = handler;
}

[[noreturn]] void aurora_legacy_gx_panic(const char* entry) {
    if (panicHandler) panicHandler(entry);
    std::fprintf(stderr, "Aurora GX PANIC: %s\n", entry);
    std::fflush(stderr);
    std::abort();
}

