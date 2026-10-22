// The running quest, RMHP08: its step and its timed, extra and marathon
// monsters. C reconstructions from the translated code.
//
//   0x803B0228 QuestStep(): once a logic step. +0x2C 0 starts the quest
//              (0x803AFDA8); 1 runs it while +0x6A3C is 2: paused (the event
//              demo 0x806D4FA0 past 1, the event state 0x806C13A0, the system
//              pause 0x8065AA90) it only moves the clock's time base on, else
//              the reward check (0 or less fails), the objectives by the
//              objective mode +0x93 (1 any, 2 the first, 3 the first two, 4
//              all, 5 and 6 special), the clock (QuestClockTick), the time
//              messages, the deliveries (+0x6A28), the timed, marathon and
//              extra monsters (below) and every 32 steps a message for each
//              objective met but not yet confirmed; 2 and 4 the end
//   0x803B0E28 QuestTimedSpawn(): with the timed flag, frames counted at
//              +0x697C; when they reach 5 minutes (5.0 * 60 * fps) or a quest
//              monster falls (+0x6977 0), the second slot's monster, if held
//              (mode 3), is summoned (SummonRelease); online the leader
//              summons and tells the others (0x80339AAC)
//   0x803B0FFC QuestExtraSpawn(): the extra monster (+0x6975): with flag 0x100
//              3 minutes in the third slot's (+0x6760, copied to the slot
//              copy +0x331); an intruder of kind 1/2/3 (+0x32E) after 3
//              minutes, 3 with one monster placed and it not 0x8012ED54, or
//              with two; then its files, its placement and a live spawn
//              (SpawnLive); online through the members' sync (states 10-13)
//   0x803B17E0 QuestMarathonSpawn(field): with the marathon flag, the next
//              slot's monster once the last one fell (+0x6977 0) or the
//              timed flag's clock ran out: its files, its slot copied from
//              the entry, a live spawn
#include "abi_bridge.h"
#include "guest_call.h"
#include "port_check.h"
#include "ppc_runtime.h"
#include "quest_work.h"

#include <cstdint>
#include <utility>

namespace QuestSpawns {
void SpawnLive(GuestCall& game, uint32_t kind, uint32_t record, uint32_t id);
void CopySpawnRecord(GuestCall& game, uint32_t to, uint32_t from);
uint32_t SummonRelease(GuestCall& game, const uint32_t (&args)[11]);
} // namespace QuestSpawns

namespace {
using MemoryInline::FlatRead16;
using MemoryInline::FlatRead32;
using MemoryInline::FlatRead8;
using MemoryInline::FlatWrite16;
using MemoryInline::FlatWrite32;
using MemoryInline::FlatWrite8;
using QuestWork::Online;
using QuestWork::Work;

constexpr uint32_t kFrameRate = 0x8065B4D0;
constexpr uint32_t kMagic = 0x8079E9E8;      // f64 2^52 + 2^31 (r2 - 0x1578)
constexpr uint32_t kSixty = 0x8079E9E4;      // f32 60 (r2 - 0x157C)
constexpr uint32_t kTimedMinutes = 0x8079EA08; // f32 5 (r2 - 0x1558)
constexpr uint32_t kExtraMinutes = 0x8079EA0C; // f32 3 (r2 - 0x1554)
constexpr uint32_t kLists = 0x805F7E60, kMapKinds = 0x805CF500, kTimeMessages = 0x805F8084;
constexpr uint32_t kNowMap = 0x806BB805;  // get_now_mapno, inlined
constexpr uint32_t kMyPlayer = 0x8065AA87; // MyPlayerIndex, inlined
constexpr uint32_t kIntruderPicker = 0x806CE890; // (kind byte, index, quest work) -> 1 to send one

enum : uint32_t {
    ObjectiveDone = 0x803AA0C4,
    ObjectiveFailed = 0x803AA21C,
    QuestClockTick = 0x803A9014,
    QuestTimeLeft = 0x803A8844,
    QuestFail = 0x803AFEB0,
    QuestClear = 0x803AFF98,
    NetQuestEnd = 0x80339C4C,
    NetQuestFail = 0x80339B2C,
    Message = 0x803B3414,
    Sprintf = 0x8045E54C,
    ShowMessage = 0x8035B660,
    VecInit = 0x80125FB8,
    LoadMonster = 0x801410B4,
    MonsterLoaded = 0x80146B80,
    FileLoading = 0x800CEA98,
    EnemyOf = 0x80144644, // em_get_unique_work(id, &work, &mini)
    EnemyLeaving = 0x8012ED54,
    TimedFlag = 0x803B4D4C,
    MarathonFlag = 0x803B4DC4,
    ExtraFlag = 0x803B4E3C,
};

double Float(uint32_t value) {
    return double(float(int32_t(value)));
}

// MapKind(get_now_mapno()), inlined (0x802B06CC).
uint32_t MapKindNow() {
    const uint32_t map = FlatRead8(kNowMap);
    const uint32_t kind = FlatRead8(kMapKinds + map);
    return kind == 0xFF ? map : kind;
}

// The spawn list entry of `monster` for the current map, 0 for none.
uint32_t SpawnListEntry(uint32_t monster) {
    uint32_t entry = FlatRead32(kLists + monster * 4);

    if (!entry)
        return 0;

    for (uint32_t key; (key = FlatRead8(entry)) != 0xFF; entry += 8)
        if (key == MapKindNow())
            return entry;

    return 0;
}

// Frames counted towards a timed spawn reached `minutes` (+0x697C as a
// float, minutes * 60 * fps).
bool FramesReached(uint32_t work, uint32_t minutes) {
    const float frames = float(int32_t(FlatRead32(work + QuestWork::kSpawnFrames)));
    const float perMinute = Memory::ReadFloat32(kSixty) * Memory::ReadFloat32(kFrameRate);
    return !(frames < Memory::ReadFloat32(minutes) * perMinute);
}

void CountFrames(uint32_t work, uint32_t site) {
    FlatWrite32(work + QuestWork::kSpawnFrames, MH3_TickLoad(FlatRead32(work + QuestWork::kSpawnFrames), site) + 1);
}

void Format(GuestCall& game, uint32_t buffer, uint32_t format, uint32_t value) {
    game.cpu->cr &= ~0x02000000u; // no float arguments
    game.Call<Sprintf>({buffer, format, value});
}

// The second slot's monster summoned, when it is held (mode 3).
void ReleaseSecond(GuestCall& game, uint32_t work) {
    const uint32_t monster = FlatRead8(work + QuestWork::kSlots + 8);

    if (!monster || FlatRead8(work + QuestWork::kSlots + 10) != 3)
        return;

    const uint32_t args[11] = {0xFFFF, monster, 0, 0xFF, 0xFF, 1, 1, 0, 0xFF, 0, 0};
    QuestSpawns::SummonRelease(game, args);
}

void QuestTimedSpawn(GuestCall& game) {
    if (game.Call<TimedFlag>({0}) == 0 || !QuestWork::MoveWork(QuestWork::kField))
        return;

    const uint32_t work = Work();

    if (!work)
        return;

    switch (int8_t(FlatRead8(work + QuestWork::kSpawnState))) {
    case 0:
        CountFrames(work, 0x803B0E84);

        if (FlatRead8(work + QuestWork::kWaiting) == 0 || FramesReached(work, kTimedMinutes))
            FlatWrite8(work + QuestWork::kSpawnState, Online() ? 2 : 1);

        break;
    case 1:
        if (!Online()) {
            FlatWrite8(work + QuestWork::kSpawnState, 2);
        } else if (QuestWork::Host()) {
            if (FlatRead8(work + QuestWork::kSlots + 8) && FlatRead8(work + QuestWork::kSlots + 10) == 3) {
                ReleaseSecond(game, work);
                game.Call<0x80339AAC>();
            }

            FlatWrite8(work + QuestWork::kSpawnState, 3);
        }

        break;
    case 2:
        ReleaseSecond(game, work);
        FlatWrite8(work + QuestWork::kSpawnState, 3);
        break;
    default:
        break;
    }
}

// The slot copy `n` (+0x319 + 8n) noted as spawn `n` in the quest work and
// the field's tables, from its spawn list entry.
void NoteSlotSpawn(uint32_t field, uint32_t n, uint32_t slot, uint32_t entry) {
    FlatWrite16(Work() + QuestWork::kSpawnIds + n * 2, uint16_t(n));
    FlatWrite8(Work() + n + 0x478, FlatRead8(slot + 4));
    FlatWrite8(Work() + n + 0x47E, FlatRead8(slot + 5));
    FlatWrite8(Work() + n + 0x484, FlatRead8(slot + 6));
    FlatWrite8(Work() + n + 0x48A, FlatRead8(slot + 7));
    const uint32_t at = field + n * 16;
    FlatWrite32(at + 0x2274, FlatRead16(Work() + QuestWork::kSpawnIds + n * 2));
    FlatWrite32(at + 0x227C, FlatRead32(entry + 4));
    FlatWrite16(at + 0x2278, uint16_t(int16_t(FlatRead32(FlatRead32(entry + 4) + 4))));
    FlatWrite16(at + 0x227A, FlatRead8(slot));
    FlatWrite32(at + 0x2280, 0);
}

void SpawnFromSlot(GuestCall& game, uint32_t desc, uint32_t slot, uint32_t entry, uint32_t id) {
    QuestSpawns::CopySpawnRecord(game, desc, FlatRead32(entry + 4) + FlatRead8(slot + 1) * 48 + 8);
    QuestSpawns::SpawnLive(game, FlatRead8(slot), desc, id & 0xFFFF);
}

// The extra spawn's live enemy came (em_get_unique_work): the state machine
// starts over for the next one.
void ExtraArrived(GuestCall& game, uint32_t work, uint32_t outs) {
    FlatWrite32(outs + 4, 0);
    FlatWrite32(outs, 0);

    if ((game.Call<EnemyOf>({(FlatRead8(work + QuestWork::kWaiting) + 3u) & 0xFFFF, outs + 4, outs}) & 0xFF) == 0)
        return;

    FlatWrite8(work + QuestWork::kSpawnState, 0);
    FlatWrite8(work + QuestWork::kWaiting, uint8_t(FlatRead8(work + QuestWork::kWaiting) + 1));
}

void QuestExtraSpawn(GuestCall& game, uint32_t sp) {
    const uint32_t field = QuestWork::MoveWork(QuestWork::kField);
    const uint32_t work = Work();

    if (!field || !work || FlatRead8(work + QuestWork::kExtraOn) == 0)
        return;

    const uint32_t kind = game.Call<0x803B4B9C>({0}) & 0xFF;
    const uint8_t state = FlatRead8(work + QuestWork::kSpawnState);
    const auto next = [&] { FlatWrite8(work + QuestWork::kSpawnState, uint8_t(FlatRead8(work + QuestWork::kSpawnState) + 1)); };

    switch (state) {
    case 0:
        if (game.Call<ExtraFlag>({0}) == 1) {
            if (FlatRead8(work + QuestWork::kWaiting) == 2)
                break;

            CountFrames(work, 0x803B1098);

            if (!FramesReached(work, kExtraMinutes))
                break;

            game.Call<0x80004000>({work + QuestWork::kSlots + 24, work + QuestWork::kExtraSlot, 8});

            if (!Online()) {
                FlatWrite8(work + QuestWork::kSpawnState, 4);
            } else {
                game.Call<0x80339730>();
                FlatWrite8(work + QuestWork::kSpawnState, 11);
            }
        } else if (kind == 1) {
            if (FlatRead8(work + QuestWork::kWaiting) == 2)
                break;

            CountFrames(work, 0x803B1144);

            if (!FramesReached(work, kExtraMinutes))
                break;

            FlatWrite32(work + QuestWork::kSpawnFrames, 0);
            next();
        } else if (kind == 2) {
            FlatWrite32(sp + 36, 0);
            FlatWrite32(sp + 32, 0);

            if (FlatRead8(work + QuestWork::kWaiting) == 1 || int8_t(FlatRead8(work + QuestWork::kPlaced)) != 1)
                break;

            if ((game.Call<EnemyOf>({0, sp + 36, sp + 32}) & 0xFF) == 0 || game.Call<EnemyLeaving>({FlatRead32(sp + 36)}) == 1)
                break;

            CountFrames(work, 0x803B11F8);

            if (!FramesReached(work, kExtraMinutes))
                break;

            FlatWrite32(work + QuestWork::kSpawnFrames, 0);
            next();
        } else if (kind == 3) {
            FlatWrite32(sp + 28, 0);
            FlatWrite32(sp + 24, 0);

            if (FlatRead8(work + QuestWork::kWaiting) == 1 || int8_t(FlatRead8(work + QuestWork::kPlaced)) != 2)
                break;

            // The one live slot (of the first three) with a count.
            uint32_t live = 0, index = 0;

            for (uint32_t i = 0; i < 3; ++i)
                if (int16_t(FlatRead16(field + 0x2278 + i * 16)) > 0)
                    ++live, index = i;

            if (live > 1)
                break;

            if ((game.Call<EnemyOf>({index, sp + 28, sp + 24}) & 0xFF) == 0 || game.Call<EnemyLeaving>({FlatRead32(sp + 28)}) == 1)
                break;

            CountFrames(work, 0x803B12EC);

            if (!FramesReached(work, kExtraMinutes))
                break;

            FlatWrite32(work + QuestWork::kSpawnFrames, 0);
            next();
        }

        break;
    case 1: {
        FlatWrite32(work + QuestWork::kSpawnFrames, 0);
        const uint32_t picked = game(FlatRead32(kIntruderPicker), {FlatRead8(FlatRead32(Work() + QuestWork::kEntry) + 0x32F), FlatRead8(work + QuestWork::kWaiting), work});

        if ((picked & 0xFF) == 0) {
            FlatWrite8(work + QuestWork::kSpawnState, 0);
        } else if (game.Call<0x803B6F90>({FlatRead8(work + (FlatRead8(work + QuestWork::kWaiting) + 3) * 8 + QuestWork::kSlots)}) == 1) {
            FlatWrite8(work + QuestWork::kSpawnState, 0);
        } else {
            next();
        }

        break;
    }
    case 2: {
        const uint32_t slot = work + (FlatRead8(work + QuestWork::kWaiting) + 3) * 8 + QuestWork::kSlots;

        if (game.Call<MonsterLoaded>({FlatRead8(slot)}) == 0)
            break;

        game.Call<LoadMonster>({FlatRead8(slot)});
        FlatWrite8(work + QuestWork::kExtraLoaded, 1);

        if (!Online())
            next();
        else
            FlatWrite8(work + QuestWork::kSpawnState, 10);

        break;
    }
    case 3:
        if (game.Call<FileLoading>({0, 0}) != 1)
            next();

        break;
    case 4:
    case 12: {
        const uint32_t desc = sp + (state == 4 ? 72 : 40);
        game.Call<VecInit>({desc});
        const uint32_t n = game.Call<ExtraFlag>({0}) == 1 ? 3 : FlatRead8(work + QuestWork::kWaiting) + 3;
        const uint32_t slot = work + n * 8 + QuestWork::kSlots;
        const uint32_t entry = FlatRead32(kLists + FlatRead8(slot) * 4) ? SpawnListEntry(FlatRead8(slot)) : 0;

        if (!entry)
            break;

        NoteSlotSpawn(field, n, slot, entry);

        if (state == 12 && !QuestWork::Host()) {
            ExtraArrived(game, work, sp + 8);
            break;
        }

        SpawnFromSlot(game, desc, slot, entry, n);
        next();
        break;
    }
    case 5:
        ExtraArrived(game, work, sp + 16);
        break;
    case 10:
        if (game.Call<FileLoading>({0, 0}) != 1) {
            game.Call<0x80339730>();
            next();
        }

        break;
    case 11:
        if (int32_t(int8_t(game.Call<0x80451350>())) <= int32_t(FlatRead8(work + 0x69A4))) {
            FlatWrite8(work + QuestWork::kSpawnState, 12);
            FlatWrite8(work + 0x69A4, 0);
        }

        break;
    case 13:
        ExtraArrived(game, work, sp + 8);
        break;
    default:
        break;
    }
}

// The marathon's next monster arrived: it gets its objective, and the next
// slot waits.
void MarathonArrived(GuestCall& game, uint32_t work, uint32_t outs) {
    FlatWrite32(outs + 4, 0);
    FlatWrite32(outs, 0);
    const int32_t n = int8_t(FlatRead8(work + QuestWork::kPlaced));

    if ((game.Call<EnemyOf>({uint32_t(n) & 0xFFFF, outs + 4, outs}) & 0xFF) == 0)
        return;

    FlatWrite8(FlatRead32(outs + 4) + 13, uint8_t(int8_t(FlatRead8(work + QuestWork::kPlaced)))); // 0x80137668, inlined
    FlatWrite8(work + QuestWork::kSpawnState, 0);
    FlatWrite8(work + QuestWork::kWaiting, 1);
    FlatWrite8(work + QuestWork::kPlaced, uint8_t(FlatRead8(work + QuestWork::kPlaced) + 1));
}

void QuestMarathonSpawn(GuestCall& game, uint32_t sp, uint32_t field) {
    const uint32_t work = Work();

    if (!work)
        return;

    const uint8_t state = FlatRead8(work + QuestWork::kSpawnState);
    const auto next = [&] { FlatWrite8(work + QuestWork::kSpawnState, uint8_t(FlatRead8(work + QuestWork::kSpawnState) + 1)); };
    const auto slotOf = [&] { return work + uint32_t(int32_t(int8_t(FlatRead8(work + QuestWork::kPlaced)))) * 8; };

    switch (state) {
    case 0:
        if (FlatRead8(work + QuestWork::kWaiting) == 0)
            FlatWrite8(work + QuestWork::kSpawnState, uint8_t(state + 1));

        break;
    case 1: {
        const uint32_t slot = slotOf();

        if (game.Call<MonsterLoaded>({FlatRead8(slot + QuestWork::kSlots)}) == 0)
            break;

        game.Call<LoadMonster>({FlatRead8(slot + QuestWork::kSlots)});

        if (!Online())
            next();
        else
            FlatWrite8(work + QuestWork::kSpawnState, 10);

        break;
    }
    case 2:
        if (game.Call<FileLoading>({0, 0}) != 1)
            next();

        break;
    case 3:
    case 12: {
        const uint32_t desc = sp + (state == 3 ? 56 : 24);
        const uint32_t copy = slotOf();
        game.Call<VecInit>({desc});

        if (!FlatRead32(kLists + FlatRead8(copy + QuestWork::kSlots) * 4))
            break;

        const uint32_t entry = SpawnListEntry(FlatRead8(copy + QuestWork::kSlots));

        if (!entry)
            break;

        // The slot copy from the entry (all but the count, +3).
        const int32_t n = int8_t(FlatRead8(work + QuestWork::kPlaced));
        const uint32_t from = FlatRead32(work + QuestWork::kEntry) + uint32_t(n) * 8 + 0x314, slot = copy + QuestWork::kSlots;

        for (const uint32_t i : {0u, 1u, 2u, 4u, 5u, 6u, 7u})
            FlatWrite8(slot + i, FlatRead8(from + i));

        NoteSlotSpawn(field, uint32_t(n), slot, entry);

        if (state == 12 && !QuestWork::Host()) {
            MarathonArrived(game, work, sp + 8);
            break;
        }

        SpawnFromSlot(game, desc, slot, entry, uint32_t(int32_t(int8_t(FlatRead8(work + QuestWork::kPlaced)))));
        next();
        break;
    }
    case 4:
        MarathonArrived(game, work, sp + 16);
        break;
    case 10:
        if (game.Call<FileLoading>({0, 0}) != 1) {
            game.Call<0x803397B4>();
            next();
        }

        break;
    case 11:
        if (int32_t(int8_t(game.Call<0x80451350>())) <= int32_t(FlatRead8(work + 0x69A4))) {
            FlatWrite8(work + QuestWork::kSpawnState, 12);
            FlatWrite8(work + 0x69A4, 0);
        }

        break;
    case 13:
        MarathonArrived(game, work, sp + 8);
        break;
    default:
        break;
    }
}
} // namespace

namespace {
bool Done(GuestCall& game, uint32_t work, uint32_t objective) {
    return game.Call<ObjectiveDone>({work, objective, 1}) == 1;
}

bool Failed(GuestCall& game, uint32_t work, uint32_t objective) {
    return game.Call<ObjectiveFailed>({work, objective}) == 1;
}

// 0x803AA324, inlined: objective `k` is set and started.
bool Started(uint32_t work, uint32_t k) {
    const uint32_t flags = FlatRead32(work + k * 96 + 148);
    return flags && (flags & 8);
}

void FailQuest(GuestCall& game, uint32_t work, uint32_t field) {
    FlatWrite8(Work() + 0x6A3C, 5);

    if (!Online())
        game.Call<QuestFail>({work, field, 0});
    else if (int32_t(FlatRead32(Work() + 0x2E8)) <= 0)
        game.Call<NetQuestFail>({work, 0, FlatRead32(work + 0x24), 1});
    else
        game.Call<NetQuestEnd>({work, 0, FlatRead32(work + 0x24)});
}

// The objectives by the objective mode: 1 met, -1 failed, 0 not yet.
int32_t ObjectivesByMode(GuestCall& game, uint32_t work) {
    switch (int8_t(FlatRead8(Work() + QuestWork::kObjectiveMode))) {
    case 1:
        return Done(game, work, 0) || Done(game, work, 1) || Done(game, work, 2);
    case 2:
        return Done(game, work, 0);
    case 3:
        return Done(game, work, 0) && Done(game, work, 1);
    case 4: {
        int32_t met = 0;

        for (uint32_t k = 0; k < 3; ++k)
            if (!Started(work, k) || Done(game, work, k))
                ++met;

        if (met == 3)
            return 1;

        for (uint32_t k = 0; k < 3; ++k)
            if (Failed(game, work, k))
                return -1;

        return 0;
    }
    case 5:
        if (game.Call<0x803B510C>({0}) == 1) {
            if (Failed(game, work, 0))
                return -1;

            if (Done(game, work, 1) && Done(game, work, 2)) {
                FlatWrite8(work + 0x6A3A, 1);
                FlatWrite8(work + 0x6A3B, 2);
                return 1;
            }

            return 0;
        }

        if (Done(game, work, 0))
            return 1;

        return Failed(game, work, 1) ? -1 : 0;
    case 6:
        if (Done(game, work, 0) && Done(game, work, 2))
            return 1;

        return Failed(game, work, 0) || Failed(game, work, 2) ? -1 : 0;
    default:
        return 0;
    }
}

// The deliveries and area changes (+0x6A28, +0x6A2C: three of 6 bytes).
void QuestDeliveries(GuestCall& game, uint32_t work, uint32_t field) {
    const int8_t state = int8_t(FlatRead8(work + 0x6A28));

    if (state == 2) {
        FlatWrite8(work + 0x6A28, 3);
        return;
    }

    if (state == 3) {
        if (game.Call<FileLoading>({0, 0}) != 1) {
            game.Call<0x803A7ECC>({field, work + 0x69A8, FlatRead8(work + 0x69A5), FlatRead8(work + 0x6A2A)});
            FlatWrite8(work + 0x6A28, 0);
            FlatWrite8(work + 0x6A29, 0);
        }

        return;
    }

    if (state != 0 || int8_t(FlatRead8(work + 0x6A2A)) == 2)
        return;

    const auto at = [&] { return work + uint32_t(int32_t(int8_t(FlatRead8(work + 0x6A2A)))) * 6; };
    bool advance = false;

    switch (int16_t(FlatRead16(at() + 0x6A2C))) {
    case 3:
        advance = (game.Call<0x803B5670>({1}) & 2) && Done(game, work, 1);
        break;
    case 4:
        advance = (game.Call<0x803B5670>({2}) & 2) && Done(game, work, 2);
        break;
    case 1:
        advance = int16_t(FlatRead16(at() + 0x6A30)) == 0;
        break;
    case 2: {
        const int8_t waiting = int8_t(FlatRead8(work + 0x6A29));

        if (waiting == 1) {
            FlatWrite8(work + 0x6A29, 0);
            advance = true;
        } else if (waiting == 0) {
            const uint32_t hunters = QuestWork::MoveWork(QuestWork::kHunters);

            if (!hunters)
                break;

            const uint32_t hunter = hunters + uint32_t(int32_t(int8_t(FlatRead8(kMyPlayer)))) * 0xB20;
            const uint32_t have = game.Call<0x80273218>({hunter, FlatRead16(at() + 0x6A2E)}) & 0xFFFF;

            if (have != 0 && !(FlatRead16(at() + 0x6A30) < have)) {
                game.Call<0x80339674>();
                FlatWrite8(work + 0x6A29, 1);
                FlatWrite8(work + 0x6A29, 0);
                advance = true;
            }
        }

        break;
    }
    default:
        break;
    }

    if (!advance)
        return;

    game.Call<0x80143BE8>();
    FlatWrite8(work + 0x6A28, 1);
    FlatWrite8(work + 0x6A2A, uint8_t(FlatRead8(work + 0x6A2A) + 1));
    game.Call<0x803AEE84>({FlatRead8(kNowMap), FlatRead8(work + 0x6A2A)});
}

// The quest ended well: the end flags, then the clear (offline) or the
// session's end message.
void QuestEnding(GuestCall& game, uint32_t work, uint32_t field) {
    if (FlatRead8(work + 0x6A3E) != 4 && (int8_t(FlatRead8(work + 0x6A3A)) == 0 || game.Call<0x803B5DF4>() == 1))
        FlatWrite8(work + 0x6A3A, 1);

    if (!Online())
        game.Call<QuestClear>({work, field});
    else
        game.Call<NetQuestEnd>({work, 0, FlatRead32(work + 0x24)});
}

// The quest is under way: the clock, the time messages, the deliveries, the
// extra monsters and the objective messages.
void QuestRunning(GuestCall& game, uint32_t work, uint32_t field, uint32_t buffer) {
    game.Call<QuestClockTick>({1});

    if (int8_t(FlatRead8(work + 0x6A39)) == 0 && int32_t(FlatRead32(work + 0x1C)) - 10 >= int32_t(FlatRead32(work + 0x24))) {
        const uint32_t format = game.Call<Message>({7});
        const float perMinute = Memory::ReadFloat32(kSixty) * Memory::ReadFloat32(kFrameRate);
        const int32_t minutes = int32_t(float(int32_t(FlatRead32(work + 0x1C))) / perMinute);
        Format(game, buffer, format, uint32_t(minutes));
        game.Call<ShowMessage>({0, buffer});
        FlatWrite8(work + 0x6A39, 1);
    }

    const int32_t left = int32_t(game.Call<QuestTimeLeft>());

    if (left <= 0) {
        if (game.Call<0x803B5B80>({work}) != 1) {
            FlatWrite8(Work() + 0x6A3C, 5);

            if (!Online())
                game.Call<QuestFail>({work, field, 1});
            else
                game.Call<NetQuestEnd>({work, 1, FlatRead32(work + 0x24)});

            return;
        }

        FlatWrite8(work + 0x6A3A, 1);
        FlatWrite8(work + 0x6A3C, 3);
        FlatWrite8(work + 0x6A3D, 4);

        if (FlatRead8(0x8065B2C8) != 1) // 0x80058C14, inlined
            FlatWrite8(0x8065B2C8, 1);

        QuestEnding(game, work, field);
        return;
    }

    // The minutes-left messages (+0x6A38 counts down the table 0x805F8084).
    const bool silent = game.Call<0x803B4BD8>({0}) == 1 && (FlatRead8(field + 0xFB) == 6 || FlatRead8(field + 0xFB) == 4);

    if (!silent && int8_t(FlatRead8(work + 0x6A38)) > 0) {
        const uint32_t minutes = FlatRead32(kTimeMessages + uint32_t(int32_t(int8_t(FlatRead8(work + 0x6A38)))) * 4);
        const float perMinute = Memory::ReadFloat32(kSixty) * Memory::ReadFloat32(kFrameRate);

        if (float(left) <= perMinute * float(int32_t(minutes))) {
            const uint32_t format = game.Call<Message>({6});
            Format(game, buffer, format, minutes);
            game.Call<ShowMessage>({0, buffer});
            FlatWrite8(work + 0x6A38, uint8_t(FlatRead8(work + 0x6A38) - 1));
        }
    }

    game.Call<0x803B6B78>();
    QuestDeliveries(game, work, field);
    game.Call<0x803B0E28>();

    if (game.Call<MarathonFlag>({0}) == 1)
        game.Call<0x803B17E0>({field});
    else if ((game.Call<0x803B4B2C>({0}) & 0xFF) == 1 || game.Call<ExtraFlag>({0}) == 1)
        game.Call<0x803B0FFC>();

    FlatWrite32(work + 0x28, MH3_TickLoad(FlatRead32(work + 0x28), 0x803B0C00) + 1);

    if ((MH3_HookedLoad32(work, 40, 0x803B0C0C) & 31) != 0)
        return;

    for (uint32_t k = 0; k < 3; ++k)
        if (game.Call<ObjectiveDone>({work, k, 0}) == 1 && game.Call<ObjectiveDone>({work, k, 1}) == 0)
            game.Call<0x803397F4>({32, k & 0xFF});
}

void QuestMain(GuestCall& game, uint32_t work, uint32_t field, uint32_t buffer) {
    // The reward ran out (not in the event modes 0x8065B231/2): failed.
    if (FlatRead8(0x8065B232) == 0 && FlatRead8(0x8065B231) == 0 && FlatRead32(Work() + QuestWork::kEntry) != 0 && int32_t(FlatRead32(Work() + 0x2E8)) <= 0) {
        FlatWrite32(Work() + 0x2E8, 0);
        FailQuest(game, work, field);
        return;
    }

    game.Call<0x803B5764>({work});
    game.Call<0x803B591C>({work});

    if (game.Call<0x803AA354>({work}) == 1) {
        FailQuest(game, work, field);
        return;
    }

    int32_t done = 0;

    if (!Online() && game.Call<0x803B4BD8>({0}) == 1) {
        if (FlatRead8(field + 0xFB) == 6) {
            game.Call<QuestFail>({work, field, 0});
        } else if (FlatRead8(field + 0xFB) == 4) {
            FlatWrite8(field + 0xFD, 4);
            done = 1;
        }
    } else if (game.Call<0x803B5280>({0}) == 1 && FlatRead8(work + 0x6A3E) == 4) {
        done = 1;
        FlatWrite8(work + 0x6A3F, 4);
    } else if (game.Call<0x803B4C50>({0}) == 1) {
        if (game.Call<ObjectiveDone>({work, 4, 1}) == 1) {
            FlatWrite8(work + 0x6A3F, 4);
            FlatWrite8(work + 0x6A3C, 4);
            done = 1;
        }
    } else {
        done = ObjectivesByMode(game, work);

        if (done == -1) {
            FailQuest(game, work, field);
            return;
        }

        if (done == 1) {
            if (!Online())
                FlatWrite8(field + 0xFD, 4);

            FlatWrite8(work + 0x6A3D, 4);
        }
    }

    if (done != 1)
        QuestRunning(game, work, field, buffer);
    else
        QuestEnding(game, work, field);
}

void QuestStep(GuestCall& game, uint32_t sp) {
    const uint32_t field = QuestWork::MoveWork(QuestWork::kField);
    const uint32_t work = field ? FlatRead32(field + 0xDC) : 0;

    if (!work)
        return;

    if (FlatRead8(field + 0x112) == 0)
        game.Call<0x800F4AF4>({0, FlatRead8(field + 0xE9), FlatRead8(field + 0xEA)});

    FlatWrite8(0x8065AA8A, 0);

    switch (int8_t(FlatRead8(work + 0x2C))) {
    case 0:
        FlatWrite8(work + 0x2C, uint8_t(FlatRead8(work + 0x2C) + 1));
        game.Call<0x803AFDA8>({work});

        if (game.Call<0x803B501C>({0}) == 1)
            game.Call<0x8033A984>({0});

        break;
    case 1:
        if (FlatRead8(Work() + 0x6A3C) != 2)
            break;

        // Paused (an event demo past 1, the event state, the system pause):
        // the clock's time base follows the real time.
        if (FlatRead8(0x806D4FA0) > 1 || FlatRead8(0x806C13A0) != 0 || FlatRead8(0x8065AA90) != 0) {
            game.Call<0x804D53D0>(); // OSGetTime
            game.Call<0x80457948>(); // to a double
            Memory::WriteFloat64(work + 0x6A48, game.cpu->fpr[1].d);
            Memory::WriteFloat64(work + 0x6A50, Memory::ReadFloat64(work + 0x6A48));
            break;
        }

        QuestMain(game, work, field, sp + 8);
        break;
    case 2:
        game.Call<QuestClockTick>({1});

        if (int8_t(FlatRead8(work + 0x2D)) == 0) {
            FlatWrite8(work + 0x2D, uint8_t(FlatRead8(work + 0x2D) + 1));
        } else if (int32_t(game.Call<QuestTimeLeft>()) <= 0) {
            if (uint8_t(FlatRead8(field + 0xFA) + 251) <= 1)
                FlatWrite8(field + 0xFA, 6);

            if (uint8_t(FlatRead8(field + 0xFA) + 253) <= 1)
                FlatWrite8(field + 0xFA, 4);

            FlatWrite8(work + 0x2C, uint8_t(FlatRead8(work + 0x2C) + 1));
        }

        break;
    case 4:
        FlatWrite32(work + 0x28, MH3_TickLoad(FlatRead32(work + 0x28), 0x803B0CE4) + 1);

        if ((MH3_HookedLoad32(work, 40, 0x803B0CF0) & 31) == 0)
            game.Call<NetQuestEnd>({work, FlatRead8(work + 0x6A68), FlatRead32(work + 0x20)});

        break;
    default:
        break;
    }

    if (FlatRead8(field + 0x112) == 0)
        game.Call<0x800F3FFC>();
}

// A large or summoned monster fell (803A9728: work, how 0 a live enemy, 1 a
// summon entry) or a member's did (803A9BE0, online): the kill and capture
// counts, the objectives, the timed and marathon signal (+0x6977 0).
void QuestMonsterDown(GuestCall& game, uint32_t field, uint32_t monster, uint32_t how, uint32_t outs) {
    const uint32_t work = game.Call<0x803A880C>();

    if (!work)
        return;

    uint32_t enemy = 0, id, kind, group;
    int32_t objective;
    uint32_t captured, size = 0;
    bool large = false;

    if ((how & 0xFF) == 0) {
        enemy = monster;
        id = FlatRead16(monster + 26);
        objective = FlatRead8(monster + 13);
        kind = FlatRead8(monster + 3);
        captured = game.Call<0x8012E8E8>({monster});
        game.Call<0x8012E6A8>({monster});

        if (FlatRead32(monster + 456) & 1) {
            large = true;
            game.Call<0x801359B4>({monster});
            const float scale = Memory::ReadFloat32(0x8079E97C) * float(game.cpu->fpr[1].d); // r2 - 0x15E4
            size = uint32_t(int32_t(float(Memory::ReadFloat32(0x8079E978) + scale)));     // r2 - 0x15E8
        }

        group = FlatRead8(monster + 19);
    } else {
        id = FlatRead16(monster + 22);
        objective = FlatRead8(monster + 13);
        kind = FlatRead8(monster + 2);
        captured = FlatRead8(monster + 20) == 2 ? 2 : 0; // 0x8012E924, inlined
        game.Call<0x8012E6B8>({monster});
        group = FlatRead8(monster + 29);
    }

    if ((captured & 0xFF) == 0) {
        FlatWrite16(work + kind * 2 + 0x490, uint16_t(FlatRead16(work + kind * 2 + 0x490) + 1));
        game.Call<0x803B1E1C>({1, kind & 0xFFFF, 1});
    }

    if ((captured & 0xFF) == 1)
        FlatWrite16(work + kind * 2 + 0x4E2, uint16_t(FlatRead16(work + kind * 2 + 0x4E2) + 1));

    if (large) {
        const uint32_t at = work + kind * 4;
        const uint32_t smallest = FlatRead16(at + 0x534), biggest = FlatRead16(at + 0x536);

        if (smallest == 0 || smallest > (size & 0xFFFF))
            FlatWrite16(at + 0x534, uint16_t(size));

        if (biggest == 0 || biggest < (size & 0xFFFF))
            FlatWrite16(at + 0x536, uint16_t(size));
    }

    if (const uint32_t herd = game.Call<0x8028F118>({id, group & 0xFF}); herd && int16_t(FlatRead16(herd + 4)) > 0)
        FlatWrite16(herd + 4, uint16_t(int16_t(FlatRead16(herd + 4)) - 1));

    if (uint32_t(objective) > 2) {
        // An extra monster: its kills (+0x6980 by slot 3..5).
        if (!large || game.Call<0x8027BCAC>({0}) != 0)
            return;

        for (uint32_t slot = 3; slot < 6; ++slot)
            if (kind == FlatRead16(field + slot * 16 + 0x227A)) {
                FlatWrite8(work + slot + 0x697D, uint8_t(FlatRead8(work + slot + 0x697D) + 1));
                return;
            }

        return;
    }

    if (game.Call<MarathonFlag>({0}) == 1) {
        FlatWrite8(work + QuestWork::kWaiting, 0);
        objective = int8_t(2 - objective);
    }

    if (game.Call<TimedFlag>({0}) == 1)
        FlatWrite8(work + QuestWork::kWaiting, 0);

    if (game.Call<0x803B6558>({kind & 0xFFFF, captured & 0xFF, outs}) != 1 || game.Call<0x803B4BD8>({0}) == 1)
        return;

    bool quiet = false;

    if (game.Call<0x803B4C50>({0}) != 0) {
        int32_t left = 0;

        for (uint32_t k = 0; k < 3; ++k) {
            game.Call<0x803B6954>({k});
            const int32_t n = int32_t(game.Call<0x803B6864>({k}));

            if (n >= 0)
                left += n;
        }

        if (left != 0) {
            game.Call<0x800F45C4>({0});
            return;
        }
    } else if (game.Call<MarathonFlag>({0}) == 1) {
        quiet = uint8_t(objective - 1) <= 1;
    } else if (game.Call<0x803B4CC8>({0}) == 1 || game.Call<TimedFlag>({0}) == 1) {
        const uint32_t first = FlatRead32(work + 148) & 9;

        if (first == 9 && (FlatRead32(work + 244) & 9) == 9) {
            if (int8_t(objective) == 0)
                quiet = game.Call<ObjectiveDone>({work, 1, 0}) == 0;
            else if (int8_t(objective) == 1)
                quiet = game.Call<ObjectiveDone>({work, 0, 0}) == 0;
        } else if (first == 9 && FlatRead16(work + 154) != 0) {
            quiet = true;
        }
    } else if (game.Call<0x803B5208>({0}) == 1) {
        for (uint32_t k = 0; k < 3; ++k)
            if (Started(work, k) && game.Call<ObjectiveDone>({work, k, 0}) == 0) {
                quiet = true;
                break;
            }
    } else if (int8_t(objective) != 0 || int8_t(FlatRead8(outs)) != 0) {
        quiet = true;
    }

    // The fallen monster's camera moment, unless more are to come.
    if (!enemy || quiet)
        return;

    if (FlatRead8(0x8065B4D6) != 0)
        game.Call<0x802BEEA0>({1, enemy});
    else if (game.Call<0x8012D0D8>({enemy}) == 1)
        game.Call<0x802BC5C8>({1, enemy});
}

void MemberMonsterDown(GuestCall& game, uint32_t group, uint32_t id, uint32_t count, uint32_t how, uint32_t objective, uint32_t outs) {
    const uint32_t field = QuestWork::MoveWork(QuestWork::kField);

    if (!field || FlatRead8(field + 275) == 1)
        return;

    const uint32_t work = game.Call<0x803A880C>();

    if (!work)
        return;

    uint32_t taken = 0, kind = 0;

    if (const uint32_t herd = game.Call<0x8028F118>({id & 0xFFFF, group & 0xFF})) {
        kind = FlatRead16(herd + 6) & 0xFF;
        const int32_t have = int16_t(FlatRead16(herd + 4));

        if (have >= int32_t(count & 0xFFFF)) {
            taken = uint32_t(have - int32_t(count & 0xFFFF) + 1);
            FlatWrite16(herd + 4, uint16_t(have - int16_t(taken)));
        }
    }

    if (!kind || !taken)
        return;

    const uint32_t save = FlatRead32(0x80796D40); // r13 - 0x45A0: the save data

    if ((how & 0xFF) == 0) {
        FlatWrite16(work + kind * 2 + 0x490, uint16_t(FlatRead16(work + kind * 2 + 0x490) + taken));
        game.Call<0x803B1E1C>({1, kind & 0xFFFF, taken & 0xFFFF});

        // 0x8004E5CC, inlined: the save's hunted count (at most 9999).
        FlatWrite16(save + kind * 2 + 0x39C0, uint16_t(FlatRead16(save + kind * 2 + 0x39C0) + (taken & 0xFFFF)));

        if (FlatRead16(FlatRead32(0x80796D40) + kind * 2 + 0x39C0) > 9999)
            FlatWrite16(FlatRead32(0x80796D40) + kind * 2 + 0x39C0, 9999);
    }

    if ((how & 0xFF) == 1) {
        FlatWrite16(work + kind * 2 + 0x4E2, uint16_t(FlatRead16(work + kind * 2 + 0x4E2) + taken));

        // 0x8004E608, inlined: the save's captured count.
        FlatWrite16(FlatRead32(0x80796D40) + kind * 2 + 0x3AC0, uint16_t(FlatRead16(FlatRead32(0x80796D40) + kind * 2 + 0x3AC0) + (taken & 0xFFFF)));

        if (FlatRead16(FlatRead32(0x80796D40) + kind * 2 + 0x3AC0) > 9999)
            FlatWrite16(FlatRead32(0x80796D40) + kind * 2 + 0x3AC0, 9999);
    }

    if ((objective & 0xFF) >= 3)
        return;

    if (game.Call<MarathonFlag>({0}) == 1) {
        FlatWrite8(work + QuestWork::kWaiting, 0);
        objective = (2 - objective) & 0xFF;
    }

    if (game.Call<TimedFlag>({0}) == 1)
        FlatWrite8(work + QuestWork::kWaiting, 0);

    if (game.Call<0x803B6558>({kind & 0xFFFF, how & 0xFF, outs}) != 1 || game.Call<0x803B4BD8>({0}) == 1 || game.Call<0x803B4C50>({0}) != 0 ||
        game.Call<MarathonFlag>({0}) == 1)
        return;

    if (game.Call<0x803B4CC8>({0}) == 1 || game.Call<TimedFlag>({0}) == 1) {
        if ((objective & 0xFF) == 0)
            game.Call<ObjectiveDone>({work, 1, 0});
        else if ((objective & 0xFF) == 1)
            game.Call<ObjectiveDone>({work, 0, 0});

        return;
    }

    if (game.Call<0x803B5208>({0}) == 1)
        for (uint32_t k = 0; k < 3; ++k)
            if (Started(work, k) && game.Call<ObjectiveDone>({work, k, 0}) == 0)
                return;
}

// 0x803A9F8C (quest work, objective record, k): objective k met: the first
// free of the three met slots (+0x309, 0xFF free) takes it and its confirmed
// word (+0x2D8 + 4k) gets 0x20; then, but for the main objective and the
// special modes, the sub-objective messages and the HUD mark.
void ObjectiveMet(GuestCall& game, uint32_t work, uint32_t record, uint32_t k) {
    for (uint32_t i = 0; i < 3; ++i) {
        if (FlatRead8(work + i + 777) != 0xFF)
            continue;

        FlatWrite8(work + i + 777, uint8_t(k));
        const uint32_t n = k & 0xFFFF;
        FlatWrite32(work + n * 4 + 728, FlatRead32(work + n * 4 + 728) | 32);

        if (n == 0 || (FlatRead32(record) & 16) || game.Call<MarathonFlag>({0}) != 0)
            return;

        const uint32_t mode = game.Call<0x803B5528>({0}) & 0xFF;

        if (mode == 7 || mode == 9 || game.Call<0x803B4C50>({0}) != 0 || game.Call<0x803B4CC8>({0}) != 0)
            return;

        game.Call<ShowMessage>({0, game.Call<Message>({n + 23})});
        game.Call<0x800F45C4>({0});
        game.Call<ShowMessage>({1, game.Call<Message>({n + 33})});
        const uint32_t item = game.Call<0x803B42E0>({0}) & 0xFFFF;
        game.Call<0x803AA8FC>({work + 1506, item, uint32_t(int32_t(int8_t(k - 1)))});
        return;
    }
}

// 0x803B6558 (monster, how, out): a quest monster of that kind fell (how 1:
// captured): each live objective that targets it (or its kind, 0x80126000)
// and is started counts down; a hunt-only objective (0x80) failed by a
// capture, a capture-only one (0x100) by a kill. 1 when one was met (out
// the first objective met, else -1); the countdown messages.
uint32_t ObjectiveCount(GuestCall& game, uint32_t monster, uint32_t how, uint32_t out, uint32_t buffer) {
    FlatWrite8(out, 0xFF);

    if (game.Call<0x800CF2E4>() == 1)
        return 0;

    uint32_t alone = game.Call<0x803B4C50>({0});

    if (alone == 0)
        alone = game.Call<0x803B4CC8>({0});

    uint32_t met = 0;
    bool told = false;
    uint32_t record = QuestWork::kWork + QuestWork::kObjectives;

    for (uint32_t k = 0; k < 3; ++k, record += 96) {
        const uint32_t flags = FlatRead32(record);

        if ((flags & 0x60) || (flags & 9) != 9)
            continue;

        if (const uint32_t target = FlatRead16(record + 4); target != (monster & 0xFFFF) && game.Call<0x80126000>({target & 0xFF, monster & 0xFF}) == 0)
            continue;

        const uint32_t now = FlatRead32(record);

        if ((now & 0x80) && (how & 0xFF) != 1) {
            FlatWrite32(record, now | 64);
            FlatWrite8(out, uint8_t(k));
            return 0;
        }

        if ((now & 0x100) && (how & 0xFF) == 1) {
            FlatWrite32(record, FlatRead32(record) | 64);
            FlatWrite8(out, uint8_t(k));
            return 0;
        }

        if (FlatRead16(record + 6) != 0)
            FlatWrite16(record + 6, uint16_t(FlatRead16(record + 6) - 1));

        if (FlatRead16(record + 6) == 0) {
            game.Call<0x803A9E50>({QuestWork::kWork, k & 0xFFFF, 0x100});

            if (int8_t(FlatRead8(out)) < 0)
                FlatWrite8(out, uint8_t(k));

            met = 1;
        }

        if (!told) {
            // Every five left: "N to go" (0x8000 hunts, 0x10000 captures).
            for (const auto [bit, message] : {std::pair{0x8000u, 37u}, std::pair{0x10000u, 38u}}) {
                const uint32_t left = FlatRead16(record + 6);

                if ((FlatRead32(record) & bit) && left != 0 && int32_t(left) % 5 == 0) {
                    Format(game, buffer, game.Call<Message>({message}), left);
                    game.Call<ShowMessage>({0, buffer});
                    told = true;
                }
            }

            if (FlatRead32(record) & 0x20000) {
                const uint32_t slots = game.Call<ExtraFlag>({0}) == 1 ? 2 : 3;
                int32_t left = 0;

                for (uint32_t i = 0; i < slots; ++i)
                    if (const int32_t n = int32_t(game.Call<0x803B6864>({i})); n >= 0)
                        left += n;

                if (left != 0) {
                    Format(game, buffer, game.Call<Message>({37}), uint32_t(left));
                    game.Call<ShowMessage>({0, buffer});
                    told = true;
                }
            }
        }

        if (alone == 1)
            break;
    }

    return met;
}

// 0x803B3C18 (k): the HUD's "hunted N of M" line for objective k into
// 0x806CE790 (with 0x00800000 for an objective, "-"); every objective
// together in the special modes.
uint32_t TargetText(GuestCall& game, uint32_t k) {
    constexpr uint32_t kText = 0x806CE790;
    FlatWrite8(kText, 0);

    const auto none = [&] {
        game.cpu->cr &= ~0x02000000u;
        game.Call<Sprintf>({kText, game.Call<Message>({27})});
    };

    if ((k & 0xFF) != 0 && game.Call<0x803B5184>({0}) == 1) {
        none();
        return kText;
    }

    int32_t total, left;

    if (game.Call<0x803B4C50>({0}) == 1 || game.Call<0x803B4CC8>({0}) == 1 || game.Call<TimedFlag>({0}) == 1 || game.Call<MarathonFlag>({0}) == 1) {
        const uint32_t slots = game.Call<ExtraFlag>({0}) == 1 ? 2 : 3;
        int32_t all = 0, done = 0;

        for (uint32_t i = 0; i < slots; ++i) {
            if (const int32_t n = int32_t(game.Call<0x803B6954>({i})); n >= 0)
                all += n;

            if (const int32_t n = int32_t(game.Call<0x803B6864>({i})); n >= 0)
                done += n;
        }

        total = all, left = all - done;
    } else {
        total = int32_t(game.Call<0x803B6954>({k & 0xFF}));
        left = total - int32_t(game.Call<0x803B6864>({k & 0xFF}));
    }

    if (left < 0 || total <= 0) {
        none();
    } else {
        game.cpu->cr &= ~0x02000000u;
        game.Call<Sprintf>({kText, game.Call<Message>({26}), uint32_t(left), uint32_t(total)});
    }

    return kText;
}

// 0x80337BD0 (session, what, slot): online, a summon (2) or a spawn (1)
// sent to the members.
void NetSpawn(GuestCall& game, uint32_t session, uint32_t what, uint32_t slot) {
    if (!Online() || game.Call<0x80334AB0>() == 0)
        return;

    const uint32_t me = FlatRead8(kMyPlayer);

    if ((what & 0xFF) == 1)
        game.Call<0x80337624>({session, me, 9, 1});
    else if ((what & 0xFF) == 2)
        game.Call<0x80337A0C>({session, me, 9, 2, slot & 0xFFFF});
}

// 0x803AA0C4 QuestObjectiveDone(work, k, confirmed): 1 when objective k (4:
// all three) is set, not failed (0x40) and met (0x20 of its word, or with
// `confirmed` of its confirmed word +0x2D8 + 4k).
uint32_t ObjectiveDoneOf(uint32_t work, int32_t k, uint32_t confirmed) {
    if (k == 4) {
        uint32_t count = 0;

        for (uint32_t i = 0; i < 3; ++i) {
            const uint32_t flags = FlatRead32(work + 0x94 + i * 0x60);
            const uint32_t met = confirmed & 0xFF ? FlatRead32(work + 0x2D8 + i * 4) : flags;

            if (flags == 0 || ((flags & 8) && (met & 0x20)))
                ++count;
        }

        return count > 2;
    }

    uint32_t flags = FlatRead32(work + uint32_t(k) * 0x60 + 0x94);

    if (!(flags & 8) || (flags & 0x40) == 0x40)
        return 0;

    if (confirmed & 0xFF)
        flags = FlatRead32(work + uint32_t(k) * 4 + 0x2D8);

    return (flags & 0x20) != 0;
}

// Ports.

void QuestStepPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x140);
    GuestCall game{ctx};
    QuestStep(game, frame[0]);
}

void QuestTimedSpawnPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x30);
    GuestCall game{ctx};
    QuestTimedSpawn(game);
}

void QuestExtraSpawnPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x90);
    GuestCall game{ctx};
    QuestExtraSpawn(game, frame[0]);
}

void QuestMarathonSpawnPort(CpuContext* ctx) {
    const uint32_t field = ctx->gpr[3];
    GuestFrame frame(ctx, 0x70);
    GuestCall game{ctx};
    QuestMarathonSpawn(game, frame[0], field);
}

void QuestMonsterDownPort(CpuContext* ctx) {
    const uint32_t field = ctx->gpr[3], monster = ctx->gpr[4], how = ctx->gpr[5];
    GuestFrame frame(ctx, 0x50);
    GuestCall game{ctx};
    QuestMonsterDown(game, field, monster, how, frame[8]);
}

void MemberMonsterDownPort(CpuContext* ctx) {
    const uint32_t group = ctx->gpr[3], id = ctx->gpr[4], count = ctx->gpr[5], how = ctx->gpr[6], objective = ctx->gpr[7];
    GuestFrame frame(ctx, 0x30);
    GuestCall game{ctx};
    MemberMonsterDown(game, group, id, count, how, objective, frame[8]);
}

void ObjectiveMetPort(CpuContext* ctx) {
    const uint32_t work = ctx->gpr[3], record = ctx->gpr[4], k = ctx->gpr[5];
    GuestFrame frame(ctx, 0x10);
    GuestCall game{ctx};
    ObjectiveMet(game, work, record, k);
}

void ObjectiveCountPort(CpuContext* ctx) {
    const uint32_t monster = ctx->gpr[3], how = ctx->gpr[4], out = ctx->gpr[5];
    uint32_t met;

    {
        GuestFrame frame(ctx, 0x60);
        GuestCall game{ctx};
        met = ObjectiveCount(game, monster, how, out, frame[8]);
    }

    ctx->gpr[3] = met;
}

void TargetTextPort(CpuContext* ctx) {
    const uint32_t k = ctx->gpr[3];
    uint32_t text;

    {
        GuestFrame frame(ctx, 0x20);
        GuestCall game{ctx};
        text = TargetText(game, k);
    }

    ctx->gpr[3] = text;
}

void SpawnStateDonePort(CpuContext* ctx) {
    const uint32_t field = QuestWork::MoveWork(QuestWork::kField);

    if (field && FlatRead32(field + 0xDC))
        FlatWrite8(FlatRead32(field + 0xDC) + QuestWork::kSpawnState, 3);
}

void NetSummonedPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x10);
    GuestCall game{ctx};
    const uint32_t message = frame[8];
    FlatWrite8(message, 0);
    FlatWrite8(message + 1, FlatRead8(kMyPlayer));
    FlatWrite8(message + 2, 13);
    FlatWrite8(message + 3, 24);
    game.Call<0x804514C8>({message, 4, 13});
}

void NetSpawnPort(CpuContext* ctx) {
    const uint32_t session = ctx->gpr[3], what = ctx->gpr[4], slot = ctx->gpr[5];
    GuestFrame frame(ctx, 0x20);
    GuestCall game{ctx};
    NetSpawn(game, session, what, slot);
}

} // namespace

namespace QuestStepPorts {
// QuestObjectiveDone for the cheats' wrapper (cheat_quest.cpp).
void ObjectiveDonePort(CpuContext* ctx) {
    ctx->gpr[3] = ObjectiveDoneOf(ctx->gpr[3], int32_t(ctx->gpr[4]), ctx->gpr[5]);
}

void QuestStepPort(CpuContext* ctx) {
    ::QuestStepPort(ctx);
}
} // namespace QuestStepPorts

extern "C" void func_803B0E28(CpuContext* ctx);
extern "C" void MH3_QuestTimedSpawn(CpuContext* ctx) {
    PortCheck::Call(ctx, "QuestTimedSpawn", func_803B0E28, QuestTimedSpawnPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x803B0E28, MH3_QuestTimedSpawn, "QuestTimedSpawn");

extern "C" void func_803B0FFC(CpuContext* ctx);
extern "C" void MH3_QuestExtraSpawn(CpuContext* ctx) {
    PortCheck::Call(ctx, "QuestExtraSpawn", func_803B0FFC, QuestExtraSpawnPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x803B0FFC, MH3_QuestExtraSpawn, "QuestExtraSpawn");

extern "C" void func_803B17E0(CpuContext* ctx);
extern "C" void MH3_QuestMarathonSpawn(CpuContext* ctx) {
    PortCheck::Call(ctx, "QuestMarathonSpawn", func_803B17E0, QuestMarathonSpawnPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x803B17E0, MH3_QuestMarathonSpawn, "QuestMarathonSpawn");

extern "C" void func_803A9728(CpuContext* ctx);
extern "C" void MH3_QuestMonsterDown(CpuContext* ctx) {
    PortCheck::Call(ctx, "QuestMonsterDown", func_803A9728, QuestMonsterDownPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x803A9728, MH3_QuestMonsterDown, "QuestMonsterDown");

extern "C" void func_803A9BE0(CpuContext* ctx);
extern "C" void MH3_MemberMonsterDown(CpuContext* ctx) {
    PortCheck::Call(ctx, "MemberMonsterDown", func_803A9BE0, MemberMonsterDownPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x803A9BE0, MH3_MemberMonsterDown, "MemberMonsterDown");

extern "C" void func_803A9F8C(CpuContext* ctx);
extern "C" void MH3_ObjectiveMet(CpuContext* ctx) {
    PortCheck::Call(ctx, "ObjectiveMet", func_803A9F8C, ObjectiveMetPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x803A9F8C, MH3_ObjectiveMet, "ObjectiveMet");

extern "C" void func_803B6558(CpuContext* ctx);
extern "C" void MH3_ObjectiveCount(CpuContext* ctx) {
    PortCheck::Call(ctx, "ObjectiveCount", PortCheck::Returns::Gpr3, func_803B6558, ObjectiveCountPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x803B6558, MH3_ObjectiveCount, "ObjectiveCount");

extern "C" void func_803B3C18(CpuContext* ctx);
extern "C" void MH3_TargetText(CpuContext* ctx) {
    PortCheck::Call(ctx, "TargetText", PortCheck::Returns::Gpr3, func_803B3C18, TargetTextPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x803B3C18, MH3_TargetText, "TargetText");

extern "C" void func_80339AEC(CpuContext* ctx);
extern "C" void MH3_SpawnStateDone(CpuContext* ctx) {
    PortCheck::Call(ctx, "SpawnStateDone", func_80339AEC, SpawnStateDonePort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80339AEC, MH3_SpawnStateDone, "SpawnStateDone");

extern "C" void func_80339AAC(CpuContext* ctx);
extern "C" void MH3_NetSummoned(CpuContext* ctx) {
    PortCheck::Call(ctx, "NetSummoned", func_80339AAC, NetSummonedPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80339AAC, MH3_NetSummoned, "NetSummoned");

extern "C" void func_80337BD0(CpuContext* ctx);
extern "C" void MH3_NetSpawn(CpuContext* ctx) {
    PortCheck::Call(ctx, "NetSpawn", func_80337BD0, NetSpawnPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80337BD0, MH3_NetSpawn, "NetSpawn");
