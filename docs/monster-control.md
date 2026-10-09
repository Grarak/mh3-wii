# Monster control

Development builds can put any large monster in the arena pit and drive it:
start a chosen action (an attack, a roar, a turn) when you want, repeat it,
freeze the monster, turn it and place it. Use it to look at one attack at a
time, at fixed and variable step, with the traces on.

## The arena pit with any monster

`--quest 9004 --quest-monster NAME|ID` replaces the arena quest's large
monster (quest 9004, the arena pit's Rathian) with the named one. The name
is one of the launcher's monster names, compared without case (`Deviljho`),
or the number. The quest's objectives follow the new monster
(`MH3 quest 9004: large monster 1 replaced by 7`). `--spawn X,Y,Z` and
`--no-damage` help keep the hunter alive and out of the way.

```
MH3_SAVE_DIR=saves MH3_ARGS="--quest 9004 --quest-monster Deviljho --no-damage" \
  MH3_MONSTER_CONTROL=$PWD/logs/jho.ctl MH3_TRACE=actions,hit-shapes \
  scripts/dev/run-heartbeat.sh jho 300
```

The arena quests (`--list arena-quests`) use two maps: the land arena (map
9: 9000-9002, 9004, 9006, 9007, 9009) and the **water arena** (map 10: 9003
Royal Ludroth, 9005 Lagiacrus, 9008 Gobul and Lagiacrus), a pool for the
underwater monsters and for water rendering and animation
(`--quest 9005 --quest-monster Gobul`).

Small monsters: `--quest-small NAME|ID` makes every small monster spawn of
the quest that monster (the area spawn tables at +0x4B8; only named
monsters other than the quest's large ones change). The arenas' own spawns
are reinforcements a large monster calls in, so use a field quest:
`--quest 1028 --area 1 --quest-small Jaggi` fills Moga Woods area 1 with
Jaggi (51 spawns). Small monsters are monster works like the large ones and
start their actions the same way (`kind 10`, `list`).

## The sweep

`scripts/dev/monster_sweep.py NAME --quest Q [--area N] [--monster M]
[--small S] --kind K` checks one monster's actions for per-step tickers:
it lets the monster fight with its AI on (learning every action it starts
itself), then forces each action from the same place and yaw with `ai off`,
once at the fixed 1/30 s step and once at the variable step side by side,
and compares each action's game time, yaw turned and distance moved
(`logs/runs/NAME-sweep.txt`). A flagged action is then watched
(`scripts/dev/gdbwatch.sh` with the field that differs) to name its writer.

```
scripts/dev/monster_sweep.py sw-jho --quest 9004 --monster Deviljho --kind 7
scripts/dev/monster_sweep.py sw-jaggi --quest 1028 --area 1 --small Jaggi --kind 10
```

## Actions

A monster's action is a pair of bytes in its work: the **mode** (+0x1E5) and
the action within the mode, the **sub** (+0x1E6). The action's routine state
is +5..+7 and starts at 0. Written `mode.sub` below.

- `0x80128624 (work, mode, sub, how)` starts an action. The species' remap
  handler may change the pair first. Then the pair is stored (0x80128604) and
  the species' start handler runs (0x801283A4). `how` 0 is the AI's choice;
  online it is also sent to the other players.
- `get_enemy_data(work) + 0x10` is the species' handler table
  (`0x80126520 (work, index)` reads it):

  | Index | Handler |
  | --- | --- |
  | 0 | set up, at spawn |
  | 4 | remap an action before it starts |
  | 5 | start an action |
  | 6 | the step: `0x80138BC4` calls it every logic step while the monster is active; it runs the action and picks the next one when it ends |
  | 8 | after the step |
  | 0xC | the spawn place |

- `0x8013C4BC (work, pair*)` is the AI's pick. `{0, 0xFF}` picks the waiting
  action that fits the monster's state (0.0, 0.1, 0.2, ...).
- `0x80127FAC (work)` ends an action: back to a waiting action (how 2),
  where the AI picks the next. Deviljho waits in 0.1 or 0.2 and attacks
  with modes 2 and 7; 7.1 is its tail spin.
- `MonsterStep` (`0x80137980`) runs first each step and marks the monster
  active (+0x1DE). The freeze cheat and the `freeze` command hold it there.
- The action repeater cheat sets +5 to 0, which restarts the action running.

`MH3_TRACE=actions` prints every action start:

```
MH3 monster action: work 9014b940 kind 7 0.1 -> 7.1 (asked 7.1, how 0, lr 8013c5c8) tick 682 time 682.41 scale 0.763 yaw 34567 at 120.5 0.0 -880.2
```

That is the action before and after, the pair asked for (the remap handler
can change it), how, the caller, the step counter, the game time in ticks,
the step's length and the yaw (65536 a turn) and position the last
action left: the yaw, distance and time an action took are the differences
to its next line. `control` marks starts the control file
made. Once per kind it also prints the species' handler table
(`MH3 monster handlers`).

## The control file

`MH3_MONSTER_CONTROL=PATH` names a file of commands, one a line. The game
reads it about every third of a second and runs each new complete line once,
so append to it while the game runs (`echo "act 2 5" >> logs/jho.ctl`).
A file written anew from the start runs from its first line again. Each
command run is echoed (`MH3 monster control: act 2 5`).

| Command | Effect |
| --- | --- |
| `target N` | the monster work index the commands act on (default 0, the first large monster) |
| `kind K` | target the first monster of kind K (work +3, the monster list's id: 10 Jaggi) |
| `act MODE SUB` | start that action now, as the AI would |
| `every TICKS MODE SUB` | start it now and every TICKS ticks of game time (30 a second); `every off` stops |
| `ai off`, `ai on` | off: the monster picks no action of its own (the starts of its AI's pick, 0x8013C4BC, are dropped); it waits until a command starts one, and an action's own follow-ups and its end still run |
| `freeze on`, `freeze off` | hold the monster's step: it stands still and picks nothing |
| `face DEGREES` | set its yaw |
| `goto X Y Z` | place it |
| `hunter save`, `hunter back`, `hunter X Y Z` | remember the local hunter's place, put it back there, or place it (attacks aim at the hunter, so a sweep puts it back before each) |
| `wait TICKS` | run the next commands TICKS ticks of game time later (a whole sequence can be written before the start) |
| `list` | print each monster's index, work, kind, action, state, position and yaw |
| `# ...` | a comment |

The action pairs a monster uses come from the trace: run the pit with
`MH3_TRACE=actions`, let it fight, and read the `MH3 monster action` lines.
`MH3_TRACE=hit-shapes` names the body parts each attack hits with.

The integrated cheats work beside it (`--cheat NAME`): *Monster speed
modifier*, *Monster can't see you*, *Freeze/unfreeze monsters*, *Action
repeater for targeted monster*, *Pass through monsters (on/off)*.
