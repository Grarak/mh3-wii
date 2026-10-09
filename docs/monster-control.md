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

The arena quests (`--list arena-quests`) use two arenas: the land arena
(quest map 9: 9000-9002, 9004, 9006, 9007, 9009) and the **water arena**
(quest map 10: 9003 Royal Ludroth, 9005 Lagiacrus, 9008 Gobul and
Lagiacrus), a pool for the underwater monsters and for water rendering and
animation (`--quest 9005 --quest-monster Gobul`). Both are the arena map
(9): the land arena its area 0, the water arena its area 1, where a water
arena quest starts (field_jump.cpp, QuestMapSetupPort).

Small monsters: `--quest-small NAME|ID` makes every small monster spawn of
the quest that monster (the area spawn tables at +0x4B8; only named
monsters other than the quest's large ones change). The arenas' own spawns
are reinforcements a large monster calls in, so use a field quest:
`--quest 1028 --area 1 --quest-small Jaggi` fills Moga Woods area 1 with
Jaggi (51 spawns). Small monsters are monster works like the large ones and
start their actions the same way (`kind 10`, `list`). The quest load prints
which spawn tables (one per area) it changed and their old kinds (`MH3
quest 1010: ... (table:kinds 1:27,27 3:33,33,34 ...)`).

Where the kinds that need their own map live: Giggi `--quest 1045 --area 1
--quest-small Giggi` (Tundra), Jhen Mohran `--quest 14002` (the sandship),
Ceadeus `--quest 2006` (its lair; `--quest-monster 20` in an arena does not
spawn it), Alatreon `--quest 19001 --area 1`. Altaroth did not appear in any
run (swapped in on the Sandy Plains or Moga Woods, or natively in quest 1010
"Bug Hunt"); its actions were fuzz-checked on an Aptonoth's work instead
(`MH3_PORT_CHECK=...,A:0xADDR:27`: original and port run the same calls on
the same state, so the compare holds for any kind's work). Kind 40 (the
arena helper, and Uragaan's bouncing rock) is in every arena.

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

`--watch` forces the actions once under gdb with write watches on the
monster work's count (+0x20), yaw, pitch and x, and reports the translated
writers: the per-call counters and turns still to port. `--pairs FILE` forces
every action the kind's dispatcher has instead of the learned ones (`mode
sub` lines from `scripts/dev/enemy_action_pairs.py DISPATCHER`, the kind's
handler 3; modes 10-13, reactions, death, area changes and intros, are left
out unless `--modes` names them). The water arena's monsters need it: the
hunter cannot reach their pool there (its floor is at height 0, Lagiacrus
swims near -1100), so their AI only waits.

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

## Kinds and their code

A monster's kind is its work's byte +3, the monster list's index (`--quest-monster`
takes the same numbers) and the game's own `emNNN` (the symbols `em003_*` lie in
Qurupeco's code, `em015_denki_*` in Lagiacrus', `em030_*` in Felyne's). Each
kind's handler table (`MH3 monster handlers` lines) gives where its code lies
and its action dispatcher (handler 3):

| Kind | Monster | Code from | Dispatcher |
| --- | --- | --- | --- |
| 1, 2 | Rathian, Rathalos | 0x80147CF8 | 0x80151128 |
| 3 | Qurupeco | 0x80154EA4 | 0x8015AC34 |
| 8 | Barroth | 0x8015DA1C | 0x801632FC |
| 10 | Jaggi | 0x80166E5C | 0x8016C0B8 |
| 11 | Jaggia | 0x8016D228 | 0x8016FA64 |
| 12, 14 | Great Jaggi, Great Baggi | 0x80170664 | 0x80174158 |
| 15 | Lagiacrus | 0x80176C94 | 0x8017F19C |
| 16, 17 | Royal Ludroth, Ludroth | 0x801830A4 | 0x8018D314 |
| 18 | Gobul | 0x80192694 | 0x80199B88 |
| 27 | Aptonoth | 0x801AB0AC | 0x801AEDC4 |
| 30, 31 | Felyne, Melynx | 0x801B0920 | 0x801B2D98 |
| 34 | Kelbi | 0x801B4D78 | 0x801B6560 |
| 40 | the arenas' own (variant 3) | 0x801B99CC | 0x801BB2BC |
| 6 | Diablos | 0x801BB988 | 0x801BF9FC |
| 4 | Gigginox | 0x801C2D54 | 0x801C8924 |
| 5 | Barioth | 0x801CAAF0 | 0x801D4CF4 |
| 7 | Deviljho | 0x801D7228 | 0x801DB788 |
| 26 | Giggi | 0x8032CCF4 | 0x8032FAEC |
| 33 | Altaroth | 0x8035BB18 | 0x8035DD44 |
| 35 | Bnahabra | 0x8035F0C4 | 0x8035FA00 |
| 19 | Agnaktor | 0x803790F4 | 0x8037F588 |
| 9 | Uragaan | 0x80386940 | 0x8038CC38 |

The ports named before 2026-10-06 used `emNNN` labels that were wrong for
kinds 1-12 (0x8014xxxx was called Gigginox, 0x8015611C Barioth, 0x801C7570
Diablos, ...); they are relabeled by the dispatcher that reaches each function.
Their prose describing a monster's behaviour was written under the old names.

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
| `await K` | hold the next commands until a monster of kind K is in play, then target it (a file written before the start begins with it) |
| `act MODE SUB` | start that action now, as the AI would |
| `every TICKS MODE SUB` | start it now and every TICKS ticks of game time (30 a second); `every off` stops |
| `ai off`, `ai on` | off: the monster picks no action of its own (the starts of its AI's pick, 0x8013C4BC, are dropped); it waits until a command starts one, and an action's own follow-ups and its end still run |
| `freeze on`, `freeze off` | hold the monster's step: it stands still and picks nothing |
| `face DEGREES` | set its yaw |
| `goto X Y Z` | place it |
| `hunter save`, `hunter back`, `hunter X Y Z` | remember the local hunter's place, put it back there, or place it (attacks aim at the hunter, so a sweep puts it back before each) |
| `hunter act KIND ACTION`, `hunter list` | start the local hunter's action through FieldNextAction (0x80275B28), or print its group, action, state, timer, motion and the motion's frame, end and loop frames; the reactions are kind 6 (dispatcher 0x8025A33C), their water variants mostly the odd actions |
| `wait TICKS` | run the next commands TICKS ticks of game time later (a whole sequence can be written before the start) |
| `list` | print each monster's index, work, kind, action, state, position and yaw |
| `# ...` | a comment |

The action pairs a monster uses come from the trace: run the pit with
`MH3_TRACE=actions`, let it fight, and read the `MH3 monster action` lines.
`MH3_TRACE=hit-shapes` names the body parts each attack hits with.

The integrated cheats work beside it (`--cheat NAME`): *Monster speed
modifier*, *Monster can't see you*, *Freeze/unfreeze monsters*, *Action
repeater for targeted monster*, *Pass through monsters (on/off)*.
