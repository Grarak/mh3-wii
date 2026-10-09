// Also the quest's monster slots, RMHP08, C reconstructions from the
// translated code:
//
//   0x803AE9F4 SetQuestMonsters(): for each of the quest entry's three
//              monster slots (+0x314, 8 bytes: u8 monster, u8 record, u8 +2,
//              u8 count) with a monster, a count and a spawn list entry for
//              the map (Apply's lists), until 0x803B4E3C(0) is 1 at the third:
//              the slot copied to questwork +0x319 + 8n, the monster to
//              +0x2D4 + n, and a 48-byte placement to +0x34C + 48n: u32
//              monster, u32 count, the record's 32 bytes from +8 (with the
//              slot's +2 at +10), s8 the objective record (+40; 3 none) and
//              that record's first word | 8 (+44)
//   0x803AEBE0 CopyQuestSlot(to, from): 8 bytes
//   0x803ADFE8 QuestRecordOf(monster): the first of questwork's three live
//              objective records (+0x94, 96 bytes each: u32 flags, u16
//              target) in use that targets the monster or (0x80126000) its
//              kind; -1 for none
#include "quest_placement.h"
#include "abi_bridge.h"
#include "guest_call.h"
#include "memory.h"
#include "port_check.h"

#include <cstdio>

namespace QuestPlacement {
constexpr uint32_t kLists = 0x805F7E60, kMapKeys = 0x805F8058;

namespace {
constexpr uint32_t kMonsters = 32, kRecords = 4, kRecordBytes = 48;
constexpr uint32_t kQuestWork = 0x80797100u, kLiveRecords = 0x806C7D6Cu;

// The records of `list`'s entry for map key `key`, 0 for none.
uint32_t Records(uint32_t list, uint8_t key) {
    for (uint32_t at = list; list && Memory::Read8(at) != 0xFF; at += 8)
        if (Memory::Read8(at) == key)
            return Memory::Read32(at + 4);

    return 0;
}
} // namespace

void Apply(uint32_t base) {
    uint32_t free = base;

    for (const auto [monster, map] : Requests()) {
        const uint8_t key = Memory::Read8(kMapKeys + map * 2);
        const uint32_t list = monster < kMonsters ? Memory::Read32(kLists + monster * 4) : 0;

        if (!list || !key || Records(list, key))
            continue;

        // Another monster's spawn points on the map, the first in id order
        // with records 0 and 1 (a second monster takes record 1), else the
        // first at all.
        uint32_t donor = 0, records = 0;

        for (uint32_t other = 1; other < kMonsters; ++other) {
            const uint32_t found = Records(Memory::Read32(kLists + other * 4), key);
            const bool both = found && Memory::Read32(found + 4) && Memory::Read32(found + kRecordBytes + 4);

            if (found && (!records || both)) {
                records = found;
                donor = other;
            }

            if (both)
                break;
        }

        uint32_t entries = 0;

        while (Memory::Read8(list + entries * 8) != 0xFF)
            ++entries;

        const uint32_t bytes = kRecords * kRecordBytes + (entries + 2) * 8;

        if (!records || free + bytes > base + kBytes) {
            std::fprintf(stderr, "MH3 quest placement: no spawn point for monster %u on map %u\n", unsigned(monster), unsigned(map));
            continue;
        }

        // The records, with the monster's id in their first word (803AE9F4
        // copies from +8 on), then the list.
        for (uint32_t i = 0; i < kRecords * kRecordBytes; i += 4)
            Memory::Write32(free + i, Memory::Read32(records + i));

        for (uint32_t r = 0; r < kRecords; ++r)
            if (Memory::Read32(free + r * kRecordBytes))
                Memory::Write32(free + r * kRecordBytes, monster);

        const uint32_t newList = free + kRecords * kRecordBytes;

        for (uint32_t i = 0; i < entries * 8; i += 4)
            Memory::Write32(newList + i, Memory::Read32(list + i));

        Memory::Write32(newList + entries * 8, uint32_t(key) << 24);
        Memory::Write32(newList + entries * 8 + 4, free);
        Memory::Write32(newList + entries * 8 + 8, 0xFF000000);
        Memory::Write32(newList + entries * 8 + 12, 0);
        Memory::Write32(kLists + monster * 4, newList);
        free += bytes;
        std::fprintf(stderr, "MH3 quest placement: monster %u on map %u at monster %u's spawn points\n", unsigned(monster), unsigned(map), unsigned(donor));
    }
}
} // namespace QuestPlacement

namespace {
using MemoryInline::FlatRead32;
using MemoryInline::FlatRead8;
using MemoryInline::FlatWrite32;
using MemoryInline::FlatWrite8;

void CopyQuestSlot(uint32_t to, uint32_t from) {
    for (uint32_t i = 0; i < 8; ++i)
        FlatWrite8(to + i, FlatRead8(from + i));
}

int32_t QuestRecordOf(GuestCall& game, uint8_t monster) {
    for (uint32_t k = 0; k < 3; ++k) {
        const uint32_t record = QuestPlacement::kLiveRecords + k * 96;

        if (!(FlatRead32(record) & 1))
            continue;

        const uint32_t target = MemoryInline::FlatRead16(record + 4);

        if (target == monster || game.Call<0x80126000>({target & 0xFF, monster}) == 1)
            return int32_t(k);
    }

    return -1;
}

void SetQuestMonsters(GuestCall& game) {
    const uint32_t quest = FlatRead32(QuestPlacement::kQuestWork);

    if (!quest)
        return;

    uint32_t placement = quest + 0x34C, copy = quest + 0x319, monsters = quest + 0x2D4;
    const uint32_t entry = FlatRead32(quest + 0x3C), records = entry + 0x394;
    const uint8_t key = FlatRead8(QuestPlacement::kMapKeys + FlatRead8(entry + 0x8B) * 2);
    uint32_t slot = entry + 0x314;

    for (uint32_t i = 0; i < 3; ++i, slot += 8) {
        if (game.Call<0x803B4E3C>({0}) == 1 && i == 2)
            break;

        const uint8_t monster = FlatRead8(slot);

        if (!monster || !FlatRead8(slot + 3))
            continue;

        uint32_t spawn = FlatRead32(QuestPlacement::kLists + monster * 4);

        if (!spawn)
            continue;

        while (FlatRead8(spawn) != 0xFF && FlatRead8(spawn) != key)
            spawn += 8;

        if (FlatRead8(spawn) == 0xFF)
            continue;

        CopyQuestSlot(copy, slot);
        FlatWrite8(monsters, FlatRead8(copy));
        FlatWrite32(placement, FlatRead8(copy));
        FlatWrite32(placement + 4, FlatRead8(copy + 3));
        const uint32_t record = FlatRead32(spawn + 4) + FlatRead8(copy + 1) * QuestPlacement::kRecordBytes + 8;

        for (uint32_t b = 0; b < 32; ++b)
            FlatWrite8(placement + 8 + b, FlatRead8(record + b));

        FlatWrite8(placement + 10, FlatRead8(copy + 2));
        int32_t objective = QuestRecordOf(game, FlatRead8(copy));

        if (objective == -1)
            objective = 3;

        FlatWrite8(placement + 40, uint8_t(objective));
        FlatWrite32(placement + 44, objective < 3 ? FlatRead32(records + uint32_t(objective) * 96) | 8 : 0);
        placement += 48;
        copy += 8;
        ++monsters;
    }
}

void CopyQuestSlotPort(CpuContext* ctx) {
    CopyQuestSlot(ctx->gpr[3], ctx->gpr[4]);
}

void QuestRecordOfPort(CpuContext* ctx) {
    const uint8_t monster = uint8_t(ctx->gpr[3]);
    int32_t record;

    {
        // The frame puts the caller's registers back, r3 too.
        GuestFrame frame(ctx, 0x20);
        GuestCall game{ctx};
        record = QuestRecordOf(game, monster);
    }

    ctx->gpr[3] = uint32_t(record);
}

void SetQuestMonstersPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x30);
    GuestCall game{ctx};
    SetQuestMonsters(game);
}
} // namespace

extern "C" void func_803AEBE0(CpuContext* ctx);
extern "C" void MH3_CopyQuestSlot(CpuContext* ctx) {
    PortCheck::Call(ctx, "CopyQuestSlot", func_803AEBE0, CopyQuestSlotPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x803AEBE0, MH3_CopyQuestSlot, "CopyQuestSlot");

extern "C" void func_803ADFE8(CpuContext* ctx);
extern "C" void MH3_QuestRecordOf(CpuContext* ctx) {
    PortCheck::Call(ctx, "QuestRecordOf", func_803ADFE8, QuestRecordOfPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x803ADFE8, MH3_QuestRecordOf, "QuestRecordOf");

extern "C" void func_803AE9F4(CpuContext* ctx);
extern "C" void MH3_SetQuestMonsters(CpuContext* ctx) {
    PortCheck::Call(ctx, "SetQuestMonsters", func_803AE9F4, SetQuestMonstersPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x803AE9F4, MH3_SetQuestMonsters, "SetQuestMonsters");
