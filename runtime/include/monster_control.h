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

// The return address of the AI's pick (0x8013C4BC) into EnemySetAction.
inline constexpr uint32_t kAiPickReturn = 0x8013C5C8;

// EnemySetAction (enemy_actions.cpp), development builds: whether to drop an
// action start (`ai off`: the AI's picks for the target), and the trace of
// one that ran (MH3_TRACE=actions): the monster as the start found it, then
// the action it started. `caller` is the original's return address.
struct ActionStart {
    uint32_t work, caller;
    uint8_t fromMode, fromSub, mode, sub, how;
    int32_t yaw;
    float x, y, z;
};

bool Blocked(uint32_t work, uint32_t caller);
ActionStart Begin(uint32_t work, uint8_t mode, uint8_t sub, uint8_t how, uint32_t caller);
void NoteAction(CpuContext* ctx, const ActionStart& start);

} // namespace MonsterControl
