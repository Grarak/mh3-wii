// Draw buckets, RMHP08: the two 0x4880-byte bucket arrays the task pass
// (0x800416F8) clears before the task slots queue what they draw. Their
// pointers are at 0x80796D30 and 0x80796D34; both are carved from the buffer
// at *0x8065B3A0. C ports decompiled with Ghidra (Mh3Decompile).
#include "abi_bridge.h"
#include "memory.h"
#include "port_check.h"

#include <cstdint>
#include <cstring>

// A guest memset with the fill producer's resource retirement
// (native_resource_lifetime.cpp).
void FillNativeGuestMemory(uint32_t destination, uint8_t value, uint32_t bytes);

namespace {
constexpr uint32_t kBuckets = 0x80796D30, kBuffer = 0x8065B3A0;
constexpr uint32_t kBucketBytes = 0x4880;

// The first 32 halfwords of each array are list heads: 0xFFFF is empty.
constexpr uint32_t kHeadBytes = 32 * 2;

// 0x80048890 ClearDrawBuckets: zero both arrays, then empty their heads.
void ClearDrawBuckets(CpuContext*) {
    for (uint32_t i = 0; i < 2; ++i) {
        const uint32_t buckets = Memory::Read32(kBuckets + 4 * i);
        FillNativeGuestMemory(buckets, 0, kBucketBytes);

        // The original's halfword stores (no producer retirement; the fill
        // above already retired the range).
        std::memset(Memory::GetPointer(buckets, kHeadBytes), 0xFF, kHeadBytes);
    }
}

// 0x80048834 InitDrawBuckets: point both arrays into the buffer, zeroed.
void InitDrawBuckets(CpuContext*) {
    const uint32_t buffer = Memory::Read32(kBuffer);
    Memory::Write32(kBuckets, buffer);
    Memory::Write32(kBuckets + 4, buffer + kBucketBytes);
    FillNativeGuestMemory(buffer, 0, kBucketBytes);
    FillNativeGuestMemory(buffer + kBucketBytes, 0, kBucketBytes);
}
} // namespace

extern "C" void func_80048890(CpuContext* ctx);
extern "C" void MH3_ClearDrawBuckets(CpuContext* ctx) {
#ifdef MH3_DEVELOPMENT
    PortCheck::Call(ctx, "ClearDrawBuckets", func_80048890, ClearDrawBuckets);
#else
    ClearDrawBuckets(ctx);
#endif
}
REGISTER_NATIVE_FUNCTION_AS(0x80048890, MH3_ClearDrawBuckets, "ClearDrawBuckets");

extern "C" void func_80048834(CpuContext* ctx);
extern "C" void MH3_InitDrawBuckets(CpuContext* ctx) {
#ifdef MH3_DEVELOPMENT
    PortCheck::Call(ctx, "InitDrawBuckets", func_80048834, InitDrawBuckets);
#else
    InitDrawBuckets(ctx);
#endif
}
REGISTER_NATIVE_FUNCTION_AS(0x80048834, MH3_InitDrawBuckets, "InitDrawBuckets");
