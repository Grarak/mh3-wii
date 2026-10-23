// Summons outside the quest's own machines, RMHP08: the Qurupeco's calls,
// the summon rules and the quest monster's down notice. C reconstructions
// from the translated code.
//
//   0x80155160 QurupecoEvent(work, type, id): the Qurupeco's (monster 3's,
//              its table at 0x805A4C7C) motion events. Every event clears
//              the variant (+0x1E4) after reading it. Type 1:
//                5 0x80130FD8(work); 9, 19, 20 the variant 1, 2, 3;
//                8/25, 13/26, 14/27, 15/28, 16/29, 17/30, 33/35, 34/36 the
//                monster the next call imitates (+0x330: 12 Great Jaggi,
//                1 Rathian, 2 Rathalos, 8 Barroth, 16 Royal Ludroth,
//                6 Diablos, 15 Lagiacrus, 7 Deviljho), 900 frames at +0x338;
//                7, 23, 31, 32 the small call's kind (+0x334: 10, 31, 17, 29);
//                18 0x8012D67C(work, area, by the variant); 37 0x80137718;
//                21 the small call: two at most of +0x334's kind, from the
//                Qurupeco's area (SummonRelease; the table at 0x805A4FF0
//                gives Jaggi sub 1);
//                22 the call (+0x331 1, +0x332 counts it): a held summon pool
//                entry of the imitated kind comes (SummonRelease, one), else
//                the failed call (0x8012C284).
//              Type 2 id 6: +0x333 1, +0x44C at least 3600. Type 3 id 9:
//              +0x1FB 1. Type 10 id 93: 0x8027D138(area, by the variant).
//              Type 11, in the field mode +0x113 1 (0x800CF2E4): every held
//              entry of a kind 0x8015D734 takes is called. Type 13 id 1:
//              +0x336 1. Summons only while the Qurupeco may act (+0x1F5 1).
//   0x8015D734 QurupecoCalls(kind): 1 for the kinds of the table at
//              0x805A4FE0 (Rathian, Rathalos, Great Jaggi, Barroth, Royal
//              Ludroth, Diablos, Deviljho)
//   0x80145B28 SummonRules(set): set `set` of the summon rules (0x806A7958,
//              per map: +8 a list of 6-byte rules ending at kind 0): for each
//              rule {kind, called, sub, mode, count, flags}, a live enemy of
//              the kind, else a summon pool entry of it, calls `count` of the
//              called kind to the player's area (0x801450D8)
//   0x801450E4 SummonRulesTick(): the rules of the set at 0x80796F53 (-1
//              none) once whenever 0x80796F52 is 0, which it then sets to 1
//   0x803A9238 MonsterDownNotice(enemy, how): a monster fell: the quest's
//              QuestMonsterDown (0x803A9728), or 0x803B8A30 in the field
//              mode +0x113 1
#include "abi_bridge.h"
#include "guest_call.h"
#include "port_check.h"
#include "ppc_runtime.h"
#include "quest_work.h"

#include <cstdint>

namespace QuestSpawns {
uint32_t SummonRelease(GuestCall& game, const uint32_t (&args)[11]);
} // namespace QuestSpawns

namespace {
using MemoryInline::FlatRead16;
using MemoryInline::FlatRead32;
using MemoryInline::FlatRead8;
using MemoryInline::FlatWrite16;
using MemoryInline::FlatWrite8;

constexpr uint32_t kSummons = 0x806A7960, kSummonBytes = 0x44, kSummonCount = 128;
constexpr uint32_t kEnemyBytes = 0xB18;
constexpr uint32_t kRuleSets = 0x806A7958;  // the summon rules' sets, by index
constexpr uint32_t kRules = 0x80796F50;     // r13 - 0x4390: +2 run flag, +3 s8 the set
constexpr uint32_t kSmallCalls = 0x805A4FF0; // {call kind, sub} pairs
constexpr uint32_t kCalledKinds = 0x805A4FE0; // {kind, 0xFF} pairs

enum : uint32_t {
    FieldStopMode = 0x800CF2E4,
    PlayerArea = 0x801450D8,
    QuestMonsterDown = 0x803A9728,
};

// The Qurupeco's work.
enum : uint32_t {
    kId = 0x1A,          // u16: its spawn id, the summons' group
    kArea = 0x1E1,       // u8
    kVariant = 0x1E4,    // u8: set by events 9, 19, 20; cleared by each event
    kMayAct = 0x1F5,     // u8: 1 while it may act (0x8012D204, inlined)
    kImitated = 0x330,   // u8: the monster the call imitates
    kCallMade = 0x331,   // u8
    kCalls = 0x332,      // u8: calls made (at most 255)
    kSmallKind = 0x334,  // u8: the small call's kind
    kCalledJaggi = 0x335, // u8: a small call of kind 10 was made
    kImitating = 0x338,  // u16: frames the imitation holds
    kMoveFlags = 0x836,  // u16
};

// SummonRelease of `kind` for the Qurupeco: one from anywhere (max and count
// 1, flags 0) or, for the small call, two in its area.
void Summon(GuestCall& game, uint32_t work, uint32_t kind, uint32_t sub, uint32_t area, uint32_t max, uint32_t count, uint32_t flags, uint32_t lastArea) {
    const uint32_t args[11] = {FlatRead16(work + kId), kind, sub, area, 0xFF, max, count, flags, lastArea, 0, 0};
    QuestSpawns::SummonRelease(game, args);
}

// The first used summon pool entry of `kind` held (state 1), or 0.
uint32_t HeldEntry(uint32_t kind) {
    for (uint32_t i = 0; i < kSummonCount; ++i) {
        const uint32_t entry = kSummons + i * kSummonBytes;

        if (FlatRead8(entry) && FlatRead8(entry + 8) == 1 && FlatRead8(entry + 2) == kind)
            return entry;
    }

    return 0;
}

void Motion(GuestCall& game, uint32_t work, uint32_t id, uint32_t variant) {
    // The monster the next call imitates, for 900 frames.
    const auto imitate = [&](uint8_t kind) {
        FlatWrite8(work + kImitated, kind);
        FlatWrite16(work + kImitating, 900);
    };

    switch (id) {
    case 5:
        game.Call<0x80130FD8>({work});
        break;
    case 7:
        FlatWrite8(work + kSmallKind, 10);
        break;
    case 8:
    case 25:
        imitate(12);
        break;
    case 9:
        FlatWrite8(work + kVariant, 1);
        break;
    case 13:
    case 26:
        imitate(1);
        break;
    case 14:
    case 27:
        imitate(2);
        break;
    case 15:
    case 28:
        imitate(8);
        break;
    case 16:
    case 29:
        imitate(16);
        break;
    case 17:
    case 30:
        imitate(6);
        break;
    case 18:
        if (variant >= 1 && variant <= 3)
            game.Call<0x8012D67C>({work, FlatRead8(work + kArea), variant == 1 ? 0u : variant == 2 ? 2u : 1u});

        break;
    case 19:
        FlatWrite8(work + kVariant, 2);
        break;
    case 20:
        FlatWrite8(work + kVariant, 3);
        break;
    case 21: {
        const uint32_t kind = FlatRead8(work + kSmallKind);

        if (FlatRead8(work + kMayAct) != 1 || !kind)
            break;

        // The last pair of the kind gives its sub (none: 255).
        uint32_t sub = 0xFF;

        for (uint32_t at = kSmallCalls; FlatRead8(at); at += 2)
            if (FlatRead8(at) == kind)
                sub = FlatRead8(at + 1);

        if (sub == 0xFF)
            Summon(game, work, kind, 0, FlatRead8(work + kArea), 2, 0xFF, 1, 0xFF);
        else
            Summon(game, work, kind, sub, FlatRead8(work + kArea), 2, 0xFF, 9, 0xFF);

        if (FlatRead8(work + kSmallKind) == 10)
            FlatWrite8(work + kCalledJaggi, 1);

        break;
    }
    case 22: {
        FlatWrite8(work + kCallMade, 1);

        if (const uint8_t calls = FlatRead8(work + kCalls); calls < 0xFF)
            FlatWrite8(work + kCalls, uint8_t(calls + 1));

        const uint32_t kind = FlatRead8(work + kImitated);

        if (!kind)
            break;

        if (!HeldEntry(kind))
            game.Call<0x8012C284>({kind, FlatRead8(work + kArea), 0});
        else if (FlatRead8(work + kMayAct) == 1)
            Summon(game, work, kind, 0, 0xFF, 1, 1, 0, FlatRead8(work + kArea));

        break;
    }
    case 23:
        FlatWrite8(work + kSmallKind, 31);
        break;
    case 31:
        FlatWrite8(work + kSmallKind, 17);
        break;
    case 32:
        FlatWrite8(work + kSmallKind, 29);
        break;
    case 33:
    case 35:
        imitate(15);
        break;
    case 34:
    case 36:
        imitate(7);
        break;
    case 37:
        game.Call<0x80137718>({work});
        break;
    default:
        break;
    }
}

void QurupecoEvent(GuestCall& game, uint32_t work, uint32_t type, uint32_t rawId) {
    const uint32_t variant = FlatRead8(work + kVariant), id = rawId & 0xFF;
    FlatWrite8(work + kVariant, 0);
    FlatWrite16(work + kMoveFlags, uint16_t(FlatRead16(work + kMoveFlags) | 0x8000));

    switch (type & 0xFF) {
    case 1:
        if (id >= 5 && id <= 37)
            Motion(game, work, id, variant);

        break;
    case 2:
        if (id == 6) {
            FlatWrite8(work + 0x333, 1);

            if (int16_t(FlatRead16(work + 0x44C)) < 3600)
                FlatWrite16(work + 0x44C, 3600);
        }

        break;
    case 3:
        if (id == 9)
            FlatWrite8(work + 0x1FB, 1);

        break;
    case 10:
        if (id == 93 && variant >= 1 && variant <= 3)
            game.Call<0x8027D138>({FlatRead8(work + kArea), variant == 1 ? 0u : variant == 2 ? 2u : 1u, rawId});

        break;
    case 11:
        if (game.Call<FieldStopMode>() != 1)
            break;

        for (uint32_t i = 0; i < kSummonCount; ++i) {
            const uint32_t entry = kSummons + i * kSummonBytes;

            if (FlatRead8(entry) && FlatRead8(entry + 8) == 1 && game.Call<0x8015D734>({FlatRead8(entry + 2)}) != 0)
                Summon(game, work, FlatRead8(entry + 2), 0, 0xFF, 1, 1, 0, FlatRead8(work + kArea));
        }

        break;
    case 13:
        if (id == 1)
            FlatWrite8(work + 0x336, 1);

        break;
    default:
        break;
    }
}

uint32_t QurupecoCalls(uint32_t kind) {
    for (uint32_t at = kCalledKinds; FlatRead8(at); at += 2)
        if (FlatRead8(at) == (kind & 0xFF))
            return 1;

    return 0;
}

// The called kind for a rule found as `id`: the rule's mode 1 takes the
// found one's area (+0x46C of an enemy, +9 of a pool entry).
void CallForRule(GuestCall& game, uint32_t rule, uint32_t id, uint32_t foundArea) {
    const uint32_t area = FlatRead8(rule + 3) == 1 ? foundArea : 0xFF;
    const uint32_t player = game.Call<PlayerArea>() & 0xFF;
    const uint32_t args[11] = {id, FlatRead8(rule + 1), FlatRead8(rule + 2), player, area, FlatRead8(rule + 4), FlatRead8(rule + 4), FlatRead8(rule + 5), 0xFF, 0, 0};
    QuestSpawns::SummonRelease(game, args);
}

void SummonRules(GuestCall& game, uint32_t set) {
    const uint32_t enemies = QuestWork::MoveWork(QuestWork::kEnemies);
    const int32_t count = int16_t(QuestWork::MoveWorkCount(QuestWork::kEnemies));
    const uint32_t sets = FlatRead32(kRuleSets);
    const uint32_t rules = sets ? FlatRead32(sets + (set & 0xFF) * 4) : 0;

    if (!rules || !FlatRead32(rules + 8))
        return;

    for (uint32_t rule = FlatRead32(rules + 8); FlatRead8(rule); rule += 6) {
        const uint32_t kind = FlatRead8(rule);
        bool called = false;

        // A live enemy of the kind, else a summon pool entry of it.
        for (int32_t i = 0; i < count && !called; ++i) {
            const uint32_t enemy = enemies + uint32_t(i) * kEnemyBytes;

            if (FlatRead8(enemy) && FlatRead8(enemy + 3) == kind) {
                CallForRule(game, rule, FlatRead16(enemy + 0x1A), FlatRead8(enemy + 0x46C));
                called = true;
            }
        }

        for (uint32_t i = 0; i < kSummonCount && !called; ++i) {
            const uint32_t entry = kSummons + i * kSummonBytes;

            if (FlatRead8(entry) && FlatRead8(entry + 2) == kind) {
                CallForRule(game, rule, FlatRead16(entry + 22), FlatRead8(entry + 9));
                called = true;
            }
        }
    }
}

void SummonRulesTick(GuestCall& game) {
    if (FlatRead8(kRules + 2))
        return;

    if (int8_t(FlatRead8(kRules + 3)) != -1)
        game.Call<0x80145B28>({FlatRead8(kRules + 3)});

    FlatWrite8(kRules + 2, uint8_t(FlatRead8(kRules + 2) + 1));
}

void MonsterDownNotice(GuestCall& game, uint32_t enemy, uint32_t how) {
    const uint32_t field = QuestWork::MoveWork(QuestWork::kField);

    if (!field)
        return;

    if (FlatRead8(field + 0x113) == 1)
        game.Call<0x803B8A30>({enemy, how & 0xFF});
    else
        game.Call<QuestMonsterDown>({field, enemy, how & 0xFF});
}

void QurupecoEventPort(CpuContext* ctx) {
    const uint32_t work = ctx->gpr[3], type = ctx->gpr[4], id = ctx->gpr[5];
    GuestFrame frame(ctx, 0x30);
    GuestCall game{ctx};
    QurupecoEvent(game, work, type, id);
}

void QurupecoCallsPort(CpuContext* ctx) {
    ctx->gpr[3] = QurupecoCalls(ctx->gpr[3]);
}

void SummonRulesPort(CpuContext* ctx) {
    const uint32_t set = ctx->gpr[3];
    GuestFrame frame(ctx, 0x40);
    GuestCall game{ctx};
    SummonRules(game, set);
}

void SummonRulesTickPort(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x10);
    GuestCall game{ctx};
    SummonRulesTick(game);
}

void MonsterDownNoticePort(CpuContext* ctx) {
    const uint32_t enemy = ctx->gpr[3], how = ctx->gpr[4];
    GuestFrame frame(ctx, 0x10);
    GuestCall game{ctx};
    MonsterDownNotice(game, enemy, how);
}
} // namespace

extern "C" void func_80155160(CpuContext* ctx);
extern "C" void MH3_QurupecoEvent(CpuContext* ctx) {
    PortCheck::Call(ctx, "QurupecoEvent", func_80155160, QurupecoEventPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80155160, MH3_QurupecoEvent, "QurupecoEvent");

extern "C" void func_8015D734(CpuContext* ctx);
extern "C" void MH3_QurupecoCalls(CpuContext* ctx) {
    PortCheck::Call(ctx, "QurupecoCalls", PortCheck::Returns::Gpr3, func_8015D734, QurupecoCallsPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8015D734, MH3_QurupecoCalls, "QurupecoCalls");

extern "C" void func_80145B28(CpuContext* ctx);
extern "C" void MH3_SummonRules(CpuContext* ctx) {
    PortCheck::Call(ctx, "SummonRules", func_80145B28, SummonRulesPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80145B28, MH3_SummonRules, "SummonRules");

extern "C" void func_801450E4(CpuContext* ctx);
extern "C" void MH3_SummonRulesTick(CpuContext* ctx) {
    PortCheck::Call(ctx, "SummonRulesTick", func_801450E4, SummonRulesTickPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x801450E4, MH3_SummonRulesTick, "SummonRulesTick");

extern "C" void func_803A9238(CpuContext* ctx);
extern "C" void MH3_MonsterDownNotice(CpuContext* ctx) {
    PortCheck::Call(ctx, "MonsterDownNotice", func_803A9238, MonsterDownNoticePort);
}
REGISTER_NATIVE_FUNCTION_AS(0x803A9238, MH3_MonsterDownNotice, "MonsterDownNotice");
