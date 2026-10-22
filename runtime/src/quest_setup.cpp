// Setting a quest up as its field loads, RMHP08. C reconstructions from the
// translated code.
//
//   0x8028C730 FieldOnlineLoad(field): the field task's load states (+1):
//              0 the quest (QuestSetup), the player work and the hunters'
//              motions; 1 waits for the files; 2 the lights, effects and
//              sounds reset; 3 the map (QuestMapSetup in quests), the stage and
//              event demos; 4 the area: the start position, the gathering
//              spots, the NPCs; 5 the fade in (online: every member synced,
//              a message every 32 steps); 6 the event demo driver twice; 7 the
//              field runs (+0 = 2)
//   0x803AEF34 QuestSetup(): with a quest picked (lb_param_w 0x8065B534) its
//              entry copied into the quest work's buffer (+0x3C; 60000 and up:
//              0x80454B10), the monster heap layout (+0x390), the quest work
//              (QuestLoad), the monster slots (SetQuestMonsters), the map
//              (+0x30000 of the flags: a variant), the extra monster (+0x6760:
//              flag 0x100 copies the third slot there; a Qurupeco alone picks
//              its call; an intruder chance +0x32C rolls), the objective mode
//              +0x93 (0x40000 4, 0x40 2, 0x800000 5, marathon 4), the
//              objectives, rewards and fees, QuestObjectiveSetup, the
//              gathering table (QuestGatherSetup)
//   0x803A88F0 QuestLoad(id): the quest work's +0x10 id, +0x14 the first
//              slot's monster, +0x1C/+0x24 the time limit in frames (+0x13A
//              minutes * 60 * fps), +0x30/+0x34/+0x38 from the entry
//   0x803A89F8 QuestEntryCopy(to, from): the entry, 0x4B8 bytes but +0x30E
//   0x803ADAD4 QuestLoadMonsters(): each slot's monster's files (LoadMonster
//              0x801410B4); the marathon and the timed flag stop after the
//              first (+0x6977 = 1, +0x308 = 1), flag 0x100 after two; then the
//              extra monster's
//   0x803ADD88 QuestObjectiveSetup(): the objectives started (+8); with
//              0x02000000 / 0x100 the mode from 0x80795B30 by the hunted
//              ones, with the timed flag 2; the delivery and +0x2000 targets
//   0x803AE094 QuestPlaceMonsters(map key, field): each slot's monster
//              created (CreateQuestEnemy) or held (mode 3: SpawnHeld) with its
//              spawn id and objective; the marathon stops after the first,
//              flag 0x100 after two; then the extra monster (+0x6760) held
//   0x803AE488 PlaceMonsters(quest work): QuestPlaceMonsters in a quest,
//              else the free roam's monsters (0x805F5740 by the field's +0x144)
//   0x803AEC24 QuestGatherSetup(table): the area spawn tables (+0x69A8) and the
//              delivery points (+0x6A2C, +0x6A32)
//   0x800EF2D4 NpcDanceLoad(kamen, dance1, dance2): the NPCs' dance and mask
//              files (16/whd, 16/tsb)
#include "abi_bridge.h"
#include "guest_call.h"
#include "port_check.h"
#include "quest_work.h"

#include <cstdint>

extern "C" uint32_t MH3_TickLoad(uint32_t value, uint32_t pc);

namespace QuestSpawns {
uint32_t SpawnHeld(GuestCall& game, uint32_t kind, uint32_t record, uint32_t id);
uint32_t CreateQuestEnemy(GuestCall& game, uint32_t kind, uint32_t record, uint32_t id);
void CopySpawnRecord(GuestCall& game, uint32_t to, uint32_t from);
} // namespace QuestSpawns

namespace {
using MemoryInline::FlatRead16;
using MemoryInline::FlatRead32;
using MemoryInline::FlatRead8;
using MemoryInline::FlatWrite16;
using MemoryInline::FlatWrite32;
using MemoryInline::FlatWrite8;
using QuestWork::Work;

constexpr uint32_t kSystem = 0x8065AA60;   // system_w
constexpr uint32_t kPlayMode = kSystem + 37; // PlayMode_ck, inlined
constexpr uint32_t kQuestPick = 0x8065B534; // lb_param_w: u16 the quest picked
constexpr uint32_t kLists = 0x805F7E60, kMapKeys = 0x805F8058, kMapIds = 0x805F8074;
constexpr uint32_t kFreeRoam = 0x805F5740;
constexpr uint32_t kModeByHunted = 0x80795B30; // r13 - 0x57B0
constexpr uint32_t kIntruderKinds = 0x80795B24; // r13 - 0x57BC
constexpr uint32_t kCallPicker = 0x806CE894;    // the Qurupeco's call: (chances, extra slot) -> 1
constexpr uint32_t kNpcWork = 0x80796EEC;      // r13 - 0x43F4
constexpr uint32_t kFrameRate = 0x8065B4D0;
constexpr uint32_t kSixty = 0x8079E938; // r2 - 0x1628

enum : uint32_t {
    QuestWorkOf = 0x803A880C,
    QuestEntryOf = 0x803B1F10,
    QuestEntrySize = 0x803B20C4,
    QuestEntryCustom = 0x80454B10,
    Memcpy = 0x80004000,
    Memset = 0x80004350,
    SetMonsterHeaps = 0x80146604,
    SetQuestMonsters = 0x803AE9F4,
    Rand = 0x800CEF20,
    LoadMapOverlay = 0x80040E4C,
    OnlineDemoCheck = 0x804469B8,
    LoadFile = 0x800CECC8,
    NpcDanceReset = 0x800EF62C,
    LoadMonster = 0x801410B4,
    AreaSpawnsSetup = 0x803A7ECC,
    AreaGatherSetup = 0x803A818C,
    AreaTableCount = 0x802AFF64,
    AreaTable = 0x801478F8,
    Sprintf = 0x8045E54C,
    LoadFileRequest = 0x800CEB14,
    LoadDone = 0x800F0294,
    MarathonFlag = 0x803B4DC4,
    TimedFlag = 0x803B4D4C,
    ExtraFlag = 0x803B4E3C,
    QurupecoAlone = 0x803B32F4,
    Flag02000000Or100 = 0x803B4CC8,
    ObjectiveOf = 0x803ADFE8, // QuestRecordOf
    VecInit = 0x80125FB8,
};

// The quest entry's fields.
enum : uint32_t {
    kEntryFlags = 0x310,
    kSlotBytes = 8,
};

uint32_t QuestEntryOfWork() {
    return FlatRead32(Work() + QuestWork::kEntry);
}

void QuestEntryCopy(uint32_t to, uint32_t from) {
    for (uint32_t i = 0; i < 0x30E; ++i)
        FlatWrite8(to + i, FlatRead8(from + i));

    for (uint32_t i = 0x310; i < 0x4B8; ++i)
        FlatWrite8(to + i, FlatRead8(from + i));
}

int32_t Fctiwz(float v) {
    return int32_t(v); // the limits are small whole numbers of frames
}

void QuestLoad(GuestCall& game, uint32_t id) {
    const uint32_t work = game.Call<QuestWorkOf>();

    if (!work || !FlatRead32(work + QuestWork::kEntry))
        return;

    const uint32_t entry = game.Call<QuestEntryOf>({id & 0xFFFF});
    QuestEntryCopy(FlatRead32(work + QuestWork::kEntry), entry);
    FlatWrite16(work + 0x10, FlatRead16(entry + 0x2C));
    FlatWrite8(work + 0x14, FlatRead8(entry + 0x314));
    const float perMinute = Memory::ReadFloat32(kSixty) * Memory::ReadFloat32(kFrameRate);
    const float minutes = float(int16_t(FlatRead16(entry + 0x13A)));
    FlatWrite32(work + 0x1C, uint32_t(Fctiwz(minutes * perMinute)));
    FlatWrite32(work + 0x24, uint32_t(Fctiwz(minutes * perMinute)));
    FlatWrite32(work + 0x30, FlatRead32(entry + 0x34C));
    FlatWrite32(work + 0x34, FlatRead32(entry + 0x358));
    FlatWrite32(work + 0x38, FlatRead32(entry + kEntryFlags));
}

void NpcDanceLoad(GuestCall& game, uint32_t sp, uint32_t kamen, uint32_t dance1, uint32_t dance2) {
    const uint32_t npc = FlatRead32(kNpcWork);
    FlatWrite8(npc + 0x97AC, uint8_t(dance1));
    FlatWrite8(npc + 0x97AD, uint8_t(dance2));
    const uint32_t flag = sp + 8, path = sp + 24;

    // 0x800F0474, inlined: the slot's three load states cleared.
    const auto clear = [](uint32_t slot) {
        const uint32_t at = FlatRead32(kNpcWork);
        FlatWrite8(at + 0x9774 + slot, 0);
        FlatWrite8(at + 0x96B3 + slot, 0);
        FlatWrite8(at + 0x9682 + slot, 0);
    };

    const auto sprintf = [&](uint32_t format, uint32_t a, uint32_t b = 0) {
        game.cpu->cr &= ~0x02000000u; // no float arguments
        game.Call<Sprintf>({path, format, a, b});
    };

    const auto load = [&](uint32_t dest, uint32_t size, uint32_t whd) {
        FlatWrite32(flag, whd);
        const uint32_t at = FlatRead32(kNpcWork);
        game.Call<LoadFileRequest>({path, FlatRead32(at + dest), FlatRead32(at + size), LoadDone, 3, flag});
    };

    clear(0x1C);
    sprintf(0x8059975C, (kamen & 0xFF) == 6 ? 0x8059976C : 0x8059977C);
    load(0x80, 0x144, 1);
    sprintf(0x80599788, 0x8059977C);
    load(0x208, 0x2CC, 0);
    clear(0x20);
    sprintf(0x8059975C, FlatRead32(0x805992F8 + (dance1 & 0xFF) * 4));
    load(0x90, 0x154, 1);
    sprintf(0x80599788, FlatRead32(0x805992F8 + (dance1 & 0xFF) * 4));
    load(0x218, 0x2DC, 0);
    clear(0x21);
    sprintf(0x8059975C, FlatRead32(0x80599330 + (dance2 & 0xFF) * 4));
    load(0x94, 0x158, 1);
    sprintf(0x80599788, FlatRead32(0x80599330 + (dance2 & 0xFF) * 4));
    load(0x21C, 0x2E0, 0);
    clear(0x24);
    sprintf(0x80599798, 0x805997AC, kamen & 0xFF);
    load(0xA0, 0x164, 1);
    sprintf(0x805997B8, 0x805997AC, kamen & 0xFF);
    load(0x228, 0x2EC, 0);
}

void QuestObjectiveSetup(GuestCall& game) {
    const bool counted = game.Call<Flag02000000Or100>({0}) == 1;
    uint32_t hunted = 0;

    for (uint32_t k = 0; k < 3; ++k) {
        const uint32_t record = Work() + QuestWork::kObjectives + k * 0x60;
        const uint32_t flags = FlatRead32(record);

        if (flags & (1 | 2 | 0x1000 | 0x2000 | 4)) {
            FlatWrite32(record, FlatRead32(record) | 8);

            if (counted)
                hunted = (hunted | (1u << k)) & 0xFF;
        }
    }

    if (counted)
        FlatWrite8(Work() + QuestWork::kObjectiveMode, FlatRead8(kModeByHunted + hunted));

    if (game.Call<TimedFlag>({0}) == 1)
        FlatWrite8(Work() + QuestWork::kObjectiveMode, 2);

    const uint32_t work = Work();

    for (uint32_t k = 0; k < 3; ++k) {
        const uint32_t record = work + QuestWork::kObjectives + k * 0x60;

        if ((FlatRead32(record) & 2) && FlatRead16(record + 4) == 0) {
            FlatWrite8(Work() + 0x68F + k * 2, 0);
            FlatWrite8(Work() + 0x690 + k * 2, uint8_t(FlatRead16(record + 6)));
        }
    }

    for (uint32_t k = 0; k < 3; ++k) {
        const uint32_t record = work + QuestWork::kObjectives + k * 0x60;

        if ((FlatRead32(record) & 0x2000) && FlatRead16(record + 4) == 4)
            FlatWrite16(Work() + 0x5D8, FlatRead16(record + 6));
    }
}

void QuestGatherSetup(GuestCall& game) {
    for (uint32_t i = 0; i < 0x80; i += 4)
        FlatWrite32(Work() + 0x69A8 + i, 0);

    const uint32_t work = Work(), entry = FlatRead32(work + QuestWork::kEntry);

    for (uint32_t k = 0; k < 2; ++k) {
        const uint32_t at = 0x6A2C + k * 6;

        if (FlatRead16(entry + 0x384 + k * 6) == 0) {
            FlatWrite16(Work() + at, 0);
            FlatWrite16(Work() + at + 2, 0);
            FlatWrite16(Work() + at + 4, 0);
        } else {
            FlatWrite16(work + at, FlatRead16(entry + 0x384 + k * 6));
            FlatWrite16(Work() + at + 2, FlatRead16(entry + 0x386 + k * 6));
            FlatWrite16(Work() + at + 4, FlatRead16(entry + 0x388 + k * 6));
        }
    }

    FlatWrite8(Work() + 0x6A28, 0);
    FlatWrite8(Work() + 0x6A29, 0);
    FlatWrite8(Work() + 0x6A2A, 0);
    const uint32_t offset = FlatRead32(entry + 0x37C);
    const uint32_t table = offset ? offset + FlatRead32(Work() + QuestWork::kEntry) : 0;
    uint32_t count = game.Call<AreaTableCount>({FlatRead8(QuestEntryOfWork() + 0x8B)}) & 0xFF;

    if (count > 0x20)
        count = 0x20;

    FlatWrite8(Work() + 0x69A5, uint8_t(count));

    for (uint32_t i = 0; table && i < count; ++i)
        FlatWrite32(Work() + 0x69A8 + i * 4, game.Call<AreaTable>({table, 0, i & 0xFF}));

    FlatWrite32(Work() + QuestWork::kQuestActive, 1);
}

void QuestSetup(GuestCall& game) {
    const uint32_t field = QuestWork::MoveWork(QuestWork::kField);

    if (!field)
        return;

    const uint32_t pick = FlatRead16(kQuestPick);
    const bool picked = pick != 0;

    if (picked) {
        if (pick < 60000) {
            const uint32_t size = game.Call<QuestEntrySize>({FlatRead16(kQuestPick)}) & 0xFFFF;
            const uint32_t from = game.Call<QuestEntryOf>({FlatRead16(kQuestPick)});
            game.Call<Memcpy>({QuestEntryOfWork(), from, size});
        } else {
            game.Call<QuestEntryCustom>({QuestEntryOfWork()});
        }
    }

    if (Work()) {
        FlatWrite32(Work() + QuestWork::kQuestActive, 0);
        game.Call<Memset>({Work() + QuestWork::kExtraSlot, 0, 24});
    }

    FlatWrite8(field + 0x22D4, FlatRead8(kQuestPick + 8));
    FlatWrite8(field + 0x22D5, FlatRead8(kQuestPick + 41));
    FlatWrite8(field + 0x22D6, FlatRead8(kQuestPick + 42));

    if (const uint32_t dance = FlatRead8(field + 0x22D4); dance & 0x80)
        game.Call<0x800EF2D4>({dance & 0x7F, FlatRead8(field + 0x22D5), FlatRead8(field + 0x22D6)});

    if (!picked) {
        if (!Work()) {
            FlatWrite8(field + 0xED, uint8_t(FlatRead32(field + 0x120)));
            FlatWrite8(field + 0xE9, FlatRead8(kMapKeys + FlatRead32(field + 0x120) * 2));
        } else {
            FlatWrite16(Work() + 0x10, uint16_t(FlatRead32(field + 0x120)));
        }

        return;
    }

    if (FlatRead32(QuestEntryOfWork() + kEntryFlags) & 8) {
        FlatWrite8(field + 0x22D4, 0x80);
        FlatWrite8(field + 0x22D5, 0);
        FlatWrite8(field + 0x22D6, 0);
        game.Call<0x800EF2D4>({0, 0, 0});
    }

    if (const uint32_t layout = FlatRead32(QuestEntryOfWork() + 0x390)) {
        FlatWrite8(field + 0x22DB, uint8_t(layout - 1));
        game.Call<SetMonsterHeaps>({uint32_t(int32_t(int8_t(FlatRead32(QuestEntryOfWork() + 0x390) - 1)))});
    } else {
        FlatWrite8(field + 0x22DB, 0);
        game.Call<SetMonsterHeaps>({0});
    }

    FlatWrite32(Work() + QuestWork::kQuestActive, 1);
    FlatWrite32(field + 0x120, FlatRead16(kQuestPick));
    game.Call<0x803A88F0>({FlatRead32(field + 0x120)});
    game.Call<SetQuestMonsters>();

    // The map: the flags' 0x30000 pick a variant of the entry's map.
    {
        const uint32_t entry = QuestEntryOfWork();
        const uint32_t keys = kMapKeys + FlatRead8(entry + 0x8B) * 2;
        const uint32_t variant = FlatRead32(entry + kEntryFlags) & 0x30000;

        if (variant == 0x30000) {
            FlatWrite8(field + 0xED, FlatRead8(keys + FlatRead8(kQuestPick + 9)));
        } else if (variant == 0x20000) {
            FlatWrite8(field + 0xED, FlatRead8(keys + 1));
            FlatWrite8(kQuestPick + 9, 1);
        } else {
            FlatWrite8(field + 0xED, FlatRead8(keys));
            FlatWrite8(kQuestPick + 9, 0);
        }
    }

    FlatWrite8(field + 0x22D8, FlatRead8(kQuestPick + 9));
    FlatWrite8(Work() + QuestWork::kExtraOn, 0);

    if (game.Call<QurupecoAlone>() == 1) {
        const uint32_t work = Work();
        const uint32_t picked = game(FlatRead32(kCallPicker), {FlatRead32(work + QuestWork::kEntry) + 0x32C, work + QuestWork::kExtraSlot});

        if ((picked & 0xFF) == 1) {
            FlatWrite8(Work() + QuestWork::kExtraLoaded, 1);
            FlatWrite8(Work() + QuestWork::kExtraOn, 1);
        }
    }

    if (game.Call<ExtraFlag>({0}) == 1) {
        game.Call<Memcpy>({Work() + QuestWork::kExtraSlot, FlatRead32(Work() + QuestWork::kEntry) + 0x324, 8});
        FlatWrite8(Work() + QuestWork::kExtraLoaded, 1);
        FlatWrite8(Work() + QuestWork::kExtraOn, 1);
    }

    {
        const uint32_t work = Work();

        if (const uint32_t chance = FlatRead8(FlatRead32(work + QuestWork::kEntry) + 0x32C)) {
            const uint32_t roll = game.Call<Rand>({3}) & 0xFFFF;

            if (roll % 100 < chance) {
                FlatWrite8(work + QuestWork::kExtraOn, 1);
                const uint32_t kind = FlatRead8(FlatRead32(Work() + QuestWork::kEntry) + 0x32D);
                FlatWrite8(Work() + 0x6AB4, kind < 7 ? FlatRead8(kIntruderKinds + kind) : 0);
            }
        }
    }

    const auto flags = [] { return FlatRead32(QuestEntryOfWork() + kEntryFlags); };

    if (flags() & 0x40000)
        FlatWrite8(Work() + QuestWork::kObjectiveMode, 4);

    if (flags() & 0x40)
        FlatWrite8(Work() + QuestWork::kObjectiveMode, 2);

    if (flags() & 0x800000)
        FlatWrite8(Work() + QuestWork::kObjectiveMode, 5);

    if (game.Call<MarathonFlag>({0}) == 1)
        FlatWrite8(Work() + QuestWork::kObjectiveMode, 4);

    FlatWrite8(field + 0xEE, 0);
    FlatWrite8(field + 0xE9, FlatRead8(QuestEntryOfWork() + 0x8B));
    game.Call<LoadMapOverlay>({FlatRead8(field + 0xED)});
    game.Call<Memcpy>({Work() + 0x94, QuestEntryOfWork() + 0x394, 0x120});
    game.Call<Memcpy>({Work() + 0x1B4, QuestEntryOfWork() + 0x394, 0x120});
    game.Call<Memcpy>({Work() + 0x2E4, QuestEntryOfWork() + 0x348, 0x24});
    FlatWrite32(Work() + 0x2D8, 0);
    FlatWrite32(Work() + 0x2DC, 0);
    FlatWrite32(Work() + 0x2E0, 0);
    FlatWrite8(field + 0xEA, 0);
    FlatWrite8(field + 0xEE, 0);

    // The faints the reward allows: reward / penalty, rounded up.
    {
        const uint32_t work = Work();
        const int32_t penalty = int32_t(FlatRead32(work + 0x2F4));

        if (penalty < 1) {
            FlatWrite8(work + 0x90, 0);
        } else {
            const int32_t reward = int32_t(FlatRead32(work + 0x2E8));
            FlatWrite8(work + 0x90, uint8_t(reward / penalty));

            if (reward - int32_t(int8_t(FlatRead8(work + 0x90))) * penalty > 0)
                FlatWrite8(work + 0x90, uint8_t(FlatRead8(work + 0x90) + 1));
        }
    }

    FlatWrite8(Work() + 0x91, 0);
    FlatWrite8(Work() + 0x92, 0);
    game.Call<0x803ADD88>();

    if (game.Call<OnlineDemoCheck>() == 1) {
        const uint8_t which = FlatRead8(0x8065B5CD);

        if (which == 0x13 || which == 0x14) {
            FlatWrite32(QuestEntryOfWork() + 0x37C, 0x4C0);
            const uint32_t entry = QuestEntryOfWork();
            game.Call<LoadFile>({which == 0x13 ? 0x805F8310u : 0x805F8324u, FlatRead32(entry + 0x37C) + entry, which == 0x13 ? 0x600u : 0xB00u});
        }
    }

    game.Call<0x803AEC24>({FlatRead8(kMapIds + FlatRead8(field + 0xE9))});
    const uint32_t entry = QuestEntryOfWork();

    if (FlatRead32(entry + kEntryFlags) & 0x1000000) {
        // 0x8033A1A0, inlined: the event's state.
        FlatWrite8(0x806C13A0, 0);
        FlatWrite8(0x806C13A8, 0);
        FlatWrite8(0x806C13A6, FlatRead8(entry + 0x36E));
        FlatWrite32(0x806C13AC, 0);
        game.Call<NpcDanceReset>();
    }
}
} // namespace

namespace {
void QuestLoadMonsters(GuestCall& game) {
    if (!Work())
        return;

    const uint32_t field = QuestWork::MoveWork(QuestWork::kField);

    if (!field)
        return;

    const uint8_t mode = FlatRead8(kPlayMode);

    if (!Work() || !FlatRead32(Work() + QuestWork::kQuestActive))
        return;

    int32_t loaded = 0;

    for (uint32_t i = 0; i < 3; ++i) {
        const uint8_t monster = FlatRead8(Work() + i + QuestWork::kMonsters);

        if (!monster)
            continue;

        game.Call<LoadMonster>({monster});
        ++loaded;

        if (game.Call<MarathonFlag>({0}) == 1 || game.Call<TimedFlag>({0}) == 1) {
            FlatWrite8(Work() + QuestWork::kWaiting, 1);
            FlatWrite8(Work() + QuestWork::kPlaced, 1);
            break;
        }

        if (game.Call<ExtraFlag>({0}) == 1 && loaded == 2) {
            FlatWrite8(Work() + QuestWork::kWaiting, 1);
            FlatWrite8(Work() + QuestWork::kPlaced, 2);
            break;
        }
    }

    if (game.Call<MarathonFlag>({0}) == 0) {
        if (game.Call<ExtraFlag>({0}) == 1)
            game.Call<LoadMonster>({FlatRead8(Work() + QuestWork::kExtraSlot)});
        else if (const uint8_t extra = FlatRead8(Work() + QuestWork::kExtraSlot); extra && loaded <= 1)
            game.Call<LoadMonster>({extra});
    }

    game.Call<AreaSpawnsSetup>({field, Work() + 0x69A8, FlatRead8(Work() + 0x69A5), 0});
    game.Call<AreaGatherSetup>({field, FlatRead8(kMapIds + FlatRead8(field + 0xE9)), FlatRead8(field + 0xEA)});

    if (mode == 6)
        FlatWrite8(kPlayMode, 3); // PlayMode_set(3), inlined
}

// A placed monster's tables in the field work (+0x2274 + 16n, +0x225C + 4n).
void NoteSpawn(uint32_t field, uint32_t n, uint32_t id, uint32_t record, uint32_t count, uint32_t monster) {
    const uint32_t at = field + n * 16;
    FlatWrite32(at + 0x2274, id);
    FlatWrite32(at + 0x227C, record);
    FlatWrite16(at + 0x2278, uint16_t(count));
    FlatWrite16(at + 0x227A, uint16_t(monster));
    FlatWrite32(at + 0x2280, 0);
}

int32_t Placed() {
    return int8_t(FlatRead8(Work() + QuestWork::kPlaced));
}

void QuestPlaceMonsters(GuestCall& game, uint32_t desc, uint32_t key, uint32_t field) {
    game.Call<VecInit>({desc});
    uint32_t placement = Work() + 0x34C, slot = Work() + QuestWork::kSlots;
    FlatWrite8(Work() + QuestWork::kPlaced, 0);

    for (uint32_t i = 0; i < 3; ++i, placement += 48, slot += 8) {
        const uint8_t monster = FlatRead8(slot);

        if (!monster)
            continue;

        int32_t n = Placed();
        FlatWrite16(Work() + QuestWork::kSpawnIds + n * 2, uint16_t(n));
        FlatWrite8(Work() + 0x478 + n, FlatRead8(slot + 4));
        FlatWrite8(Work() + 0x47E + n, FlatRead8(slot + 5));
        FlatWrite8(Work() + 0x484 + n, FlatRead8(slot + 6));
        FlatWrite8(Work() + 0x48A + n, FlatRead8(slot + 7));
        QuestSpawns::CopySpawnRecord(game, desc, placement + 8);
        n = Placed();
        NoteSpawn(field, uint32_t(n), FlatRead16(Work() + QuestWork::kSpawnIds + n * 2), placement, uint32_t(int16_t(FlatRead32(placement + 4))), FlatRead8(slot));
        uint32_t held = 0, work;

        if (FlatRead8(slot + 2) == 3) {
            held = QuestSpawns::SpawnHeld(game, FlatRead8(slot), desc, uint32_t(Placed()) & 0xFFFF);

            if (!held) {
                FlatWrite16(Work() + QuestWork::kSpawnIds + Placed() * 2, 0xFFFF);
                continue;
            }

            work = FlatRead32(held + 48);
            FlatWrite32(field + uint32_t(n) * 16 + 0x2280, held);
        } else {
            work = QuestSpawns::CreateQuestEnemy(game, FlatRead8(slot), desc, uint32_t(Placed()) & 0xFFFF);

            if (!work) {
                FlatWrite16(Work() + QuestWork::kSpawnIds + Placed() * 2, 0xFFFF);
                continue;
            }
        }

        const int32_t objective = int8_t(game.Call<ObjectiveOf>({FlatRead8(slot)}));

        if (objective >= 0)
            FlatWrite8((FlatRead8(slot + 2) == 3 ? held : work) + 13, uint8_t(objective));

        FlatWrite8(Work() + QuestWork::kPlaced, uint8_t(FlatRead8(Work() + QuestWork::kPlaced) + 1));

        if (game.Call<MarathonFlag>({0}) == 1)
            return;

        if (game.Call<ExtraFlag>({0}) == 1 && Placed() == 2)
            return;
    }

    // The extra monster, held.
    const uint32_t extra = FlatRead8(Work() + QuestWork::kExtraSlot);

    if (!extra || Placed() > 1)
        return;

    uint32_t list = FlatRead32(kLists + extra * 4);

    if (!list)
        return;

    while (FlatRead8(list) != 0xFF && FlatRead8(list) != (key & 0xFF))
        list += 8;

    if (FlatRead8(list) == 0xFF)
        return;

    const uint32_t record = FlatRead32(list + 4) + FlatRead8(Work() + QuestWork::kExtraSlot + 1) * 48;
    QuestSpawns::CopySpawnRecord(game, desc, record + 8);
    FlatWrite8(desc + 2, 3);
    const int32_t n = Placed();
    FlatWrite16(Work() + QuestWork::kSpawnIds + n * 2, uint16_t(n));

    for (uint32_t i = 0; i < 4; ++i)
        FlatWrite8(Work() + 0x478 + i * 6 + Placed(), FlatRead8(Work() + QuestWork::kExtraSlot + 4 + i));

    if (!QuestSpawns::SpawnHeld(game, FlatRead8(Work() + QuestWork::kExtraSlot), desc, uint32_t(Placed()) & 0xFFFF)) {
        FlatWrite16(Work() + QuestWork::kSpawnIds + Placed() * 2, 0xFFFF);
        return;
    }

    FlatWrite32(field + 0x22A4, FlatRead16(Work() + QuestWork::kSpawnIds + Placed() * 2));
    FlatWrite32(field + 0x22AC, record);
    FlatWrite16(field + 0x22A8, uint16_t(int16_t(FlatRead32(record + 4))));
    FlatWrite16(field + 0x22AA, FlatRead8(Work() + QuestWork::kExtraSlot));
    FlatWrite32(field + 0x22B0, 0);
    FlatWrite8(Work() + QuestWork::kPlaced, uint8_t(FlatRead8(Work() + QuestWork::kPlaced) + 1));
}

// The first of the six slot copies (+0x319, 8 apart) holding `monster` (or
// none: 0), 6 when none does.
uint32_t SlotOf(uint32_t work, uint32_t monster) {
    for (uint32_t i = 0; i < 6; ++i)
        if (FlatRead8(work + QuestWork::kSlots + i * 8) == monster)
            return i;

    return 6;
}

void PlaceMonsters(GuestCall& game, uint32_t work, uint32_t desc) {
    game.Call<VecInit>({desc});
    const uint32_t field = QuestWork::MoveWork(QuestWork::kField);

    if (!field)
        return;

    uint32_t list, quest = 0;

    if (const uint32_t q = Work(); q && (list = FlatRead32(q + QuestWork::kQuestActive)) != 0)
        quest = list;
    else
        list = FlatRead32(kFreeRoam + FlatRead32(field + 0x144) * 4);

    if (!list)
        return;

    if (quest) {
        game.Call<0x803AE094>({FlatRead8(field + 0xE9), field});

        for (uint32_t k = 0; k < 3; ++k)
            if (FlatRead32(work + 0x94 + k * 0x60) & 1)
                FlatWrite32(work + 0x94 + k * 0x60, FlatRead32(work + 0x94 + k * 0x60) | 8);

        return;
    }

    // The free roam: each listed monster, `count` (+4) times.
    for (uint32_t entry = list; FlatRead32(entry) != 0; entry += 48) {
        if (int8_t(FlatRead8(entry + 40)) == -1)
            continue;

        for (int32_t c = 0; c < int32_t(FlatRead32(entry + 4)); ++c) {
            if (int8_t(FlatRead8(work + QuestWork::kPlaced)) >= 2)
                continue;

            uint32_t k;

            if (const uint32_t q = Work(); q && FlatRead32(q + QuestWork::kQuestActive)) {
                k = SlotOf(q, FlatRead32(entry) & 0xFF);

                if (k >= 6)
                    break;
            } else {
                k = uint32_t(int32_t(int8_t(FlatRead8(entry + 40))));
            }

            const int32_t n = int8_t(FlatRead8(work + QuestWork::kPlaced));
            FlatWrite16(work + QuestWork::kSpawnIds + k * 2, uint16_t(n));
            FlatWrite32(field + k * 4 + 0x225C, uint32_t(int32_t(int8_t(FlatRead8(work + QuestWork::kPlaced)))));
            QuestSpawns::CopySpawnRecord(game, desc, entry + 8);
            NoteSpawn(field, k, FlatRead32(field + k * 4 + 0x225C), entry, uint32_t(int16_t(FlatRead32(entry + 4))), FlatRead32(entry) & 0xFFFF);
            const uint8_t mode = FlatRead8(entry + 10);
            const uint32_t monster = FlatRead32(entry) & 0xFF;
            uint32_t held = 0, enemy;

            if (mode == 3) {
                held = QuestSpawns::SpawnHeld(game, monster, desc, uint32_t(int32_t(int8_t(FlatRead8(work + QuestWork::kPlaced)))) & 0xFFFF);

                if (!held)
                    continue;

                enemy = FlatRead32(held + 48);
                FlatWrite32(field + k * 16 + 0x2280, held);
            } else {
                enemy = QuestSpawns::CreateQuestEnemy(game, monster, desc, uint32_t(int32_t(int8_t(FlatRead8(work + QuestWork::kPlaced)))) & 0xFFFF);

                if (!enemy)
                    continue;
            }

            const int32_t placed = int8_t(FlatRead8(work + QuestWork::kPlaced));
            FlatWrite16(work + QuestWork::kSpawnIds + placed * 2, uint16_t(placed));

            if (int8_t(FlatRead8(entry + 40)) >= 3)
                continue;

            const uint32_t record = work + k * 0x60;
            FlatWrite32(record + 0x94, FlatRead32(entry + 44) | 8);
            FlatWrite16(record + 0x98, uint16_t(FlatRead32(entry)));
            FlatWrite16(record + 0x9A, 1);
            FlatWrite8((mode == 3 ? held : enemy) + 13, uint8_t(k));
            FlatWrite8(work + QuestWork::kMonsters + k, uint8_t(FlatRead32(entry)));
            FlatWrite8(work + QuestWork::kPlaced, uint8_t(FlatRead8(work + QuestWork::kPlaced) + 1));
        }
    }

    // The extra monster of the list at +0x675C.
    const uint32_t q = Work();

    if (!q || !(list = FlatRead32(q + QuestWork::kQuestActive)) || !FlatRead8(q + QuestWork::kExtraSlot) || int8_t(FlatRead8(work + QuestWork::kPlaced)) >= 2)
        return;

    const uint32_t k = SlotOf(q, 0);

    if (k >= 3)
        return;

    for (uint32_t entry = list; FlatRead32(entry) != 0; entry += 48) {
        if (int8_t(FlatRead8(entry + 40)) == -1 || FlatRead8(Work() + QuestWork::kExtraSlot) != (FlatRead32(entry) & 0xFF))
            continue;

        for (int32_t c = 0; c < int32_t(FlatRead32(entry + 4)); ++c) {
            QuestSpawns::CopySpawnRecord(game, desc, entry + 8);
            const uint8_t mode = FlatRead8(entry + 10);
            const uint32_t monster = FlatRead32(entry) & 0xFF;
            uint32_t held = 0, enemy;

            if (mode == 3) {
                held = QuestSpawns::SpawnHeld(game, monster, desc, uint32_t(int32_t(int8_t(FlatRead8(work + QuestWork::kPlaced)))) & 0xFFFF);

                if (!held)
                    continue;

                enemy = FlatRead32(held + 48);
            } else {
                enemy = QuestSpawns::CreateQuestEnemy(game, monster, desc, uint32_t(int32_t(int8_t(FlatRead8(work + QuestWork::kPlaced)))) & 0xFFFF);

                if (!enemy)
                    continue;
            }

            const int32_t placed = int8_t(FlatRead8(work + QuestWork::kPlaced));
            FlatWrite16(work + QuestWork::kSpawnIds + placed * 2, uint16_t(placed));
            const uint32_t record = work + k * 0x60;
            FlatWrite32(record + 0x94, FlatRead32(entry + 44) | 8);
            FlatWrite16(record + 0x98, uint16_t(FlatRead32(entry)));
            FlatWrite16(record + 0x9A, 1);
            FlatWrite8((mode == 3 ? held : enemy) + 13, uint8_t(k));
            FlatWrite8(work + QuestWork::kMonsters + k, uint8_t(FlatRead32(entry)));
            FlatWrite8(work + QuestWork::kPlaced, uint8_t(FlatRead8(work + QuestWork::kPlaced) + 1));
            return;
        }
    }
}

void FieldOnlineLoad(GuestCall& game, uint32_t field) {
    game.Call<0x800D0F78>({1, 1}); // loading_disp_set

    switch (FlatRead8(field + 1)) {
    case 0:
        FlatWrite8(field + 0x22E5, 1);
        game.Call<0x800EEF60>(); // quest_snd_wk_init
        game.Call<0x803AEF34>();
        game.Call<0x802678D8>(); // init_player_work
        game.Call<0x80142360>();
        game.Call<0x8028C4CC>({field, 0});
        FlatWrite8(field + 1, uint8_t(FlatRead8(field + 1) + 1));
        break;
    case 1:
        if (game.Call<0x800CEA98>({0, 0}) != 1) { // file_loading_ck
            game.Call<0x8028C618>();               // pl_motion_set
            FlatWrite8(field + 1, uint8_t(FlatRead8(field + 1) + 1));
        }

        break;
    case 2:
        game.Call<0x802BF2E8>(); // light_init

        if (FlatRead8(field + 0xEF) != 0)
            game.Call<0x802C2764>();

        for (const uint32_t reset : {0x8038531Cu, 0x803850CCu, 0x800F6618u, 0x802AA70Cu, 0x802B5744u, 0x802ADA30u, 0x802E4B38u, 0x802A05CCu, 0x800D849Cu, 0x800F6398u})
            game(reset);

        FlatWrite8(field + 1, uint8_t(FlatRead8(field + 1) + 1));
        break;
    case 3:
        if (game.Call<0x80268250>() == 0) // player_move_start_ck
            break;

        game.Call<0x802682C4>({0xFFFFFFFF}); // player_move_start

        if (FlatRead8(kPlayMode) == 3 || FlatRead8(kPlayMode) == 6)
            game.Call<0x803AF518>(); // QuestMapSetup
        else
            FlatWrite8(field + 0xFA, 2);

        game.Call<LoadMapOverlay>({FlatRead8(field + 0xED)});
        game.Call<0x802B2214>({FlatRead8(field + 0xED)});
        game.Call<0x802B237C>({FlatRead8(field + 0xED)});
        game.Call<0x802AE624>({0, FlatRead8(field + 0xED), FlatRead8(field + 0xEA)});
        game.Call<0x803C3AD4>(); // EventDemoSetup

        if (FlatRead8(0x8065B231) != 1)
            game.Call<0x803A8754>({FlatRead8(field + 0xED), FlatRead8(field + 0xEA)});

        FlatWrite8(field + 1, uint8_t(FlatRead8(field + 1) + 1));
        break;
    case 4: {
        if (game.Call<0x800CEA98>({0, 0}) == 1)
            break;

        FlatWrite16(field + 4, 0);
        FlatWrite8(field + 1, uint8_t(FlatRead8(field + 1) + 1));
        const uint32_t map = FlatRead8(field + 0xED), area = FlatRead8(field + 0xEA);
        game.Call<0x802AEF5C>({map, area}); // StageSet
        const uint32_t start = game.Call<0x802AFCF8>({FlatRead8(field + 0xED), FlatRead8(field + 0xEA)});
        game.Call<0x80041EA4>({field + 0x128, start});
        const uint32_t angle = game.Call<0x802AFE6C>({FlatRead8(field + 0xED), FlatRead8(field + 0xEA)}) & 0xFFFF;
        FlatWrite16(field + 0x134, uint16_t(angle));
        game.Call<0x802466B8>({field + 0x128, angle, FlatRead8(field + 0xED), FlatRead8(field + 0xEA)});
        game.Call<0x802C078C>();
        game.Call<0x802B8BF0>();
        game.Call<0x803A8750>({field});

        if (FlatRead8(0x8065B231) != 1)
            game.Call<0x80385638>();

        game.Call<0x803AD990>();
        game.Call<0x803BA604>(); // GatherSpotsUpdate
        game.Call<0x8035ABB4>();
        const int32_t kind = int8_t(game.Call<0x802AFBAC>({FlatRead8(field + 0xED), FlatRead8(field + 0xEA)}));
        FlatWrite32(field + FlatRead8(field + 0xE8) * 4 + 0xE0, uint32_t(kind));
        game.Call<0x800EF0C8>({FlatRead8(field + 0xE9), FlatRead8(field + 0xEA), FlatRead8(field + 0x22D8)});
        game.Call<0x800EFB24>();
        FlatWrite8(field + 2, 0);
        break;
    }
    case 5: {
        const uint8_t sub = FlatRead8(field + 2);

        if (sub == 0) {
            if (game.Call<0x800CEA98>({0, 0}) == 1 || game.Call<0x800EF218>() == 0)
                break;

            FlatWrite8(field + 2, uint8_t(FlatRead8(field + 2) + 1));
        }

        if (sub <= 1) {
            game.Call<0x800580A8>({0, 2}); // fade_set

            if (game.Call<0x8026744C>() == 0)
                break;

            if (QuestWork::Online()) {
                FlatWrite16(field + 4, 0);
                FlatWrite16(field + 6, 0);
                FlatWrite8(field + 2, uint8_t(FlatRead8(field + 2) + 1));
            } else {
                FlatWrite8(field + 1, uint8_t(FlatRead8(field + 1) + 1));
                FlatWrite8(field + 2, 0);
            }

            break;
        }

        if (sub == 2) {
            game.Call<0x800580A8>({0, 2});

            if (!QuestWork::Online())
                break;

            if (game.Call<0x8042A690>() == 1 || FlatRead8(0x8065B321) == 1) { // NetCtrlLost
                game.Call<0x800D0F78>({0, 1});
                FlatWrite8(field + 0xFA, 8);

                if (game.Call<0x8042A690>() == 1)
                    game.Call<0x8042B620>();

                game.Call<0x800427F0>({0}); // PadClearClassic
                FlatWrite8(field, 3);
                FlatWrite8(field + 1, 2);
                FlatWrite8(field + 2, 2);
                FlatWrite16(field + 4, 0);
                FlatWrite8(field + 0x22DA, 1);
                FlatWrite8(0x8065B235, 1);
                break;
            }

            if (game.Call<0x8042B2C0>() == 0)
                break;

            if (QuestWork::Host() && int16_t(FlatRead16(field + 4)) == 0 && game.Call<0x80338CAC>({3}) == 1) {
                FlatWrite16(field + 4, 1);
                game.Call<0x80338BB0>({game.Call<0x80451344>() & 0xFF, 3});
            }

            // Steps waiting for the members: a sync message every 32 ticks.
            const uint32_t steps = MH3_TickLoad(uint32_t(int32_t(int16_t(FlatRead16(field + 6)))), 0x8028CB4C) + 1;
            FlatWrite16(field + 6, uint16_t(steps));

            if ((int16_t(steps) & 31) == 0)
                game.Call<0x80338B38>({game.Call<0x80451344>() & 0xFF, 3});

            if (game.Call<0x80338D48>({3}) == 0)
                break;

            FlatWrite8(field + 1, uint8_t(FlatRead8(field + 1) + 1));
            FlatWrite8(field + 2, 0);
            FlatWrite16(field + 4, 0);

            for (uint32_t member = 0; member < 4; ++member)
                if (game.Call<0x804515A0>({member}) == 1)
                    FlatWrite8(field + 0x22DD + member, 1);

            FlatWrite8(field + 0x22E1, uint8_t(game.Call<0x804513DC>()));
            break;
        }

        if (sub == 3) {
            game.Call<0x800580A8>({0, 2});
            FlatWrite8(field + 1, uint8_t(FlatRead8(field + 1) + 1));
            FlatWrite8(field + 2, 0);
            FlatWrite16(field + 4, 0);
        }

        break;
    }
    case 6:
        if (FlatRead8(field + 2) < 2) {
            game.Call<0x803C3FD0>(); // EventDemoDriver
            FlatWrite8(field + 2, uint8_t(FlatRead8(field + 2) + 1));
        } else if (FlatRead8(field + 2) == 2) {
            FlatWrite8(field + 1, uint8_t(FlatRead8(field + 1) + 1));
        }

        break;
    case 7:
        FlatWrite8(0x8065B235, 1);

        if (QuestWork::Online()) {
            FlatWrite8(field + 0x22DA, 1);
            FlatWrite8(0x8065B2D0, 1);
            game.Call<0x80456268>({1});
        } else {
            FlatWrite8(field + 0x22DA, 0);
            FlatWrite8(0x8065B2D0, 0);
        }

        FlatWrite8(field + 0x22E5, 0);
        FlatWrite8(field, 2);
        FlatWrite8(field + 1, 0);
        FlatWrite8(field + 2, 0);
        game.Call<0x80057904>(); // GlareFilter_on
        game.Call<0x8005806C>();
        FlatWrite16(field + 4, 30);
        game.Call<0x800580A8>({0, 2});
        game.Call<0x800D0F78>({0, 0});
        game.Call<0x8043608C>();
        game.Call<0x80040870>();
        return;
    default:
        break;
    }

    game.Call<0x80267D64>(); // player_control_move
}

// Ports.

void QuestSetupPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x20);
    GuestCall game{ctx};
    QuestSetup(game);
}

void QuestLoadPort(CpuContext* ctx) {
    const uint32_t id = ctx->gpr[3];
    GuestFrame frame(ctx, 0x30);
    GuestCall game{ctx};
    QuestLoad(game, id);
}

void QuestEntryCopyPort(CpuContext* ctx) {
    QuestEntryCopy(ctx->gpr[3], ctx->gpr[4]);
}

void QuestLoadMonstersPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x20);
    GuestCall game{ctx};
    QuestLoadMonsters(game);
}

void QuestObjectiveSetupPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x10);
    GuestCall game{ctx};
    QuestObjectiveSetup(game);
}

void QuestPlaceMonstersPort(CpuContext* ctx) {
    const uint32_t key = ctx->gpr[3], field = ctx->gpr[4];
    GuestFrame frame(ctx, 0x50);
    GuestCall game{ctx};
    QuestPlaceMonsters(game, frame[8], key, field);
}

void PlaceMonstersPort(CpuContext* ctx) {
    const uint32_t work = ctx->gpr[3];
    GuestFrame frame(ctx, 0x50);
    GuestCall game{ctx};
    PlaceMonsters(game, work, frame[8]);
}

void QuestGatherSetupPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x20);
    GuestCall game{ctx};
    QuestGatherSetup(game);
}

void NpcDanceLoadPort(CpuContext* ctx) {
    const uint32_t kamen = ctx->gpr[3], dance1 = ctx->gpr[4], dance2 = ctx->gpr[5];
    GuestFrame frame(ctx, 0x140);
    GuestCall game{ctx};
    NpcDanceLoad(game, frame[0], kamen, dance1, dance2);
}

void FieldOnlineLoadPort(CpuContext* ctx) {
    const uint32_t field = ctx->gpr[3];
    GuestFrame frame(ctx, 0x20);
    GuestCall game{ctx};
    FieldOnlineLoad(game, field);
}
} // namespace

extern "C" void func_803AEF34(CpuContext* ctx);
extern "C" void MH3_QuestSetup(CpuContext* ctx) {
    PortCheck::Call(ctx, "QuestSetup", func_803AEF34, QuestSetupPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x803AEF34, MH3_QuestSetup, "QuestSetup");

extern "C" void func_803A88F0(CpuContext* ctx);
extern "C" void MH3_QuestLoad(CpuContext* ctx) {
    PortCheck::Call(ctx, "QuestLoad", func_803A88F0, QuestLoadPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x803A88F0, MH3_QuestLoad, "QuestLoad");

extern "C" void func_803A89F8(CpuContext* ctx);
extern "C" void MH3_QuestEntryCopy(CpuContext* ctx) {
    PortCheck::Call(ctx, "QuestEntryCopy", func_803A89F8, QuestEntryCopyPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x803A89F8, MH3_QuestEntryCopy, "QuestEntryCopy");

extern "C" void func_803ADAD4(CpuContext* ctx);
extern "C" void MH3_QuestLoadMonsters(CpuContext* ctx) {
    PortCheck::Call(ctx, "QuestLoadMonsters", func_803ADAD4, QuestLoadMonstersPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x803ADAD4, MH3_QuestLoadMonsters, "QuestLoadMonsters");

extern "C" void func_803ADD88(CpuContext* ctx);
extern "C" void MH3_QuestObjectiveSetup(CpuContext* ctx) {
    PortCheck::Call(ctx, "QuestObjectiveSetup", func_803ADD88, QuestObjectiveSetupPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x803ADD88, MH3_QuestObjectiveSetup, "QuestObjectiveSetup");

extern "C" void func_803AE094(CpuContext* ctx);
extern "C" void MH3_QuestPlaceMonsters(CpuContext* ctx) {
    PortCheck::Call(ctx, "QuestPlaceMonsters", func_803AE094, QuestPlaceMonstersPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x803AE094, MH3_QuestPlaceMonsters, "QuestPlaceMonsters");

extern "C" void func_803AE488(CpuContext* ctx);
extern "C" void MH3_PlaceMonsters(CpuContext* ctx) {
    PortCheck::Call(ctx, "PlaceMonsters", func_803AE488, PlaceMonstersPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x803AE488, MH3_PlaceMonsters, "PlaceMonsters");

extern "C" void func_803AEC24(CpuContext* ctx);
extern "C" void MH3_QuestGatherSetup(CpuContext* ctx) {
    PortCheck::Call(ctx, "QuestGatherSetup", func_803AEC24, QuestGatherSetupPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x803AEC24, MH3_QuestGatherSetup, "QuestGatherSetup");

extern "C" void func_800EF2D4(CpuContext* ctx);
extern "C" void MH3_NpcDanceLoad(CpuContext* ctx) {
    PortCheck::Call(ctx, "NpcDanceLoad", func_800EF2D4, NpcDanceLoadPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x800EF2D4, MH3_NpcDanceLoad, "NpcDanceLoad");

extern "C" void func_8028C730(CpuContext* ctx);
extern "C" void MH3_FieldOnlineLoad(CpuContext* ctx) {
    PortCheck::Call(ctx, "FieldOnlineLoad", func_8028C730, FieldOnlineLoadPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8028C730, MH3_FieldOnlineLoad, "FieldOnlineLoad");
