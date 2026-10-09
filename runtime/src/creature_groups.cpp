// Small-creature group predicates, RMHP08, decompiled with Ghidra. Every step
// 0x80344F00 matches the ten creature groups (0x806C1520, 0x3C bytes each)
// against the enemies and the 128 entries at 0x806A7960 (0x44 bytes each),
// calling 0x803444C4 for each group and entry: in the Tundra about 1.6% of the
// devkit's game thread, mostly in the calls themselves. The predicates and
// their helpers run natively here:
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
//
// MH3_PORT_CHECK=CreatureKindClass,CreatureGroupFind,CreatureGroupFlags,
// CreatureGroupHolds,CreatureGroupRelated checks each call against the original.
#include "abi_bridge.h"
#include "memory.h"
#include "port_check.h"

#include <cstdint>

namespace {

constexpr uint32_t kGroups = 0x806C1520; // 10 of 0x3C bytes
constexpr uint32_t kGroupBytes = 0x3C;
constexpr unsigned kGroupCount = 10;
constexpr uint32_t kClassTables = 0x805E9928; // u32 per class: its table

enum : uint32_t { // group
    kGroupClass = 0x00,
    kGroupId = 0x01,
    kGroupLink = 0x14,
    kGroupFamily = 0x17,
};

enum : uint32_t { // entry
    kEntrySet = 0x00,
    kEntryKind = 0x02,
    kEntryVariant = 0x03,
    kEntryGone = 0x08,
    kEntryId = 0x09,
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

bool Related(uint32_t group, uint32_t entry) {
    if (!Live(entry))
        return false;

    const uint8_t groupClass = Memory::Read8(group + kGroupClass), id = Memory::Read8(entry + kEntryId);

    if (groupClass == EntryClass(entry) && id == Memory::Read8(group + kGroupId) && id != 0xFF)
        return true;

    const uint32_t other = FindGroup(groupClass, id);

    if (!other)
        return false;

    const uint8_t family = Memory::Read8(group + kGroupFamily), otherFamily = Memory::Read8(other + kGroupFamily);

    if (family == otherFamily && family != 0xFF && otherFamily != 0xFF)
        return true;

    return (ClassFlags(group) & 8) && Memory::Read8(group + kGroupLink) == Memory::Read8(other + kGroupLink);
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
