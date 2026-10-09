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
//   0x803AEBE0 CopyQuestSlot(to, from): 8 bytes (u8 kind, ..., +6 size, +7
//              level); the cheats Boss/s mod, Set boss/s size to and Set
//              boss/s level to (EU 0x803AEBE0/C10/C18: the loads replaced by
//              the value) set them
//   0x803ADFE8 QuestRecordOf(monster): the first of questwork's three live
//              objective records (+0x94, 96 bytes each: u32 flags, u16
//              target) in use that targets the monster or (0x80126000) its
//              kind; -1 for none
#include "quest_placement.h"
#include "abi_bridge.h"
#include "cheats.h"
#include "guest_call.h"
#include "memory.h"
#include "port_check.h"

#include <algorithm>
#include <cstdio>

namespace QuestPlacement {
constexpr uint32_t kLists = 0x805F7E60, kMapKeys = 0x805F8058;

namespace {
constexpr uint32_t kMonsters = 32, kRecords = 4, kRecordBytes = 48;
constexpr uint32_t kQuestWork = 0x80797100u, kLiveRecords = 0x806C7D6Cu, kLayouts = 0x805A23F8u;

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

    // The custom layout's slots.
    for (uint32_t i = 0; Layout() && i < 6; ++i)
        Memory::Write8(kLayouts + 3 * 6 + i, uint8_t((*Layout())[i]));

    // The highest record each monster needs on each map.
    std::vector<Request> needs;

    for (const Request& request : Requests()) {
        auto same = std::find_if(needs.begin(), needs.end(), [&](const Request& need) { return need.monster == request.monster && need.map == request.map; });

        if (same == needs.end())
            needs.push_back(request);
        else
            same->record = std::max(same->record, request.record);
    }

    for (const auto [monster, map, record] : needs) {
        const uint8_t key = Memory::Read8(kMapKeys + map * 2);
        const uint32_t list = monster < kMonsters ? Memory::Read32(kLists + monster * 4) : 0;
        const uint32_t own = list && key ? Records(list, key) : 0;

        if (!list || !key || record >= kRecords)
            continue;

        bool complete = own != 0;

        for (uint32_t r = 0; own && r <= record; ++r)
            complete = complete && Memory::Read32(own + r * kRecordBytes + 4);

        if (complete)
            continue;

        // The monster's own records there, else another monster's, the first
        // in id order with the records up to the one needed, else the first
        // at all.
        uint32_t donor = monster, records = own, first = 0, firstDonor = 0;

        for (uint32_t other = 1; !records && other < kMonsters; ++other) {
            const uint32_t found = Records(Memory::Read32(kLists + other * 4), key);
            bool all = found != 0;

            for (uint32_t r = 0; found && r <= record; ++r)
                all = all && Memory::Read32(found + r * kRecordBytes + 4);

            if (all) {
                records = found;
                donor = other;
            } else if (found && !first) {
                first = found;
                firstDonor = other;
            }
        }

        if (!records && first) {
            records = first;
            donor = firstDonor;
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
        // copies from +8 on) and the empty ones (count 0) a copy of the
        // first that is not.
        for (uint32_t i = 0; i < kRecords * kRecordBytes; i += 4)
            Memory::Write32(free + i, Memory::Read32(records + i));

        uint32_t filled = 0;

        while (filled < kRecords && !Memory::Read32(free + filled * kRecordBytes + 4))
            ++filled;

        for (uint32_t r = 0; r < kRecords && filled < kRecords; ++r) {
            const uint32_t at = free + r * kRecordBytes;

            if (!Memory::Read32(at + 4))
                for (uint32_t i = 0; i < kRecordBytes; i += 4)
                    Memory::Write32(at + i, Memory::Read32(free + filled * kRecordBytes + i));

            Memory::Write32(at, monster);
        }

        // The list: its entries, the map's pointing at the records (added
        // when it has none).
        const uint32_t newList = free + kRecords * kRecordBytes;
        uint32_t written = 0;

        for (uint32_t e = 0; e < entries; ++e, ++written) {
            Memory::Write32(newList + e * 8, Memory::Read32(list + e * 8));
            Memory::Write32(newList + e * 8 + 4, Memory::Read8(list + e * 8) == key ? free : Memory::Read32(list + e * 8 + 4));
        }

        if (!own) {
            Memory::Write32(newList + written * 8, uint32_t(key) << 24);
            Memory::Write32(newList + written * 8 + 4, free);
            ++written;
        }

        Memory::Write32(newList + written * 8, 0xFF000000);
        Memory::Write32(newList + written * 8 + 4, 0);
        Memory::Write32(kLists + monster * 4, newList);
        free += bytes;
        std::fprintf(stderr, "MH3 quest placement: monster %u on map %u (record %u) at monster %u's spawn points\n", unsigned(monster), unsigned(map), unsigned(record),
                     unsigned(donor));
    }
}
} // namespace QuestPlacement

namespace {
using MemoryInline::FlatRead32;
using MemoryInline::FlatRead8;
using MemoryInline::FlatWrite32;
using MemoryInline::FlatWrite8;

void CopyQuestSlot(uint32_t to, uint32_t from) {
    using Cheats::Id;
    FlatWrite8(to, Cheats::On(Id::BossMod) ? uint8_t(Cheats::Value(Id::BossMod)) : FlatRead8(from));

    for (uint32_t i = 1; i < 6; ++i)
        FlatWrite8(to + i, FlatRead8(from + i));

    FlatWrite8(to + 6, Cheats::On(Id::BossSize) ? uint8_t(Cheats::Value(Id::BossSize)) : FlatRead8(from + 6));
    FlatWrite8(to + 7, Cheats::On(Id::BossLevel) ? uint8_t(Cheats::Value(Id::BossLevel)) : FlatRead8(from + 7));
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
