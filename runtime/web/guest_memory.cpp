// The web build's guest RAM (guest_flat_memory.h): MEM1, MEM2 and the locked
// cache are fixed ranges of linear memory below the static data, which
// -sGLOBAL_BASE moves past them (runtime/web/web.cmake). Aliases fold onto
// them in Host(); nothing is mapped, and WebAssembly has no faults to report.
#include "guest_flat_memory.h"
#include "fatal.h"

#include <cstdio>
#include <cstring>
#include <stdexcept>

extern "C" char __global_base;

namespace GuestFlat {
namespace {
constexpr uint32_t Mem1Size = 24u << 20;
constexpr uint32_t Mem2Size = 64u << 20;
constexpr uint32_t LockedSize = 0x4000;
} // namespace

void Initialize() {
    // The folded window ends at Host(0x90000000) + 64 MiB.
    if (reinterpret_cast<uintptr_t>(Host(0x90000000u)) + Mem2Size > reinterpret_cast<uintptr_t>(&__global_base))
        MH3_THROW(std::runtime_error("The guest window overlaps the static data; raise -sGLOBAL_BASE"));

    std::memset(Host(0x80000000u), 0, Mem1Size);
    std::memset(Host(0x90000000u), 0, Mem2Size);
    std::memset(Host(0xE0000000u), 0, LockedSize);
    g_mapped = true;
    std::printf("MH3 web: guest RAM folded into linear memory at %p\n", static_cast<void*>(Host(0x80000000u)));
}
} // namespace GuestFlat
