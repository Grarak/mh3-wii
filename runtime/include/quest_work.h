// The quest work, the running quest's entry and its flags, RMHP08: the
// fields the quest ports (quest_flags.cpp, quest_setup.cpp, quest_spawns.cpp,
// quest_step.cpp, quest_result.cpp) share.
//
// The quest work (0x806C7CD8, also at r13 - 0x41E0 = 0x80797100 while a quest
// runs) holds the running quest: +0x3C its entry, a copy of the quest file's
// entry. The entry's +0x310 is a word of flags that the readers at
// 0x803B4BD8..0x803B53F4 test (QuestFlag); +0x314 its three large monster
// slots of 8 bytes: u8 monster, u8 spawn record, u8 spawn mode (3: held in
// the summon pool, quest_spawns.cpp), u8 count, u8 +4, u8 +5, u8 size, u8
// level.
#pragma once

#include "memory.h"

#include <cstdint>

namespace QuestWork {

constexpr uint32_t kPointer = 0x80797100; // r13 - 0x41E0: the quest work while a quest runs
constexpr uint32_t kWork = 0x806C7CD8;
constexpr uint32_t kMoveWorks = 0x8065AB04; // the move works: +0x10 by kind
constexpr uint32_t kNetState = 0x807972A8;  // r13 - 0x4038

// Quest work fields.
enum : uint32_t {
    kEntry = 0x3C,          // the running quest's entry
    kObjectiveMode = 0x93,  // u8: which objectives end the quest (QuestStep)
    kObjectives = 0x94,     // three live objectives of 0x60 bytes: u32 flags, u16 target, u16 count
    kMonsters = 0x2D4,      // u8 x3: the slots' monsters
    kPlaced = 0x308,        // s8: the large monsters placed (the next slot)
    kSlots = 0x319,         // the monster slots (8 bytes each, from the entry's +0x314)
    kSpawnIds = 0x46C,      // s16 per slot: its spawn id (0xFFFF none)
    kQuestActive = 0x675C,  // u32: 1 once the quest is set up
    kExtraSlot = 0x6760,    // 8 bytes: the extra monster (the third slot with flag 0x100, an intruder)
    kExtraOn = 0x6975,      // u8: the extra monster's state machine runs (0x803B0FFC)
    kExtraLoaded = 0x6976,  // u8
    kWaiting = 0x6977,      // u8: the timed and marathon spawns' signal (0: the last one is gone)
    kSpawnState = 0x6978,   // u8: the state of 0x803B0E28 / 0x803B0FFC / 0x803B17E0
    kSpawnFrames = 0x697C,  // s32: frames counted towards the timed spawn
};

// Move work kinds.
enum : uint32_t {
    kField = 0,   // the field work: +0xDC the quest work, +0xE9 the map key, +0xEA the area
    kHunters = 2, // 0xB20 bytes each
    kEnemies = 3, // 0xB18 bytes each
};

inline uint32_t Work() {
    return Memory::Read32(kPointer);
}

// 0x800CFAF4 get_move_work_adrs(kind), inlined by the translator: 0 past 6.
inline uint32_t MoveWork(uint32_t kind) {
    const uint32_t table = Memory::Read32(kMoveWorks);
    return table && (kind & 0xFF) <= 6 ? Memory::Read32(table + (kind & 0xFF) * 4 + 0x10) : 0;
}

// 0x803B33F8 (inlined): the running quest's entry.
inline uint32_t Entry() {
    return Memory::Read32(kWork + kEntry);
}

// 0x8045169C (inlined): an online session (the net state's +0x11 is 7).
inline bool Online() {
    return Memory::Read8(Memory::Read32(kNetState) + 0x11) == 7;
}

// 0x80451720 (inlined): this console hosts the session (+0x44 is 1).
inline bool Host() {
    return Memory::Read32(Memory::Read32(kNetState) + 0x44) == 1;
}

} // namespace QuestWork
