// Small-creature group predicates and the passes over them, RMHP08,
// decompiled with Ghidra and the translation. Every step ObjectAgesUpdate
// (0x80345188) counts each of the ten creature groups' members (0x806C1520,
// 0x3C bytes each) among the enemies (move work 3, 0xB18 bytes each) and the
// 128 entries at 0x806A7960 (0x44 bytes each), then matches the groups
// against both: about 2600 predicate calls a step, 4.4% of the devkit's game
// thread in the Deserted Island's area 10 through the guest calls alone. The
// passes, the predicates and their helpers run natively here:
//
//   0x80343A38 (kind, variant) -> the kind's class: 2 for kinds 10-12, 8 for
//     13-14, 7 for 0x16 (6 when variant is 0), 5 for 0x17, 3 for 0x1B-0x1C,
//     4 for 0x20 with variant 2, else 1.
//   0x80343948 (class, id) -> the group with that class (+0) and id (+1), or 0.
//   0x80343AD0 (group) -> the first byte of the class's table at 0x805E9928.
//   0x80344260 (group, entry) -> 1 when the entry (+0 set, +8 clear) is of
//     the group's class and carries its id (+9, not 0xFF).
//   0x803444C4 (group, entry) -> that, or: the group for the entry's class and
//     id exists and shares this group's +0x17 (neither 0xFF), or the class
//     table's byte has bit 8 and the two share +0x14.
//   0x803441C8, 0x803443B4 (group, enemy) -> the two for an enemy alive (+0
//     set, +4 below 2) and not in mode 11 or 12 (+0x1E5): its class from kind
//     +3 and variant +0xA, its id +0x46C.
//   0x803442EC (group): +0xD counts the enemies (0x803441C8) and entries
//     (0x80344260) the group holds.
//   0x803445C8 (group) -> 0 when the group (+2 set) is related to an enemy,
//     else 1 when to an entry in the current area (+0xA, 0x806BB806).
//   0x80344F00: each active group (+0 set, +0x16 not 1) numbers the related
//     enemies (+0x46D, +0xE the count), hands them to 0x803446BC with its
//     leader (the group of its class and id +0x17, else itself), and counts
//     the related entries on; a group of the same class and a member's id
//     later in the list is marked done (the walk numbers it from the member's
//     index, as the original) and takes the count of an earlier one sharing
//     its +0x17.
//
// MH3_PORT_CHECK=CreatureKindClass,CreatureGroupFind,CreatureGroupFlags,
// CreatureGroupHolds,CreatureGroupRelated,CreatureEnemyHolds,
// CreatureEnemyRelated,CreatureGroupCount,CreatureGroupAway,
// CreatureGroupsMatch checks each call against the original.
#include "abi_bridge.h"
#include "guest_call.h"
#include "memory.h"
#include "port_check.h"

#include <cstdint>

namespace {

constexpr uint32_t kGroups = 0x806C1520; // 10 of 0x3C bytes
constexpr uint32_t kGroupBytes = 0x3C;
constexpr unsigned kGroupCount = 10;
constexpr uint32_t kClassTables = 0x805E9928; // u32 per class: its table
constexpr uint32_t kEntries = 0x806A7960;     // 128 of 0x44 bytes
constexpr uint32_t kEntryBytes = 0x44;
constexpr unsigned kEntryCount = 128;
constexpr uint32_t kMoveWorks = 0x8065AB04; // the move work table: u16 counts at +0, works at +0x10 by kind
constexpr uint32_t kEnemyKind = 3;          // the enemies' move work kind
constexpr uint32_t kEnemyBytes = 0xB18;
constexpr uint32_t kArea = 0x806BB806;

enum : uint32_t { // group
    kGroupClass = 0x00,
    kGroupId = 0x01,
    kGroupSet = 0x02,
    kGroupHeld = 0x0D,
    kGroupRelated = 0x0E,
    kGroupLink = 0x14,
    kGroupState = 0x16,
    kGroupFamily = 0x17,
};

enum : uint32_t { // entry
    kEntrySet = 0x00,
    kEntryKind = 0x02,
    kEntryVariant = 0x03,
    kEntryGone = 0x08,
    kEntryId = 0x09,
    kEntryArea = 0x0A,
};

enum : uint32_t { // enemy
    kEnemySet = 0x000,
    kEnemyKindByte = 0x003,
    kEnemyLife = 0x004,
    kEnemyVariant = 0x00A,
    kEnemyMode = 0x1E5,
    kEnemyId = 0x46C,
    kEnemyNumber = 0x46D,
};

uint8_t KindClass(uint8_t kind, uint8_t variant) {
    switch (kind) {
    case 0x0A:
    case 0x0B:
    case 0x0C:
        return 2;

    case 0x0D:
    case 0x0E:
        return 8;

    case 0x16:
        return variant ? 7 : 6;

    case 0x17:
        return 5;

    case 0x1B:
    case 0x1C:
        return 3;

    case 0x20:
        return variant == 2 ? 4 : 1;

    default:
        return 1;
    }
}

uint32_t FindGroup(uint8_t groupClass, uint8_t id) {
    for (unsigned i = 0; i < kGroupCount; ++i) {
        const uint32_t group = kGroups + i * kGroupBytes;

        if (Memory::Read8(group + kGroupClass) == groupClass && Memory::Read8(group + kGroupId) == id)
            return group;
    }

    return 0;
}

uint8_t ClassFlags(uint32_t group) {
    return Memory::Read8(Memory::Read32(kClassTables + 4 * Memory::Read8(group + kGroupClass)));
}

// The entry is live and of the group's class.
bool Live(uint32_t entry) {
    return Memory::Read8(entry + kEntrySet) != 0 && Memory::Read8(entry + kEntryGone) == 0;
}

uint8_t EntryClass(uint32_t entry) {
    return KindClass(Memory::Read8(entry + kEntryKind), Memory::Read8(entry + kEntryVariant));
}

bool Holds(uint32_t group, uint32_t entry) {
    if (!Live(entry) || Memory::Read8(group + kGroupClass) != EntryClass(entry))
        return false;

    const uint8_t id = Memory::Read8(entry + kEntryId);
    return id != 0xFF && id == Memory::Read8(group + kGroupId);
}

// The group's class and id are these, or its group of them shares its
// family, or (with the class flag 8) its link.
bool RelatedTo(uint32_t group, uint8_t memberClass, uint8_t id) {
    const uint8_t groupClass = Memory::Read8(group + kGroupClass);

    if (groupClass == memberClass && id == Memory::Read8(group + kGroupId) && id != 0xFF)
        return true;

    const uint32_t other = FindGroup(groupClass, id);

    if (!other)
        return false;

    const uint8_t family = Memory::Read8(group + kGroupFamily), otherFamily = Memory::Read8(other + kGroupFamily);

    if (family == otherFamily && family != 0xFF && otherFamily != 0xFF)
        return true;

    return (ClassFlags(group) & 8) && Memory::Read8(group + kGroupLink) == Memory::Read8(other + kGroupLink);
}

bool Related(uint32_t group, uint32_t entry) {
    return Live(entry) && RelatedTo(group, EntryClass(entry), Memory::Read8(entry + kEntryId));
}

// The enemy is alive (em_work_die_ck, 0x8012D068) and not in mode 11 or 12.
bool EnemyActive(uint32_t enemy) {
    if (!Memory::Read8(enemy + kEnemySet) || Memory::Read8(enemy + kEnemyLife) >= 2)
        return false;

    const uint8_t mode = Memory::Read8(enemy + kEnemyMode);
    return mode != 11 && mode != 12;
}

uint8_t EnemyClass(uint32_t enemy) {
    return KindClass(Memory::Read8(enemy + kEnemyKindByte), Memory::Read8(enemy + kEnemyVariant));
}

bool EnemyHolds(uint32_t group, uint32_t enemy) {
    if (!EnemyActive(enemy) || Memory::Read8(group + kGroupClass) != EnemyClass(enemy))
        return false;

    const uint8_t id = Memory::Read8(enemy + kEnemyId);
    return id != 0xFF && id == Memory::Read8(group + kGroupId);
}

bool EnemyRelated(uint32_t group, uint32_t enemy) {
    return EnemyActive(enemy) && RelatedTo(group, EnemyClass(enemy), Memory::Read8(enemy + kEnemyId));
}

// get_move_work_adrs and get_move_work_max (0x800CFAF4, 0x800CFB34) of the
// enemies.
uint32_t Enemies() {
    const uint32_t table = Memory::Read32(kMoveWorks);
    return table ? Memory::Read32(table + 0x10 + 4 * kEnemyKind) : 0;
}

int32_t EnemyCount() {
    const uint32_t table = Memory::Read32(kMoveWorks);
    return table ? Memory::Read16(table + 2 * kEnemyKind) : 0;
}

uint32_t Entry(unsigned i) {
    return kEntries + i * kEntryBytes;
}

void Count(uint32_t group) {
    uint8_t held = 0;
    uint32_t enemy = Enemies();
    const int32_t count = EnemyCount();

    // The original's index is a byte.
    for (uint32_t i = 0; int32_t(i & 0xFF) < count; ++i, enemy += kEnemyBytes)
        held += EnemyHolds(group, enemy);

    for (unsigned i = 0; i < kEntryCount; ++i)
        held += Holds(group, Entry(i));

    Memory::Write8(group + kGroupHeld, held);
}

bool Away(uint32_t group) {
    if (!Memory::Read8(group + kGroupSet))
        return false;

    uint32_t enemy = Enemies();

    for (int32_t i = 0, count = EnemyCount(); i < count; ++i, enemy += kEnemyBytes) {
        if (EnemyRelated(group, enemy))
            return false;
    }

    const uint8_t area = Memory::Read8(kArea);

    for (unsigned i = 0; i < kEntryCount; ++i) {
        const uint32_t entry = Entry(i);

        if (Related(group, entry) && Memory::Read8(entry + kEntryArea) == area)
            return true;
    }

    return false;
}

// A later group of the class (or, for an entry, another class) with the
// member's id is done: the original walks the groups after this one but
// numbers them from the member's index `from`, so it reads past the list at
// the last groups.
void MarkLater(uint8_t (&done)[kGroupCount], uint32_t group, unsigned from, uint8_t id, bool sameClass) {
    const uint8_t groupClass = Memory::Read8(group + kGroupClass);
    uint32_t other = group + kGroupBytes;

    for (unsigned i = from + 1; i < kGroupCount; ++i, other += kGroupBytes) {
        if ((Memory::Read8(other + kGroupClass) == groupClass) == sameClass && Memory::Read8(other + kGroupId) == id) {
            done[i] = 1;
            return;
        }
    }
}

void Match(CpuContext* cpu) {
    GuestFrame frame(cpu, 0xD0);
    GuestCall game{cpu};
    const uint32_t members = frame[0x18]; // the related enemies, for 0x803446BC
    const uint32_t enemies = Enemies();
    const int32_t enemyCount = EnemyCount();
    uint8_t done[kGroupCount] = {};

    for (unsigned g = 0; g < kGroupCount; ++g) {
        const uint32_t group = kGroups + g * kGroupBytes;

        if (done[g]) {
            const uint8_t groupClass = Memory::Read8(group + kGroupClass), family = Memory::Read8(group + kGroupFamily);

            for (unsigned i = 0; i < g; ++i) {
                const uint32_t other = kGroups + i * kGroupBytes;

                if (Memory::Read8(other + kGroupClass) == groupClass && family != 0xFF && Memory::Read8(other + kGroupFamily) == family)
                    Memory::Write8(group + kGroupRelated, Memory::Read8(other + kGroupRelated));
            }

            continue;
        }

        done[g] = 1;

        if (!Memory::Read8(group + kGroupClass) || Memory::Read8(group + kGroupState) == 1)
            continue;

        Memory::Write8(group + kGroupRelated, 0);
        uint32_t enemy = enemies;

        for (int32_t i = 0; i < enemyCount; ++i, enemy += kEnemyBytes) {
            if (!EnemyRelated(group, enemy))
                continue;

            const uint8_t number = Memory::Read8(group + kGroupRelated);
            Memory::Write8(enemy + kEnemyNumber, number);
            Memory::Write32(members + 4 * number, enemy);
            Memory::Write8(group + kGroupRelated, uint8_t(number + 1));
            const uint8_t id = Memory::Read8(enemy + kEnemyId);

            if (id != Memory::Read8(group + kGroupId))
                MarkLater(done, group, unsigned(i), id, true);
        }

        if (const uint8_t related = Memory::Read8(group + kGroupRelated)) {
            const uint8_t family = Memory::Read8(group + kGroupFamily);
            const uint32_t leader = family == 0xFF ? group : FindGroup(Memory::Read8(group + kGroupClass), family);
            game.Call<0x803446BC>({leader, members, related});
        }

        for (unsigned i = 0; i < kEntryCount; ++i) {
            const uint32_t entry = Entry(i);

            if (!Related(group, entry))
                continue;

            Memory::Write8(group + kGroupRelated, uint8_t(Memory::Read8(group + kGroupRelated) + 1));
            const uint8_t id = Memory::Read8(entry + kEntryId);

            if (id != Memory::Read8(group + kGroupId))
                MarkLater(done, group, i, id, false);
        }
    }
}

void KindClassPort(CpuContext* cpu) {
    cpu->gpr[3] = KindClass(uint8_t(cpu->gpr[3]), uint8_t(cpu->gpr[4]));
}

void FindGroupPort(CpuContext* cpu) {
    cpu->gpr[3] = FindGroup(uint8_t(cpu->gpr[3]), uint8_t(cpu->gpr[4]));
}

void ClassFlagsPort(CpuContext* cpu) {
    cpu->gpr[3] = ClassFlags(cpu->gpr[3]);
}

void HoldsPort(CpuContext* cpu) {
    cpu->gpr[3] = Holds(cpu->gpr[3], cpu->gpr[4]);
}

void RelatedPort(CpuContext* cpu) {
    cpu->gpr[3] = Related(cpu->gpr[3], cpu->gpr[4]);
}

void EnemyHoldsPort(CpuContext* cpu) {
    cpu->gpr[3] = EnemyHolds(cpu->gpr[3], cpu->gpr[4]);
}

void EnemyRelatedPort(CpuContext* cpu) {
    cpu->gpr[3] = EnemyRelated(cpu->gpr[3], cpu->gpr[4]);
}

void CountPort(CpuContext* cpu) {
    Count(cpu->gpr[3]);
}

void AwayPort(CpuContext* cpu) {
    cpu->gpr[3] = Away(cpu->gpr[3]);
}

void Checked(CpuContext* cpu, const char* name, void (*original)(CpuContext*), void (*port)(CpuContext*)) {
    if (PortCheck::Enabled(name))
        PortCheck::RunPure(cpu, name, PortCheck::Returns::Gpr3, original, port);
    else
        port(cpu);
}

} // namespace

extern "C" void func_80343A38(CpuContext* cpu);
extern "C" void MH3_CreatureKindClass(CpuContext* cpu) {
    Checked(cpu, "CreatureKindClass", func_80343A38, KindClassPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80343A38, MH3_CreatureKindClass, "CreatureKindClass");

extern "C" void func_80343948(CpuContext* cpu);
extern "C" void MH3_CreatureGroupFind(CpuContext* cpu) {
    Checked(cpu, "CreatureGroupFind", func_80343948, FindGroupPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80343948, MH3_CreatureGroupFind, "CreatureGroupFind");

extern "C" void func_80343AD0(CpuContext* cpu);
extern "C" void MH3_CreatureGroupFlags(CpuContext* cpu) {
    Checked(cpu, "CreatureGroupFlags", func_80343AD0, ClassFlagsPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80343AD0, MH3_CreatureGroupFlags, "CreatureGroupFlags");

extern "C" void func_80344260(CpuContext* cpu);
extern "C" void MH3_CreatureGroupHolds(CpuContext* cpu) {
    Checked(cpu, "CreatureGroupHolds", func_80344260, HoldsPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80344260, MH3_CreatureGroupHolds, "CreatureGroupHolds");

extern "C" void func_803444C4(CpuContext* cpu);
extern "C" void MH3_CreatureGroupRelated(CpuContext* cpu) {
    Checked(cpu, "CreatureGroupRelated", func_803444C4, RelatedPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x803444C4, MH3_CreatureGroupRelated, "CreatureGroupRelated");

extern "C" void func_803441C8(CpuContext* cpu);
extern "C" void MH3_CreatureEnemyHolds(CpuContext* cpu) {
    Checked(cpu, "CreatureEnemyHolds", func_803441C8, EnemyHoldsPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x803441C8, MH3_CreatureEnemyHolds, "CreatureEnemyHolds");

extern "C" void func_803443B4(CpuContext* cpu);
extern "C" void MH3_CreatureEnemyRelated(CpuContext* cpu) {
    Checked(cpu, "CreatureEnemyRelated", func_803443B4, EnemyRelatedPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x803443B4, MH3_CreatureEnemyRelated, "CreatureEnemyRelated");

extern "C" void func_803442EC(CpuContext* cpu);
extern "C" void MH3_CreatureGroupCount(CpuContext* cpu) {
    PortCheck::Call(cpu, "CreatureGroupCount", func_803442EC, CountPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x803442EC, MH3_CreatureGroupCount, "CreatureGroupCount");

extern "C" void func_803445C8(CpuContext* cpu);
extern "C" void MH3_CreatureGroupAway(CpuContext* cpu) {
    Checked(cpu, "CreatureGroupAway", func_803445C8, AwayPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x803445C8, MH3_CreatureGroupAway, "CreatureGroupAway");

extern "C" void func_80344F00(CpuContext* cpu);
extern "C" void MH3_CreatureGroupsMatch(CpuContext* cpu) {
    PortCheck::Call(cpu, "CreatureGroupsMatch", func_80344F00, Match);
}
REGISTER_NATIVE_FUNCTION_AS(0x80344F00, MH3_CreatureGroupsMatch, "CreatureGroupsMatch");
