// MH3's quest files, read from the disc on the host: listing and lookup for
// the --list / --quest options (development builds).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace QuestCatalog {

// The disc's quest sets: 05/quest00 (village), 05/quest01 (online, Port
// Tanzia), 05/quest03 (arena).
enum class Set {
    Village,
    Online,
    Arena
};

struct Objective {
    uint32_t type;   // 1 hunt, 0x81 capture, 0x101/0x8101 slay, 2 deliver, 0x404 repel, ...
    uint16_t target; // monster or item id
    uint16_t count;
};

struct Quest {
    uint16_t id;
    uint8_t stars, map;
    std::string name, goal;
    std::vector<Objective> objectives;
    std::vector<uint8_t> largeMonsters; // monster ids
    std::vector<uint8_t> smallMonsters; // monster ids spawned in the areas
};

std::vector<Quest> Load(Set set);
std::vector<std::string> MonsterNames(); // by monster id (0: none)
std::string MapName(uint8_t map);
void Print(Set set);

} // namespace QuestCatalog
