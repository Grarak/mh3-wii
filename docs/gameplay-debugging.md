# Gameplay debugging

`./scripts/replay_village.py --name my-run` copies `saves/nand` into
`logs/replays/my-run/user/nand`, overlays `fixtures/post-chief-data00.bin` on the
copied first save slot, boots `--village`, and waits for an unobstructed village
view. The fixture is a real in-game save made after talking to the Village
Chief through the Game Menu's Quit Game / Save flow. It avoids the opening
dialogue on every run without modifying the original NAND. Use `--fresh` to
replay from the original save and advance the conversation with Classic A.
`--dock-edge` then holds the left stick right to revisit the reported dock
boundary. `--profile` records CPU, renderer, EFB, and frame-interval counters.

The script prints the running PID, diagnostic input-file path, capture directory,
and log. Write controller states atomically: a line contains a hexadecimal
button mask and four decimal stick axes, for example `0 511 0 0 0` for right.
The script's `controller()` function performs the safe temporary-file replace;
truncating the live file can make the controller reader see an incomplete report.
Frame capture frequency is set with `--capture-interval N` (default 5); captures
are large. Runs must use unique names. See [controls](controller-bringup.md) for
the keyboard layout.

For a Dolphin reference run, `./scripts/run-dolphin-reference.sh --name reference-1
--post-chief --pipe-controller --dump-frames -b` uses the extracted ISO, a fresh per-run NAND at
`logs/dolphin-runs/reference-1/user/Wii`, and the same post-Chief save. The
launcher copies `saves/nand`, overwrites slot one with the fixture, and verifies
that the copy matches byte for byte. Run names must be unique. Its emulated Wii
Remote has a Classic Controller extension. `--pipe-controller` configures Dolphin's
built-in named-pipe device and enables background input, allowing direct control
without a movie or an active Dolphin window. For example, run
`python3 scripts/dolphin-input.py --run reference-1 tap A` to press Classic A, or
`python3 scripts/dolphin-input.py --run reference-1 stick 0 -1 --seconds 1` to
move forward. The pipe accepts Classic buttons, D-pad, and left-stick input;
without `--pipe-controller`, `scripts/dolphin-classic.ini` maps the keyboard to
the same layout as this port. `--dump-frames` writes native
Dolphin frames under `logs/dolphin-runs/reference-1/user/Dump/Frames`. The script
requires an installed Dolphin.app and desktop display access. Dolphin was cloned
at `build/dolphin-src` for source comparison; that checkout is ignored by Git.
The direct pipe was verified by skipping the opening cinematic with A, advancing
from the title screen, selecting Continue and the post-Chief character, loading
Village, and moving to the dock edge. Dolphin holds the character on the dock
when the stick continues pointing off its edge. The route to a field map is from
the village dock through the fish market, up the stone stairs, and across the
upper-right bridge. Pressing A at the `Exit to Moga Woods` marker loaded Area 1
with its field HUD and map. Dolphin's pipe backend is generic: `PipeDevice`
exposes button and axis inputs, while `WiimoteNew.ini` binds those inputs to
the Classic Controller extension. `BackgroundInput = True` is necessary for
unfocused or batch reference runs; Dolphin's video-interface poll otherwise
gates controller input. Stop a finished named run with
`./scripts/stop-dolphin-reference.sh reference-1`; the helper checks the stored
PID and command line, sends TERM, escalates to KILL if needed, and verifies exit.

The executable retains `-O1 -gline-tables-only -ffp-contract=off` for generated
PPC C++ (`runtime/CMakeLists.txt`). An `-O2` Metal build passed all three CTest
targets and loaded the post-Chief village, but a matched stationary scene stayed
near 66 ms per display copy and 53 ms process CPU per copy under both `-O1` and
`-O2`. We therefore kept `-O1`. Metal queue completion was about 1–2 ms/copy;
this scene is primarily CPU-bound. The most significant measured improvement so
far came from stable, content-derived texture revisions: repeated BP texture
binds can now hit Aurora's decoded-texture cache while guest RAM rewrites still
change the revision. A separate VI deadline change preserves retrace phase after
slow frames, although it cannot make a 50+ ms CPU frame meet a 33 ms game tick.
`MH3_PROFILE_FRAMES=50` enables the counters without the replay script.
An uninstrumented `--profile --dock-edge` replay starting at the post-Chief
checkpoint measured 53.65–54.46 ms process CPU/copy and roughly 66.6 ms
wall/copy while standing in the village (copies 1001–1150). Movement began near
copy 1160. During the rightward dock route, CPU/copy fell from about 49 ms to
43 ms as draws/copy fell from roughly 15,000 to 13,000; Metal completion stayed
near 1.5–1.6 ms. The game remained slower than its 33 ms target and copy
intervals were uneven, but this route did not show collision work increasing
frame time. The scene and draw count changed during movement, so these numbers
do not isolate the sweep's cost or predict what a collision fix will do.

The dock edge and the stone stairway next to the player's house both permit
implausible movement in the native build. Horizontal collision is therefore not
validated, and reaching Moga Woods or a quest map remains outstanding. The
ground-hit functions at `0x80291D34`, `0x802963CC`, `0x802968D4`, `0x80296A5C`,
`0x80296B28`, and `0x80296BA4` are labeled in `symbols.txt` for follow-up.
Temporary counters on a dock-edge replay showed about 2,000 calls to
`hit_ground_comon`, including hundreds of successful ground hits. Its bounds and
triangle helpers also returned hits. `findInterSection3` at `0x802959FC` was
not called during that replay, so it is unlikely to explain the dock failure.
The build-gated guest-call tracer (`scripts/trace_guest_calls.py`) now captures
the active path during movement:
`player_control_move -> 0x80208590 -> 0x802094EC -> 0x8029513C ->
0x802952F4 -> 0x802920F0`. On a 609-update dock replay, the horizontal sweep
ran on all 609 updates, so the collision caller is not skipped. The sweep
accumulates flags at player offset `+868`; most sampled results were zero and a
few carried nonzero contact bits. A candidate branch from `0x802920F0` reaches
`0x80292A40 -> 0x80293568 -> 0x80293AE0` when it examines land geometry.
The same gated replay observed 225, 26, and 25 calls along those three edges,
respectively. At both patch sites, the running game's `r2` was `0x8079FF60`.
These functions are labeled in `symbols.txt`. To reproduce the call graph,
run `python3 scripts/trace_guest_calls.py enable`, configure with
`-DMH3_ENABLE_GUEST_CALL_TRACE=ON`, build, and set `MH3_CALL_TRACE` to the CSV
path. Optionally set `MH3_CALL_TRACE_GATE` to a marker path; `--dock-edge`
creates the marker immediately before movement. Summarize with
`python3 scripts/trace_guest_calls.py summarize PATH`. Restore generated shards
with `python3 scripts/trace_guest_calls.py restore` and configure the release
build with `-DMH3_ENABLE_GUEST_CALL_TRACE=OFF`.

A [community PAL port of the Walk through Walls Gecko code](https://wiird.gamehacking.org/forum/index.php?topic=5501.1050)
identifies two instruction addresses for this RMHP08 disc. Comparing the user's
USA and PAL ROMs confirms both instructions are offset by `+0x64`: PAL writes
`0xC002FDD0` at `0x80293AE0` and `0xC022FDEC` at `0x802951CC`. Both original
PAL instructions load the same small-data float at `r2 - 14184`:
`0xC002C898` and `0xC022C898`. That original float is `1.0` on both discs.
The first patched load is in the land sweep's vector normalizer; the second
initializes the three sweep-axis values. The patched loads instead read
zero-initialized small-data space. The adjacent original instructions match
across regions: `lfs f0; fdivs f1,f0,f1; b` scales a contact vector by its
reciprocal length, while `lfs f1; fmr f2,f1; fmr f3,f1` initializes all three
sweep components. `scripts/disassemble.py --dol PATH ADDRESS BYTE_COUNT`
shows both originals using `tools/python/bin/python`.
The patched loads read
`r2 - 560` and `r2 - 532`. With the USA `r2 = 0x8079DAA0`, the first operand is
`0x8079D870`, exactly the USA cheat's toggled data address. With the PAL
`r2 = 0x8079FF60`, the corresponding operand is `0x8079FD30`. **The published
PAL port incorrectly toggles `0x8079D8D4`**, which initially holds `6000.0`
and is read by `0x80350114` for an actor distance threshold. The replacement
data address is not a simple `+0x64` shift. The button condition in the posted
code contains placeholders, so the code as printed is not directly runnable.
`python3 scripts/audit_wall_gecko.py` reproduces this address audit from the
two original DOLs after extracting the USA RVZ. This is a diagnostic map of
relevant code, not evidence that the cheat fixes the native collision defect.
Applying the NTSC-U addresses to this PAL image would overwrite unrelated
instructions.
The generated signed-area function preserves the original PPC subtraction,
single-precision multiplication, raw-bit one-ULP correction, and edge-class
operations. Dolphin's interpreter performs corresponding forced-single
arithmetic; this static comparison has not yet identified a translator defect
or established that these ground-hit functions implement the missing wall
collision. `RMH.ini` in Dolphin enables CPU EFB access for Tri, so globally
disabling EFB synchronization would change the game's configured behavior.
On 2026-09-27, upstream WiiCompiled `main` was still at `85f2501`, the same
commit as the local translator, so there was no newer decompiler change to test.
