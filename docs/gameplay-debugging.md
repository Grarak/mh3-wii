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

For a Dolphin reference run, `./scripts/run-dolphin-reference.sh --post-chief
--dump-frames -b` uses the extracted ISO, an isolated `logs/dolphin-user` NAND,
and the same post-Chief save. Its emulated Wii Remote has a Classic Controller
extension and `scripts/dolphin-classic.ini` maps the keyboard to the same layout
as this port. `--dump-frames` writes native Dolphin frames under
`logs/dolphin-user/Dump/Frames`. The script requires an installed Dolphin.app and
desktop display access. Dolphin was cloned at `build/dolphin-src` for source
comparison; that checkout is ignored by Git. Frame dumping was verified at the
wrist-strap screen, but an automated Dolphin gameplay replay has not yet been
established.

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

The dock edge and the stone stairway next to the player's house both permit
implausible movement in the native build. Horizontal collision is therefore not
validated, and reaching Moga Woods or a quest map remains outstanding. The
ground-hit functions at `0x80291D34`, `0x802963CC`, `0x802968D4`, `0x80296A5C`,
`0x80296B28`, and `0x80296BA4` are labeled in `symbols.txt` for follow-up.
The generated signed-area function preserves the original PPC subtraction,
single-precision multiplication, raw-bit one-ULP correction, and edge-class
operations. Dolphin's interpreter performs corresponding forced-single
arithmetic; this static comparison has not yet identified a translator defect
or established that these ground-hit functions implement the missing wall
collision. `RMH.ini` in Dolphin enables CPU EFB access for Tri, so globally
disabling EFB synchronization would change the game's configured behavior.
