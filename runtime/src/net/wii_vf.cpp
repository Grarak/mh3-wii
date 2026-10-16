// VF, the SDK's FAT file system library, as DWC starts it around NWC24's
// user ID (RMHP08 0x805219C0..0x805229B0; decompiled with Ghidra, the call
// arguments and store widths checked in the translated C++), ported
// natively: its expanded heap over the caller's buffer with a drive entry
// per 16 KiB (at most 26) and their work blocks, the Shift-JIS code page
// (to and from UCS-2, lead and trail bytes) and the mutex its start and
// finish take. MEM and OS stay guest calls.
#include "abi_bridge.h"
#include "guest_call.h"
#include "memory.h"

namespace {

enum : uint32_t {
    MEMCreateExpHeapEx = 0x804C27A0,
    MEMDestroyExpHeap = 0x804C2850,
    MEMAllocFromExpHeapEx = 0x804C2880,
    MEMFreeToExpHeap = 0x804C2930,
    OSInitMutex = 0x804D2560,
    OSLockMutex = 0x804D25A0,
    OSUnlockMutex = 0x804D2680,
};

// The code page's entry points, installed in its table.
enum : uint32_t {
    kToUnicodeAddress = 0x80521DB0,
    kFromUnicodeAddress = 0x80521F70,
    kCharLengthAddress = 0x805221D0,
    kIsSjisByteAddress = 0x80522210,
    kMaxCharAddress = 0x80522290,
    kNoOpAddress = 0x805222A0,
};

enum : uint32_t {
    kSjisTable = 0x805755D8,   // UCS-2 by (row, trail - 0x40), 0xBD per row
    kVolumes = 0x80769128,     // 0xBCC bytes
    kCodePageWork = 0x80769D40, // 26 x 0x1880 bytes
    kMutex = 0x80791A40,
    kSlots = 0x80791A58,       // a 0xA0-byte block per pool entry
    kNames = 0x80791AC0,       // 31 x 0x12 bytes
    kCache = 0x80791CF0,       // 130 words
    kCodePageState = 0x80797DB0,
    kStarted = 0x80797DB8,
    kMutexReady = 0x80797DBC,
    kCount = 0x80797DC0,
    kCurrent = 0x80797DC4,
    kHeap = 0x80797DC8,
    kSlotsReady = 0x80797DCC,
    kPool = 0x80797DD0,
    kHook = 0x80797DD4,
    kLimitA = 0x80797DD8,
    kLimitB = 0x80797DDC,
};

constexpr uint32_t kCodePage = 0x80769CF8;

// The code page's fields.
enum : uint32_t {
    pFlags = 0x0,
    pWork = 0x8,
    pWork1 = 0xC,
    pWork2 = 0x10,
    pOptions = 0x14,
    pParam = 0x18,
    pError = 0x1C,
    pError2 = 0x20,
    pToUnicode = 0x24,
    pFromUnicode = 0x28,
    pCharLength = 0x2C,
    pIsSjisByte = 0x30,
    pMaxChar = 0x34,
    pNoOp = 0x38,
    pReady = 0x3C,
};

// A pool entry (0x140 bytes): its heap and two blocks from it.
enum : uint32_t {
    eHeap = 0x1C,
    eSize = 0x20,
    eBlockA = 0x24,
    eBlockB = 0x28,
    kEntrySize = 0x140,
};

uint32_t Read(uint32_t address) {
    return Memory::Read32(address);
}

void Write(uint32_t address, uint32_t value) {
    Memory::Write32(address, value);
}

// 0x805219C0: the library's memset.
void Fill(uint32_t to, uint32_t value, uint32_t length) {
    const uint8_t byte = uint8_t(value);

    for (; (to & 3) && length; --length)
        Memory::Write8(to++, byte);

    const uint32_t word = byte * 0x01010101u;

    for (; length >= 4; length -= 4, to += 4)
        Write(to, word);

    for (; length; --length)
        Memory::Write8(to++, byte);
}

uint32_t Pack(uint32_t count, uint32_t width) {
    return count * 0x10000 + width;
}

// The Shift-JIS lead byte's row in the table (-1 for none).
int32_t SjisRow(uint32_t lead) {
    if (((lead + 0x7F) & 0xFF) < 4)
        return int32_t(lead) - 0x81;

    if (((lead + 0x79) & 0xFF) < 0x19)
        return int32_t(lead) - 0x83;

    if (((lead + 0x20) & 0xFF) < 0xB)
        return int32_t(lead) - 0xC3;

    if (((lead + 0x13) & 0xFF) < 2)
        return int32_t(lead) - 0xC5;

    if (((lead + 6) & 0xFF) < 3)
        return int32_t(lead) - 0xD0;

    return -1;
}

// 0x80521DB0: a Shift-JIS character to UCS-2 ('_' when unknown); answers
// (bytes read << 16) | 2.
uint32_t ToUnicode(uint32_t from, uint32_t to) {
    const uint32_t lead = Memory::Read8(from);
    const auto single = [&](uint16_t c) {
        Memory::Write16(to, c);
        return Pack(1, 2);
    };

    if (lead < 0x80)
        return single(uint16_t(lead));

    if (lead >= 0xA1 && lead < 0xE0)
        return single(uint16_t(lead - 0x140));

    const bool doubleByte = (lead >= 0x81 && lead < 0x85) || (lead >= 0x87 && lead < 0xA0) || (lead >= 0xE0 && lead < 0xEB) || lead >= 0xED;

    if (!doubleByte || ((lead + 0x11) & 0xFF) <= 10 || lead == 0xFF)
        return single(0x5F);

    const uint32_t trail = Memory::Read8(from + 1);

    if (trail >= 0xFD)
        return single(0x5F);

    const int32_t row = SjisRow(lead);

    if (row < 0)
        return single(0x5F);

    const uint32_t column = trail - 0x40;

    if (column >= 0xBD)
        return single(0x5F);

    const uint16_t c = Memory::Read16(kSjisTable + column * 2 + uint32_t(row) * 0x17A);

    if (!c)
        return single(0x5F);

    Memory::Write16(to, c);
    return Pack(2, 2);
}

// 0x80521F70: a UCS-2 character to Shift-JIS ('_' when unknown); answers
// (bytes written << 16) | 2.
uint32_t FromUnicode(uint32_t from, uint32_t to) {
    const uint32_t c = Memory::Read16(from);

    if ((c & 0xFF) < 0x80 && (c >> 8) == 0) {
        Memory::Write8(to, uint8_t(c));
        Memory::Write8(to + 1, 0);
        return Pack(1, 2);
    }

    if (c >= 0xFF61 && c <= 0xFF9F) {
        Memory::Write8(to, uint8_t(c + 0x40));
        Memory::Write8(to + 1, 0);
        return Pack(1, 2);
    }

    if (c == 0x5F) {
        Memory::Write8(to, '_');
        return Pack(1, 2);
    }

    uint32_t row = 0, column = 0xBD;

    for (; row < 0x2D; ++row) {
        for (column = 0; column < 0xBD; ++column) {
            if (Memory::Read16(kSjisTable + (row * 0xBD + column) * 2) == c)
                break;
        }

        if (column < 0xBD)
            break;
    }

    if (column == 0xBD) {
        Memory::Write8(to, '_');
        return Pack(1, 2);
    }

    uint32_t lead;

    if (row < 4)
        lead = row + 0x81;
    else if (row - 4 < 0x19)
        lead = row + 0x83;
    else if (row - 0x1D < 0xB)
        lead = row + 0xC3;
    else if (row - 0x28 < 2)
        lead = row + 0xC5;
    else if (row - 0x2A < 3)
        lead = row + 0xD0;
    else {
        Memory::Write8(to, '_');
        return Pack(1, 2);
    }

    Memory::Write8(to, uint8_t(lead));
    Memory::Write8(to + 1, uint8_t(column + 0x40));
    return Pack(2, 2);
}

// 0x80522210: a Shift-JIS lead (kind 1) or trail (kind 2) byte.
bool IsSjisByte(uint32_t value, uint32_t kind) {
    const uint8_t b = uint8_t(value);

    if (kind == 1)
        return (b >= 0x81 && b <= 0x9F) || (b >= 0xE0 && b <= 0xFC);

    if (kind == 2)
        return (b >= 0x40 && b <= 0x7E) || (b >= 0x80 && b <= 0xFC);

    return false;
}

// 0x80521AE0: the volume table cleared.
void ClearVolumes() {
    Fill(kVolumes, 0, 0xBCC);

    for (uint32_t i = 0; i < 26; ++i) {
        Write(0x807692E0 + i * 0x38, 0);
        Write(0x8076988C + i * 0x2C, 0);
    }
}

// 0x80521C20: the Shift-JIS code page with `options` (bit 16 only).
uint32_t SetCodePage(uint32_t options, uint32_t param) {
    if ((options & 0xFFFCFFFF) != 0 || (options & 0x30000) == 0x30000) {
        Write(kCodePage + pError, 10);
        return 10;
    }

    const auto setOption = [&] {
        const uint32_t flags = Read(kCodePage + pOptions);
        Write(kCodePage + pOptions, (options & 0x10000) ? flags | 0x10000 : flags & ~0x10000u);
    };

    setOption();
    Write(kCodePage + pWork1, 0);
    Write(kCodePage + pWork, kCodePageWork);
    Write(kCodePage + pFlags, Read(kCodePage + pFlags) | 1);
    Write(kCodePage + pWork2, 0);
    setOption();
    Write(kCodePage + pError, 0);
    Write(kCodePage + pError2, 0);
    Write(kCodePage + pReady, 1);
    Write(kCodePage + pToUnicode, kToUnicodeAddress);
    Write(kCodePage + pFromUnicode, kFromUnicodeAddress);
    Write(kCodePage + pCharLength, kCharLengthAddress);
    Write(kCodePage + pIsSjisByte, kIsSjisByteAddress);
    Write(kCodePage + pMaxChar, kMaxCharAddress);
    Write(kCodePage + pNoOp, kNoOpAddress);
    Write(kCodePage + pParam, param);

    for (uint32_t i = 0; i < 26; ++i)
        Fill(kCodePageWork + i * 0x1880, 0, 0x1880);

    Write(kCodePageState, 0);
    return 0;
}

// 0x80522480: the heap over `buffer`, the pool (a 0x140-byte entry per 16
// KiB, at most 26) and a 0xA0-byte block per entry (once).
void PoolSetup(CpuContext* cpu, uint32_t buffer, uint32_t size) {
    GuestCall call{cpu};

    if (buffer && size && !Read(kHeap))
        Write(kHeap, call.Call<MEMCreateExpHeapEx>({buffer, size, 0}));

    Write(kCount, size >> 14 < 0x1B ? size >> 14 : 0x1A);
    const uint32_t count = Read(kCount);

    if (!Read(kPool))
        Write(kPool, Read(kHeap) ? call.Call<MEMAllocFromExpHeapEx>({Read(kHeap), count * kEntrySize, 0x20}) : 0);

    const uint32_t pool = count && Read(kPool) ? Read(kPool) : 0;

    for (uint32_t entry = pool; entry != pool + count * kEntrySize; entry += kEntrySize) {
        Fill(entry, 0, kEntrySize);
        Write(entry + eHeap, 0);
        Write(entry + eSize, 0);
        Write(entry + eBlockA, 0);
        Write(entry + eBlockB, 0);
    }

    if (!Read(kSlotsReady)) {
        for (uint32_t i = 0; i < count; ++i)
            Write(kSlots + 4 * i, Read(kHeap) ? call.Call<MEMAllocFromExpHeapEx>({Read(kHeap), 0xA0, 0x20}) : 0);

        Write(kSlotsReady, 1);
    }

    for (uint32_t i = 0; i < 130; ++i)
        Write(kCache + 4 * i, 0);

    Write(kCurrent, 0);
    Write(kLimitB, 2);
    Write(kLimitA, 8);
}

// 0x80522610: frees the entries' blocks and heaps, the pool, the slots and
// the heap.
void PoolTeardown(CpuContext* cpu) {
    GuestCall call{cpu};

    if (Read(kPool)) {
        const uint32_t count = Read(kCount);
        const uint32_t pool = count ? Read(kPool) : 0;

        if (pool) {
            for (uint32_t entry = pool; entry != pool + count * kEntrySize; entry += kEntrySize) {
                const uint32_t heap = Read(entry + eHeap);

                if (Read(entry + eBlockA) && heap)
                    call.Call<MEMFreeToExpHeap>({heap, Read(entry + eBlockA)});

                if (Read(entry + eBlockB) && Read(entry + eHeap))
                    call.Call<MEMFreeToExpHeap>({Read(entry + eHeap), Read(entry + eBlockB)});

                Write(entry + eHeap, 0);
                Write(entry + eSize, 0);
                Write(entry + eBlockA, 0);
                Write(entry + eBlockB, 0);

                if (heap != Read(kHeap) && heap)
                    call.Call<MEMDestroyExpHeap>({heap});
            }
        }

        if (Read(kHeap))
            call.Call<MEMFreeToExpHeap>({Read(kHeap), Read(kPool)});

        Write(kPool, 0);
    }

    const uint32_t count = Read(kCount);

    if (Read(kSlotsReady)) {
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t slot = kSlots + 4 * i;

            if (Read(slot)) {
                if (Read(kHeap))
                    call.Call<MEMFreeToExpHeap>({Read(kHeap), Read(slot)});

                Write(slot, 0);
            }
        }

        Write(kSlotsReady, 0);
    }

    if (Read(kHeap)) {
        call.Call<MEMDestroyExpHeap>({Read(kHeap)});
        Write(kHeap, 0);
    }

    Write(kCount, 0);
}

// 0x805227B0: the name table cleared.
void ClearNames() {
    for (uint32_t i = 0; i < 31; ++i) {
        Memory::Write16(kNames + i * 0x12, 0);
        Memory::Write8(kNames + i * 0x12 + 0x10, 0);
    }
}

// 0x805227A0: swaps the hook; answers the old one.
uint32_t SwapHook(uint32_t hook) {
    const uint32_t old = Read(kHook);
    Write(kHook, hook);
    return old;
}

void LockIfReady(CpuContext* cpu) {
    if (Read(kMutexReady))
        GuestCall{cpu}.Call<OSLockMutex>({kMutex});
}

void UnlockIfReady(CpuContext* cpu) {
    if (Read(kMutexReady))
        GuestCall{cpu}.Call<OSUnlockMutex>({kMutex});
}

// 0x80522320: starts the library over `buffer` (once).
void Start(CpuContext* cpu, uint32_t buffer, uint32_t size) {
    if (!Read(kMutexReady)) {
        GuestCall{cpu}.Call<OSInitMutex>({kMutex});
        Write(kMutexReady, 1);
    }

    LockIfReady(cpu);

    if (!Read(kStarted)) {
        PoolSetup(cpu, buffer, size);
        ClearVolumes();
        SetCodePage(0, 0);
        ClearNames();
        SwapHook(0);
        Write(kStarted, 1);
    }

    UnlockIfReady(cpu);
}

// 0x805223F0.
void Finish(CpuContext* cpu) {
    LockIfReady(cpu);

    if (Read(kStarted)) {
        PoolTeardown(cpu);
        Write(kStarted, 0);
    }

    UnlockIfReady(cpu);
}

// 0x805221D0: the length of the Shift-JIS character at `from`.
int32_t CharLength(CpuContext* cpu, uint32_t from) {
    GuestFrame frame(cpu, 0x20);
    return int16_t(ToUnicode(from, frame[0x10]) >> 16);
}

uint32_t Arg(CpuContext* cpu, int n) {
    return cpu->gpr[n];
}

} // namespace

extern "C" void MH3_VFFill(CpuContext* cpu) {
    Fill(Arg(cpu, 3), Arg(cpu, 4), Arg(cpu, 5));
}
REGISTER_NATIVE_FUNCTION_AS(0x805219C0, MH3_VFFill, "VFi_memset");

extern "C" void MH3_VFPack(CpuContext* cpu) {
    cpu->gpr[3] = Pack(Arg(cpu, 3), Arg(cpu, 4));
}
REGISTER_NATIVE_FUNCTION_AS(0x80521AC0, MH3_VFPack, "VFi_PackResult");

extern "C" void MH3_VFUnpack(CpuContext* cpu) {
    const uint32_t value = Arg(cpu, 3);
    Memory::Write16(Arg(cpu, 4), uint16_t(value >> 16));
    Memory::Write16(Arg(cpu, 5), uint16_t(value));
}
REGISTER_NATIVE_FUNCTION_AS(0x80521AD0, MH3_VFUnpack, "VFi_UnpackResult");

extern "C" void MH3_VFClearVolumes(CpuContext* cpu) {
    ClearVolumes();
    cpu->gpr[3] = 0;
}
REGISTER_NATIVE_FUNCTION_AS(0x80521AE0, MH3_VFClearVolumes, "VFi_InitVolumes");

extern "C" void MH3_VFSetCodePageThunk(CpuContext* cpu) {
    cpu->gpr[3] = SetCodePage(Arg(cpu, 3), Arg(cpu, 4));
}
REGISTER_NATIVE_FUNCTION_AS(0x80521C10, MH3_VFSetCodePageThunk, "VFi_SetCodePageSjis");

extern "C" void MH3_VFSetCodePage(CpuContext* cpu) {
    cpu->gpr[3] = SetCodePage(Arg(cpu, 3), Arg(cpu, 4));
}
REGISTER_NATIVE_FUNCTION_AS(0x80521C20, MH3_VFSetCodePage, "VFi_SetCodePage");

extern "C" void MH3_VFToUnicode(CpuContext* cpu) {
    cpu->gpr[3] = ToUnicode(Arg(cpu, 3), Arg(cpu, 4));
}
REGISTER_NATIVE_FUNCTION_AS(0x80521DB0, MH3_VFToUnicode, "VFi_SjisToUnicode");

extern "C" void MH3_VFFromUnicode(CpuContext* cpu) {
    cpu->gpr[3] = FromUnicode(Arg(cpu, 3), Arg(cpu, 4));
}
REGISTER_NATIVE_FUNCTION_AS(0x80521F70, MH3_VFFromUnicode, "VFi_UnicodeToSjis");

extern "C" void MH3_VFCharLength(CpuContext* cpu) {
    const uint32_t from = Arg(cpu, 3);
    cpu->gpr[3] = uint32_t(CharLength(cpu, from));
}
REGISTER_NATIVE_FUNCTION_AS(0x805221D0, MH3_VFCharLength, "VFi_SjisCharLength");

extern "C" void MH3_VFIsSjisByte(CpuContext* cpu) {
    cpu->gpr[3] = IsSjisByte(Arg(cpu, 3), Arg(cpu, 4)) ? 1 : 0;
}
REGISTER_NATIVE_FUNCTION_AS(0x80522210, MH3_VFIsSjisByte, "VFi_IsSjisByte");

extern "C" void MH3_VFMaxChar(CpuContext* cpu) {
    cpu->gpr[3] = 2;
}
REGISTER_NATIVE_FUNCTION_AS(0x80522290, MH3_VFMaxChar, "VFi_SjisMaxCharBytes");

extern "C" void MH3_VFNoOp(CpuContext* cpu) {
    cpu->gpr[3] = 0;
}
REGISTER_NATIVE_FUNCTION_AS(0x805222A0, MH3_VFNoOp, "VFi_SjisNoOp");

extern "C" void MH3_VFErrorOf(CpuContext* cpu) {
    cpu->gpr[3] = Arg(cpu, 3) ? 0xFFFFFFFF : 0;
}
REGISTER_NATIVE_FUNCTION_AS(0x805222B0, MH3_VFErrorOf, "VFi_ErrorOf");

extern "C" void MH3_VFInitCodePage(CpuContext* cpu) {
    cpu->gpr[3] = SetCodePage(Arg(cpu, 3), Arg(cpu, 4)) ? 0xFFFFFFFF : 0;
}
REGISTER_NATIVE_FUNCTION_AS(0x805222C0, MH3_VFInitCodePage, "VFi_InitCodePage");

extern "C" void MH3_VFResetCodePageState(CpuContext*) {
    Write(kCodePageState, 0);
}
REGISTER_NATIVE_FUNCTION_AS(0x80522300, MH3_VFResetCodePageState, "VFi_ResetCodePageState");

extern "C" void MH3_VFCodePageReady(CpuContext*) {
}
REGISTER_NATIVE_FUNCTION_AS(0x80522310, MH3_VFCodePageReady, "VFi_CodePageReady");

extern "C" void MH3_VFStart(CpuContext* cpu) {
    Start(cpu, Arg(cpu, 3), Arg(cpu, 4));
}
REGISTER_NATIVE_FUNCTION_AS(0x80522320, MH3_VFStart, "VFInitEx");

extern "C" void MH3_VFFinish(CpuContext* cpu) {
    Finish(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x805223F0, MH3_VFFinish, "VFFinish");

extern "C" void MH3_VFIsStarted(CpuContext* cpu) {
    cpu->gpr[3] = Read(kStarted) ? 1 : 0;
}
REGISTER_NATIVE_FUNCTION_AS(0x80522460, MH3_VFIsStarted, "VFIsInitialized");

extern "C" void MH3_VFPoolSetup(CpuContext* cpu) {
    PoolSetup(cpu, Arg(cpu, 3), Arg(cpu, 4));
}
REGISTER_NATIVE_FUNCTION_AS(0x80522480, MH3_VFPoolSetup, "VFi_InitPool");

extern "C" void MH3_VFPoolTeardown(CpuContext* cpu) {
    PoolTeardown(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x80522610, MH3_VFPoolTeardown, "VFi_FinishPool");

extern "C" void MH3_VFSwapHook(CpuContext* cpu) {
    cpu->gpr[3] = SwapHook(Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x805227A0, MH3_VFSwapHook, "VFi_SetHook");

extern "C" void MH3_VFClearNames(CpuContext*) {
    ClearNames();
}
REGISTER_NATIVE_FUNCTION_AS(0x805227B0, MH3_VFClearNames, "VFi_InitNames");

extern "C" void MH3_VFClearCache(CpuContext*) {
    for (uint32_t i = 0; i < 130; ++i)
        Write(kCache + 4 * i, 0);
}
REGISTER_NATIVE_FUNCTION_AS(0x805228C0, MH3_VFClearCache, "VFi_InitCache");

extern "C" void MH3_VFCacheReady(CpuContext*) {
}
REGISTER_NATIVE_FUNCTION_AS(0x805229B0, MH3_VFCacheReady, "VFi_CacheReady");
