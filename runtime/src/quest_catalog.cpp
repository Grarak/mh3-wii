// MH3's quest files (05/questNN_eng.bin), read on the host.
//
// A quest file: "OMOM", u32 count, then count {u32 offset, u32 size} of the
// quest entries (the game loads them as is, 0x803AD264):
//   +0x00 name (0x2C bytes), +0x2C u16 id, +0x2E goal text,
//   +0x8A u8 stars, +0x8B u8 map,
//   +0x314 up to 3 large monsters of 8 bytes (+0 monster id, +3 1 = present),
//   +0x330 3 objectives {u32 type, u16 target, u16 count},
//   +0x4B8 the area spawn tables: u32 count, u32 header size, u32, then
//   {offset, size} pairs from +0x4B8 of 0x30-byte spawns (+0 u32 monster id).
// Monster names are the game's list in 00/data00_eng.bin (after "NO_DATA").
#include "quest_catalog.h"
#include "fatal.h"
#include "disc_image.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <set>
#include <span>
#include <stdexcept>

namespace QuestCatalog {
namespace {

uint32_t U32(const std::vector<uint8_t>& d, size_t o) {
    if (o + 4 > d.size())
        MH3_THROW(std::runtime_error("Quest file truncated"));

    return uint32_t(d[o]) << 24 | uint32_t(d[o + 1]) << 16 | uint32_t(d[o + 2]) << 8 | d[o + 3];
}

uint16_t U16(const std::vector<uint8_t>& d, size_t o) {
    return uint16_t(U32(d, o) >> 16);
}

std::string Text(const uint8_t* p, size_t max) {
    std::string s(reinterpret_cast<const char*>(p), strnlen(reinterpret_cast<const char*>(p), max));
    std::replace(s.begin(), s.end(), '\n', ' ');
    return s;
}

enum : size_t {
    kName = 0x00,
    kId = 0x2C,
    kGoal = 0x2E,
    kGoalEnd = 0x88,
    kStars = 0x8A,
    kMap = 0x8B,
    kLarge = 0x314,
    kObjectives = 0x330,
    kSpawnTables = 0x4B8,
    kSpawnSize = 0x30,
};

const char* FileName(Set set) {
    switch (set) {
    case Set::Village:
        return "05/quest00_eng.bin";
    case Set::Online:
        return "05/quest01_eng.bin";
    case Set::Arena:
        return "05/quest03_eng.bin";
    }

    return "";
}

} // namespace

std::vector<Quest> Load(Set set) {
    const auto data = GameDiscImage().ReadFile(FileName(set));

    if (data.size() < 8 || std::string(data.begin(), data.begin() + 4) != "OMOM")
        MH3_THROW(std::runtime_error(std::string("Not a quest file: ") + FileName(set)));

    std::vector<Quest> quests;
    const uint32_t count = U32(data, 4);

    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t offset = U32(data, 8 + 8 * i), size = U32(data, 12 + 8 * i);

        if (uint64_t(offset) + size > data.size() || size < kSpawnTables)
            MH3_THROW(std::runtime_error("Bad quest entry"));

        const std::vector<uint8_t> e(data.begin() + offset, data.begin() + offset + size);
        Quest q;
        q.id = U16(e, kId);
        q.name = Text(&e[kName], kId - kName);
        q.goal = Text(&e[kGoal], kGoalEnd - kGoal);
        q.stars = e[kStars];
        q.map = e[kMap];

        for (size_t k = 0; k < 3; ++k) {
            const Objective o{U32(e, kObjectives + 8 * k), U16(e, kObjectives + 8 * k + 4), U16(e, kObjectives + 8 * k + 6)};

            if (o.type || o.target)
                q.objectives.push_back(o);

            const uint8_t monster = e[kLarge + 8 * k];

            if (e[kLarge + 8 * k + 3] == 1 && monster)
                q.largeMonsters.push_back(monster);
        }

        std::set<uint8_t> small;

        // Some quests (the Ceadeus and Alatreon fights) have no spawn tables.
        const uint32_t tables = e.size() >= kSpawnTables + 12 ? U32(e, kSpawnTables) : 0;

        for (uint32_t t = 0; t < tables; ++t) {
            const size_t at = kSpawnTables + 12 + 8 * t;

            if (at + 8 > e.size())
                break;

            const uint32_t spawns = U32(e, at), bytes = U32(e, at + 4);

            for (uint32_t b = 0; b + kSpawnSize <= bytes; b += kSpawnSize) {
                const size_t s = kSpawnTables + spawns + b;

                if (s + 4 > e.size())
                    break;

                const uint32_t monster = U32(e, s);

                if (monster > 0 && monster < 0x40)
                    small.insert(uint8_t(monster));
            }
        }

        for (uint8_t m : q.largeMonsters)
            small.erase(m);

        q.smallMonsters.assign(small.begin(), small.end());
        quests.push_back(std::move(q));
    }

    return quests;
}

std::vector<std::string> MonsterNames() {
    const auto data = GameDiscImage().ReadFile("00/data00_eng.bin");
    const std::string text(data.begin(), data.end());
    size_t at = text.find(std::string("NO_DATA\0", 8));

    if (at == std::string::npos)
        MH3_THROW(std::runtime_error("Monster name list not found"));

    std::vector<std::string> names{""};
    at += 8;

    while (at < text.size()) {
        while (at < text.size() && text[at] == '\0')
            ++at;

        const size_t end = text.find('\0', at);
        const std::string name = text.substr(at, end - at);

        if (name == "BrdWyv" || end == std::string::npos)
            break; // the class names follow
        names.push_back(name);
        at = end;
    }

    return names;
}

std::string MapName(uint8_t map) {
    switch (map) {
    case 1:
        return "Deserted Island (Moga Woods)";
    case 2:
        return "Sandy Plains";
    case 3:
        return "Flooded Forest";
    case 4:
        return "Tundra";
    case 5:
        return "Volcano";
    case 6:
        return "Great Desert";
    case 7:
        return "Ceadeus's lair";
    case 9:
        return "Land Arena";
    case 10:
        return "Water Arena";
    case 11:
        return "Sacred Pinnacle";
    }

    return "map " + std::to_string(map);
}

namespace {

// The numbers the maps show for their areas, by the game's area index
// (get_now_areano 0x802AFCE8): 0 the base camp, -1 an area that has none.
// The game keeps no such table. A map's art (04/map_mXXd_eng.brres) has no
// numbers in it; the minimap draws them at places on the art. These were
// read off the minimap in every area (--spawn exit:tour and --area runs,
// 2026-10-07). The small maps number them in the game's order.
constexpr int8_t kDesertedIsland[] = {0, 1, 3, 2, 7, 5, 9, 11, 12, 6, 8, 4, 10};
constexpr int8_t kSandyPlains[] = {0, 1, 2, 3, 4, 5, 8, 9, 10, 7, 11, 6};
constexpr int8_t kFloodedForest[] = {0, 1, 3, 4, 5, 2, 8, 7, 6, 9, 10};
constexpr int8_t kTundra[] = {0, 1, 3, 2, 5, 7, 4, 6, -1, -1}; // 8: the path between 7 and 6; 9: off the art beside 7
constexpr int8_t kVolcano[] = {0, 2, 1, 3, 5, 6, 4, 7, 8, 10, 9};
constexpr int8_t kGreatDesert[] = {0, 1, 2};
constexpr int8_t kCeadeusLair[] = {0, 1, 2, 3};
constexpr int8_t kSacredPinnacle[] = {-1, 1}; // one area (stage/m11d/m11a01d)

// Empty for a map without numbers (the arenas: 0 the land arena, 1 the
// water arena) or not known here: its areas keep the game's index.
std::span<const int8_t> AreaNumbers(uint8_t map) {
    switch (map) {
    case 1:
        return kDesertedIsland;
    case 2:
        return kSandyPlains;
    case 3:
        return kFloodedForest;
    case 4:
        return kTundra;
    case 5:
        return kVolcano;
    case 6:
        return kGreatDesert;
    case 7:
        return kCeadeusLair;
    case 11:
        return kSacredPinnacle;
    }

    return {};
}

} // namespace

int AreaIndex(uint8_t map, int number) {
    const auto numbers = AreaNumbers(map);

    if (numbers.empty())
        return number;

    const auto it = std::find(numbers.begin(), numbers.end(), number);
    return number >= 0 && it != numbers.end() ? int(it - numbers.begin()) : -1;
}

int AreaNumber(uint8_t map, int index) {
    const auto numbers = AreaNumbers(map);

    if (numbers.empty())
        return index;

    return index >= 0 && size_t(index) < numbers.size() ? numbers[size_t(index)] : -1;
}

std::string AreaName(uint8_t map, int index) {
    const auto numbers = AreaNumbers(map);
    const int number = AreaNumber(map, index);

    if (numbers.empty())
        return "area " + std::to_string(index);

    const std::string game = number == index ? "" : " (the game's " + std::to_string(index) + ")";

    if (number == 0)
        return "the base camp" + game;

    return (number < 0 ? std::string("an area without a number") : "area " + std::to_string(number)) + game;
}

std::string AreaNumbersText(uint8_t map) {
    const auto numbers = AreaNumbers(map);

    if (numbers.empty())
        return "its areas have no numbers";

    // The numbers run from 1 without gaps.
    const int last = *std::max_element(numbers.begin(), numbers.end());
    const bool camp = std::find(numbers.begin(), numbers.end(), 0) != numbers.end();
    const std::string areas = last == 1 ? "1" : "1 to " + std::to_string(last);
    return camp ? "0 the base camp, " + areas : areas;
}

void Print(Set set) {
    const auto names = MonsterNames();
    const auto name = [&](uint8_t id) {
        return id < names.size() ? names[id] : "monster " + std::to_string(id);
    };

    const auto join = [&](const std::vector<uint8_t>& ids) {
        std::string s;

        for (uint8_t id : ids)
            s += (s.empty() ? "" : ", ") + name(id);

        return s.empty() ? std::string("-") : s;
    };

    std::printf("%-6s %-5s %-29s %-38s %-34s %s\n", "id", "stars", "map", "name", "large monsters", "small monsters");

    for (const Quest& q : Load(set))
        std::printf("%-6u %-5u %-29s %-38s %-34s %s\n    goal: %s\n", q.id, q.stars, MapName(q.map).c_str(), q.name.c_str(), join(q.largeMonsters).c_str(), join(q.smallMonsters).c_str(),
                    q.goal.c_str());
}

} // namespace QuestCatalog
