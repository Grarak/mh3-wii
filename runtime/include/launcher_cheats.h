#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

// Native gameplay cheats, available in release builds: the launcher's two
// toggles and the RMHE08 Gecko code lists ported to RMHP08 (the table in
// runtime/src/cheats.cpp). No PPC instruction patches: executable ROM cheats
// cannot change statically translated code, so each code is native code (a
// port, a hook in a port or a write to the game's data) switched by its
// toggle. Toggles apply to the current launch.
namespace LauncherCheats {

void SetNoStaminaDrain(bool enabled);

struct Option {
    const char* label;
    int32_t value;
};

// A numeric value instead of options: min..max in steps.
struct Range {
    int32_t min = 0, max = 0, step = 1;
};

struct Cheat {
    const char* group;  // the code list's category tag
    const char* name;
    const char* detail; // what it does, and its buttons
    std::span<const Option> options;
    Range range;        // when there are no options and max > min
    uint8_t defaultOption = 0;
};

std::span<const Cheat> List();

// The groups in menu order.
std::span<const char* const> Groups();
bool Enabled(size_t cheat);
void Enable(size_t cheat, bool enabled);

// Choices of a cheat's value: its options or its range's steps (0: a plain
// toggle).
size_t Choices(size_t cheat);
size_t Selected(size_t cheat);
void Select(size_t cheat, size_t choice);
std::string ValueLabel(size_t cheat);
size_t EnabledCount();

// Turns a cheat on by name, with a value by option label or number (empty:
// its default). Returns false for an unknown name or value.
bool EnableByName(std::string_view name, std::string_view value);

} // namespace LauncherCheats
