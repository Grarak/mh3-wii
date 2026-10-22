// The quest's flags, RMHP08, C reconstructions from the translated code.
// Each reader takes a quest entry (0: the running quest's, 0x803B33F8) and
// tests bits of its flag word (+0x310); all give 0 while 0x800CF2E4 is 1.
//
//   0x800CF2E4 FieldStopMode(): the field work's +0x113 is 1 (0x803B890C may
//              end its update early, field_frame.cpp); the quest flags read 0
//   0x803B4D4C QuestTimedMonster: 0x80000000. The second slot's monster, held
//              in the summon pool (spawn mode 3), is released 5 minutes into
//              the quest or when a quest monster falls (0x803B0E28); only the
//              first monster's files load at the start, objective 0 alone
//              ends the quest (+0x93 = 2), the result shows six reward rows
//   0x803B4FA4 0x40000000
//   0x803B4EB4 0x10000000
//   0x803B4DC4 QuestMarathon: 0x04000000. The slots' monsters come one after
//              another (0x803B17E0), each when the last falls; no break or
//              capture rewards (0x803B23B8)
//   0x803B537C 0x02000000
//   0x803B501C 0x01000000
//   0x803B5184 0x00800000, or of 0xC0 only 0x40
//   0x803B510C 0x00200000
//   0x803B4F2C 0x00100000
//   0x803B4C50 0x00080000: every objective must be met (QuestStep)
//   0x803B5208 0x00040000
//   0x803B53F4 0x00000800
//   0x803B4E3C QuestExtraMonster: 0x00000100. The third slot is the extra
//              monster (+0x6760): it comes 3 minutes in (0x803B0FFC)
//   0x803B4CC8 0x02000000 or 0x100
//   0x803B52F8 0x02000000 or 0x80000000
//   0x803B5280 0x00000080
//   0x803B5094 QuestArena: 0x00000010 (the arena's records, ResultListKind)
//   0x803B4BD8 0x00000008
//   0x803B4AA0 (entry, n) objective n's item (u16 +0x334 + 8n) when flag 2
//              and the objective's word (+0x330 + 8n) has 2; 0 otherwise
//   0x803B4B2C QuestHasExtra: flag 0x100, or an intruder chance (+0x32C)
//   0x803B4B9C (entry) the intruder's kind, the entry's +0x32E
//   0x803B55F8 FieldStopMode, the marathon flag or an intruder chance
//   0x803B32F4 QuestQurupecoAlone(): the quest's monsters count fewer than
//              two and one is a Qurupeco (monster 3)
//   0x803AE998 (): the field's quest work's +0x6A2A is not 0
//   0x803AB1F4 (n): QuestObjectiveDone(field's quest work, n, 1); 0 without
//   0x803B0D60 (): the quest work's +0x6A3A set and the 0x00800000 reader,
//              then 0x803B0D38
#include "abi_bridge.h"
#include "guest_call.h"
#include "port_check.h"
#include "quest_work.h"

#include <cstdint>

namespace {
using MemoryInline::FlatRead16;
using MemoryInline::FlatRead32;
using MemoryInline::FlatRead8;

enum : uint32_t {
    kFlags = 0x310,
    kIntruderChance = 0x32C, // u8 percent
    kIntruderKind = 0x32E,   // u8
};

enum : uint32_t {
    ObjectiveDone = 0x803AA0C4, // QuestObjectiveDone(work, objective, flag)
    FlagEnd = 0x803B0D38,
};

bool FieldStopMode() {
    const uint32_t field = QuestWork::MoveWork(QuestWork::kField);
    return field && FlatRead8(field + 0x113) == 1;
}

// The entry a reader looks at: `entry`, else the running quest's; 0 while
// the field is in its stop mode or without a quest.
uint32_t ReaderEntry(uint32_t entry) {
    if (FieldStopMode())
        return 0;

    return entry ? entry : QuestWork::Entry();
}

uint32_t Flags(uint32_t entry) {
    const uint32_t at = ReaderEntry(entry);
    return at ? FlatRead32(at + kFlags) : 0;
}

bool Flag(uint32_t entry, uint32_t mask) {
    return (Flags(entry) & mask) != 0;
}

void Return(CpuContext* ctx, uint32_t value) {
    ctx->gpr[3] = value;
}

template <uint32_t Mask>
void FlagPort(CpuContext* ctx) {
    Return(ctx, Flag(ctx->gpr[3], Mask));
}

void FieldStopModePort(CpuContext* ctx) {
    Return(ctx, FieldStopMode());
}

// 0x803B4CC8 / 0x803B52F8: 0x02000000, else the other bit.
template <uint32_t Other>
void FlagEitherPort(CpuContext* ctx) {
    const uint32_t flags = Flags(ctx->gpr[3]);
    Return(ctx, (flags & 0x02000000) != 0 || (flags & Other) != 0);
}

bool Flag00800000(uint32_t entry) {
    if (!ReaderEntry(entry))
        return false;

    const uint32_t flags = Flags(entry);
    return (flags & 0x00800000) != 0 || (flags & 0xC0) == 0x40;
}

void Flag00800000Port(CpuContext* ctx) {
    Return(ctx, Flag00800000(ctx->gpr[3]));
}

void ObjectiveItemPort(CpuContext* ctx) {
    const uint32_t entry = ctx->gpr[3] ? ctx->gpr[3] : QuestWork::Entry();
    const uint32_t n = ctx->gpr[4] & 0xFF;
    uint32_t item = 0;

    if (entry && n < 3 && (FlatRead32(entry + kFlags) & 2) && (FlatRead32(entry + 0x330 + n * 8) & 2))
        item = FlatRead16(entry + 0x334 + n * 8);

    Return(ctx, item);
}

void HasExtraPort(CpuContext* ctx) {
    const uint32_t entry = ctx->gpr[3] ? ctx->gpr[3] : QuestWork::Entry();
    uint32_t has = 0;

    if (entry)
        has = Flag(entry, 0x100) || FlatRead8(entry + kIntruderChance) != 0;

    Return(ctx, has);
}

void IntruderKindPort(CpuContext* ctx) {
    const uint32_t entry = ctx->gpr[3] ? ctx->gpr[3] : QuestWork::Entry();
    Return(ctx, entry ? FlatRead8(entry + kIntruderKind) : 0);
}

void ExtraOrMarathonPort(CpuContext* ctx) {
    uint32_t result = 1;

    if (!FieldStopMode()) {
        const uint32_t entry = QuestWork::Entry();
        result = entry && (Flag(0, 0x04000000) || FlatRead8(entry + kIntruderChance) != 0);
    }

    Return(ctx, result);
}

void QurupecoAlonePort(CpuContext* ctx) {
    const uint32_t work = QuestWork::Work();
    const uint8_t a = FlatRead8(work + QuestWork::kMonsters), b = FlatRead8(work + QuestWork::kMonsters + 1), c = FlatRead8(work + QuestWork::kMonsters + 2);
    const uint32_t count = (a != 0) + (b != 0) + (c != 0);
    Return(ctx, (a == 3 || b == 3 || c == 3) && count < 2);
}

void FieldQuest6A2APort(CpuContext* ctx) {
    const uint32_t field = QuestWork::MoveWork(QuestWork::kField);
    const uint32_t work = field ? FlatRead32(field + 0xDC) : 0;
    Return(ctx, work && FlatRead8(work + 0x6A2A) != 0);
}

void FieldObjectiveDonePort(CpuContext* ctx) {
    const uint32_t objective = ctx->gpr[3] & 0xFF;
    uint32_t done;

    {
        GuestFrame frame(ctx, 0x10);
        GuestCall game{ctx};
        const uint32_t field = QuestWork::MoveWork(QuestWork::kField);
        const uint32_t work = field ? FlatRead32(field + 0xDC) : 0;
        done = work ? game.Call<ObjectiveDone>({work, objective, 1}) : 0;
    }

    Return(ctx, done);
}

void Flag6A3APort(CpuContext* ctx) {
    uint32_t result;

    {
        GuestFrame frame(ctx, 0x10);
        GuestCall game{ctx};
        const uint32_t work = QuestWork::Work();
        result = 0;

        if (work && FlatRead8(work + 0x6A3A) != 0 && Flag00800000(0))
            result = game.Call<FlagEnd>();
    }

    Return(ctx, result);
}
} // namespace

extern "C" void func_800CF2E4(CpuContext* ctx);
extern "C" void MH3_FieldStopMode(CpuContext* ctx) {
    PortCheck::Call(ctx, "FieldStopMode", PortCheck::Returns::Gpr3, func_800CF2E4, FieldStopModePort);
}
REGISTER_NATIVE_FUNCTION_AS(0x800CF2E4, MH3_FieldStopMode, "FieldStopMode");

extern "C" void func_803B4D4C(CpuContext* ctx);
extern "C" void MH3_QuestTimedMonster(CpuContext* ctx) {
    PortCheck::Call(ctx, "QuestTimedMonster", PortCheck::Returns::Gpr3, func_803B4D4C, FlagPort<0x80000000>);
}
REGISTER_NATIVE_FUNCTION_AS(0x803B4D4C, MH3_QuestTimedMonster, "QuestTimedMonster");

extern "C" void func_803B4FA4(CpuContext* ctx);
extern "C" void MH3_QuestFlag40000000(CpuContext* ctx) {
    PortCheck::Call(ctx, "QuestFlag40000000", PortCheck::Returns::Gpr3, func_803B4FA4, FlagPort<0x40000000>);
}
REGISTER_NATIVE_FUNCTION_AS(0x803B4FA4, MH3_QuestFlag40000000, "QuestFlag40000000");

extern "C" void func_803B4EB4(CpuContext* ctx);
extern "C" void MH3_QuestFlag10000000(CpuContext* ctx) {
    PortCheck::Call(ctx, "QuestFlag10000000", PortCheck::Returns::Gpr3, func_803B4EB4, FlagPort<0x10000000>);
}
REGISTER_NATIVE_FUNCTION_AS(0x803B4EB4, MH3_QuestFlag10000000, "QuestFlag10000000");

extern "C" void func_803B4DC4(CpuContext* ctx);
extern "C" void MH3_QuestMarathon(CpuContext* ctx) {
    PortCheck::Call(ctx, "QuestMarathon", PortCheck::Returns::Gpr3, func_803B4DC4, FlagPort<0x04000000>);
}
REGISTER_NATIVE_FUNCTION_AS(0x803B4DC4, MH3_QuestMarathon, "QuestMarathon");

extern "C" void func_803B537C(CpuContext* ctx);
extern "C" void MH3_QuestFlag02000000(CpuContext* ctx) {
    PortCheck::Call(ctx, "QuestFlag02000000", PortCheck::Returns::Gpr3, func_803B537C, FlagPort<0x02000000>);
}
REGISTER_NATIVE_FUNCTION_AS(0x803B537C, MH3_QuestFlag02000000, "QuestFlag02000000");

extern "C" void func_803B501C(CpuContext* ctx);
extern "C" void MH3_QuestFlag01000000(CpuContext* ctx) {
    PortCheck::Call(ctx, "QuestFlag01000000", PortCheck::Returns::Gpr3, func_803B501C, FlagPort<0x01000000>);
}
REGISTER_NATIVE_FUNCTION_AS(0x803B501C, MH3_QuestFlag01000000, "QuestFlag01000000");

extern "C" void func_803B5184(CpuContext* ctx);
extern "C" void MH3_QuestFlag00800000(CpuContext* ctx) {
    PortCheck::Call(ctx, "QuestFlag00800000", PortCheck::Returns::Gpr3, func_803B5184, Flag00800000Port);
}
REGISTER_NATIVE_FUNCTION_AS(0x803B5184, MH3_QuestFlag00800000, "QuestFlag00800000");

extern "C" void func_803B510C(CpuContext* ctx);
extern "C" void MH3_QuestFlag00200000(CpuContext* ctx) {
    PortCheck::Call(ctx, "QuestFlag00200000", PortCheck::Returns::Gpr3, func_803B510C, FlagPort<0x00200000>);
}
REGISTER_NATIVE_FUNCTION_AS(0x803B510C, MH3_QuestFlag00200000, "QuestFlag00200000");

extern "C" void func_803B4F2C(CpuContext* ctx);
extern "C" void MH3_QuestFlag00100000(CpuContext* ctx) {
    PortCheck::Call(ctx, "QuestFlag00100000", PortCheck::Returns::Gpr3, func_803B4F2C, FlagPort<0x00100000>);
}
REGISTER_NATIVE_FUNCTION_AS(0x803B4F2C, MH3_QuestFlag00100000, "QuestFlag00100000");

extern "C" void func_803B4C50(CpuContext* ctx);
extern "C" void MH3_QuestAllObjectives(CpuContext* ctx) {
    PortCheck::Call(ctx, "QuestAllObjectives", PortCheck::Returns::Gpr3, func_803B4C50, FlagPort<0x00080000>);
}
REGISTER_NATIVE_FUNCTION_AS(0x803B4C50, MH3_QuestAllObjectives, "QuestAllObjectives");

extern "C" void func_803B5208(CpuContext* ctx);
extern "C" void MH3_QuestFlag00040000(CpuContext* ctx) {
    PortCheck::Call(ctx, "QuestFlag00040000", PortCheck::Returns::Gpr3, func_803B5208, FlagPort<0x00040000>);
}
REGISTER_NATIVE_FUNCTION_AS(0x803B5208, MH3_QuestFlag00040000, "QuestFlag00040000");

extern "C" void func_803B53F4(CpuContext* ctx);
extern "C" void MH3_QuestFlag00000800(CpuContext* ctx) {
    PortCheck::Call(ctx, "QuestFlag00000800", PortCheck::Returns::Gpr3, func_803B53F4, FlagPort<0x00000800>);
}
REGISTER_NATIVE_FUNCTION_AS(0x803B53F4, MH3_QuestFlag00000800, "QuestFlag00000800");

extern "C" void func_803B4E3C(CpuContext* ctx);
extern "C" void MH3_QuestExtraMonster(CpuContext* ctx) {
    PortCheck::Call(ctx, "QuestExtraMonster", PortCheck::Returns::Gpr3, func_803B4E3C, FlagPort<0x00000100>);
}
REGISTER_NATIVE_FUNCTION_AS(0x803B4E3C, MH3_QuestExtraMonster, "QuestExtraMonster");

extern "C" void func_803B4CC8(CpuContext* ctx);
extern "C" void MH3_QuestFlag02000000Or100(CpuContext* ctx) {
    PortCheck::Call(ctx, "QuestFlag02000000Or100", PortCheck::Returns::Gpr3, func_803B4CC8, FlagEitherPort<0x00000100>);
}
REGISTER_NATIVE_FUNCTION_AS(0x803B4CC8, MH3_QuestFlag02000000Or100, "QuestFlag02000000Or100");

extern "C" void func_803B52F8(CpuContext* ctx);
extern "C" void MH3_QuestFlag02000000OrTimed(CpuContext* ctx) {
    PortCheck::Call(ctx, "QuestFlag02000000OrTimed", PortCheck::Returns::Gpr3, func_803B52F8, FlagEitherPort<0x80000000>);
}
REGISTER_NATIVE_FUNCTION_AS(0x803B52F8, MH3_QuestFlag02000000OrTimed, "QuestFlag02000000OrTimed");

extern "C" void func_803B5280(CpuContext* ctx);
extern "C" void MH3_QuestFlag00000080(CpuContext* ctx) {
    PortCheck::Call(ctx, "QuestFlag00000080", PortCheck::Returns::Gpr3, func_803B5280, FlagPort<0x00000080>);
}
REGISTER_NATIVE_FUNCTION_AS(0x803B5280, MH3_QuestFlag00000080, "QuestFlag00000080");

extern "C" void func_803B5094(CpuContext* ctx);
extern "C" void MH3_QuestArena(CpuContext* ctx) {
    PortCheck::Call(ctx, "QuestArena", PortCheck::Returns::Gpr3, func_803B5094, FlagPort<0x00000010>);
}
REGISTER_NATIVE_FUNCTION_AS(0x803B5094, MH3_QuestArena, "QuestArena");

extern "C" void func_803B4BD8(CpuContext* ctx);
extern "C" void MH3_QuestFlag00000008(CpuContext* ctx) {
    PortCheck::Call(ctx, "QuestFlag00000008", PortCheck::Returns::Gpr3, func_803B4BD8, FlagPort<0x00000008>);
}
REGISTER_NATIVE_FUNCTION_AS(0x803B4BD8, MH3_QuestFlag00000008, "QuestFlag00000008");

extern "C" void func_803B4AA0(CpuContext* ctx);
extern "C" void MH3_QuestObjectiveItem(CpuContext* ctx) {
    PortCheck::Call(ctx, "QuestObjectiveItem", PortCheck::Returns::Gpr3, func_803B4AA0, ObjectiveItemPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x803B4AA0, MH3_QuestObjectiveItem, "QuestObjectiveItem");

extern "C" void func_803B4B2C(CpuContext* ctx);
extern "C" void MH3_QuestHasExtra(CpuContext* ctx) {
    PortCheck::Call(ctx, "QuestHasExtra", PortCheck::Returns::Gpr3, func_803B4B2C, HasExtraPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x803B4B2C, MH3_QuestHasExtra, "QuestHasExtra");

extern "C" void func_803B4B9C(CpuContext* ctx);
extern "C" void MH3_QuestIntruderKind(CpuContext* ctx) {
    PortCheck::Call(ctx, "QuestIntruderKind", PortCheck::Returns::Gpr3, func_803B4B9C, IntruderKindPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x803B4B9C, MH3_QuestIntruderKind, "QuestIntruderKind");

extern "C" void func_803B55F8(CpuContext* ctx);
extern "C" void MH3_QuestExtraOrMarathon(CpuContext* ctx) {
    PortCheck::Call(ctx, "QuestExtraOrMarathon", PortCheck::Returns::Gpr3, func_803B55F8, ExtraOrMarathonPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x803B55F8, MH3_QuestExtraOrMarathon, "QuestExtraOrMarathon");

extern "C" void func_803B32F4(CpuContext* ctx);
extern "C" void MH3_QuestQurupecoAlone(CpuContext* ctx) {
    PortCheck::Call(ctx, "QuestQurupecoAlone", PortCheck::Returns::Gpr3, func_803B32F4, QurupecoAlonePort);
}
REGISTER_NATIVE_FUNCTION_AS(0x803B32F4, MH3_QuestQurupecoAlone, "QuestQurupecoAlone");

extern "C" void func_803AE998(CpuContext* ctx);
extern "C" void MH3_FieldQuest6A2A(CpuContext* ctx) {
    PortCheck::Call(ctx, "FieldQuest6A2A", PortCheck::Returns::Gpr3, func_803AE998, FieldQuest6A2APort);
}
REGISTER_NATIVE_FUNCTION_AS(0x803AE998, MH3_FieldQuest6A2A, "FieldQuest6A2A");

extern "C" void func_803AB1F4(CpuContext* ctx);
extern "C" void MH3_FieldObjectiveDone(CpuContext* ctx) {
    PortCheck::Call(ctx, "FieldObjectiveDone", PortCheck::Returns::Gpr3, func_803AB1F4, FieldObjectiveDonePort);
}
REGISTER_NATIVE_FUNCTION_AS(0x803AB1F4, MH3_FieldObjectiveDone, "FieldObjectiveDone");

extern "C" void func_803B0D60(CpuContext* ctx);
extern "C" void MH3_QuestFlag6A3A(CpuContext* ctx) {
    PortCheck::Call(ctx, "QuestFlag6A3A", PortCheck::Returns::Gpr3, func_803B0D60, Flag6A3APort);
}
REGISTER_NATIVE_FUNCTION_AS(0x803B0D60, MH3_QuestFlag6A3A, "QuestFlag6A3A");
