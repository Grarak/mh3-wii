# Debug port

Development builds on Linux can be driven over TCP while they run: read and
write guest memory, call guest functions, turn on cheats and dump frames.
Its commands skip the game's menus: they do what a menu would by calling the
game directly, instead of navigating it with button presses.

```
MH3_DEBUG_PORT=7777 build/linux-arm64-dev/MH3 --village
scripts/dev/debug-port.py state 'wait 30' "shot $PWD/village.bmp"
```

`MH3_DEBUG_PORT=PORT` listens on 127.0.0.1, `MH3_DEBUG_PORT=ADDR:PORT` on
that address (`0.0.0.0:7777` to reach a game on another machine). Each
line sent is a command; each command gets one answer line, starting with
`ok` or `error`. The game thread runs the commands in order, once a logic
step before the tasks (game_frame.cpp). Commands that take game time
(`wait`, `steps`) answer when they are done and hold the commands after
them until then, so a script is a plain list of lines. Several clients can
connect; each gets its own answers.

`scripts/dev/debug-port.py [--port [HOST:]PORT] COMMAND...` sends each
argument as a command (or stdin's lines) and prints the answers; it exits 1
when one is an error.

| Command | Effect |
| --- | --- |
| `help` | the command names |
| `state` | the step count, game time in ticks, the step's scale, the map and area, the local hunter's work and position (`hunter none` in menus) |
| `peek ADDR [N]` | N 32-bit words at ADDR (hex); `peekh` 16-bit, `peekb` bytes, `peekf` floats |
| `peeks ADDR [MAX]` | the NUL-terminated string at ADDR |
| `poke ADDR VALUE...` | writes 32-bit words (hex); `pokeh`, `pokeb`, `pokef` (decimal floats) |
| `wait TICKS` | answers TICKS ticks of game time later (30 a second) |
| `steps N` | answers N logic steps later |
| `shot PATH` | dumps the next drawn frame to PATH (BMP) |
| `call ADDR [INT...] [f FLOAT...]` | calls the guest function at ADDR with the ints (hex) in r3.. and the floats in f1..; answers r3 and f1 |
| `cheat NAME[=VALUE]` | turns on an integrated cheat (as `--cheat`) |
| `movie skip` | ends the open movie: its player sees it finished at its next check (once no frame is pending, as the game's own Movie_cut) and closes it as at its last frame, also where a button can't skip it (the opening after character creation, the Loc Lac movie); `error: no movie is open` otherwise |
| `trace` | the `MH3_TRACE` items that can be switched, each `=on` or `=off` |
| `trace ITEM on\|off` | turns a trace item on or off ([flags](flags.md#mh3_trace-game-state-over-game-time)) |
| `online [state]` | online: the network state and sub-state, the menu command, whether the hunter leads, the joined circle (quest group), members present and standing, the gate and city names |
| `online post QUEST [MEMBERS]`, `join [N]`, `ready`, `unready`, `depart`, `accept [QUEST]`, `circles` | posts a quest, joins the Nth posted one, readies, departs, accepts a quest at the counter, counts the posted quests (the game's own calls; `online_auto.cpp`) |
| `online auto SPEC` | starts the online autopilot with an `MH3_ONLINE_AUTO` spec ([online](online.md)) |

Addresses are guest virtual addresses (0x80000000 up); others answer
`error: no guest RAM`.

The Vita build has no debug port yet (DebugPort::Step does nothing there).
