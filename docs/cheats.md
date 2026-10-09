# Cheats as native code

## What the original does

On a Wii, cheat codes for MH3 are Gecko codes, run by a code handler that
a loader patches into the game. Each frame the handler performs the code
list: writes to RAM (`00`-`08` type codes, with conditions), and
instruction patches or branches to code of the author's own that change
how a game function behaves. The published lists are for the US disc
(`RMHE08`).

## What the port does instead

No Gecko code is interpreted at run time: the code is statically
translated, so an instruction patch could not take effect. Each code is
native code instead ([launcher](vita-launcher.md#custom-quests-and-cheats)):

- **Data codes** (`runtime/src/cheat_data.cpp`): the RAM writes and
  conditions as C, at the European addresses (heap data where the US disc
  has it; static data moved with the binary), once per logic step, as the
  handler did once per frame.
- **Hooks in native ports** for health, stamina, timers, the quest clock and
  the jump and run actions.
- **Wrappers** registered at a translated function's address that change its
  arguments, a constant it loads, or its result, then call the original
  (`runtime/src/cheat_hunter.cpp`, `cheat_items.cpp`, `cheat_monsters.cpp`,
  `cheat_movement.cpp`, `cheat_quest.cpp`).
- **The table** (`runtime/src/cheats.cpp`): 257 codes from the code lists,
  grouped by the lists' author tags, plus four entries of the launcher's own
  (No damage, No action stamina drain, All items in the item box, All
  equipment in the equipment box). Codes that only work online are left out,
  and codes that did the same thing are merged. The Vita launcher's Cheats
  page and `--cheat <name>[=<value>]` turn them on.

The cheat lists themselves are not part of the repository; only the code's
effect, the entries' names and the authors' tags are.

## Why

Instruction patches assume an interpreter or JIT that executes the patched
words. A static recompilation needs each change in source form. Ported this
way, cheats also work with the variable step and on every host.

## Example

`runtime/src/cheat_hunter.cpp`, a code that made a health check fail:

```cpp
// 0x802747F8 (plw): 1 when the hunter can leave the area. Can't leave area
// (US 0x802747DC, EU 0x80274840: the health check made to fail), switched by
// its buttons, says 0.
extern "C" void func_802747F8(CpuContext* ctx);

extern "C" void MH3_CheatLeaveArea(CpuContext* ctx) {
    if (Toggled(Id::CantLeaveArea)) {
        ctx->gpr[3] = 0;
        return;
    }

    func_802747F8(ctx);
}

REGISTER_NATIVE_FUNCTION_AS(0x802747F8, MH3_CheatLeaveArea, "CheatLeaveArea");
```

## Applying it to another Wii game

Generic: the three forms (per-step data writes, hooks in ports, wrappers
around translated functions) and the table and launcher UI.

Game-specific: each code. Translate its addresses to the disc revision
being ported (here US to EU: heap addresses matched, static data moved with
the binary), find the function an instruction patch lands in, and express
the patch as a change of that function's inputs or outputs. Respect the
lists' authorship: credit the authors and do not copy the lists into the
repository.
