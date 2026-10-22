// The quest's end: its rewards and the result screen, RMHP08. C
// reconstructions from the translated code.
//
// The reward lists the quest's end fills (0x806C79D8: 48 items of {u16 item,
// s16 count} for the main box, 0x806C7A98 for the second; 0x806C7B58 and
// 0x806C7C18 hold three monsters' break/capture rows of 12 before they are
// moved in), the quest result work (0x8066C668, get_qResult_work) and the
// result screen's work (its +0x3070 / +0x3130 the boxes' copies).
//
//   0x803B23B8 QuestResultSetup(field): the result work from the quest work
//              (+0x1E2 1 cleared, 2 failed, 3 other), the main rewards (by
//              ResultListKind / 0x02000000 / the marathon flag: 0x803ACEF4,
//              0x803ABBD8, 0x803ABD40 or 0x803AB978) and, for a cleared quest
//              but not with the marathon flag, the break (0x803ABEA8) and
//              capture (0x803AC714) rewards; the extra monsters' (0x803ACD74)
//   0x803AB978 / 0x803ABBD8 / 0x803ABD40 / 0x803ACEF4 (hunter, result): the
//              main reward rows (the objectives' reward tables +0x9C, +0xC8,
//              subs +0xFC and +0x15C)
//   0x803ABEA8 BreakRewards(quest work): each slot's monster's parts broken
//              (+0x6803 + 8 * monster + part, at most 5) give rows from the
//              part's table (0x807970EC, or 0x807970F0 high rank), up to four
//              per monster, the first two slots' into the main box at 16 and
//              32; then every other monster's into the second box
//   0x803AC714 CaptureRewards(quest work, hunter): each slot's monster
//              captured (+0x4E2 + 2 * monster) gives 2 rows (3 with the skill
//              0xBD, one more for more than one) from 0x807970F4 / 0x807970F8,
//              at 28 and 44; the others' into the second box
//   0x803ACD74 ExtraRewards(quest work, hunter): the extra monsters killed
//              (+0x6980 by slot) give rows into the second box
//   0x803AB420 RewardsCopy(main, second): the lists into the result screen
//   0x803975D0 QuestResultRecords(screen): the money and points (the main
//              reward, the subs', the extra monsters'), the arena records,
//              the reward boxes and the page order; then the pages
//   0x80396AF4 (screen, result): the new size records (+0x3BC0 of the save)
//   0x80396C7C ResultListKind(): the arena flag 0x10
//   0x80396CC0 (): 0x02000000 / 0x100 or the timed flag: six rows a page
//   0x80396F38 / 0x80396F68 (): the result work's +0x1E2 is 2 / 3
//   0x80397160 (screen, page): the next page that has something
//   0x8039729C (screen): the reward box's cursor and grid reset
//   0x803973A8 RewardPageStart(screen, page)
//   0x803982B8 RewardsAny(screen, box): a box has an item
//   0x8039A890 / 0x8039AD2C (screen, ...): the box title and grid drawn
//   0x8036C9EC (): the online quest kind (0 none, 1 field stop mode or
//              no quest picked, 2 picked, 3..6 by the flags)
//   0x8036CEDC (): online, the quest's goal met
#include "abi_bridge.h"
#include "cheats.h"
#include "guest_call.h"
#include "port_check.h"
#include "ppc_runtime.h"
#include "quest_work.h"

#include <cstdint>

namespace {
using MemoryInline::FlatRead16;
using MemoryInline::FlatRead32;
using MemoryInline::FlatRead8;
using MemoryInline::FlatWrite16;
using MemoryInline::FlatWrite32;
using MemoryInline::FlatWrite8;
using QuestWork::Work;

constexpr uint32_t kMainRewards = 0x806C79D8, kSecondRewards = 0x806C7A98;
constexpr uint32_t kSlotRows = 0x806C7B58, kOtherRows = 0x806C7C18;
constexpr uint32_t kResult = 0x8066C668; // get_qResult_work, inlined
constexpr uint32_t kUserData = 0x8065B3BC; // system_w +0x95C: the save data
constexpr uint32_t kSavePointer = 0x80796D40; // r13 - 0x45A0
constexpr uint32_t kSystem = 0x8065AA60;
constexpr uint32_t kFrameRate = 0x8065B4D0;

enum : uint32_t {
    ObjectiveDone = 0x803AA0C4,
    Flag02000000Or100 = 0x803B4CC8,
    TimedFlag = 0x803B4D4C,
    MarathonFlag = 0x803B4DC4,
    ExtraFlag = 0x803B4E3C,
    ArenaFlag = 0x803B5094,
};

// get_userdata, inlined: the save data, also kept at r13 - 0x45A0.
uint32_t UserData() {
    const uint32_t save = FlatRead32(kUserData);
    FlatWrite32(kSavePointer, save);
    return save;
}

void RewardsCopy(uint32_t main, uint32_t second) {
    for (uint32_t i = 0; i < 48; ++i) {
        FlatWrite16(main + i * 4, FlatRead16(kMainRewards + i * 4));
        FlatWrite16(main + i * 4 + 2, FlatRead16(kMainRewards + i * 4 + 2));
        FlatWrite16(second + i * 4, FlatRead16(kSecondRewards + i * 4));
        FlatWrite16(second + i * 4 + 2, FlatRead16(kSecondRewards + i * 4 + 2));
    }
}

uint32_t ResultListKind(GuestCall& game) {
    return game.Call<ArenaFlag>({0}) == 1;
}

uint32_t SixRows(GuestCall& game) {
    return game.Call<Flag02000000Or100>({0}) == 1 || game.Call<TimedFlag>({0}) == 1;
}

uint32_t ResultIs(uint32_t value) {
    return FlatRead8(kResult + 0x1E2) == value;
}

// 0x80396AF4: the monsters' new smallest and biggest sizes (rows at +0x60).
void SizeRecords(GuestCall& game, uint32_t screen, uint32_t result) {
    const uint32_t save = UserData();
    FlatWrite32(screen + 256, 0);
    const uint32_t kinds = FlatRead32(0x806AFAFC);

    for (uint32_t kind = 0; kind < 41; ++kind) {
        const uint32_t at = result + kind * 4;

        if (int16_t(FlatRead16(at + 164)) == 0)
            continue;

        const int32_t index = int8_t(game.Call<0x8034C3B4>({kind}));

        if (index < 0 || FlatRead8(kinds + uint32_t(index) * 4 + 1) != 1)
            continue;

        const uint32_t row = screen + FlatRead32(screen + 256) * 16, best = save + kind * 4;
        const uint32_t smallest = FlatRead16(best + 15296);
        FlatWrite8(row + 96, smallest == 0 || FlatRead16(at + 164) < smallest ? 1 : 0);
        FlatWrite8(row + 97, uint8_t(kind));
        FlatWrite16(row + 100, FlatRead16(at + 164));

        if (FlatRead16(at + 166) > FlatRead16(best + 15298))
            FlatWrite8(row + 96, FlatRead8(row + 96) | 2);

        FlatWrite8(row + 97, uint8_t(kind));
        FlatWrite16(row + 102, FlatRead16(at + 166));
        FlatWrite32(screen + 256, FlatRead32(screen + 256) + 1);
    }
}

uint32_t NextPage(GuestCall& game, uint32_t screen, uint32_t page) {
    for (;;) {
        switch (page & 0xFF) {
        case 0:
            page = ResultIs(2) ? 3 : ResultIs(3) ? 8 : 1;
            break;
        case 1:
            page = 2;
            break;
        case 2:
            page = 3;
            break;
        case 3:
            page = 5;
            break;
        case 5:
            page = 6;
            break;
        case 6:
            page = 7;
            break;
        case 7:
            if (!ResultIs(3)) {
                game.Call<0x8004E538>({kResult + 164, 0});
                game.Call<0x8004E538>({kResult + 166, 1});
            }

            page = 8;
            break;
        default:
            return 0;
        }

        if (game.Call<0x80396F98>({screen, page}) != 0) {
            FlatWrite32(screen + 12272, page);
            return 1;
        }
    }
}

void RewardBoxReset(GuestCall& game, uint32_t screen) {
    const uint32_t save = UserData();

    for (const uint32_t at : {12326u, 12323u, 12322u, 12328u, 12329u})
        FlatWrite8(screen + at, 0);

    FlatWrite16(screen + 12340, 0);
    FlatWrite16(screen + 12338, 0);
    FlatWrite8(screen + 12332, 0);
    FlatWrite32(screen + 12352, 0);
    FlatWrite8(screen + 12330, 0);
    FlatWrite8(screen + 12331, 0);
    FlatWrite16(screen + 12342, 0xFFFF);
    FlatWrite16(screen + 12356, 0);
    FlatWrite16(screen + 12358, 0);
    FlatWrite16(screen + 12324, 0);
    FlatWrite8(screen + 12333, 0);
    FlatWrite8(screen + 12335, 0);
    FlatWrite32(screen + 12360, kResult + 340);
    int32_t cells = 24;

    if (game.Call<0x8004AF24>({save}) == 1) {
        FlatWrite32(screen + 12364, kResult + 436);
        cells = 32;
    } else {
        FlatWrite32(screen + 12364, 0);
    }

    // 0x802A8F78, inlined: pages of 8, rounded up.
    int32_t pages = int16_t(cells / 8);

    if (cells - pages * 8 != 0)
        pages = int16_t(pages + 1);

    FlatWrite8(screen + 12334, uint8_t(pages));
    FlatWrite16(screen + 12344, 0);
    FlatWrite16(screen + 12346, 0);
    FlatWrite16(screen + 12348, 0);
}

// 0x80397384, inlined: the box's started flag.
void StartBox(uint32_t screen) {
    if (FlatRead8(screen + 12327) == 0)
        FlatWrite8(screen + 12327, 1);
}

void RewardPageStart(GuestCall& game, uint32_t screen, uint32_t page) {
    const uint32_t save = UserData();
    FlatWrite32(screen + 12272, page);
    FlatWrite8(screen + 2, 0);
    FlatWrite8(screen + 3, 0);
    FlatWrite32(screen + 12276, uint32_t(int32_t(Memory::ReadFloat32(0x8079E7C0) * Memory::ReadFloat32(kFrameRate))));
    FlatWrite8(screen + 12280, 0);

    switch (int32_t(page)) {
    case 1:
        game.Call<0x8039729C>({screen});
        FlatWrite8(screen + 12323, 3);

        if (game.Call<0x80396C7C>({screen}) == 1)
            FlatWrite8(screen + 12332, 2);
        else
            FlatWrite8(screen + 12332, game.Call<0x80396CC0>({screen}) == 1 ? 6 : 5);

        break;
    case 2:
        game.Call<0x8039729C>({screen});
        FlatWrite8(screen + 12323, 3);
        FlatWrite8(screen + 12332, 6);
        break;
    case 3:
        game.Call<0x8039729C>({screen});
        FlatWrite8(screen + 12323, 1);
        FlatWrite8(screen + 12332, 6);
        FlatWrite8(screen + 12337, 0);
        break;
    case 5:
        StartBox(screen);
        game.Call<0x8004D14C>({FlatRead32(screen + 88) + (FlatRead32(screen + 80) + FlatRead32(screen + 72)), FlatRead32(kSavePointer) + 24}); // MoneyAdd
        FlatWrite8(screen + 12322, 0);
        FlatWrite8(screen + 12323, 4);
        break;
    case 6:
        StartBox(screen);
        FlatWrite16(screen + 12380, FlatRead16(save + 15844));
        FlatWrite32(screen + 12376, game.Call<0x8004E488>({FlatRead32(screen + 76)}));
        FlatWrite16(screen + 12382, 0);
        break;
    case 7:
        StartBox(screen);

        for (uint32_t i = 12385; i <= 12394; ++i)
            FlatWrite8(screen + i, 0);

        FlatWrite8(screen + 12322, 0);
        FlatWrite8(screen + 12323, uint8_t(FlatRead32(screen + 256)));
        break;
    case 8:
        StartBox(screen);
        game.Call<0x8039739C>({screen}); // ResultRowSe
        FlatWrite8(screen + 12323, FlatRead8(screen + 12398));
        FlatWrite8(screen + 12322, 0);
        FlatWrite8(screen + 12399, uint8_t(game.Call<0x80396D10>({screen}))); // ResultNextRow
        FlatWrite16(screen + 12320, 10);
        break;
    default:
        break;
    }
}

uint32_t RewardsAny(uint32_t screen, uint32_t box) {
    uint32_t items = 0, extra = 0;

    if ((box & 0xFF) == 0 || (box & 0xFF) == 2)
        items = screen + 0x3070;
    else if ((box & 0xFF) == 1)
        items = screen + 0x3130;
    else if ((box & 0xFF) == 3)
        extra = screen + 0x31F0;
    else
        return 0;

    for (uint32_t i = 0; items && i < 48; ++i)
        if (FlatRead16(items + i * 4) != 0 && int16_t(FlatRead16(items + i * 4 + 2)) > 0)
            return 1;

    for (uint32_t i = 0; extra && i < 40; ++i)
        if (FlatRead8(extra + i * 12) != 0)
            return 1;

    return 0;
}

void BoxTitleDraw(GuestCall& game, uint32_t sp, uint32_t screen, uint32_t box) {
    game.Call<0x802E0B38>({sp + 12, 4619, FlatRead8(screen + 12326) & 0xFFFFu, sp + 8});
    uint32_t sprite, anim;

    if ((box & 0xFF) == 1) {
        sprite = 0x80795980, anim = 1;
    } else if ((box & 0xFF) == 2) {
        sprite = 0x80795988, anim = 2;
    } else {
        sprite = game.Call<0x80396CC0>({screen}) == 1 ? 0x80795980 : 0x80795974;
        anim = 0;
    }

    game.Call<0x802E09A8>({sprite, sp + 8});
    game.Call<0x802E0E70>({0x80795990, anim & 0xFFFF, sp + 8});
}

void BoxGridDraw(GuestCall& game, uint32_t sp, uint32_t screen, uint32_t box, uint32_t cursor) {
    uint32_t items = 0, extras = 0, kind = 0, layout;
    const uint32_t chosen = 0;
    uint32_t chosenFlag = 0;
    game.Call<0x802E0B38>({sp + 20, 4619, FlatRead8(screen + 12326) & 0xFFFFu, sp + 16});
    uint32_t rows = FlatRead8(screen + 12332);
    (void)chosen;

    if ((box & 0xFF) == 1) {
        layout = 0x805F1D90;
        items = screen + 12592;
    } else if ((box & 0xFF) == 3) {
        layout = 0x805F1D9C;

        if (FlatRead8(screen + 2) == 0) {
            kind = 1;
        } else if (FlatRead8(screen + 2) == 1) {
            kind = 2;
            chosenFlag = 1;
        } else {
            kind = 3;
        }

        if (FlatRead8(screen + 12337) != 0)
            extras = screen + 12784;

        rows = rows - 1;
    } else {
        layout = game.Call<0x80396CC0>({screen}) == 1 ? 0x805F1D90 : 0x805F1D84;
        items = screen + 12400;
    }

    const uint32_t picked = FlatRead16(screen + 12342) != 0xFFFF;
    uint32_t index = 0;

    for (uint32_t row = 0; (row & 0xFF) < (rows & 0xFF); ++row) {
        game.Call<0x802E05B4>({FlatRead16(layout + (row & 0xFF) * 2), sp + 12});
        FlatWrite16(sp + 12, uint16_t(int16_t(FlatRead16(sp + 12)) + int16_t(FlatRead16(sp + 16))));
        FlatWrite16(sp + 14, uint16_t(int16_t(FlatRead16(sp + 14)) + int16_t(FlatRead16(sp + 18))));
        FlatWrite16(sp + 10, FlatRead16(sp + 14));

        for (uint32_t col = 0; col < 8; ++col, ++index) {
            FlatWrite16(sp + 8, uint16_t(int16_t(FlatRead16(sp + 12)) + int32_t(col * 31)));
            uint32_t marked = 0, over = 0, sel = 0;

            if (cursor == 1 && FlatRead8(screen + 12328) == col && FlatRead8(screen + 12329) == (row & 0xFF))
                over = 1;

            if (picked == 1 && FlatRead8(screen + 12330) == col && FlatRead8(screen + 12331) == (row & 0xFF))
                sel = 1;

            if (FlatRead8(screen + 12337) == 0 && (kind == 1 || kind == 2))
                items = game.Call<0x80398A48>({screen, index & 0xFFFF});

            if (items) {
                const uint32_t item = kind == 0 ? items + (index & 0xFFFF) * 4 : items;
                game.Call<0x8039ABCC>({screen, sp + 8, FlatRead16(item), over, 0, sel, chosenFlag});
            } else if (extras) {
                if (FlatRead16(screen + 12344 + (int32_t(index & 0xFFFF) >> 4) * 2) & (1u << (index & 15)))
                    marked = 1;

                game.Call<0x8039AC7C>({screen, sp + 8, extras + (index & 0xFFFF) * 12, over, marked, sel, chosenFlag});
            }
        }
    }
}

uint32_t OnlineQuestKind(GuestCall& game) {
    const uint8_t mode = FlatRead8(kSystem + 36);

    if (mode == 2)
        return FlatRead16(0x8065B534) == 0 ? 1 : 2;

    if (mode != 1)
        return 0;

    if (game.Call<0x800CF2E4>() == 1)
        return 1;

    if (game.Call<0x803B4F2C>({0}) == 1)
        return 6;

    if (game.Call<ArenaFlag>({0}) == 1)
        return 5;

    if (game.Call<Flag02000000Or100>({0}) == 1 || game.Call<TimedFlag>({0}) == 1 || game.Call<MarathonFlag>({0}) == 1)
        return 4;

    return 3;
}

uint32_t OnlineGoalMet(GuestCall& game) {
    const uint32_t work = game.Call<0x803A880C>();

    if (!work)
        return 0;

    if (game.Call<0x803B0D60>() == 1)
        return 1;

    if (game.Call<Flag02000000Or100>({0}) == 1 || game.Call<TimedFlag>({0}) == 1)
        return game.Call<ObjectiveDone>({work, 0, 1}) == 1 && game.Call<ObjectiveDone>({work, 1, 1}) == 1;

    return game.Call<0x803AB1F4>({game.Call<MarathonFlag>({0}) == 1 ? 2u : 0u}) == 1;
}
} // namespace

namespace {
constexpr uint32_t kBreakParts = 0x807970E8;  // r13 - 0x41F8: per monster {part, table} pairs
constexpr uint32_t kBreakTables = 0x807970EC; // r13 - 0x41F4 (low rank), 0x807970F0 high
constexpr uint32_t kCaptureTables = 0x807970F4; // r13 - 0x41EC (low rank), 0x807970F8 high
constexpr uint32_t kRowKinds = 0x805F8280;    // by count (at most 5): u8 +1 rolled rows, +2 fixed rows

// The row kinds of a reward line into `buffer` (12 bytes): `rolled` 0x20,
// then `fixed` 0x16, then 0 up to 12.
void RowKinds(uint32_t buffer, uint32_t rolled, uint32_t fixed) {
    uint32_t at = 0;

    for (; at < rolled; ++at)
        FlatWrite8(buffer + at, 0x20);

    for (; at < rolled + fixed; ++at)
        FlatWrite8(buffer + at, 0x16);

    for (; at < 12; ++at)
        FlatWrite8(buffer + at, 0);
}

// The probability total of a reward table ({u16 item, u8 count, u8 chance}
// until 0).
uint32_t TableTotal(uint32_t table) {
    uint32_t total = 0;

    for (; FlatRead16(table) != 0; table += 4)
        total = (total + FlatRead8(table + 3)) & 0xFFFF;

    return total;
}

// One reward line: rows rolled (0x803AB78C) and fixed (0x803AB870) from
// `table` into `rows`.
void RewardLine(GuestCall& game, uint32_t buffer, uint32_t table, uint32_t rows, uint32_t at, uint32_t line, uint32_t rolled, uint32_t fixed, uint32_t total) {
    uint32_t done = 0;

    if (int32_t(rolled) > 0)
        done = game.Call<0x803AB78C>({buffer, table, rows + at, rolled, total & 0xFFFF});

    if (int32_t(fixed) > 0)
        game.Call<0x803AB870>({buffer + done, table, rows + (done + line) * 4, fixed, total & 0xFFFF});
}

// The rows of three monsters into a list from `first`, 12 apart in the
// list's 48... (copied 16 / 12 apart).
void MoveRows(uint32_t from, uint32_t to, uint32_t start, uint32_t step, uint32_t count, uint32_t perLine) {
    uint32_t index = start;

    for (uint32_t slot = 0; slot < count; ++slot, from += 0x40, index += step) {
        uint32_t taken = 0;

        for (uint32_t i = 0; i < perLine; ++i)
            if (FlatRead16(from + (perLine == 4 ? 0x30 : 0) + i * 4) != 0) {
                // 0x8004A25C, inlined: the item and its count.
                FlatWrite16(to + (index + taken) * 4, FlatRead16(from + (perLine == 4 ? 0x30 : 0) + i * 4));
                FlatWrite16(to + (index + taken) * 4 + 2, FlatRead16(from + (perLine == 4 ? 0x30 : 0) + i * 4 + 2));
                ++taken;
            }
    }
}

// A monster's broken parts: up to four lines of its parts (entries of the
// part table, {u8 part, u8 table}), the counts cleared.
void PartLines(GuestCall& game, uint32_t work, uint32_t buffer, uint32_t monster, uint32_t parts, uint32_t rows, uint32_t rank, bool ceadeus) {
    uint32_t lines = 0, at = 0, line = 0;

    for (uint32_t entry = parts; FlatRead8(entry) != 0 && lines < 4; entry += 2) {
        const uint32_t part = FlatRead8(entry);
        const uint32_t counter = work + monster * 8 + part + 0x6803;
        uint32_t count = FlatRead8(counter), table = 0, rolled = 0, fixed = 0, broken = 0;

        if (count != 0) {
            table = FlatRead8(entry + 1);

            if (count > 5)
                count = 5;

            rolled = FlatRead8(kRowKinds + count * 3 + 1);
            fixed = FlatRead8(kRowKinds + count * 3 + 2);
            RowKinds(buffer, rolled, fixed);
            FlatWrite8(counter, 0);
            broken = count;
        }

        if (!table || !broken)
            continue;

        // Ceadeus (20): each of four parts gives its line once a quest.
        if (ceadeus) {
            static constexpr uint8_t kBits[5] = {0, 2, 8, 1, 4}; // by part 1..4
            const uint32_t bit = part <= 4 ? kBits[part] : 0;

            if (bit) {
                if (FlatRead32(work + 0x6AAC) & bit)
                    continue;

                FlatWrite32(work + 0x6AAC, FlatRead32(work + 0x6AAC) | bit);
            }
        }

        const uint32_t list = FlatRead32(FlatRead32(rank == 0 ? kBreakTables : kBreakTables + 4) + table * 4 - 4);
        const uint32_t total = TableTotal(list);

        if (total == 0)
            continue;

        RewardLine(game, buffer, list, rows, at, line, rolled, fixed, total);
        at += 12, line += 3, ++lines;
    }
}

void BreakRewards(GuestCall& game, uint32_t work, uint32_t buffer) {
    const uint32_t rank = game.Call<0x803B421C>({0}) & 0xFF;

    if (rank == 2)
        return;

    // The Ceadeus parts broken already in an earlier try don't count again.
    if (FlatRead8(work + 27300) == 1) {
        const uint32_t done = FlatRead32(work + 27308);

        if (done & 1)
            FlatWrite8(work + 26790, 0);

        if (FlatRead32(work + 27308) & 2)
            FlatWrite8(work + 26788, 0);

        if (FlatRead32(work + 27308) & 4)
            FlatWrite8(work + 26791, 0);

        if (FlatRead32(work + 27308) & 8)
            FlatWrite8(work + 26789, 0);
    }

    uint32_t slot = FlatRead32(work + 0x3C) + 0x314;

    if (slot) {
        for (uint32_t i = 0; i < 3; ++i, slot += 8) {
            if (game.Call<ExtraFlag>({0}) == 1 && i == 2)
                break;

            const uint32_t monster = FlatRead8(slot);
            const uint32_t parts = monster ? FlatRead32(FlatRead32(kBreakParts) + monster * 4) : 0;

            if (parts)
                PartLines(game, work, buffer, monster, parts, kSlotRows + i * 0x40, rank, monster == 0x14);
        }
    }

    // Every other monster's parts into the second box.
    uint32_t rows = kOtherRows;

    for (uint32_t monster = 0; monster < 41; ++monster) {
        const uint32_t parts = FlatRead32(FlatRead32(kBreakParts) + monster * 4);

        if (!parts)
            continue;

        const uint32_t before = FlatRead16(rows);
        uint32_t lines = 0, at = 0, line = 0;

        for (uint32_t entry = parts; FlatRead8(entry) != 0 && lines < 4; entry += 2) {
            const uint32_t counter = work + monster * 8 + FlatRead8(entry) + 0x6803;
            uint32_t count = FlatRead8(counter), table = 0, rolled = 0, fixed = 0;

            if (count != 0) {
                table = FlatRead8(entry + 1);

                if (count > 5)
                    count = 5;

                rolled = FlatRead8(kRowKinds + count * 3 + 1);
                fixed = FlatRead8(kRowKinds + count * 3 + 2);

                // The fill stops short of the zeros here (0 at the start
                // only when the kinds leave room).
                for (uint32_t b = 0; b < rolled; ++b)
                    FlatWrite8(buffer + b, 0x20);

                for (uint32_t b = rolled; b < rolled + fixed; ++b)
                    FlatWrite8(buffer + b, 0x16);

                if (rolled + fixed < 12)
                    FlatWrite8(buffer, 0);

                FlatWrite8(counter, 0);
            }

            if (!table || !count)
                continue;

            const uint32_t list = FlatRead32(FlatRead32(rank == 0 ? kBreakTables : kBreakTables + 4) + table * 4 - 4);
            const uint32_t total = TableTotal(list);

            if (total == 0)
                continue;

            RewardLine(game, buffer, list, rows, at, line, rolled, fixed, total);
            at += 12, line += 3, ++lines;
        }

        (void)before;

        if (lines > 0)
            rows += 0x40;
    }

    MoveRows(kSlotRows, kMainRewards, 16, 16, 2, 12);
    MoveRows(kOtherRows, kSecondRewards, 16, 12, 2, 12);
}
} // namespace

namespace {
// A capture line: 2 rows (3 with the hunter's skill 0xBD, one more for more
// than one captured), the count cleared.
uint32_t CaptureRows(GuestCall& game, uint32_t hunter, uint32_t buffer, uint32_t count) {
    uint32_t rows = 2;

    if (game.Call<0x80271374>({hunter, 0xBD}) == 1) // Pl_Skill_ck
        rows = 3;

    if (count > 1)
        ++rows;

    RowKinds(buffer, rows, 0);
    return rows;
}

void CaptureRewards(GuestCall& game, uint32_t work, uint32_t hunter, uint32_t buffer) {
    const uint32_t rank = game.Call<0x803B421C>({0}) & 0xFF;

    if (rank == 2)
        return;



    for (uint32_t i = 0; i < 3; ++i) {
        if (game.Call<ExtraFlag>({0}) == 1 && i == 2)
            break;

        const uint32_t slot = FlatRead32(work + 0x3C) + i * 8 + 0x314;
        const uint32_t monster = FlatRead8(slot);
        const uint32_t count = monster ? FlatRead16(work + monster * 2 + 0x4E2) : 0;

        if (!count)
            continue;

        const uint32_t rows = CaptureRows(game, hunter, buffer, count);
        FlatWrite16(work + monster * 2 + 0x4E2, 0);
        const uint32_t table = FlatRead32(FlatRead32(rank == 0 ? kCaptureTables : kCaptureTables + 4) + monster * 4);
        const uint32_t total = TableTotal(table);

        if (total && rows)
            game.Call<0x803AB78C>({buffer, table, kSlotRows + i * 0x40 + 0x30, rows, total});
    }


    uint32_t lines = 0, to = kOtherRows;

    for (uint32_t monster = 0; monster < 41 && lines <= 2; ++monster) {
        const uint32_t count = FlatRead16(work + monster * 2 + 0x4E2);

        if (!count)
            continue;

        const uint32_t rows = CaptureRows(game, hunter, buffer, count);
        FlatWrite16(work + monster * 2 + 0x4E2, 0);
        const uint32_t table = FlatRead32(FlatRead32(rank == 0 ? kCaptureTables : kCaptureTables + 4) + monster * 4);
        const uint32_t total = TableTotal(table);

        if (!total)
            continue;

        if (rows)
            game.Call<0x803AB78C>({buffer, table, to + 0x30, rows, total});

        to += 0x40;
        ++lines;
    }

    MoveRows(kSlotRows, kMainRewards, 0x1C, 16, 2, 4);
    MoveRows(kOtherRows, kSecondRewards, 0x28, 4, 2, 4);
}

void ClearRewardLists(GuestCall& game) {
    for (const uint32_t list : {kMainRewards, kSecondRewards, kSlotRows, kOtherRows})
        game.Call<0x80004350>({list, 0, 192});
}

// The objective's reward table total, or 0.
uint32_t Rolled(uint32_t table) {
    return TableTotal(table);
}

// 0x803AB978: the main rewards, the sub-objectives' too.
void MainRewards(GuestCall& game, uint32_t sp, uint32_t hunter, uint32_t result) {
    game.Call<0x80004000>({sp + 8, 0x805F8270, 16});
    game.Call<0x803AB49C>({hunter, sp + 8, 0});
    const uint32_t work = game.Call<0x803A880C>();
    ClearRewardLists(game);

    if ((result & 0xFF) != 4)
        return;

    if ((FlatRead32(FlatRead32(work + 0x3C) + 0x310) & 0x800000) || game.Call<0x803B5E90>() == 1 || game.Call<ObjectiveDone>({work, 0, 1}) != 0) {
        if (const uint32_t total = Rolled(work + 0x9C))
            game.Call<0x803AB678>({sp + 8, work + 0x9C, kMainRewards, 8, total});

        if (const uint32_t total = Rolled(work + 200))
            game.Call<0x803AB870>({sp + 16, work + 200, kMainRewards + 32, 8, total});
    }

    game.Call<0x80004000>({sp + 8, 0x80795AC0, 4});

    if (game.Call<ObjectiveDone>({work, 1, 1}) != 0)
        if (const uint32_t total = Rolled(work + 0xFC))
            game.Call<0x803AB870>({sp + 8, work + 0xFC, 0x806C7A58, 4, total});

    if (game.Call<ObjectiveDone>({work, 2, 1}) != 0)
        if (const uint32_t total = Rolled(work + 0x15C))
            game.Call<0x803AB870>({sp + 8, work + 0x15C, 0x806C7A68, 4, total});
}

// 0x803ABBD8 (0x02000000) and 0x803ABD40 (marathon): the main reward only.
void MainOnlyRewards(GuestCall& game, uint32_t sp, uint32_t hunter, uint32_t result, uint32_t first, bool rolledBoth) {
    game.Call<0x80004000>({sp + 8, 0x805F8298, 16});
    game.Call<0x803AB49C>({hunter, sp + 8, first});
    const uint32_t work = game.Call<0x803A880C>();
    ClearRewardLists(game);

    if ((result & 0xFF) != 4 || game.Call<ObjectiveDone>({work, 0, 1}) == 0)
        return;

    if (const uint32_t total = Rolled(work + 0x9C))
        game.Call<0x803AB678>({sp + 8, work + 0x9C, kMainRewards, 8, total});

    if (const uint32_t total = Rolled(work + 200)) {
        if (rolledBoth)
            game.Call<0x803AB678>({sp + 16, work + 200, kMainRewards + 32, 8, total});
        else
            game.Call<0x803AB870>({sp + 16, work + 200, kMainRewards + 32, 8, total});
    }
}

// 0x803ACEF4: the arena's rewards, more rows for a better time (+0x434).
void ArenaRewards(GuestCall& game, uint32_t sp, uint32_t result, uint32_t screen) {
    ClearRewardLists(game);

    if ((result & 0xFF) != 4)
        return;

    const uint32_t work = game.Call<0x803A880C>();
    const uint32_t kinds = sp + 8;
    game.Call<0x80004350>({kinds, 0, 8});
    uint32_t rows;

    if (FlatRead8(screen + 0x434) == 0) {
        FlatWrite8(kinds + 1, 0x16);
        FlatWrite8(kinds + 2, 0xC);
        rows = 3;
    } else if (FlatRead8(screen + 0x434) == 1) {
        FlatWrite8(kinds + 1, 0x16);
        FlatWrite8(kinds + 2, 0x16);
        FlatWrite8(kinds + 3, 0xC);
        FlatWrite8(kinds + 4, 0xC);
        rows = 5;
    } else {
        FlatWrite8(kinds + 1, 0x20);
        FlatWrite8(kinds + 2, 0x16);
        FlatWrite8(kinds + 3, 0x16);
        FlatWrite8(kinds + 4, 0x16);
        FlatWrite8(kinds + 5, 0xC);
        FlatWrite8(kinds + 6, 0xC);
        FlatWrite8(kinds + 7, 0xC);
        rows = 8;
    }

    FlatWrite8(kinds, 0x20);

    if (const uint32_t total = Rolled(work + 0x9C))
        game.Call<0x803AB678>({kinds, work + 0x9C, kMainRewards, rows, total});
}

// 0x803ACD74: the extra monsters' rewards into the second box, the bigger
// one first, two lines at most.
void ExtraRewards(GuestCall& game, uint32_t sp, uint32_t work, uint32_t hunter) {
    game.Call<0x80004000>({sp + 16, 0x805F82A8, 16});
    game.Call<0x803AB49C>({hunter, sp + 16, 0});
    const uint32_t field = QuestWork::MoveWork(QuestWork::kField);

    if (!field)
        return;

    FlatWrite8(sp + 8, 0);
    FlatWrite8(sp + 9, 1);

    if (FlatRead8(work + 27008) && FlatRead8(work + 27009) && FlatRead16(field + 8874) > FlatRead16(field + 8890)) {
        FlatWrite8(sp + 8, 1);
        FlatWrite8(sp + 9, 0);
    }

    uint32_t lines = 0, order = sp + 8;

    for (uint32_t j = 0; j < 2; ++j)
        for (int32_t c = 0; c < int32_t(FlatRead8(work + j + 27008)); ++c) {
            const uint32_t table = game.Call<ExtraFlag>({0}) == 1 ? work + 0x15C : game(FlatRead32(0x806CE898), {FlatRead16(work + j * 2 + 27012)});

            if (!table)
                continue;

            const uint32_t total = TableTotal(table);

            if (!total)
                continue;

            game.Call<0x803AB870>({sp + 16, table, kSecondRewards + FlatRead8(order) * 32, 8, total});
            ++order;

            if (++lines >= 2)
                return;
        }
}
} // namespace

namespace {
constexpr uint32_t kMagicSigned = 0x8079E9E8;   // f64 2^52 + 2^31 (r2 - 0x1578)
constexpr uint32_t kMagicUnsigned = 0x8079EA10; // f64 2^52 (r2 - 0x1550)
constexpr uint32_t kSixty = 0x8079E9E4;         // f32 60 (r2 - 0x157C)

// The time the quest took, in frames: the limit less what is left.
uint32_t Elapsed() {
    return FlatRead32(QuestWork::kWork + 28) - FlatRead32(QuestWork::kWork + 32);
}

void QuestResultSetup(GuestCall& game, uint32_t sp, uint32_t field) {
    const uint32_t work = Work();
    FlatWrite16(kResult + 480, FlatRead16(work + 16));
    FlatWrite32(kResult + 484, 0);
    const uint32_t hunters = QuestWork::MoveWork(QuestWork::kHunters);
    const uint32_t hunter = hunters + uint32_t(int32_t(int8_t(FlatRead8(kSystem + 39)))) * 2848;
    const auto say = [&](uint32_t message) {
        game.cpu->cr &= ~0x02000000u;
        game.Call<0x8045E54C>({sp + 8, game.Call<0x803B3414>({message})});
        game.Call<0x8035B660>({0, sp + 8});
    };

    if (FlatRead8(field + 250) != 8)
        say(21);

    if (FlatRead8(kSystem + 2319) == 1) {
        game.Call<0x80004000>({kResult + 340, Work() + 26488, 140});
    } else {
        game.Call<0x803B211C>({hunter + 632, kResult});

        if ((game.Call<0x803B224C>({kResult, hunter + 632}) & 0xFF) != 0 && FlatRead8(field + 250) != 8 && FlatRead8(field + 250) != 7)
            say(28);

        game.Call<0x803B234C>({hunter + 632});
        game.Call<0x80004000>({kResult + 340, hunter + 632, 140});
        game.Call<0x80004000>({kResult + 436, hunter + 736, 32});
    }

    // The time taken: frames, and minutes for the guild card.
    FlatWrite32(kResult + 328, Elapsed());
    const float perMinute = Memory::ReadFloat32(kSixty) * Memory::ReadFloat32(kFrameRate);
    const float minutes = float(double(FlatRead32(kResult + 328)));
    FlatWrite32(kResult + 332, game.Call<0x80457360>({}, {double(float(minutes / perMinute))})); // __cvt_fp2unsigned
    FlatWrite32(kResult + 336, FlatRead32(work + 32));
    FlatWrite16(kResult + 934, uint16_t(game.Call<0x802D8B20>({Work() + 1168, Work() + 1250})));
    const int32_t taken = int32_t(float(int32_t(Elapsed())) / perMinute);
    FlatWrite16(kResult + 936, uint16_t(game.Call<0x802D8D2C>({uint32_t(taken) & 0xFFFF})));
    game.Call<0x802D8E58>({kResult + 938});
    FlatWrite32(kResult + 988, game.Call<0x802D8F0C>({Work() + 1168, Work() + 1250}));
    game.Call<0x80004000>({kResult, Work() + 1168, 82});
    game.Call<0x80004000>({kResult + 82, Work() + 1250, 82});
    game.Call<0x80004000>({kResult + 164, Work() + 1332, 164});
    FlatWrite8(kResult + 1076, 0);

    // An arena record: 2 under the record time, 1 under the second.
    if (game.Call<ArenaFlag>({0}) == 1) {
        const uint32_t index = (game.Call<0x803A8DB0>() - 60000) & 0xFFFF;

        if (index < 12) {
            const uint32_t times = FlatRead32(0x805F82B8 + index * 4);

            if (FlatRead32(kResult + 328) <= FlatRead16(times) * 30u)
                FlatWrite8(kResult + 1076, 2);
            else if (FlatRead32(kResult + 328) <= FlatRead16(times + 2) * 30u)
                FlatWrite8(kResult + 1076, 1);
        }
    }

    if (game.Call<0x803AAF24>() == 1) {
        if ((game.Call<0x803A8DB0>() & 0xFFFF) == 1)
            FlatWrite8(field + 250, (FlatRead32(Work() + 148) & 32) ? 4 : 6); // 0x803AA304, inlined
        else
            FlatWrite8(field + 250, 4);
    } else if (FlatRead8(field + 250) == 4) {
        const uint32_t result = FlatRead8(field + 250);

        if (game.Call<ArenaFlag>({0}) == 1)
            game.Call<0x803ACEF4>({hunter, result, kResult});
        else if (game.Call<0x803B537C>({0}) == 1)
            game.Call<0x803ABBD8>({hunter, result});
        else if (game.Call<MarathonFlag>({0}) == 1)
            game.Call<0x803ABD40>({hunter, result});
        else
            game.Call<0x803AB978>({hunter, result});
    }

    if (FlatRead8(field + 250) == 4) {
        FlatWrite8(kResult + 482, 1);
    } else if (FlatRead8(field + 250) == 6) {
        FlatWrite8(kResult + 482, 2);
    } else {
        FlatWrite8(kResult + 482, 3);
        game.Call<0x80004000>({kResult + 340, Work() + 26488, 140});
        game.Call<0x80004350>({kResult + 938, 0, 48});
        FlatWrite16(kResult + 934, 0);
        FlatWrite16(kResult + 936, 0);
        FlatWrite32(kResult + 988, 0);
    }

    if (FlatRead8(kResult + 482) == 1) {
        // The marathon flag gives no break or capture rewards.
        if (game.Call<MarathonFlag>({0}) == 0) {
            game.Call<0x803ABEA8>({Work()});
            FlatWrite32(kResult + 1000, FlatRead32(Work() + 27308));
            game.Call<0x803AC714>({Work(), hunter});
        }

        game.Call<0x803ACD74>({Work(), hunter});
        const uint32_t main = FlatRead32(Work() + 148);

        if (((main & 1) || (main & 1024)) && FlatRead16(Work() + 152) == 25)
            FlatWrite8(kSystem + 2226, 1);
    }

    FlatWrite8(kResult + 483, FlatRead16(work + 16) < 10000 ? 1 : 3);
    FlatWrite32(kResult + 1008, FlatRead32(0x8065B538));
}

// The money, points and records of a quest's end (0x803975D0, state 0).
uint32_t ResultRecordsStart(GuestCall& game, uint32_t field, uint32_t screen, uint32_t save, uint32_t work) {
    if (game.Call<0x800CEA98>({0, 0}) == 1)
        return 0;

    FlatWrite8(screen, uint8_t(FlatRead8(screen) + 1));
    FlatWrite8(screen + 1, 0);
    FlatWrite8(screen + 6, FlatRead8(kSystem + 39));

    if (FlatRead16(kResult + 480) < 10000)
        FlatWrite8(screen + 7, 1);
    else
        FlatWrite8(screen + 7, FlatRead8(field + 8930) ? FlatRead8(field + 8930) : 1);

    const uint32_t hunter = QuestWork::MoveWork(QuestWork::kHunters) + FlatRead8(screen + 6) * 2848;

    if (FlatRead16(kResult + 480) < 10000 || FlatRead16(save + 15844) < 31)
        FlatWrite8(screen + 94, 0);
    else
        FlatWrite8(screen + 94, (game.Call<0x803B421C>({0}) & 0xFF) == 0 ? 1 : 0);

    FlatWrite16(screen + 12396, 0);

    for (uint32_t i = 16; i < 32; i += 4)
        FlatWrite32(screen + i, 0);

    FlatWrite32(kResult + 484, 0);

    for (uint32_t i = 32; i < 44; i += 4)
        FlatWrite32(screen + i, 0);

    FlatWrite32(kResult + 488, 0);
    FlatWrite32(screen + 44, 0);
    FlatWrite32(screen + 48, 0);

    if (game.Call<0x80396F68>({screen}) == 0) {
        FlatWrite8(screen + 92, uint8_t(game.Call<0x80272B14>({hunter, 27})));
        FlatWrite8(screen + 93, uint8_t(game.Call<0x80272B14>({hunter, 41})));
        const uint32_t entry = FlatRead32(work + 60);

        if (int32_t(FlatRead32(entry + 844)) > 0) {
            FlatWrite8(screen + 9, 1);
            const uint32_t main = game.Call<MarathonFlag>({0}) == 1 ? 2 : 0;

            if (game.Call<ObjectiveDone>({work, main, 1}) == 1 ||
                (FlatRead8(field + 250) == 4 && ((FlatRead32(FlatRead32(work + 60) + 784) & 0x800000) || game.Call<0x803B5E90>() == 1)))
                FlatWrite8(screen + 12, 1);
        }

        if (game.Call<ExtraFlag>({0}) == 0) {
            if (int32_t(FlatRead32(entry + 848)) > 0) {
                FlatWrite8(screen + 10, 1);

                if (game.Call<ObjectiveDone>({work, 1, 1}) == 1)
                    FlatWrite8(screen + 13, 1);
            }

            if (int32_t(FlatRead32(entry + 852)) > 0) {
                FlatWrite8(screen + 11, 1);
                const uint32_t sub = game.Call<MarathonFlag>({0}) == 1 ? 0 : 2;

                if (game.Call<ObjectiveDone>({work, sub, 1}) == 1)
                    FlatWrite8(screen + 14, 1);
            }
        }

        FlatWrite32(screen + 80, 0);
        FlatWrite32(screen + 84, 0);
        const uint32_t hunters = FlatRead8(screen + 7);

        if (game.Call<0x80396F38>({screen}) == 0) {
            for (uint32_t j = 0; j < 2; ++j) {
                FlatWrite32(screen + 44 + j * 4, FlatRead8(work + j + 27008));
                FlatWrite8(screen + 68 + j, uint8_t(FlatRead16(field + 8874 + j * 16)));

                if (game.Call<ExtraFlag>({0}) == 1) {
                    FlatWrite32(screen + 52 + j * 4, FlatRead32(work + 752));
                    FlatWrite32(screen + 60 + j * 4, FlatRead32(work + 772));
                } else {
                    FlatWrite32(screen + 52 + j * 4, FlatRead32(work + 27020 + j * 4));
                    FlatWrite32(screen + 60 + j * 4, FlatRead32(work + 27032 + j * 4));
                }
            }

            const auto share = [&](uint32_t at, uint32_t from) { FlatWrite32(screen + at, uint32_t(int32_t(FlatRead32(work + from)) / int32_t(FlatRead8(screen + 7)))); };

            if (int8_t(FlatRead8(screen + 12)) == 1) {
                share(20, 744);

                if (FlatRead8(screen + 6) == 0)
                    FlatWrite32(screen + 16, FlatRead32(work + 740) << 1);
            }

            if (int8_t(FlatRead8(screen + 13)) == 1)
                share(24, 748);

            if (int8_t(FlatRead8(screen + 14)) == 1)
                share(28, 752);

            float scale = Memory::ReadFloat32(0x8079E7C4); // r2 - 0x179C

            if (FlatRead8(screen + 94) != 0)
                scale *= Memory::ReadFloat32(0x8079E7C8);

            if (FlatRead8(screen + 93) == 1)
                scale *= Memory::ReadFloat32(0x8079E7CC);

            for (const uint32_t at : {20u, 24u, 28u})
                FlatWrite32(screen + at, uint32_t(int32_t(float(int32_t(FlatRead32(screen + at))) * scale)));

            for (uint32_t j = 0; j < 2; ++j)
                if (FlatRead32(screen + 44 + j * 4) != 0) {
                    FlatWrite32(screen + 52 + j * 4, uint32_t(int32_t(FlatRead32(screen + 52 + j * 4)) / int32_t(FlatRead8(screen + 7))));
                    FlatWrite32(screen + 80, FlatRead32(screen + 80) + FlatRead32(screen + 52 + j * 4) * FlatRead32(screen + 44 + j * 4));
                }

            // The arena's best times and names.
            if (game.Call<0x80396C7C>({screen}) == 1) {
                const uint32_t index = (FlatRead16(kResult + 480) - 60000) & 0xFF;

                if (index < 12) {
                    const uint32_t best = save + index * 4 + 21300;
                    const uint32_t record = FlatRead32(best);

                    if (record == 0 || !(record <= FlatRead32(kResult + 328))) {
                        FlatWrite32(best, FlatRead32(kResult + 328));

                        if (FlatRead8(screen + 7) == 2) {
                            uint32_t partner = QuestWork::MoveWork(QuestWork::kHunters);

                            if (FlatRead8(screen + 6) == 0)
                                partner += 2848;

                            game.Call<0x8045FBD4>({save + index * 52 + 17104, partner + 1482}); // strcpy
                            game.Call<0x8045FBD4>({save + index * 52 + 17124, partner + 1499});
                        } else {
                            FlatWrite8(save + index * 52 + 17104, 0);
                            FlatWrite8(save + index * 52 + 17124, 0);
                        }
                    }

                    if (FlatRead32(kResult + 328) <= FlatRead16(FlatRead32(0x805F82B8 + index * 4) + 2) * 30u) {
                        // 0x8021AD30, inlined: the award bit 15.
                        FlatWrite16(save + 16634, uint16_t(FlatRead16(save + 16634) | (1u << (15 - 12))));
                    }
                }
            }

            if (FlatRead16(kResult + 480) == 14002 && game.Call<0x8004D770>({14002}) == 1)
                game.Call<0x8004E94C>({6});
        } else if (game.Call<Flag02000000Or100>({0}) == 1 || game.Call<TimedFlag>({0}) == 1) {
            FlatWrite8(screen + 12, 0);
            FlatWrite8(screen + 13, 0);
        }

        (void)hunters;
        FlatWrite32(kResult + 484, (FlatRead32(screen + 28) + FlatRead32(screen + 24)) + (FlatRead32(screen + 16) + FlatRead32(screen + 20)));

        if (int8_t(FlatRead8(screen + 12)) == 1)
            FlatWrite32(screen + 32, FlatRead32(work + 760));

        if (int8_t(FlatRead8(screen + 13)) == 1)
            FlatWrite32(screen + 36, FlatRead32(work + 768));

        if (int8_t(FlatRead8(screen + 14)) == 1)
            FlatWrite32(screen + 40, FlatRead32(work + 772));

        float scale = Memory::ReadFloat32(0x8079E7C4);

        if (FlatRead8(screen + 94) != 0)
            scale *= Memory::ReadFloat32(0x8079E7C8);

        if (FlatRead8(screen + 92) == 1)
            scale *= Memory::ReadFloat32(0x8079E7D0);

        for (const uint32_t at : {32u, 36u, 40u})
            FlatWrite32(screen + at, uint32_t(int32_t(float(int32_t(FlatRead32(screen + at))) * scale)));

        for (uint32_t j = 0; j < 2; ++j)
            if (const uint32_t killed = FlatRead32(screen + 44 + j * 4))
                FlatWrite32(screen + 84, FlatRead32(screen + 84) + FlatRead32(screen + 60 + j * 4) * killed);

        FlatWrite8(screen + 8, FlatRead8(work + 146));
        FlatWrite32(kResult + 488, uint32_t(int32_t(int8_t(FlatRead8(screen + 8))) * -20) +
                                       ((FlatRead32(screen + 84) + FlatRead32(screen + 40)) + (FlatRead32(screen + 32) + FlatRead32(screen + 36))));
        game.Call<0x803AB420>({screen + 12400, screen + 12592});
        FlatWrite8(screen + 12384, game.Call<0x803982B8>({screen, 1}) == 1 ? 1 : 0);
        game.Call<0x80396AF4>({screen, kResult});
        FlatWrite32(screen + 12316, 0);

        // The pouch's items and their worth.
        for (uint32_t i = 0; i < 35; ++i) {
            if (FlatRead16(kResult + 492 + i * 4) == 0)
                continue;

            const uint32_t at = screen + FlatRead32(screen + 12316) * 4 + 12282;
            FlatWrite16(at, FlatRead16(kResult + 492 + i * 4));
            FlatWrite16(at + 2, FlatRead16(kResult + 494 + i * 4));
            uint32_t item = FlatRead16(at);

            if (item >= 747) // 0x8029F740, inlined
                item = 0;

            const uint32_t price = FlatRead32(FlatRead32(0x806AED38) + item * 20 + 16);
            FlatWrite32(screen + 88, FlatRead32(screen + 88) + price * uint32_t(int32_t(int16_t(FlatRead16(at + 2)))));
            FlatWrite32(screen + 12316, FlatRead32(screen + 12316) + 1);
        }

        FlatWrite32(screen + 72, FlatRead32(kResult + 484));
        FlatWrite32(screen + 76, FlatRead32(kResult + 488));
        game.Call<0x80396C08>({kResult});
    }

    // The rows of the guild card's new marks (awards and quests).
    FlatWrite8(screen + 12398, 0);

    for (uint32_t j = 0; j < 2; ++j) {
        FlatWrite32(screen + 12260 + j * 4, FlatRead32(field + 328 + j * 4) & ~FlatRead32(kSystem + 2184 + j * 4));

        for (uint32_t bit = 0; bit < 32; ++bit)
            if (FlatRead32(screen + 12260 + j * 4) & (1u << bit))
                FlatWrite8(screen + 12398, uint8_t(FlatRead8(screen + 12398) + 1));
    }

    FlatWrite32(kSystem + 2184, FlatRead32(kSystem + 2184) | FlatRead32(field + 328));
    FlatWrite32(kSystem + 2188, FlatRead32(kSystem + 2188) | FlatRead32(field + 332));
    const uint32_t before = FlatRead16(save + 21218);

    for (uint32_t entry = 0x805F1D14; FlatRead16(entry) != 0; entry += 6) {
        if (game.Call<0x8004D770>({FlatRead16(entry)}) != 1) // QuestCleared
            continue;

        const uint32_t bit = 1u << uint32_t(int32_t(int8_t(FlatRead16(entry + 2) - 9000)));
        const uint32_t marks = FlatRead32(kSavePointer);

        if (FlatRead16(marks + 21218) & bit) // 0x8004D800, inlined
            continue;

        FlatWrite16(FlatRead32(kSavePointer) + 21218, uint16_t(FlatRead16(FlatRead32(kSavePointer) + 21218) | (bit & 0xFFFF))); // 0x8004D7D8
        FlatWrite8(screen + 12398, uint8_t(FlatRead8(screen + 12398) + 1));
    }

    FlatWrite16(screen + 12268, uint16_t(FlatRead16(save + 21218) & ~before));
    FlatWrite8(screen + 12327, 0);
    return game.Call<0x80397160>({screen, 0}) == 0 ? 1 : 0;
}

uint32_t QuestResultRecords(GuestCall& game, uint32_t field) {
    const uint32_t save = UserData();
    const uint32_t screen = FlatRead32(field + 336);
    const uint32_t work = game.Call<0x803A880C>();

    switch (int8_t(FlatRead8(screen))) {
    case 0:
        return ResultRecordsStart(game, field, screen, save, work);
    case 1:
        break;
    default:
        return 0;
    }

    if (int16_t(FlatRead16(screen + 12396)) > 0)
        FlatWrite16(screen + 12396, uint16_t(int16_t(FlatRead16(screen + 12396)) - 1));

    switch (int8_t(FlatRead8(screen + 1))) {
    case 0:
        if (FlatRead8(kSystem + 2152) == 2) // 0x80058F4C, inlined
            FlatWrite8(kSystem + 2153, 1);

        game.Call<0x80398154>({screen});
        break;
    case 1:
        if (int32_t(game.Call<0x80398FE0>({screen})) == 1) // RewardPages
            FlatWrite8(screen + 1, uint8_t(FlatRead8(screen + 1) + 1));

        break;
    case 2:
        FlatWrite32(screen + 12276, MH3_TickLoad(FlatRead32(screen + 12276), 0x80398008) + 1);

        if (int32_t(FlatRead32(screen + 12276)) <= 20)
            break;

        FlatWrite32(screen + 12276, 0);

        if (game.Call<0x80397160>({screen, FlatRead32(screen + 12272) & 0xFF}) == 0) {
            FlatWrite8(screen + 1, uint8_t(FlatRead8(screen + 1) + 1));
            FlatWrite8(kSystem + 2152, 0); // 0x80058F6C, inlined
            FlatWrite8(kSystem + 2153, 0);
            FlatWrite32(screen + 12276, 16);

            if (int32_t(FlatRead32(screen + 12276)) < int32_t(int16_t(FlatRead16(screen + 12396))))
                FlatWrite32(screen + 12276, uint32_t(int32_t(int16_t(FlatRead16(screen + 12396)))));
        } else {
            FlatWrite8(screen + 1, 0);
            game.Call<0x80398154>({screen});
        }

        break;
    case 3: {
        const uint32_t left = MH3_TickLoad(FlatRead32(screen + 12276), 0x80398080);

        if (int32_t(left) <= 0)
            return 1;

        FlatWrite32(screen + 12276, left - 1);
        break;
    }
    default:
        break;
    }

    game.Call<0x80049240>({0x803980D0, 0, 0}); // subTransSet
    return 0;
}
} // namespace

namespace {
// Ports.

template <uint32_t (*F)(GuestCall&)>
void Predicate(CpuContext* ctx) {
    uint32_t result;

    {
        GuestFrame frame(ctx, 0x10);
        GuestCall game{ctx};
        result = F(game);
    }

    ctx->gpr[3] = result;
}

uint32_t ResultIs2(GuestCall&) {
    return ResultIs(2);
}

uint32_t ResultIs3(GuestCall&) {
    return ResultIs(3);
}

void RewardsCopyPort(CpuContext* ctx) {
    RewardsCopy(ctx->gpr[3], ctx->gpr[4]);
}

void SizeRecordsPort(CpuContext* ctx) {
    const uint32_t screen = ctx->gpr[3], result = ctx->gpr[4];
    GuestFrame frame(ctx, 0x30);
    GuestCall game{ctx};
    SizeRecords(game, screen, result);
}

void NextPagePort(CpuContext* ctx) {
    const uint32_t screen = ctx->gpr[3], page = ctx->gpr[4];
    uint32_t found;

    {
        GuestFrame frame(ctx, 0x20);
        GuestCall game{ctx};
        found = NextPage(game, screen, page);
    }

    ctx->gpr[3] = found;
}

void RewardBoxResetPort(CpuContext* ctx) {
    const uint32_t screen = ctx->gpr[3];
    GuestFrame frame(ctx, 0x20);
    GuestCall game{ctx};
    RewardBoxReset(game, screen);
}

void RewardPageStartPort(CpuContext* ctx) {
    const uint32_t screen = ctx->gpr[3], page = ctx->gpr[4];
    GuestFrame frame(ctx, 0x20);
    GuestCall game{ctx};
    RewardPageStart(game, screen, page);
}

void RewardsAnyPort(CpuContext* ctx) {
    ctx->gpr[3] = RewardsAny(ctx->gpr[3], ctx->gpr[4]);
}

void BoxTitleDrawPort(CpuContext* ctx) {
    const uint32_t screen = ctx->gpr[3], box = ctx->gpr[4];
    GuestFrame frame(ctx, 0x40);
    GuestCall game{ctx};
    BoxTitleDraw(game, frame[0], screen, box);
}

void BoxGridDrawPort(CpuContext* ctx) {
    const uint32_t screen = ctx->gpr[3], box = ctx->gpr[4], cursor = ctx->gpr[5];
    GuestFrame frame(ctx, 0x80);
    GuestCall game{ctx};
    BoxGridDraw(game, frame[0], screen, box, cursor);
}

void BreakRewardsPort(CpuContext* ctx) {
    const uint32_t work = ctx->gpr[3];
    GuestFrame frame(ctx, 0x60);
    GuestCall game{ctx};
    BreakRewards(game, work, frame[8]);
}

void CaptureRewardsPort(CpuContext* ctx) {
    const uint32_t work = ctx->gpr[3], hunter = ctx->gpr[4];
    GuestFrame frame(ctx, 0x50);
    GuestCall game{ctx};
    CaptureRewards(game, work, hunter, frame[8]);
}

void MainRewardsPort(CpuContext* ctx) {
    const uint32_t hunter = ctx->gpr[3], result = ctx->gpr[4];
    GuestFrame frame(ctx, 0x20);
    GuestCall game{ctx};
    MainRewards(game, frame[0], hunter, result);
}

void MainOnlyRewardsPort(CpuContext* ctx) {
    const uint32_t hunter = ctx->gpr[3], result = ctx->gpr[4];
    GuestFrame frame(ctx, 0x20);
    GuestCall game{ctx};
    MainOnlyRewards(game, frame[0], hunter, result, 1, true);
}

void MarathonRewardsPort(CpuContext* ctx) {
    const uint32_t hunter = ctx->gpr[3], result = ctx->gpr[4];
    GuestFrame frame(ctx, 0x20);
    GuestCall game{ctx};
    MainOnlyRewards(game, frame[0], hunter, result, 0, false);
}

void ArenaRewardsPort(CpuContext* ctx) {
    const uint32_t result = ctx->gpr[4], screen = ctx->gpr[5];
    GuestFrame frame(ctx, 0x20);
    GuestCall game{ctx};
    ArenaRewards(game, frame[0], result, screen);
}

void ExtraRewardsPort(CpuContext* ctx) {
    const uint32_t work = ctx->gpr[3], hunter = ctx->gpr[4];
    GuestFrame frame(ctx, 0x50);
    GuestCall game{ctx};
    ExtraRewards(game, frame[0], work, hunter);
}

void QuestResultSetupPort(CpuContext* ctx) {
    const uint32_t field = ctx->gpr[3];
    GuestFrame frame(ctx, 0xC0);
    GuestCall game{ctx};
    QuestResultSetup(game, frame[0], field);
}

void OnlineQuestKindPort(CpuContext* ctx) {
    uint32_t kind;

    {
        GuestFrame frame(ctx, 0x10);
        GuestCall game{ctx};
        kind = OnlineQuestKind(game);
    }

    ctx->gpr[3] = kind;
}

void OnlineGoalMetPort(CpuContext* ctx) {
    uint32_t met;

    {
        GuestFrame frame(ctx, 0x10);
        GuestCall game{ctx};
        met = OnlineGoalMet(game);
    }

    ctx->gpr[3] = met;
}
} // namespace

namespace QuestResultPorts {
// QuestResultRecords for the cheats' wrapper (cheat_quest.cpp).
void ResultRecordsPort(CpuContext* ctx) {
    const uint32_t field = ctx->gpr[3];
    uint32_t done;

    {
        GuestFrame frame(ctx, 0x60);
        GuestCall game{ctx};
        done = QuestResultRecords(game, field);
    }

    ctx->gpr[3] = done;
}
} // namespace QuestResultPorts

extern "C" void func_803AB420(CpuContext* ctx);
extern "C" void MH3_RewardsCopy(CpuContext* ctx) {
    PortCheck::Call(ctx, "RewardsCopy", func_803AB420, RewardsCopyPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x803AB420, MH3_RewardsCopy, "RewardsCopy");

extern "C" void func_80396AF4(CpuContext* ctx);
extern "C" void MH3_SizeRecords(CpuContext* ctx) {
    PortCheck::Call(ctx, "SizeRecords", func_80396AF4, SizeRecordsPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80396AF4, MH3_SizeRecords, "SizeRecords");

extern "C" void func_80396C7C(CpuContext* ctx);
extern "C" void MH3_ResultListKind(CpuContext* ctx) {
    PortCheck::Call(ctx, "ResultListKind", PortCheck::Returns::Gpr3, func_80396C7C, Predicate<ResultListKind>);
}
REGISTER_NATIVE_FUNCTION_AS(0x80396C7C, MH3_ResultListKind, "ResultListKind");

extern "C" void func_80396CC0(CpuContext* ctx);
extern "C" void MH3_SixRewardRows(CpuContext* ctx) {
    PortCheck::Call(ctx, "SixRewardRows", PortCheck::Returns::Gpr3, func_80396CC0, Predicate<SixRows>);
}
REGISTER_NATIVE_FUNCTION_AS(0x80396CC0, MH3_SixRewardRows, "SixRewardRows");

extern "C" void func_80396F38(CpuContext* ctx);
extern "C" void MH3_ResultFailed(CpuContext* ctx) {
    PortCheck::Call(ctx, "ResultFailed", PortCheck::Returns::Gpr3, func_80396F38, Predicate<ResultIs2>);
}
REGISTER_NATIVE_FUNCTION_AS(0x80396F38, MH3_ResultFailed, "ResultFailed");

extern "C" void func_80396F68(CpuContext* ctx);
extern "C" void MH3_ResultOther(CpuContext* ctx) {
    PortCheck::Call(ctx, "ResultOther", PortCheck::Returns::Gpr3, func_80396F68, Predicate<ResultIs3>);
}
REGISTER_NATIVE_FUNCTION_AS(0x80396F68, MH3_ResultOther, "ResultOther");

extern "C" void func_80397160(CpuContext* ctx);
extern "C" void MH3_NextResultPage(CpuContext* ctx) {
    PortCheck::Call(ctx, "NextResultPage", PortCheck::Returns::Gpr3, func_80397160, NextPagePort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80397160, MH3_NextResultPage, "NextResultPage");

extern "C" void func_8039729C(CpuContext* ctx);
extern "C" void MH3_RewardBoxReset(CpuContext* ctx) {
    PortCheck::Call(ctx, "RewardBoxReset", func_8039729C, RewardBoxResetPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8039729C, MH3_RewardBoxReset, "RewardBoxReset");

extern "C" void func_803973A8(CpuContext* ctx);
extern "C" void MH3_RewardPageStart(CpuContext* ctx) {
    PortCheck::Call(ctx, "RewardPageStart", func_803973A8, RewardPageStartPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x803973A8, MH3_RewardPageStart, "RewardPageStart");

extern "C" void func_803982B8(CpuContext* ctx);
extern "C" void MH3_RewardsAny(CpuContext* ctx) {
    PortCheck::Call(ctx, "RewardsAny", PortCheck::Returns::Gpr3, func_803982B8, RewardsAnyPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x803982B8, MH3_RewardsAny, "RewardsAny");

extern "C" void func_8039A890(CpuContext* ctx);
extern "C" void MH3_RewardBoxTitleDraw(CpuContext* ctx) {
    PortCheck::Call(ctx, "RewardBoxTitleDraw", func_8039A890, BoxTitleDrawPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8039A890, MH3_RewardBoxTitleDraw, "RewardBoxTitleDraw");

extern "C" void func_8039AD2C(CpuContext* ctx);
extern "C" void MH3_RewardBoxGridDraw(CpuContext* ctx) {
    PortCheck::Call(ctx, "RewardBoxGridDraw", func_8039AD2C, BoxGridDrawPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8039AD2C, MH3_RewardBoxGridDraw, "RewardBoxGridDraw");

extern "C" void func_803ABEA8(CpuContext* ctx);
extern "C" void MH3_BreakRewards(CpuContext* ctx) {
    PortCheck::Call(ctx, "BreakRewards", func_803ABEA8, BreakRewardsPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x803ABEA8, MH3_BreakRewards, "BreakRewards");

extern "C" void func_803AC714(CpuContext* ctx);
extern "C" void MH3_CaptureRewards(CpuContext* ctx) {
    PortCheck::Call(ctx, "CaptureRewards", func_803AC714, CaptureRewardsPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x803AC714, MH3_CaptureRewards, "CaptureRewards");

extern "C" void func_803AB978(CpuContext* ctx);
extern "C" void MH3_MainRewards(CpuContext* ctx) {
    PortCheck::Call(ctx, "MainRewards", func_803AB978, MainRewardsPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x803AB978, MH3_MainRewards, "MainRewards");

extern "C" void func_803ABBD8(CpuContext* ctx);
extern "C" void MH3_MainOnlyRewards(CpuContext* ctx) {
    PortCheck::Call(ctx, "MainOnlyRewards", func_803ABBD8, MainOnlyRewardsPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x803ABBD8, MH3_MainOnlyRewards, "MainOnlyRewards");

extern "C" void func_803ABD40(CpuContext* ctx);
extern "C" void MH3_MarathonRewards(CpuContext* ctx) {
    PortCheck::Call(ctx, "MarathonRewards", func_803ABD40, MarathonRewardsPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x803ABD40, MH3_MarathonRewards, "MarathonRewards");

extern "C" void func_803ACEF4(CpuContext* ctx);
extern "C" void MH3_ArenaRewards(CpuContext* ctx) {
    PortCheck::Call(ctx, "ArenaRewards", func_803ACEF4, ArenaRewardsPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x803ACEF4, MH3_ArenaRewards, "ArenaRewards");

extern "C" void func_803ACD74(CpuContext* ctx);
extern "C" void MH3_ExtraRewards(CpuContext* ctx) {
    PortCheck::Call(ctx, "ExtraRewards", func_803ACD74, ExtraRewardsPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x803ACD74, MH3_ExtraRewards, "ExtraRewards");

extern "C" void func_803B23B8(CpuContext* ctx);
extern "C" void MH3_QuestResultSetup(CpuContext* ctx) {
    PortCheck::Call(ctx, "QuestResultSetup", func_803B23B8, QuestResultSetupPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x803B23B8, MH3_QuestResultSetup, "QuestResultSetup");

extern "C" void func_8036C9EC(CpuContext* ctx);
extern "C" void MH3_OnlineQuestKind(CpuContext* ctx) {
    PortCheck::Call(ctx, "OnlineQuestKind", PortCheck::Returns::Gpr3, func_8036C9EC, OnlineQuestKindPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8036C9EC, MH3_OnlineQuestKind, "OnlineQuestKind");

extern "C" void func_8036CEDC(CpuContext* ctx);
extern "C" void MH3_OnlineGoalMet(CpuContext* ctx) {
    PortCheck::Call(ctx, "OnlineGoalMet", PortCheck::Returns::Gpr3, func_8036CEDC, OnlineGoalMetPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8036CEDC, MH3_OnlineGoalMet, "OnlineGoalMet");
