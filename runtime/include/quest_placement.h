// Large monsters on maps the game has no placement for: a custom quest (or
// --quest-monster) may put any monster on any map. 803AE9F4 (the quest's
// monster slots) and 803B17E0 (placing them again) look the monster's spawn
// point up in its list (0x805F7E60, one per monster id: 8-byte entries {map
// key, 0, 0, 0, records}, ending in key 0xFF; the map's key at 0x805F8058 +
// map * 2), and a monster without an entry for the map never appears. Apply
// gives each requested monster a new list: its own entries plus one for the
// map whose records (48 bytes each, the slot's +1 byte picks one) are a copy
// of another monster's records there, and a monster whose own records there
// lack the requested one (count 0) a copy of them with it filled.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace QuestPlacement {
struct Request {
    uint8_t monster, map, record;
};

inline std::vector<Request>& Requests() {
    static std::vector<Request> requests;
    return requests;
}

// `monster` on the quest's map `map` (the quest's +0x8B byte), from its spawn
// record `record` (the slot's +1 byte).
inline void Add(uint8_t monster, uint8_t map, uint8_t record = 0) {
    Requests().push_back({monster, map, record});
}

// The monster heap layout 4 (a quest's +0x390 4; 0x805A23F8 + 3 * 6), when
// set: six slot classes, -1 for none. No disc quest uses it.
inline std::optional<std::array<int8_t, 6>>& Layout() {
    static std::optional<std::array<int8_t, 6>> layout;
    return layout;
}

// The guest bytes Apply may use, reserved at boot (main.cpp).
inline constexpr uint32_t kBytes = 0x800;

// Writes the lists the requests need into [base, base + kBytes) and points
// the monsters at them, and the layout; called once the game's data sections
// are loaded.
void Apply(uint32_t base);
} // namespace QuestPlacement
