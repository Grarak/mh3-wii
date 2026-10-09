// Shared backing and virtual views follow DSVita's src/mmap/vita.rs pattern.
// Requires kubridge >= 0.3.1; aliases do not commit additional physical RAM.
#include "guest_flat_memory.h"
#include <kubridge.h>
#include <psp2/kernel/sysmem.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace GuestFlat {
namespace {
constexpr uint32_t Mem1Size = 24u << 20;
constexpr uint32_t Mem2Size = 64u << 20;
constexpr uint32_t LockedSize = 0x4000;
constexpr uint32_t BackingSize = Mem1Size + Mem2Size + LockedSize;
struct View { uint32_t address, size, offset; };
constexpr std::array<View, 3> Views{{
    {0x80000000u, Mem1Size, 0},
    {0x90000000u, Mem2Size, Mem1Size},
    {0xe0000000u, LockedSize, Mem1Size + Mem2Size},
}};
SceUID backing = -1;
void* backingBase = nullptr;
std::array<SceUID, Views.size()> reservations{-1, -1, -1};

[[noreturn]] void fail(const char* operation, uint32_t address, int result) {
    char message[160];
    std::snprintf(message, sizeof(message), "MH3 kubridge %s guest %08x host %08x failed: %08x",
                  operation, address, unsigned(reinterpret_cast<uintptr_t>(Host(address))), unsigned(result));
    throw std::runtime_error(message);
}
void release() noexcept {
    for (size_t i = 0; i < Views.size(); ++i) {
        if (reservations[i] < 0) continue;
        kuKernelMemDecommit(Host(Views[i].address), Views[i].size);
        sceKernelFreeMemBlock(reservations[i]);
        reservations[i] = -1;
    }
    if (backing >= 0) sceKernelFreeMemBlock(backing);
    backing = -1;
    backingBase = nullptr;
}
}

void Initialize() {
    if (!g_mapped) {
        try {
            backing = sceKernelAllocMemBlock("MH3 shared guest RAM", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,
                                            BackingSize, nullptr);
            if (backing < 0) fail("allocate backing", 0, backing);
            int result = sceKernelGetMemBlockBase(backing, &backingBase);
            if (result < 0) fail("get backing", 0, result);
            for (size_t i = 0; i < Views.size(); ++i) {
                const auto& view = Views[i];
                void* address = Host(view.address);
                reservations[i] = kuKernelMemReserve(&address, view.size, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW);
                if (reservations[i] < 0) fail("reserve", view.address, reservations[i]);
                if (address != Host(view.address)) fail("reserve wrong address", view.address, -1);
                KuKernelMemCommitOpt opt{};
                opt.size = sizeof(opt);
                opt.attr = KU_KERNEL_MEM_COMMIT_ATTR_HAS_BASE;
                opt.baseBlock = backing;
                opt.baseOffset = view.offset;
                result = kuKernelMemCommit(address, view.size, KU_KERNEL_PROT_READ | KU_KERNEL_PROT_WRITE, &opt);
                if (result < 0) fail("map shared backing", view.address, result);
            }
        } catch (...) {
            release();
            throw;
        }
    }
    std::memset(backingBase, 0, BackingSize);
    g_mapped = true;
    std::printf("MH3 Vita: kubridge RAM aliases, physical backing 88 MiB + 16 KiB\n");
}
}
