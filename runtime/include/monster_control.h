// Monster control for debugging (development builds; docs/monster-control.md):
// a trace of the actions monsters start, and commands that start chosen
// actions, repeat them, freeze, turn and place a monster.
#pragma once

#include <cstdint>

struct CpuContext;

namespace MonsterControl {

// Game thread, once a logic step before the tasks run: reads new commands
// (MH3_MONSTER_CONTROL's file) and runs the repeats due.
void Step(CpuContext* ctx);

// Whether `work`'s step is held (the `freeze` command).
bool Frozen(uint32_t work);

} // namespace MonsterControl
