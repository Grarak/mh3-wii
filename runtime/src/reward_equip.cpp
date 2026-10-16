// The quest reward equipment, RMHP08, ported natively: the shards and charms a
// quest result lists (qResult work +0x304: 40 pairs of item and count) become
// equipment records when the result screen ends (RewardEquipBuild), and the
// ones taken go to the equipment box (RewardEquipToBox). Item 110 (Rustshard)
// and 111 (Ancient Shard) pick a weapon from a weighted table, 440 (Mystery
// Charm), 441 (Shining Charm) and 442 (Timeworn Charm) generate a talisman
// from table 0, 1 or 2. Every draw is stream 1 of ran_suu (0x8065AA7A), one
// for the row, up to two for the slots and one per skill, so the cycle
// that seed lives in decides which talismans can come out at all.
//
// A record (12 bytes): kind (6 a talisman), slots, u16 id (a talisman's
// rarity), u16 skill points (+10: the first skill's in the low byte, the
// second's in the high one), then three u16s: the slots, and after them
// the skills. A talisman with three slots has no skill and one with two at
// most one.
//
// The result screen's reward pages run here too (0x80398FE0, once a step
// from QuestResultRecords 0x803975D0): the two item lists (states 1 and 2:
// take items to the pouch or the box, or leave them to be sold), the
// equipment page (3), and the money, point and record pages (5 to 8). A
// page's countdown (+0x2FF4, seconds * 30 from 0x803973A8) ends it and sells
// what is left. That countdown and the pages' own counters (their fade-in,
// the equipment page's build delay, the rows of pages 6 to 8, the blink and
// the taken item's flash) count ticks of game time, as the steps did at
// 30 Hz.
#include "ran_suu.h"
#include "game_step.h"
#include "port_check.h"
#include "abi_bridge.h"
#include "guest_call.h"
#include "memory.h"

#include <algorithm>
#include <cstdint>

namespace {

uint32_t Read8(uint32_t address) {
    return Memory::Read8(address);
}

uint32_t Read16(uint32_t address) {
    return Memory::Read16(address);
}

uint32_t Read32(uint32_t address) {
    return Memory::Read32(address);
}

void Write8(uint32_t address, uint8_t value) {
    Memory::Write8(address, value);
}

void Write16(uint32_t address, uint16_t value) {
    Memory::Write16(address, value);
}

void Write32(uint32_t address, uint32_t value) {
    Memory::Write32(address, value);
}

enum : uint32_t {
    ItemBoxSlots = 0x8004AED4,
    ItemBoxRoom = 0x8004B814, // (item, box, slots): how many more fit, at most 999
    ItemBoxAdd = 0x8004BF08,  // (item, count, out)
    TalismanToBox = 0x8004C620, // its native is in cheat_items.cpp
    EquipBoxAdd = 0x8004C578,
    SeRequest = 0x800DBC94,    // sysSE_req (sound)
    ResultSe = 0x800DBDF4,     // (sound): one of the result screen's own
    MenuGrid = 0x802A8F24,     // (cursor, count, buttons, back, forward, scroll): the cursor
    MenuCursor = 0x802A8F60,   // (cursor, count, buttons, back, forward): the cursor
    MenuPrompt = 0x802A8FB4,   // (prompt, buttons, 4, 8, 0xFFFF): 1 yes, 2 no
    PulseStep = 0x802D9F08,    // the shared blink (PulseUpdate, pulse.cpp)
    ResultListKind = 0x80396C7C, // 1 when the first list is kind 2 (0x803B5094(0) == 1)
    ResultNextRow = 0x80396D10,
    ResultRowSe = 0x8039739C,  // a row's sound and its delay (+0x306C = 90)
    RewardsAny = 0x803982B8,   // (work, list): whether it holds anything
    RewardsPrompt = 0x803985D0, // (work, list): the "leave the rest" prompt
    RewardsSell = 0x803986F0,  // (work, list): sells what is left, equipment to +0x304
    RewardsCursor = 0x80398834, // (work, pad, list): moves the cursor
    RewardsSelected = 0x80398930, // (work, list): the item and count under the cursor
    RewardsEquipItem = 0x80398B58, // (work, entry): an equipment item to +0x304
    RewardsRecordAt = 0x80398BF8, // (work, 3): the record under the cursor
    RewardsPouchSlot = 0x80398C90, // (work): the pouch slot under the cursor
    RewardMarkTaken = 0x80398E04,
    RewardsTakenFlash = 0x80398F88, // (work, entry, x, y)
    PouchTake = 0x8039CE18,   // (item, count, out taken): 0 all, 2 some, 3 full, else none
    PouchTaken = 0x8039CF38,  // (item, count): logs what went to the pouch
    PouchSet = 0x8039D0CC,    // (item, count, slot)
};

constexpr uint32_t kRandom = 1; // the reward equipment's ran_suu stream

// Talisman tables 0..2 (Mystery, Shining, Timeworn Charm): rows of 8 bytes,
// a weight (s8; the table ends at one of 0 or less), the rarity - 1, the
// slot mode, the first skill's group + 1 and points (s8), the second's, a
// pad byte.
constexpr uint32_t kTalismanTables = 0x805C6E08;

// Skill groups 0..2: u16 skill trees ending at 0xFFFF.
constexpr uint32_t kSkillGroups = 0x805C6B88;

// Shard tables 0..1 (Rustshard, Ancient Shard): rows of 4 bytes, a weight
// (s8; ends at 0 or less), the equipment kind and the u16 id.
constexpr uint32_t kShardTables = 0x80794618;

constexpr uint32_t kResultWork = 0x8066C668; // get_qResult_work
constexpr uint32_t kResultEquip = kResultWork + 0x304;
constexpr uint32_t kUserData = 0x8065B3BC;      // system_w+0x95C: the user data
constexpr uint32_t kUserDataCache = 0x80796D40; // r13-0x45A0: get_userdata's copy
constexpr uint32_t kItemTable = 0x806AED38;     // GetItemData's table: 20 bytes an item
constexpr uint32_t kItems = 747;

// The pad: the buttons pressed this step (u16 +4) and the held ones that
// repeat (+0x14). 0x10 confirms, 0x20 cancels, 3 left and right, 0xC up and
// down.
constexpr uint32_t kPad = 0x8065BA90;

// The result screen's work.
enum : uint32_t {
    wStep = 0x2,          // u8: the page's step
    wPage = 0x2FF0,       // s32: the page (state)
    wCountdown = 0x2FF4,  // s32: ticks left on the page
    wDelay = 0x3020,      // s16: page 8's delay before the next row
    wCursor = 0x3022,     // u8
    wCursorCount = 0x3023, // u8
    wScroll = 0x3024,     // u16
    wFadeIn = 0x3026,     // u8: 0..7
    wColumn = 0x3028,     // u8
    wRow = 0x3029,        // u8
    wRows = 0x302C,       // u8
    wPouchRow = 0x302D,   // u8
    wPouchRows = 0x302E,  // u8
    wPouchColumn = 0x302F, // u8
    wToBox = 0x3030,      // u8: the list goes to the box, not the pouch
    wBuilt = 0x3031,      // u8: the records were built
    wEquipWait = 0x3032,  // u16: ticks on the equipment page
    wBlink = 0x3034,      // u16: 0..40
    wFlash = 0x3036,      // u16: 0..12, 0xFFFF off
    wTaken = 0x3038,      // u16 bits: the records taken
    wPrompt = 0x3040,     // u32
    wPromptText = 0x3050, // u32
    wPointsBefore = 0x305C, // u16
    wPointsWait = 0x305E, // u16: 0..40
    wRowShown = 0x3061,   // u8 a row: 0..8
    wNextRow = 0x306F,    // u8
    wRecords = 0x31F0,    // 40 records
};

constexpr uint32_t kRowFlags = 0x60; // a row of pages 7 and 8: 16 bytes, u8 flag first
constexpr uint32_t kPointsTotal = 0x3DE4; // the user data's points (u16)

constexpr uint32_t kRecord = 12;
constexpr uint32_t kRewards = 40;
constexpr uint32_t kTalisman = 6;

void Clear(uint32_t record) {
    for (uint32_t i = 0; i < kRecord; ++i)
        Write8(record + i, 0);
}

int32_t Weight(uint32_t row) {
    return int8_t(Read8(row));
}

// A row by weight: one draw.
uint32_t PickRow(uint32_t table, uint32_t stride) {
    int32_t total = 0;

    for (uint32_t row = table; Weight(row) > 0; row += stride)
        total += Weight(row);

    int32_t roll = int32_t(RanSuu(kRandom) & 0xFFFF) % int16_t(total);
    uint32_t row = table;

    for (; Weight(row) <= roll; row += stride)
        roll -= Weight(row);

    return row;
}

// TalismanSkillPick (0x8027F6D8): a skill of the group; one that is not
// `other` when the group holds it.
uint32_t PickSkill(uint32_t group, int16_t other) {
    const uint32_t skills = Read32(kSkillGroups + 4 * (group & 0xFF));
    uint32_t count = 0;
    uint32_t at = 0xFFFF;

    for (; Read16(skills + 2 * count) != 0xFFFF; ++count)
        if (other && int16_t(Read16(skills + 2 * count)) == other)
            at = count;

    const uint32_t roll = RanSuu(kRandom) & 0xFFFF;
    const uint32_t pick = at == 0xFFFF ? roll % count : (at + roll % (count - 1) + 1) % count;
    return Read16(skills + 2 * pick);
}

// TalismanGenerate (0x8027F7DC).
void GenerateTalisman(uint32_t record, uint32_t table) {
    const uint32_t row = PickRow(Read32(kTalismanTables + 4 * (table & 0xFF)), 8);

    Write8(record, kTalisman);
    Write16(record + 2, uint16_t(Read8(row + 1) + 1));

    const uint32_t mode = Read8(row + 2);
    uint32_t slots = mode;

    if (mode == 2)
        slots = RanSuu(kRandom) & 1 ? 2 : 1;
    else if (mode > 2)
        slots = !(RanSuu(kRandom) & 1) ? 1 : (RanSuu(kRandom) & 0xFFFF) % 100 < 20 ? 3 : 2;

    Write8(record + 1, uint8_t(slots));

    for (uint32_t i = 4; i < kRecord; i += 2)
        Write16(record + i, 0);

    const int32_t points = int8_t(Read8(row + 4));
    uint32_t at = slots;

    if (at >= 3 || !points)
        return;

    if (const uint32_t group = Read8(row + 3)) {
        Write16(record + 6 + 2 * at, uint16_t(PickSkill(group - 1, 0)));
        Write16(record + 4, uint16_t((points + 10) & 0xFF));
        ++at;
    }

    const int32_t second = int8_t(Read8(row + 6));
    const uint32_t group = Read8(row + 5);

    if (at >= 3 || !second || !group)
        return;

    const int16_t first = int16_t(Read16(record + 6 + 2 * (at - 1)));

    Write16(record + 6 + 2 * at, uint16_t(PickSkill(group - 1, first)));
    Write16(record + 4, uint16_t(Read16(record + 4) | (second + 10) * 0x100));
}

// RewardEquipPick (0x8027FA5C): a shard's weapon.
void PickShardEquip(uint32_t record, uint32_t table) {
    const uint32_t row = PickRow(Read32(kShardTables + 4 * (table & 0xFF)), 4);

    Write8(record, Read8(row + 1));
    Write16(record + 2, Read16(row + 2));
}

// RewardEquipGenerate (0x8027FB28): the record of a reward item.
void GenerateEquip(uint32_t item, uint32_t record) {
    Clear(record);

    switch (item & 0xFFFF) {
    case 110:
        PickShardEquip(record, 0);
        break;
    case 111:
        PickShardEquip(record, 1);
        break;
    case 440:
        GenerateTalisman(record, 0);
        break;
    case 441:
        GenerateTalisman(record, 1);
        break;
    case 442:
        GenerateTalisman(record, 2);
        break;
    default:
        break;
    }
}

// RewardEquipBuild (0x8039899C): a record per item and count, one cleared
// for each empty pair.
void Build(uint32_t work) {
    uint32_t record = work + wRecords;

    for (uint32_t i = 0; i < kRewards; ++i) {
        const uint32_t item = Read16(kResultEquip + 4 * i);
        const int32_t count = int16_t(Read16(kResultEquip + 4 * i + 2));

        if (!item || count < 1) {
            Clear(record);
            continue;
        }

        for (int32_t n = 0; n < count; ++n, record += kRecord)
            GenerateEquip(item, record);
    }

    Write8(work + wBuilt, 1);
}

// TalismanSkills (0x803191DC): a talisman's skills and points; how many.
uint32_t Skills(uint32_t record, uint32_t skills, uint32_t points) {
    const uint32_t packed = Read16(record + 4);
    uint32_t count = 0;
    uint32_t at = Read8(record + 1);

    if (at < 3) {
        if (const uint32_t skill = Read16(record + 6 + 2 * at)) {
            Write16(skills, uint16_t(skill));
            Write8(points, uint8_t((packed & 0xFF) - 10));
            count = 1;
        }
    }

    at = (at + 1) & 0xFF;

    if (at < 3) {
        if (const uint32_t skill = Read16(record + 6 + 2 * at)) {
            Write16(skills + 2 * count, uint16_t(skill));
            Write8(points + count, uint8_t((packed >> 8) - 10));
            ++count;
        }
    }

    return count;
}

// get_userdata (0x8004D184, inlined by the translator).
uint32_t UserData() {
    const uint32_t user = Read32(kUserData);

    Memory::Write32(kUserDataCache, user);
    return user;
}

// RewardEquipToBox (0x80398EAC): the records taken to the box, each cleared
// and marked; whether any went.
uint32_t TakenToBox(CpuContext* cpu, uint32_t work) {
    GuestCall game{cpu};
    const uint32_t user = UserData();
    uint32_t added = 0;

    for (uint32_t i = 0; i < kRewards; ++i) {
        if (!(Read16(work + wTaken + 2 * (i >> 4)) & 1u << (i & 15)))
            continue;

        const uint32_t record = work + wRecords + i * kRecord;
        const uint32_t kind = Read8(record);

        if (kind == kTalisman) {
            game.Call<TalismanToBox>({user, record});
            added = 1;
        } else if (kind) {
            game.Call<EquipBoxAdd>({user, kind, Read16(record + 2)});
            added = 1;
        }

        Clear(record);
        game.Call<RewardMarkTaken>({work, i});
    }

    return added;
}

uint32_t Trigger() {
    return Read16(kPad + 4);
}

uint32_t Buttons() {
    return (Read16(kPad + 4) | Read16(kPad + 0x14)) & 0xFFFF;
}

// GetItemData (0x8029F740, inlined by the translator).
uint32_t ItemData(uint32_t item) {
    item &= 0xFFFF;
    return Read32(kItemTable) + (item < kItems ? item : 0) * 20;
}

// 0x80398264: the page's blink (+0x3034, 0..40) and the taken item's flash
// (+0x3036: 0..12, then off), a tick at a time.
void Animate(uint32_t work, uint32_t ticks) {
    Write16(work + wScroll, 0);

    for (uint32_t tick = 0; tick < ticks; ++tick) {
        const uint32_t blink = (Read16(work + wBlink) + 1) & 0xFFFF;

        Write16(work + wBlink, uint16_t(blink > 40 ? 0 : blink));

        if (Read16(work + wFlash) == 0xFFFF)
            continue;

        const uint32_t flash = (Read16(work + wFlash) + 1) & 0xFFFF;
        Write16(work + wFlash, uint16_t(flash < 13 ? flash : 0xFFFF));
    }
}

// The page's fade-in (+0x3026, 0..7), with a sound as it starts.
void FadeIn(GuestCall& game, uint32_t work, uint32_t ticks) {
    for (uint32_t tick = 0; tick < ticks && Read8(work + wFadeIn) < 7; ++tick) {
        if (!Read8(work + wFadeIn))
            game.Call<SeRequest>({5});

        Write8(work + wFadeIn, uint8_t(Read8(work + wFadeIn) + 1));
    }
}

void Se(GuestCall& game, uint32_t sound) {
    game.Call<SeRequest>({sound});
}

// A list page's item under the cursor, confirmed: equipment to the equipment
// list (+0x304), anything else into the pouch or the item box, all of it or
// what fits. A full pouch asks for a slot to swap (step 3).
void TakeItem(CpuContext* cpu, uint32_t work, uint32_t list) {
    GuestCall game{cpu};
    const uint32_t entry = game.Call<RewardsSelected>({work, list});
    const uint32_t item = Read16(entry);

    if (!item) {
        Se(game, 2);
        return;
    }

    const uint32_t column = Read8(work + wColumn);
    const uint32_t row = Read8(work + wRow);

    if (Read8(ItemData(item) + 2) & 0x10) {
        game.Call<RewardsTakenFlash>({work, entry, column, row});
        game.Call<RewardsEquipItem>({work, entry});
        Write16(entry, 0);
        Write16(entry + 2, 0);
        Se(game, 8);
        return;
    }

    GuestFrame frame(cpu, 0x20);

    if (!Read8(work + wToBox)) {
        const uint32_t taken = frame[0xA];
        const uint32_t result = game.Call<PouchTake>({item, uint32_t(int16_t(Read16(entry + 2))), taken}) & 0xFF;

        if (result == 0) {
            game.Call<RewardsTakenFlash>({work, entry, column, row});
            game.Call<PouchTaken>({Read16(entry), uint32_t(int16_t(Read16(entry + 2)))});
            Write16(entry, 0);
            Write16(entry + 2, 0);
            Se(game, 8);
        } else if (result == 2) {
            game.Call<RewardsTakenFlash>({work, entry, column, row});
            const int32_t some = int16_t(Read16(taken));

            game.Call<PouchTaken>({Read16(entry), uint32_t(some)});
            Write16(entry + 2, uint16_t(int16_t(Read16(entry + 2)) - some));

            if (int16_t(Read16(entry + 2)) <= 0)
                Write16(entry, 0);

            Se(game, 8);
        } else if (result == 3) {
            Write8(work + wStep, 3);
            Write8(work + wPouchRow, 0);
            Write8(work + wPouchColumn, 0);
            Se(game, 0);
        } else {
            Se(game, 2);
        }

        return;
    }

    const uint32_t slots = game.Call<ItemBoxSlots>({Read32(kUserDataCache)}) & 0xFFFF;
    int32_t room = int32_t(game.Call<ItemBoxRoom>({item, Read32(kUserDataCache) + 0x180, slots}));

    if (room <= 0) {
        Se(game, 2);
        return;
    }

    room = std::min<int32_t>(room, int16_t(Read16(entry + 2)));
    game.Call<RewardsTakenFlash>({work, entry, column, row});
    game.Call<ItemBoxAdd>({Read16(entry), uint32_t(room), frame[0x8]});
    Write16(entry + 2, uint16_t(int16_t(Read16(entry + 2)) - int16_t(room)));

    if (!int16_t(Read16(entry + 2)))
        Write16(entry, 0);

    Se(game, 8);
}

// A full pouch: the list's item swaps with the pouch slot picked (the first
// 24, or any for an item of kind 1).
void SwapPouch(GuestCall& game, uint32_t work, uint32_t list) {
    const uint32_t entry = game.Call<RewardsSelected>({work, list});
    const uint32_t data = ItemData(Read16(entry));
    const int32_t slot = int16_t((Read8(work + wPouchColumn) + Read8(work + wPouchRow) * 8) & 0xFFFF);

    if (slot >= 24 && Read8(data) != 1)
        return;

    const uint32_t item = Read16(entry);
    const int32_t count = int16_t(Read16(entry + 2));
    const uint32_t pouch = game.Call<RewardsPouchSlot>({work});

    Write16(entry, uint16_t(Read16(pouch)));
    Write16(entry + 2, uint16_t(Read16(pouch + 2)));
    game.Call<PouchSet>({item, uint32_t(count), uint32_t(slot) & 0xFFFF});
    Write8(work + wStep, 1);
    Se(game, 0);
}

// The prompt to leave the rest (sold); 1 when it is answered yes.
uint32_t LeaveRest(GuestCall& game, uint32_t work, uint32_t list, uint32_t back) {
    const uint32_t answer = game.Call<MenuPrompt>({work + wPrompt, Buttons(), 4, 8, 0xFFFF});

    if (answer == 1) {
        game.Call<RewardsSell>({work, list});
        Se(game, 9);
        return 1;
    }

    if (answer == 2)
        Write8(work + wStep, uint8_t(back));

    return 0;
}

// The cursor of a page's menu, left and right.
void MenuLeftRight(GuestCall& game, uint32_t work) {
    Write8(work + wCursor, uint8_t(game.Call<MenuCursor>({Read8(work + wCursor), Read8(work + wCursorCount), Buttons(), 1, 2})));
}

// The cursor of a page's grid, up and down.
void MenuUpDown(GuestCall& game, uint32_t work) {
    Write8(work + wCursor, uint8_t(game.Call<MenuGrid>({Read8(work + wCursor), Read8(work + wCursorCount), Buttons(), 4, 8, work + wScroll})));
}

// Pages 1 and 2: the item lists. Step 0 the menu (to the pouch, to the box,
// leave the rest), 1 picking items, 2 the prompt, 3 a pouch slot to swap.
uint32_t ListPage(CpuContext* cpu, uint32_t work, uint32_t page, uint32_t ticks) {
    GuestCall game{cpu};

    game.Call<PulseStep>();

    const uint32_t list = page == 2 ? 1 : game.Call<ResultListKind>({work}) == 1 ? 2 : 0;

    Animate(work, ticks);
    FadeIn(game, work, ticks);

    const uint32_t trigger = Trigger();

    switch (Read8(work + wStep)) {
    case 0: {
        if (trigger & 0x10) {
            const uint32_t choice = Read8(work + wCursor);

            if (choice > 2)
                return 0;

            if (choice == 2) {
                Se(game, 0);

                if (game.Call<RewardsAny>({work, list}) != 1)
                    return 1;

                Write8(work + wStep, 2);
                Write32(work + wPrompt, 0);
                Write32(work + wPromptText, game.Call<RewardsPrompt>({work, list}));
                return 0;
            }

            if (game.Call<RewardsAny>({work, list}) == 1) {
                Write8(work + wStep, 1);
                Write8(work + wToBox, uint8_t(choice));
                Se(game, 0);
            } else {
                Se(game, 2);
            }

            return 0;
        }

        if (trigger & 0x20) {
            Write8(work + wCursor, uint8_t(Read8(work + wCursorCount) - 1));
            Se(game, 1);
            return 0;
        }

        if (game.Call<RewardsAny>({work, list}) == 1)
            MenuLeftRight(game, work);

        return 0;
    }

    case 1:
        if (trigger & 0x10) {
            TakeItem(cpu, work, list);

            if (!game.Call<RewardsAny>({work, list})) {
                Write8(work + wStep, 0);
                Write8(work + wCursor, uint8_t(Read8(work + wCursorCount) - 1));
            }

            return 0;
        }

        if (trigger & 0x20) {
            Write8(work + wStep, 0);
            Se(game, 1);
            return 0;
        }

        game.Call<RewardsCursor>({work, kPad, list});
        return 0;

    case 2:
        return LeaveRest(game, work, list, 0);

    case 3: {
        if (trigger & 0x10) {
            SwapPouch(game, work, list);
            return 0;
        }

        if (trigger & 0x20) {
            Write8(work + wStep, 1);
            Se(game, 1);
            return 0;
        }

        const uint32_t buttons = Buttons();

        if (buttons & 3)
            Write8(work + wPouchColumn, uint8_t(game.Call<MenuCursor>({Read8(work + wPouchColumn), 8, buttons, 1, 2})));
        else if (buttons & 0xC)
            Write8(work + wPouchRow, uint8_t(game.Call<MenuGrid>({Read8(work + wPouchRow), Read8(work + wPouchRows), buttons, 4, 8, work + wScroll})));

        return 0;
    }

    default:
        return 0;
    }
}

// Page 3: the equipment (shards and charms). Step 0 waits for a press, 1
// builds the records (after 3 ticks) and shows them (60), 2 the menu (pick,
// leave the rest), 3 marking records and sending the marked to the box, 4
// the prompt.
uint32_t EquipPage(CpuContext* cpu, uint32_t work, uint32_t ticks) {
    GuestCall game{cpu};

    game.Call<PulseStep>();
    Animate(work, ticks);
    FadeIn(game, work, ticks);

    const uint32_t trigger = Trigger();

    switch (Read8(work + wStep)) {
    case 0:
        if (trigger & 0x10) {
            Write8(work + wStep, 1);
            Write8(work + wCursorCount, 0);
            game.Call<ResultSe>({0});
        }

        return 0;

    case 1: {
        const uint32_t waited = (Read16(work + wEquipWait) + ticks) & 0xFFFF;

        Write16(work + wEquipWait, uint16_t(waited));

        if (waited >= 60) {
            Write8(work + wStep, 2);
            Write8(work + wCursorCount, 2);
        }

        if (!Read8(work + wBuilt) && Read16(work + wEquipWait) > 3)
            Build(work);

        return 0;
    }

    case 2: {
        if (trigger & 0x10) {
            Se(game, 0);
            const uint32_t choice = Read8(work + wCursor);

            if (choice == 0) {
                if (game.Call<RewardsAny>({work, 3}) == 1)
                    Write8(work + wStep, 3);
                else
                    Se(game, 2);

                return 0;
            }

            if (choice != 1)
                return 0;

            if (game.Call<RewardsAny>({work, 3}) != 1)
                return 1;

            Write8(work + wStep, 4);
            Write32(work + wPrompt, 0);
            Write32(work + wPromptText, game.Call<RewardsPrompt>({work, 3}));
            return 0;
        }

        if (trigger & 0x20) {
            Write8(work + wCursor, uint8_t(Read8(work + wCursorCount) - 1));
            Se(game, 1);
            return 0;
        }

        if (game.Call<RewardsAny>({work, 3}) == 1)
            MenuLeftRight(game, work);

        return 0;
    }

    case 3: {
        if (trigger & 0x10) {
            // The last row sends the marked records to the box.
            if (int32_t(Read8(work + wRow)) == int32_t(Read8(work + wRows)) - 1) {
                Se(game, TakenToBox(cpu, work) == 1 ? 8 : 2);

                if (!game.Call<RewardsAny>({work, 3})) {
                    Write8(work + wStep, 2);
                    Write8(work + wCursor, uint8_t(Read8(work + wCursorCount) - 1));
                }

                return 0;
            }

            const uint32_t record = game.Call<RewardsRecordAt>({work, 3});

            if (!record || !Read8(record)) {
                Se(game, 2);
                return 0;
            }

            const uint32_t index = (Read8(work + wColumn) + Read8(work + wRow) * 8) & 0xFFFF;
            Se(game, game.Call<RewardMarkTaken>({work, index}) == 1 ? 0 : 2);
            return 0;
        }

        if (trigger & 0x20) {
            Write8(work + wStep, 2);
            Se(game, 1);
            return 0;
        }

        game.Call<RewardsCursor>({work, kPad, 3});
        return 0;
    }

    case 4:
        return LeaveRest(game, work, 3, 2);

    default:
        return 0;
    }
}

// 0x80398FE0: a step of the reward pages; 1 when the page is done.
uint32_t RewardPages(CpuContext* cpu, uint32_t work) {
    GuestCall game{cpu};
    const uint32_t user = UserData();
    const uint32_t ticks = GameStep::Ticks();
    const int32_t left = int32_t(Read32(work + wCountdown)) - int32_t(ticks);
    const uint32_t page = Read32(work + wPage);

    Write32(work + wCountdown, uint32_t(left));

    // Time is up: what is left is sold (an item list's equipment kept, the
    // records built first if they were not yet).
    if (left < 1) {
        if (page == 1) {
            game.Call<RewardsSell>({work, game.Call<ResultListKind>({work}) == 1 ? 2u : 0u});
            return 1;
        }

        if (page == 2) {
            game.Call<RewardsSell>({work, 1});
            Se(game, 9);
        } else if (page != 3) {
            return 1;
        }

        if (Read8(work + wStep) < 2)
            Build(work);

        TakenToBox(cpu, work);
        game.Call<RewardsSell>({work, 3});
        return 1;
    }

    switch (page) {
    case 0:
        return 0;

    case 1:
    case 2:
        return ListPage(cpu, work, page, ticks);

    case 3:
        return EquipPage(cpu, work, ticks);

    // Money.
    case 5:
        Write16(work + wScroll, 0);
        MenuUpDown(game, work);
        break;

    // Points: their sound once if they grew, 40 ticks in.
    case 6:
        for (uint32_t tick = 0; tick < ticks && Read16(work + wPointsWait) < 40; ++tick) {
            if (!Read16(work + wPointsWait) && Read16(user + kPointsTotal) > Read16(work + wPointsBefore)) {
                game.Call<ResultSe>({4});
                game.Call<ResultRowSe>({work});
            }

            Write16(work + wPointsWait, uint16_t(Read16(work + wPointsWait) + 1));
        }

        break;

    // Records: the row under the cursor shows over 7 ticks, a flagged one
    // with its sounds; then the cursor moves.
    case 7: {
        Write16(work + wScroll, 0);
        const uint32_t row = Read8(work + wCursor);
        const uint32_t shown = work + wRowShown + row;
        const bool flagged = Read8(work + kRowFlags + row * 16) != 0;
        const uint32_t start = Read8(shown);

        for (uint32_t tick = 0; tick < ticks; ++tick) {
            const uint32_t at = Read8(shown);

            if (at < 7) {
                if (flagged && !at)
                    game.Call<ResultRowSe>({work});

                Write8(shown, uint8_t(at + 1));
            } else if (flagged && at == 7) {
                game.Call<ResultSe>({2});
                Write8(shown, uint8_t(at + 1));
            }
        }

        if (start >= 7)
            MenuUpDown(game, work);

        break;
    }

    // Rows one at a time: a press moves on after 10 ticks.
    case 8: {
        const int32_t delay = int16_t(Read16(work + wDelay));

        if (delay > 0)
            Write16(work + wDelay, uint16_t(std::max<int32_t>(delay - int32_t(ticks), 0)));

        if (int16_t(Read16(work + wDelay)) > 0 || !(Trigger() & 0x10))
            return 0;

        Se(game, 0);
        const uint32_t next = (Read8(work + wCursor) + 1) & 0xFF;
        Write8(work + wCursor, uint8_t(next));

        if (next >= Read8(work + wCursorCount))
            return 1;

        Write16(work + wDelay, 10);
        game.Call<ResultRowSe>({work});
        Write8(work + wNextRow, uint8_t(game.Call<ResultNextRow>({work})));
        return 0;
    }

    default:
        break;
    }

    if (Trigger() & 0x10) {
        Se(game, 0);
        return 1;
    }

    return 0;
}

[[maybe_unused]] void PickSkillPort(CpuContext* cpu) {
    cpu->gpr[3] = PickSkill(cpu->gpr[3], int16_t(cpu->gpr[4]));
}

[[maybe_unused]] void GenerateTalismanPort(CpuContext* cpu) {
    GenerateTalisman(cpu->gpr[3], cpu->gpr[4]);
}

[[maybe_unused]] void PickShardEquipPort(CpuContext* cpu) {
    PickShardEquip(cpu->gpr[3], cpu->gpr[4]);
}

[[maybe_unused]] void GenerateEquipPort(CpuContext* cpu) {
    GenerateEquip(cpu->gpr[3], cpu->gpr[4]);
}

[[maybe_unused]] void BuildPort(CpuContext* cpu) {
    Build(cpu->gpr[3]);
}

[[maybe_unused]] void SkillsPort(CpuContext* cpu) {
    cpu->gpr[3] = Skills(cpu->gpr[3], cpu->gpr[4], cpu->gpr[5]);
}

void TakenToBoxPort(CpuContext* cpu) {
    cpu->gpr[3] = TakenToBox(cpu, cpu->gpr[3]);
}

[[maybe_unused]] void AnimatePort(CpuContext* cpu) {
    Animate(cpu->gpr[3], GameStep::Ticks());
}

void RewardPagesPort(CpuContext* cpu) {
    cpu->gpr[3] = RewardPages(cpu, cpu->gpr[3]);
}

} // namespace

// Precise natives: translated callers call them directly.
extern "C" void func_8027F6D8(CpuContext* ctx);
extern "C" void MH3_TalismanSkillPick(CpuContext* ctx) {
#ifdef MH3_DEVELOPMENT
    if (PortCheck::RunPrecise("TalismanSkillPick", ctx->gpr[1], func_8027F6D8, PickSkillPort)) [[unlikely]]
        return;
#endif
    ctx->gpr[3] = PickSkill(ctx->gpr[3], int16_t(ctx->gpr[4]));
}
REGISTER_NATIVE_FUNCTION_AS(0x8027F6D8, MH3_TalismanSkillPick, "TalismanSkillPick");

extern "C" void func_8027F7DC(CpuContext* ctx);
extern "C" void MH3_TalismanGenerate(CpuContext* ctx) {
#ifdef MH3_DEVELOPMENT
    if (PortCheck::RunPrecise("TalismanGenerate", ctx->gpr[1], func_8027F7DC, GenerateTalismanPort)) [[unlikely]]
        return;
#endif
    GenerateTalisman(ctx->gpr[3], ctx->gpr[4]);
}
REGISTER_NATIVE_FUNCTION_AS(0x8027F7DC, MH3_TalismanGenerate, "TalismanGenerate");

extern "C" void func_8027FA5C(CpuContext* ctx);
extern "C" void MH3_RewardEquipPick(CpuContext* ctx) {
#ifdef MH3_DEVELOPMENT
    if (PortCheck::RunPrecise("RewardEquipPick", ctx->gpr[1], func_8027FA5C, PickShardEquipPort)) [[unlikely]]
        return;
#endif
    PickShardEquip(ctx->gpr[3], ctx->gpr[4]);
}
REGISTER_NATIVE_FUNCTION_AS(0x8027FA5C, MH3_RewardEquipPick, "RewardEquipPick");

extern "C" void func_8027FB28(CpuContext* ctx);
extern "C" void MH3_RewardEquipGenerate(CpuContext* ctx) {
#ifdef MH3_DEVELOPMENT
    if (PortCheck::RunPrecise("RewardEquipGenerate", ctx->gpr[1], func_8027FB28, GenerateEquipPort)) [[unlikely]]
        return;
#endif
    GenerateEquip(ctx->gpr[3], ctx->gpr[4]);
}
REGISTER_NATIVE_FUNCTION_AS(0x8027FB28, MH3_RewardEquipGenerate, "RewardEquipGenerate");

extern "C" void func_8039899C(CpuContext* ctx);
extern "C" void MH3_RewardEquipBuild(CpuContext* ctx) {
#ifdef MH3_DEVELOPMENT
    if (PortCheck::RunPrecise("RewardEquipBuild", ctx->gpr[1], func_8039899C, BuildPort)) [[unlikely]]
        return;
#endif
    Build(ctx->gpr[3]);
}
REGISTER_NATIVE_FUNCTION_AS(0x8039899C, MH3_RewardEquipBuild, "RewardEquipBuild");

extern "C" void func_803191DC(CpuContext* ctx);
extern "C" void MH3_TalismanSkills(CpuContext* ctx) {
#ifdef MH3_DEVELOPMENT
    if (PortCheck::RunPrecise("TalismanSkills", ctx->gpr[1], func_803191DC, SkillsPort)) [[unlikely]]
        return;
#endif
    ctx->gpr[3] = Skills(ctx->gpr[3], ctx->gpr[4], ctx->gpr[5]);
}
REGISTER_NATIVE_FUNCTION_AS(0x803191DC, MH3_TalismanSkills, "TalismanSkills");

extern "C" void func_80398EAC(CpuContext* ctx);
extern "C" void MH3_RewardEquipToBox(CpuContext* ctx) {
    PortCheck::Call(ctx, "RewardEquipToBox", func_80398EAC, TakenToBoxPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80398EAC, MH3_RewardEquipToBox, "RewardEquipToBox");

extern "C" void func_80398264(CpuContext* ctx);
extern "C" void MH3_RewardsAnimate(CpuContext* ctx) {
#ifdef MH3_DEVELOPMENT
    if (PortCheck::RunPrecise("RewardsAnimate", ctx->gpr[1], func_80398264, AnimatePort)) [[unlikely]]
        return;
#endif
    Animate(ctx->gpr[3], GameStep::Ticks());
}
REGISTER_NATIVE_FUNCTION_AS(0x80398264, MH3_RewardsAnimate, "RewardsAnimate");

extern "C" void func_80398FE0(CpuContext* ctx);
extern "C" void MH3_RewardPages(CpuContext* ctx) {
    PortCheck::Call(ctx, "RewardPages", func_80398FE0, RewardPagesPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80398FE0, MH3_RewardPages, "RewardPages");
