// Spawning the quest's monsters, and the summon pool, RMHP08. C
// reconstructions from the translated code.
//
// A spawn record (48 bytes in a monster's spawn lists, 0x805F7E60; quests copy
// its 32 bytes from +8 into a 40-byte placement): u8 monster, u8 record, u8
// mode, u8 count, u8 +4, u8 +5, u8 size, u8 level, +8 a position, +20 an angle
// (3 words). The mode decides where a monster goes:
//
//   - otherwise a live enemy work (move work 3, 0xB18 bytes each), at once
//     (CreateQuestEnemy);
//   - 3: an entry of the summon pool (0x806A7960, 128 entries of 0x44 bytes)
//     with its state +8 1, held until a summon (SummonRelease) takes it; a
//     monster whose action table's first word has bit 0 (a large one) also
//     keeps an enemy work reserved for it (+0x30);
//   - 4 (summon pool too, through SpawnHeld): the entry's state 5.
//
// A summon pool entry: u8 used +0, u8 kind +2, u8 sub +3, u8 state +8 (1 held,
// 2 summoned), u8 the record's +0 at +9, u8 area +10, u8 mode +12, u8
// objective +13, u8 the record's +4/+5/+6 at +15/+19/+21, u16 spawn id +22,
// u16 group +24 (0xFFFF), u8 area to appear in +31, f32 angle +36, u32 the
// reserved enemy work +48.
//
//   0x80125FB8 (p): p (an empty constructor at p + 8)
//   0x801423C0 TakeEnemyWork(): the first enemy work with +0 and +8 clear, its
//              index at +2; 0 for none
//   0x80142444 CopySpawnRecord(to, from): the 32-byte placement record
//   0x801425F4 SetEnemyRecord(work, record): the record into the enemy work
//   0x801427F0 SpawnArea(id, choice): the area the spawn id's monster starts
//              in (0x8014269C over 0x803A93A0 / 0x803A9480)
//   0x801429BC SpawnAngle(id, choice): its angle (0x80142880), 1.0 when
//              0x803A955C has none
//   0x80142BD8 NewEnemy(kind): TakeEnemyWork set up by 0x80142A34
//   0x80142C24 CreateQuestEnemy(kind, record, id): a live quest monster, its
//              start area (+0x7C8) and angle (+0x1D0) of a random choice of 2
//   0x80142D4C SpawnLive(kind, record, id): a monster that comes during the
//              quest (the timed, marathon and extra ones), through 0x803392DC
//   0x801434A0 TakeSummonEntry(): the first free summon pool entry
//   0x80143578 InitSummonEntry(entry, kind, area)
//   0x8014365C NewSummonEntry(kind, area)
//   0x801436B8 SpawnHeld(kind, record, id): a monster of mode 3 or 4 into the
//              summon pool; its area and angle
//   0x80143A18 HoldSummon(kind, record, id, work): a held entry (state 1) with
//              its reserved enemy work
//   0x80141BEC SummonRelease(id, kind, sub, area, p5, max, count, flags, p9,
//              position, angle): up to `count` less the summoned ones (at most
//              `max`) of the 10 summon requests (0x806A6C08, 0x24 bytes)
//              filled, each taking a held entry of the kind (flags 8: and sub;
//              flags 1: in area `area` exactly, else one 0x80126B34 reaches,
//              any for 255) to state 2; 1 when none were wanted. The Qurupeco's
//              call and the timed monster (0x803B0E28) use it
//   0x80141E20 SummonedCount(id, area, flags, large): the enemies and summoned
//              entries of mode 3 in group `id`
//   0x80126B34 SummonReaches(entry, map, area): 1 when the entry's area is
//              `area` or its route table (0x801269D8 / 0x80126A4C) leads there
//   0x8012E76C (work): 0x8012E704(work +0xC, u16 work +0x1A)
#include "abi_bridge.h"
#include "guest_call.h"
#include "port_check.h"
#include "quest_work.h"

#include <cstdint>

namespace {
using MemoryInline::FlatRead16;
using MemoryInline::FlatRead32;
using MemoryInline::FlatRead8;
using MemoryInline::FlatWrite16;
using MemoryInline::FlatWrite32;
using MemoryInline::FlatWrite8;

constexpr uint32_t kSummons = 0x806A7960, kSummonBytes = 0x44, kSummonCount = 128;
constexpr uint32_t kRequests = 0x806A6C08, kRequestBytes = 0x24, kRequestCount = 10;
constexpr uint32_t kEnemyBytes = 0xB18;
constexpr uint32_t kMyPlayer = 0x8065AA87; // system_w +0x27
constexpr uint32_t kNowMap = 0x806BB805;   // get_now_mapno, inlined
constexpr uint32_t kOne = 0x80799298;      // f32 1.0 (r2 - 0x6CC8)

enum : uint32_t {
    MoveWorkMax = 0x800CFB34, // get_move_work_max(kind)
    Rand = 0x800CEF20,
    EnemyActionTable = 0x80140C64,
    InitEnemy = 0x80142A34,
    Vec3Copy = 0x80041EA4,
    ToMhVec3 = 0x8005001C,
    AreaOf = 0x803A93A0,
    AreaKind = 0x803A9480,
    StartArea = 0x8014269C,
    AngleOf = 0x803A955C,
    AngleKind = 0x803A9628,
    StartAngle = 0x80142880,
    SpawnRemote = 0x803392DC,
    SummonId = 0x803A7E80,
    Memset = 0x80004350,
    FieldQuest6A2A = 0x803AE998,
    RouteTable = 0x801269D8,
    RouteReaches = 0x80126A4C,
    NetSummon = 0x80337BD0,
    EnemyKindOf = 0x8012E704,
};

// Summon pool entry fields.
enum : uint32_t {
    kUsed = 0,
    kKind = 2,
    kSub = 3,
    kState = 8,
    kArea = 10,
    kMode = 12,
    kSpawnId = 22,
    kStartArea = 31,
    kAngle = 36,
    kReserved = 48,
};

uint32_t TakeEnemyWork(GuestCall& game, uint32_t& index) {
    uint32_t work = QuestWork::MoveWork(QuestWork::kEnemies);
    const uint32_t count = game.Call<MoveWorkMax>({3}) & 0xFFFF;

    for (index = 0; index < count; ++index, work += kEnemyBytes)
        if (FlatRead8(work) == 0 && FlatRead8(work + 8) == 0) {
            FlatWrite8(work + 2, uint8_t(index));
            return work;
        }

    return 0;
}

void CopySpawnRecord(GuestCall& game, uint32_t to, uint32_t from) {
    for (uint32_t i = 0; i < 4; ++i)
        FlatWrite8(to + i, FlatRead8(from + i));

    game.Call<ToMhVec3>({to + 8, from + 8});

    for (uint32_t i = 0; i < 12; i += 4)
        FlatWrite32(to + 20 + i, FlatRead32(from + 20 + i));

    for (uint32_t i = 4; i < 8; ++i)
        FlatWrite8(to + i, FlatRead8(from + i));
}

void SetEnemyRecord(GuestCall& game, uint32_t work, uint32_t record) {
    FlatWrite8(work + 0x1E1, FlatRead8(record + 1));
    FlatWrite8(work + 0x46C, FlatRead8(record));
    FlatWrite8(work + 0xC, FlatRead8(record + 2));
    FlatWrite8(work + 0x10, FlatRead8(record + 4));
    FlatWrite8(work + 0x15, FlatRead8(record + 5));
    FlatWrite8(work + 0xA, FlatRead8(record + 3));
    game.Call<Vec3Copy>({work + 0x188, record + 8});

    for (uint32_t i = 0; i < 12; i += 4)
        FlatWrite32(work + 0x1BC + i, FlatRead32(record + 20 + i));
}

uint32_t SpawnArea(GuestCall& game, uint32_t id, uint32_t choice) {
    const uint32_t area = game.Call<AreaOf>({id & 0xFFFF}) & 0xFF;
    const uint32_t kind = game.Call<AreaKind>({id & 0xFFFF}) & 0xFF;
    return game.Call<StartArea>({area, kind, choice & 0xFFFF});
}

double SpawnAngle(GuestCall& game, uint32_t id, uint32_t choice) {
    const uint32_t angle = game.Call<AngleOf>({id & 0xFFFF}) & 0xFF;

    if (angle == 0)
        return Memory::ReadFloat32(kOne);

    const uint32_t kind = game.Call<AngleKind>({id & 0xFFFF}) & 0xFF;
    game.Call<StartAngle>({angle, kind, choice & 0xFFFF});
    return game.cpu->fpr[1].d;
}

uint32_t NewEnemy(GuestCall& game, uint32_t kind) {
    uint32_t index;
    const uint32_t work = TakeEnemyWork(game, index);

    if (work)
        game.Call<InitEnemy>({work, kind & 0xFF});

    return work;
}

uint32_t CreateQuestEnemy(GuestCall& game, uint32_t kind, uint32_t record, uint32_t id) {
    const uint32_t work = NewEnemy(game, kind & 0xFF);

    if (!work)
        return 0;

    SetEnemyRecord(game, work, record);
    FlatWrite16(work + 0x1A, uint16_t(id));
    uint32_t choice = game.Call<Rand>({2});
    FlatWrite8(work + 0x7C8, uint8_t(SpawnArea(game, FlatRead16(work + 0x1A), choice & 0xFFFF)));
    choice = game.Call<Rand>({2});
    Memory::WriteFloat32(work + 0x1D0, float(SpawnAngle(game, FlatRead16(work + 0x1A), choice & 0xFFFF)));
    return work;
}

void SpawnLive(GuestCall& game, uint32_t kind, uint32_t record, uint32_t id) {
    uint32_t choice = game.Call<Rand>({1});
    const uint32_t area = SpawnArea(game, id & 0xFFFF, choice & 0xFFFF);
    choice = game.Call<Rand>({1});
    const double angle = SpawnAngle(game, id & 0xFFFF, choice & 0xFFFF);
    game.Call<SpawnRemote>({FlatRead8(kMyPlayer), kind & 0xFF, record, id & 0xFFFF, area & 0xFF}, {angle});
}

// The free entry and the r4 the original leaves (7 per group of 8 passed).
uint32_t TakeSummonEntry(uint32_t& r4) {
    uint32_t entry = kSummons;
    r4 = 0;

    for (uint32_t group = 0; group < kSummonCount / 8; ++group, r4 += 7)
        for (uint32_t i = 0; i < 8; ++i, entry += kSummonBytes)
            if (FlatRead8(entry + kUsed) == 0)
                return entry;

    return 0;
}

void InitSummonEntry(GuestCall& game, uint32_t entry, uint32_t kind, uint32_t area) {
    // 0x801433E8, inlined: the head cleared.
    FlatWrite8(entry, 0);

    for (uint32_t i = 2; i <= 8; ++i)
        FlatWrite8(entry + i, 0);

    FlatWrite32(entry + kReserved, 0);
    FlatWrite8(entry + kUsed, 1);
    FlatWrite8(entry + kState, 0);
    FlatWrite8(entry + kMode, 0xFE);
    FlatWrite8(entry + kArea, uint8_t(area));
    FlatWrite8(entry + 11, 0xFF);
    FlatWrite16(entry + kSpawnId, 0xFFFF);
    FlatWrite8(entry + 28, 0);
    FlatWrite8(entry + 17, 0);
    FlatWrite8(entry + 18, 0);
    FlatWrite16(entry + 24, 0xFFFF);
    FlatWrite8(entry + 13, 0xFF);
    FlatWrite8(entry + 14, 0);
    FlatWrite8(entry + kKind, uint8_t(kind));
    FlatWrite8(entry + kSub, 0);
    FlatWrite8(entry + 16, 0);
    FlatWrite8(entry + 15, 0);
    FlatWrite8(entry + 19, 0);
    FlatWrite8(entry + 21, 0);
    FlatWrite8(entry + 29, uint8_t(game.Call<SummonId>()));
    FlatWrite8(entry + 30, 0);
    FlatWrite8(entry + kStartArea, 0);
    FlatWrite32(entry + kAngle, FlatRead32(kOne));
    FlatWrite32(entry + 40, 0xFFFFFFFF);
    FlatWrite32(entry + 44, 0xFFFFFFFF);
    game.Call<Memset>({entry + 52, 0, 16});
}

uint32_t NewSummonEntry(GuestCall& game, uint32_t kind, uint32_t area, uint32_t& r4) {
    const uint32_t entry = TakeSummonEntry(r4);

    if (entry)
        InitSummonEntry(game, entry, kind & 0xFF, area & 0xFF);

    return entry;
}

// 0x80143544, inlined: the record's bytes into the entry.
void SummonFromRecord(uint32_t entry, uint32_t record) {
    FlatWrite8(entry + 9, FlatRead8(record));
    FlatWrite8(entry + kMode, FlatRead8(record + 2));
    FlatWrite8(entry + kSub, FlatRead8(record + 3));
    FlatWrite8(entry + 15, FlatRead8(record + 4));
    FlatWrite8(entry + 19, FlatRead8(record + 5));
    FlatWrite8(entry + 21, FlatRead8(record + 6));
}

uint32_t HoldSummon(GuestCall& game, uint32_t kind, uint32_t record, uint32_t id, uint32_t work) {
    uint32_t r4;
    const uint32_t entry = NewSummonEntry(game, kind & 0xFF, FlatRead8(record + 1), r4);

    if (entry) {
        FlatWrite8(entry + kState, 1);

        if (work)
            FlatWrite8(work + 8, 1);

        SummonFromRecord(entry, record);
        FlatWrite16(entry + kSpawnId, uint16_t(id));
        FlatWrite32(entry + kReserved, work);
    }

    return entry;
}

uint32_t SpawnHeld(GuestCall& game, uint32_t kind, uint32_t record, uint32_t id) {
    uint32_t work = 0, entry;

    if (FlatRead8(record + 2) == 3) {
        const uint32_t table = game.Call<EnemyActionTable>({kind & 0xFF, FlatRead8(record + 3)});

        if (FlatRead32(table) & 1) {
            uint32_t index;
            work = TakeEnemyWork(game, index);
        }

        entry = HoldSummon(game, kind & 0xFF, record, id & 0xFFFF, work);
    } else {
        uint32_t r4;
        entry = NewSummonEntry(game, kind & 0xFF, FlatRead8(record + 1), r4);

        if (entry) {
            SummonFromRecord(entry, record);
            FlatWrite16(entry + kSpawnId, uint16_t(id));
        }

        // The original stores without a test (at 8 without an entry).
        if (FlatRead8(record + 2) == 4)
            Memory::Write8(entry + kState, 5);
    }

    if (!entry)
        return 0;

    if (!work) {
        FlatWrite8(entry + kStartArea, uint8_t(SpawnArea(game, FlatRead16(entry + kSpawnId), 0)));
        Memory::WriteFloat32(entry + kAngle, float(SpawnAngle(game, FlatRead16(entry + kSpawnId), 0)));

        if (game.Call<FieldQuest6A2A>() == 1)
            FlatWrite8(entry + 14, 4);
    } else {
        uint32_t choice = game.Call<Rand>({2});
        FlatWrite8(entry + kStartArea, uint8_t(SpawnArea(game, FlatRead16(entry + kSpawnId), choice & 0xFFFF)));
        choice = game.Call<Rand>({2});
        Memory::WriteFloat32(entry + kAngle, float(SpawnAngle(game, FlatRead16(entry + kSpawnId), choice & 0xFFFF)));
    }

    return entry;
}

uint32_t SummonReaches(GuestCall& game, uint32_t entry, uint32_t map, uint32_t area) {
    const uint32_t table = game.Call<EnemyActionTable>({FlatRead8(entry + kKind), FlatRead8(entry + kSub)});

    if (FlatRead8(entry + kArea) == (area & 0xFF))
        return 1;

    const uint32_t route = game.Call<RouteTable>({FlatRead32(table + 56), map & 0xFF});
    return route ? game.Call<RouteReaches>({route, FlatRead8(entry + kArea), area & 0xFF}) : 0;
}

uint32_t SummonedCount(GuestCall& game, uint32_t id, uint32_t area, uint32_t flags, uint32_t large) {
    const uint32_t count = game.Call<MoveWorkMax>({3}) & 0xFFFF;
    uint32_t work = QuestWork::MoveWork(QuestWork::kEnemies), found = 0;
    id &= 0xFFFF, area &= 0xFF, flags &= 0xFF, large &= 0xFF;

    for (uint32_t i = 0; i < count; ++i, work += kEnemyBytes) {
        const bool big = FlatRead32(work + 0x1C8) & 1;

        if (FlatRead8(work) != 0 && (!(flags & 1) || area == FlatRead8(work + 0x1E1)) && (large != 0 || !big) && (large != 1 || big) && FlatRead8(work + 0xC) == 3 &&
            id == FlatRead16(work + 0x1C))
            ++found;
    }

    uint32_t entry = kSummons;

    for (uint32_t i = 0; i < kSummonCount; ++i, entry += kSummonBytes) {
        if (FlatRead8(entry + kUsed) == 0 || FlatRead8(entry + kState) == 1 || ((flags & 1) && area != FlatRead8(entry + kArea)))
            continue;

        if (large == 0 && (FlatRead32(game.Call<EnemyActionTable>({FlatRead8(entry + kKind), FlatRead8(entry + kSub)})) & 1))
            continue;

        if (large == 1 && !(FlatRead32(game.Call<EnemyActionTable>({FlatRead8(entry + kKind), FlatRead8(entry + kSub)})) & 1))
            continue;

        if (FlatRead8(entry + kMode) == 3 && id == FlatRead16(entry + 24))
            ++found;
    }

    return found;
}

uint32_t SummonRelease(GuestCall& game, const uint32_t (&a)[11]) {
    const uint32_t id = a[0], kind = a[1], sub = a[2], area = a[3], p5 = a[4], max = a[5], count = a[6], p9 = a[8], position = a[9], angle = a[10];
    uint32_t flags = a[7];

    if (position)
        flags = (flags | 2) & 0xFF;

    if (angle)
        flags = (flags | 4) & 0xFF;

    const uint32_t table = game.Call<EnemyActionTable>({kind & 0xFF, sub & 0xFF});
    const uint32_t large = (FlatRead32(table) & 1) != 0;
    int32_t wanted = int32_t(count & 0xFF) - int32_t(SummonedCount(game, id & 0xFFFF, area & 0xFF, flags & 0xFF, large));

    if (wanted > int32_t(max & 0xFF))
        wanted = int32_t(max & 0xFF);

    uint32_t entry = kSummons, scanned = 0, request = kRequests;

    for (uint32_t slot = 0; slot < kRequestCount; ++slot, request += kRequestBytes) {
        if (wanted == 0)
            return 1;

        if (FlatRead8(request + 9) != 0)
            continue;

        FlatWrite16(request, uint16_t(id));
        FlatWrite8(request + 4, uint8_t(kind));
        FlatWrite8(request + 5, uint8_t(sub));
        FlatWrite8(request + 6, uint8_t(p5));
        FlatWrite8(request + 7, uint8_t(area));
        FlatWrite8(request + 8, uint8_t(flags));
        FlatWrite8(request + 10, uint8_t(p9));

        if (flags & 2)
            game.Call<Vec3Copy>({request + 12, position});

        if (flags & 4)
            for (uint32_t i = 0; i < 12; i += 4)
                FlatWrite32(request + 24 + i, FlatRead32(angle + i));

        // The scan goes on where the last request's left off.
        for (; scanned < kSummonCount; ++scanned, entry += kSummonBytes) {
            if (FlatRead8(entry + kUsed) == 0 || FlatRead8(entry + kState) != 1 || FlatRead8(entry + kKind) != FlatRead8(request + 4))
                continue;

            const uint32_t want = FlatRead8(request + 8);

            if ((want & 8) && FlatRead8(request + 5) != FlatRead8(entry + kSub))
                continue;

            if (want & 1) {
                if (FlatRead8(request + 7) != FlatRead8(entry + kArea))
                    continue;
            } else if (const uint32_t to = FlatRead8(request + 7); to != 0xFF && SummonReaches(game, entry, FlatRead8(kNowMap), to) == 0) {
                continue;
            }

            FlatWrite8(entry + kState, 2);
            FlatWrite16(request + 2, FlatRead16(entry + kSpawnId));
            FlatWrite8(request + 9, 1);
            game.Call<NetSummon>({0x806A6A10, 2, slot & 0xFFFF});
            break;
        }

        --wanted;
    }

    return 0;
}

// Ports.

void VecInitPort(CpuContext* ctx) {
    (void)ctx; // r3 stays the pointer
}

void TakeEnemyWorkPort(CpuContext* ctx) {
    uint32_t work, index;

    {
        GuestFrame frame(ctx, 0x10);
        GuestCall game{ctx};
        work = TakeEnemyWork(game, index);
    }

    ctx->gpr[3] = work;
    ctx->gpr[4] = index;
}

void CopySpawnRecordPort(CpuContext* ctx) {
    const uint32_t to = ctx->gpr[3], from = ctx->gpr[4];
    GuestFrame frame(ctx, 0x10);
    GuestCall game{ctx};
    CopySpawnRecord(game, to, from);
}

void SetEnemyRecordPort(CpuContext* ctx) {
    const uint32_t work = ctx->gpr[3], record = ctx->gpr[4];
    GuestFrame frame(ctx, 0x10);
    GuestCall game{ctx};
    SetEnemyRecord(game, work, record);
}

void SpawnAreaPort(CpuContext* ctx) {
    const uint32_t id = ctx->gpr[3], choice = ctx->gpr[4];
    uint32_t area;

    {
        GuestFrame frame(ctx, 0x20);
        GuestCall game{ctx};
        area = SpawnArea(game, id, choice);
    }

    ctx->gpr[3] = area;
}

void SpawnAnglePort(CpuContext* ctx) {
    const uint32_t id = ctx->gpr[3], choice = ctx->gpr[4];
    double angle;

    {
        GuestFrame frame(ctx, 0x20);
        GuestCall game{ctx};
        angle = SpawnAngle(game, id, choice);
    }

    ctx->fpr[1].d = angle;
}

void NewEnemyPort(CpuContext* ctx) {
    const uint32_t kind = ctx->gpr[3];
    uint32_t work;

    {
        GuestFrame frame(ctx, 0x10);
        GuestCall game{ctx};
        work = NewEnemy(game, kind);
    }

    ctx->gpr[3] = work;
}

void CreateQuestEnemyPort(CpuContext* ctx) {
    const uint32_t kind = ctx->gpr[3], record = ctx->gpr[4], id = ctx->gpr[5];
    uint32_t work;

    {
        GuestFrame frame(ctx, 0x20);
        GuestCall game{ctx};
        work = CreateQuestEnemy(game, kind, record, id);
    }

    ctx->gpr[3] = work;
}

void SpawnLivePort(CpuContext* ctx) {
    const uint32_t kind = ctx->gpr[3], record = ctx->gpr[4], id = ctx->gpr[5];
    GuestFrame frame(ctx, 0x20);
    GuestCall game{ctx};
    SpawnLive(game, kind, record, id);
}

void TakeSummonEntryPort(CpuContext* ctx) {
    uint32_t r4;
    ctx->gpr[3] = TakeSummonEntry(r4);
    ctx->gpr[4] = r4;
}

void InitSummonEntryPort(CpuContext* ctx) {
    const uint32_t entry = ctx->gpr[3], kind = ctx->gpr[4], area = ctx->gpr[5];
    GuestFrame frame(ctx, 0x20);
    GuestCall game{ctx};
    InitSummonEntry(game, entry, kind & 0xFF, area & 0xFF);
}

void NewSummonEntryPort(CpuContext* ctx) {
    const uint32_t kind = ctx->gpr[3], area = ctx->gpr[4];
    uint32_t entry, r4;

    {
        GuestFrame frame(ctx, 0x20);
        GuestCall game{ctx};
        entry = NewSummonEntry(game, kind, area, r4);
    }

    ctx->gpr[3] = entry;
}

void HoldSummonPort(CpuContext* ctx) {
    const uint32_t kind = ctx->gpr[3], record = ctx->gpr[4], id = ctx->gpr[5], work = ctx->gpr[6];
    uint32_t entry;

    {
        GuestFrame frame(ctx, 0x20);
        GuestCall game{ctx};
        entry = HoldSummon(game, kind, record, id, work);
    }

    ctx->gpr[3] = entry;
}

void SpawnHeldPort(CpuContext* ctx) {
    const uint32_t kind = ctx->gpr[3], record = ctx->gpr[4], id = ctx->gpr[5];
    uint32_t entry;

    {
        GuestFrame frame(ctx, 0x30);
        GuestCall game{ctx};
        entry = SpawnHeld(game, kind, record, id);
    }

    ctx->gpr[3] = entry;
}

void SummonReleasePort(CpuContext* ctx) {
    // r3..r10, then the caller's stack: +11 (u8), +12, +16.
    const uint32_t sp = ctx->gpr[1];
    const uint32_t args[11] = {ctx->gpr[3], ctx->gpr[4], ctx->gpr[5],  ctx->gpr[6],     ctx->gpr[7],          ctx->gpr[8],
                               ctx->gpr[9], ctx->gpr[10], FlatRead8(sp + 11), FlatRead32(sp + 12), FlatRead32(sp + 16)};
    uint32_t result;

    {
        GuestFrame frame(ctx, 0x50);
        GuestCall game{ctx};
        result = SummonRelease(game, args);
    }

    ctx->gpr[3] = result;
}

void SummonedCountPort(CpuContext* ctx) {
    const uint32_t id = ctx->gpr[3], area = ctx->gpr[4], flags = ctx->gpr[5], large = ctx->gpr[6];
    uint32_t count;

    {
        GuestFrame frame(ctx, 0x30);
        GuestCall game{ctx};
        count = SummonedCount(game, id, area, flags, large);
    }

    ctx->gpr[3] = count;
}

void SummonReachesPort(CpuContext* ctx) {
    const uint32_t entry = ctx->gpr[3], map = ctx->gpr[4], area = ctx->gpr[5];
    uint32_t reaches;

    {
        GuestFrame frame(ctx, 0x20);
        GuestCall game{ctx};
        reaches = SummonReaches(game, entry, map, area);
    }

    ctx->gpr[3] = reaches;
}

void EnemyKindOfPort(CpuContext* ctx) {
    const uint32_t work = ctx->gpr[3];
    uint32_t result;

    {
        GuestFrame frame(ctx, 0x10);
        GuestCall game{ctx};
        result = game.Call<EnemyKindOf>({FlatRead8(work + 0xC), FlatRead16(work + 0x1A)});
    }

    ctx->gpr[3] = result;
}
} // namespace

namespace QuestSpawns {
// For the quest ports (quest_setup.cpp, quest_step.cpp).
uint32_t SpawnHeld(GuestCall& game, uint32_t kind, uint32_t record, uint32_t id) {
    return ::SpawnHeld(game, kind, record, id);
}

uint32_t CreateQuestEnemy(GuestCall& game, uint32_t kind, uint32_t record, uint32_t id) {
    return ::CreateQuestEnemy(game, kind, record, id);
}

void SpawnLive(GuestCall& game, uint32_t kind, uint32_t record, uint32_t id) {
    ::SpawnLive(game, kind, record, id);
}

void CopySpawnRecord(GuestCall& game, uint32_t to, uint32_t from) {
    ::CopySpawnRecord(game, to, from);
}

uint32_t SummonRelease(GuestCall& game, const uint32_t (&args)[11]) {
    return ::SummonRelease(game, args);
}
} // namespace QuestSpawns

extern "C" void func_80125FB8(CpuContext* ctx);
extern "C" void MH3_VecInit(CpuContext* ctx) {
    PortCheck::Call(ctx, "VecInit", func_80125FB8, VecInitPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80125FB8, MH3_VecInit, "VecInit");

extern "C" void func_801423C0(CpuContext* ctx);
extern "C" void MH3_TakeEnemyWork(CpuContext* ctx) {
    PortCheck::Call(ctx, "TakeEnemyWork", PortCheck::Returns::Gpr3, func_801423C0, TakeEnemyWorkPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x801423C0, MH3_TakeEnemyWork, "TakeEnemyWork");

extern "C" void func_80142444(CpuContext* ctx);
extern "C" void MH3_CopySpawnRecord(CpuContext* ctx) {
    PortCheck::Call(ctx, "CopySpawnRecord", func_80142444, CopySpawnRecordPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80142444, MH3_CopySpawnRecord, "CopySpawnRecord");

extern "C" void func_801425F4(CpuContext* ctx);
extern "C" void MH3_SetEnemyRecord(CpuContext* ctx) {
    PortCheck::Call(ctx, "SetEnemyRecord", func_801425F4, SetEnemyRecordPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x801425F4, MH3_SetEnemyRecord, "SetEnemyRecord");

extern "C" void func_801427F0(CpuContext* ctx);
extern "C" void MH3_SpawnArea(CpuContext* ctx) {
    PortCheck::Call(ctx, "SpawnArea", PortCheck::Returns::Gpr3, func_801427F0, SpawnAreaPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x801427F0, MH3_SpawnArea, "SpawnArea");

extern "C" void func_801429BC(CpuContext* ctx);
extern "C" void MH3_SpawnAngle(CpuContext* ctx) {
    PortCheck::Call(ctx, "SpawnAngle", PortCheck::Returns::Fpr1, func_801429BC, SpawnAnglePort);
}
REGISTER_NATIVE_FUNCTION_AS(0x801429BC, MH3_SpawnAngle, "SpawnAngle");

extern "C" void func_80142BD8(CpuContext* ctx);
extern "C" void MH3_NewEnemy(CpuContext* ctx) {
    PortCheck::Call(ctx, "NewEnemy", PortCheck::Returns::Gpr3, func_80142BD8, NewEnemyPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80142BD8, MH3_NewEnemy, "NewEnemy");

extern "C" void func_80142C24(CpuContext* ctx);
extern "C" void MH3_CreateQuestEnemy(CpuContext* ctx) {
    PortCheck::Call(ctx, "CreateQuestEnemy", PortCheck::Returns::Gpr3, func_80142C24, CreateQuestEnemyPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80142C24, MH3_CreateQuestEnemy, "CreateQuestEnemy");

extern "C" void func_80142D4C(CpuContext* ctx);
extern "C" void MH3_SpawnLive(CpuContext* ctx) {
    PortCheck::Call(ctx, "SpawnLive", func_80142D4C, SpawnLivePort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80142D4C, MH3_SpawnLive, "SpawnLive");

extern "C" void func_801434A0(CpuContext* ctx);
extern "C" void MH3_TakeSummonEntry(CpuContext* ctx) {
    PortCheck::Call(ctx, "TakeSummonEntry", PortCheck::Returns::Gpr3, func_801434A0, TakeSummonEntryPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x801434A0, MH3_TakeSummonEntry, "TakeSummonEntry");

extern "C" void func_80143578(CpuContext* ctx);
extern "C" void MH3_InitSummonEntry(CpuContext* ctx) {
    PortCheck::Call(ctx, "InitSummonEntry", func_80143578, InitSummonEntryPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80143578, MH3_InitSummonEntry, "InitSummonEntry");

extern "C" void func_8014365C(CpuContext* ctx);
extern "C" void MH3_NewSummonEntry(CpuContext* ctx) {
    PortCheck::Call(ctx, "NewSummonEntry", PortCheck::Returns::Gpr3, func_8014365C, NewSummonEntryPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8014365C, MH3_NewSummonEntry, "NewSummonEntry");

extern "C" void func_801436B8(CpuContext* ctx);
extern "C" void MH3_SpawnHeld(CpuContext* ctx) {
    PortCheck::Call(ctx, "SpawnHeld", PortCheck::Returns::Gpr3, func_801436B8, SpawnHeldPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x801436B8, MH3_SpawnHeld, "SpawnHeld");

extern "C" void func_80143A18(CpuContext* ctx);
extern "C" void MH3_HoldSummon(CpuContext* ctx) {
    PortCheck::Call(ctx, "HoldSummon", PortCheck::Returns::Gpr3, func_80143A18, HoldSummonPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80143A18, MH3_HoldSummon, "HoldSummon");

extern "C" void func_80141BEC(CpuContext* ctx);
extern "C" void MH3_SummonRelease(CpuContext* ctx) {
    PortCheck::Call(ctx, "SummonRelease", PortCheck::Returns::Gpr3, func_80141BEC, SummonReleasePort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80141BEC, MH3_SummonRelease, "SummonRelease");

extern "C" void func_80141E20(CpuContext* ctx);
extern "C" void MH3_SummonedCount(CpuContext* ctx) {
    PortCheck::Call(ctx, "SummonedCount", PortCheck::Returns::Gpr3, func_80141E20, SummonedCountPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80141E20, MH3_SummonedCount, "SummonedCount");

extern "C" void func_80126B34(CpuContext* ctx);
extern "C" void MH3_SummonReaches(CpuContext* ctx) {
    PortCheck::Call(ctx, "SummonReaches", PortCheck::Returns::Gpr3, func_80126B34, SummonReachesPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80126B34, MH3_SummonReaches, "SummonReaches");

extern "C" void func_8012E76C(CpuContext* ctx);
extern "C" void MH3_EnemyKindOf(CpuContext* ctx) {
    PortCheck::Call(ctx, "EnemyKindOf", PortCheck::Returns::Gpr3, func_8012E76C, EnemyKindOfPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8012E76C, MH3_EnemyKindOf, "EnemyKindOf");
