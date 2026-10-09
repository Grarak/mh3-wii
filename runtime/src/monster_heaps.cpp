// The field's monster memory, RMHP08. C reconstructions from the translated
// code. A monster's files (its model, motions and effects) load into one of
// six heap slots of four memory classes (sizes at 0x805A23E8: 0xDC000,
// 0x108000, 0x358000, 0x400000). A monster id's class is a byte of
// 0x805A22C8 (-1 none; 0 the small monsters, 1 Great Jaggi and Great Baggi,
// 2 the other large monsters, 3 Ceadeus, Alatreon and Jhen Mohran); the
// quest's +0x390 picks the field's layout of slot classes (0x805A23F8, six
// bytes each, set up by 0x80146604), and a monster whose class no free slot
// takes never loads. The state at 0x806A6A10:
//
//   +0      six load entries of 24 bytes: u8 index +0, u8 in use +1, u8
//           monster +2, u8 +3
//   +0xDB0  the six heap slots of 20 bytes: u8 in use +0, u8 class +1, u8
//           monster +2, u32 base +4, u32 size +8
//   +0xF30  per class (4 bytes each) the free slots of the class, one count
//           per hunter
//
//   0x80141434 LoadEntryWith(used): the first load entry whose +1 is `used`
//   0x80141538 LoadEntryOf(monster): the index of the entry loading it, 255
//   0x801414D4 TakeLoadEntry(monster): a free entry taken, its +0; 255
//   0x80146BFC CountSlotTaken(class): each hunter's free count of the class
//              one lower (not below 0)
//   0x80146A88 SlotClassFree(class): 1 when every hunter (move work 2, 0xB20
//              bytes each) present and not 0x8012D20C's case has a free slot
//              of the class (in mode 2, the first count alone)
//   0x80146CC8 FindHeapSlot(monster): a free slot of the monster's class, else
//              one class up, else (but from class 1) two up; 0 for none. The
//              first free slot of a class decides: a class above 0 taken
//              while SlotClassFree is not 1 gives 0
//   0x80146E58 TakeHeapSlot(monster): FindHeapSlot's slot marked in use by
//              the monster, the class's counts lowered
//   0x801410B4 LoadMonster(monster): its files queued into a new entry and
//              slot (the model and motion tables at 0x805825E0 / 0x80582B08,
//              8 bytes {size, path} per id), then its effects and sounds;
//              the files queued (0 when it is loading already, -1 when no
//              entry or slot is free)
#include "abi_bridge.h"
#include "guest_call.h"
#include "memory.h"
#include "port_check.h"

#include <cstdint>

namespace {
constexpr uint32_t kState = 0x806A6A10u, kSlots = kState + 0xDB0, kFreeCounts = kState + 0xF30;
constexpr uint32_t kClasses = 0x805A22C8u, kModels = 0x805825E0u, kMotions = 0x80582B08u;
constexpr uint32_t kEntries = 6, kEntryBytes = 24, kSlotBytes = 20;
constexpr uint32_t kMoveWorks = 0x8065AA60u, kHunterWorkBytes = 0xB20;

int32_t MonsterClass(uint8_t monster) {
    return int8_t(MemoryInline::FlatRead8(kClasses + monster));
}

uint32_t LoadEntryWith(uint8_t used) {
    for (uint32_t i = 0; i < kEntries; ++i) {
        const uint32_t entry = kState + i * kEntryBytes;

        if (MemoryInline::FlatRead8(entry + 1) == used)
            return entry;
    }

    return 0;
}

uint32_t LoadEntryOf(uint8_t monster) {
    for (uint32_t i = 0; i < kEntries; ++i) {
        const uint32_t entry = kState + i * kEntryBytes;

        if (MemoryInline::FlatRead8(entry + 1) && MemoryInline::FlatRead8(entry + 2) == monster)
            return i;
    }

    return 255;
}

uint32_t TakeLoadEntry(uint8_t monster) {
    const uint32_t entry = LoadEntryWith(0);

    if (!entry)
        return 255;

    MemoryInline::FlatWrite8(entry + 1, 1);
    MemoryInline::FlatWrite8(entry + 2, monster);
    MemoryInline::FlatWrite8(entry + 3, 0);
    return MemoryInline::FlatRead8(entry);
}

void CountSlotTaken(int32_t heapClass) {
    const uint32_t counts = kFreeCounts + uint32_t(heapClass) * 4;

    for (uint32_t i = 0; i < 4; ++i)
        if (const uint8_t count = MemoryInline::FlatRead8(counts + i))
            MemoryInline::FlatWrite8(counts + i, count - 1);
}

uint32_t SlotClassFree(GuestCall& game, int32_t heapClass) {
    // get_move_work_adrs(2) (0x800CFAF4) and the game mode (0x800CF27C), inline.
    const uint32_t table = MemoryInline::FlatRead32(kMoveWorks + 0xA4);
    uint32_t hunter = table ? MemoryInline::FlatRead32(table + 0x18) : 0;
    const int32_t hunters = int16_t(game.Call<0x800CFB34>({2})); // get_move_work_max
    const uint32_t counts = kFreeCounts + uint32_t(heapClass) * 4;

    if (MemoryInline::FlatRead8(kMoveWorks + 0x25) == 2)
        return MemoryInline::FlatRead8(counts) ? 1 : 0;

    for (int32_t i = 0; i < hunters; ++i, hunter += kHunterWorkBytes)
        if (MemoryInline::FlatRead8(hunter) && game.Call<0x8012D20C>({MemoryInline::FlatRead8(hunter + 8)}) == 0 && !MemoryInline::FlatRead8(counts + i))
            return 0;

    return 1;
}

uint32_t FindHeapSlot(GuestCall& game, uint8_t monster) {
    const int32_t heapClass = MonsterClass(monster);

    if (heapClass == -1)
        return 0;

    // The first free slot of class `wanted` decides; false when none is free.
    uint32_t found = 0;
    const auto first = [&](int32_t wanted) {
        for (uint32_t i = 0; i < kEntries; ++i) {
            const uint32_t slot = kSlots + i * kSlotBytes;
            const int32_t slotClass = MemoryInline::FlatRead8(slot + 1);

            if (MemoryInline::FlatRead8(slot) || slotClass != wanted)
                continue;

            found = heapClass <= 0 || SlotClassFree(game, int8_t(slotClass)) == 1 ? slot : 0;
            return true;
        }

        return false;
    };

    if (first(heapClass) || first(heapClass + 1))
        return found;

    if (heapClass == 1)
        return 0;

    return first(heapClass + 2) ? found : 0;
}

uint32_t TakeHeapSlot(GuestCall& game, uint8_t monster) {
    const uint32_t slot = FindHeapSlot(game, monster);

    if (slot) {
        MemoryInline::FlatWrite8(slot, 1);
        MemoryInline::FlatWrite8(slot + 2, monster);
        CountSlotTaken(int8_t(MemoryInline::FlatRead8(slot + 1)));
    }

    return slot;
}

uint32_t LoadMonster(GuestCall& game, uint8_t monster) {
    if (LoadEntryOf(monster) != 255) {
        if (const int32_t heapClass = MonsterClass(monster); heapClass > 0)
            CountSlotTaken(heapClass);

        return 0;
    }

    const uint32_t entry = TakeLoadEntry(monster);

    if (entry == 255 || !TakeHeapSlot(game, monster))
        return uint32_t(-1);

    uint32_t queued = 0;

    if (const uint32_t size = MemoryInline::FlatRead32(kModels + monster * 8)) {
        game.Call<0x80140EAC>({MemoryInline::FlatRead32(kModels + monster * 8 + 4), size, entry});
        queued = 1;
    }

    if (const uint32_t size = MemoryInline::FlatRead32(kMotions + monster * 8)) {
        game.Call<0x80141014>({MemoryInline::FlatRead32(kMotions + monster * 8 + 4), size, entry});
        ++queued;
    }

    game.Call<0x801471C4>({monster});

    if ((game.Call<0x801475B4>({monster}) & 0xFF) == 1)
        if (const int32_t effects = int32_t(game.Call<0x801476E8>({monster, 0})); effects >= 0)
            game.Call<0x800FAA1C>({kState + 0xEA0 + uint32_t(effects) * 23, monster});

    game.Call<0x800F1000>({monster});
    return queued;
}

void LoadEntryWithPort(CpuContext* ctx) {
    ctx->gpr[3] = LoadEntryWith(uint8_t(ctx->gpr[3]));
}

void LoadEntryOfPort(CpuContext* ctx) {
    ctx->gpr[3] = LoadEntryOf(uint8_t(ctx->gpr[3]));
}

void TakeLoadEntryPort(CpuContext* ctx) {
    ctx->gpr[3] = TakeLoadEntry(uint8_t(ctx->gpr[3]));
}

void CountSlotTakenPort(CpuContext* ctx) {
    CountSlotTaken(int8_t(ctx->gpr[3]));
}

void SlotClassFreePort(CpuContext* ctx) {
    const int32_t heapClass = int8_t(ctx->gpr[3]);
    uint32_t free;

    {
        // The frame puts the caller's registers back, r3 too.
        GuestFrame frame(ctx, 0x20);
        GuestCall game{ctx};
        free = SlotClassFree(game, heapClass);
    }

    ctx->gpr[3] = free;
}

void FindHeapSlotPort(CpuContext* ctx) {
    const uint8_t monster = uint8_t(ctx->gpr[3]);
    uint32_t slot;

    {
        GuestFrame frame(ctx, 0x10);
        GuestCall game{ctx};
        slot = FindHeapSlot(game, monster);
    }

    ctx->gpr[3] = slot;
}

void TakeHeapSlotPort(CpuContext* ctx) {
    const uint8_t monster = uint8_t(ctx->gpr[3]);
    uint32_t slot;

    {
        GuestFrame frame(ctx, 0x10);
        GuestCall game{ctx};
        slot = TakeHeapSlot(game, monster);
    }

    ctx->gpr[3] = slot;
}

void LoadMonsterPort(CpuContext* ctx) {
    const uint8_t monster = uint8_t(ctx->gpr[3]);
    uint32_t queued;

    {
        GuestFrame frame(ctx, 0x20);
        GuestCall game{ctx};
        queued = LoadMonster(game, monster);
    }

    ctx->gpr[3] = queued;
}
} // namespace

extern "C" void func_80141434(CpuContext* ctx);
extern "C" void MH3_LoadEntryWith(CpuContext* ctx) {
    PortCheck::Call(ctx, "LoadEntryWith", func_80141434, LoadEntryWithPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80141434, MH3_LoadEntryWith, "LoadEntryWith");

extern "C" void func_80141538(CpuContext* ctx);
extern "C" void MH3_LoadEntryOf(CpuContext* ctx) {
    PortCheck::Call(ctx, "LoadEntryOf", func_80141538, LoadEntryOfPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80141538, MH3_LoadEntryOf, "LoadEntryOf");

extern "C" void func_801414D4(CpuContext* ctx);
extern "C" void MH3_TakeLoadEntry(CpuContext* ctx) {
    PortCheck::Call(ctx, "TakeLoadEntry", func_801414D4, TakeLoadEntryPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x801414D4, MH3_TakeLoadEntry, "TakeLoadEntry");

extern "C" void func_80146BFC(CpuContext* ctx);
extern "C" void MH3_CountSlotTaken(CpuContext* ctx) {
    PortCheck::Call(ctx, "CountSlotTaken", func_80146BFC, CountSlotTakenPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80146BFC, MH3_CountSlotTaken, "CountSlotTaken");

extern "C" void func_80146A88(CpuContext* ctx);
extern "C" void MH3_SlotClassFree(CpuContext* ctx) {
    PortCheck::Call(ctx, "SlotClassFree", func_80146A88, SlotClassFreePort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80146A88, MH3_SlotClassFree, "SlotClassFree");

extern "C" void func_80146CC8(CpuContext* ctx);
extern "C" void MH3_FindHeapSlot(CpuContext* ctx) {
    PortCheck::Call(ctx, "FindHeapSlot", func_80146CC8, FindHeapSlotPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80146CC8, MH3_FindHeapSlot, "FindHeapSlot");

extern "C" void func_80146E58(CpuContext* ctx);
extern "C" void MH3_TakeHeapSlot(CpuContext* ctx) {
    PortCheck::Call(ctx, "TakeHeapSlot", func_80146E58, TakeHeapSlotPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80146E58, MH3_TakeHeapSlot, "TakeHeapSlot");

extern "C" void func_801410B4(CpuContext* ctx);
extern "C" void MH3_LoadMonster(CpuContext* ctx) {
    PortCheck::Call(ctx, "LoadMonster", func_801410B4, LoadMonsterPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x801410B4, MH3_LoadMonster, "LoadMonster");
