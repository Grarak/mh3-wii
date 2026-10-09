# Pipelined draw: the 3D scene on a draw thread

The only draw path, in every build. Port checks (`MH3_PORT_CHECK`) compare
and restore whole RAM around a call, so while one is set the scene graph
walk stays on the game thread and the next logic step waits for each
frame's draw.

## What the original does

Each frame body runs one logic step and then that step's draw, back to back
on one thread. On the Vita in the Alatreon fight that was 11.3 ms of logic
and 13.4 ms of draw per frame, 24.7 ms in all.

## What the port does instead

The draw of frame N runs on a second core while the game thread runs the
logic of frame N+1:

1. At the scene's place in the frame the game thread makes the camera's
   view matrix, launches the effect calculation on the guest worker and
   hands the scene graph walk (the traverse with the models' view and
   skinning passes, the root's view matrices, the flattened buckets) to the
   draw thread. Meanwhile it draws the layers after the scene (the HUD), then
   joins the walk.
2. The draw thread, with its own CPU context, guest stack and g3d scratch,
   draws the buckets while the game thread finishes the frame's 2D work
   (fonts, fades) and runs the next logic step.
3. On the Vita the draw thread runs on core 1 ([core layout](core-layout.md)).
   It spins up to 20 ms for its next job instead of parking, because a
   parked thread's wake took 0.1-1.9 ms.

Translated code addresses guest memory through one fixed base, so the draw
thread cannot be given its own copy of RAM. Every data flow between the
next logic step and the draw had to be separated by hand: a development trace
(page protection on the host, removed since) recorded which guest pages each
draw reads and the next logic writes, and a
write-watch build attributes each conflicting write to its guest call site
(about 150 sites in ten systems). The draw's readers that are native ports
read snapshots taken at the handoff; what reads gameplay state broadly
through translated code (the HUD) stays on the game thread.

What the logic may still do beside the draw, and how it waits:

- **Loads** run serialized: each disc write waits for the draw in flight,
  except a sound stream's, whose buffer the draw never reads (the music's
  2-4 reads a second in a fight each cost about 8 ms of waiting before).
- **Copies and retirements** wait only where the draw reads tracked bytes:
  under a model certificate or a live material block (`DrawMayRead`,
  `runtime/src/model_catalog.cpp`). At the Deserted Island beach the game
  thread's waits went from 73 a second (1.10 ms a frame) to 0.
- **Heap frees** of what the draw may read wait in a list for the next
  handoff, the block allocated until then (`DeferFree`,
  `runtime/src/native_resource_lifetime.cpp`); an allocation that fails
  first waits for the draw and applies the list.
- **Destructors**: a destroyed g3d object gets its base vtable, whose draw
  virtual is null, before its memory is freed; the draw thread then called
  address 0. Every g3d destructor that reaches `G3dObj::~G3dObj` is a native
  port (`runtime/src/g3d_destroy.cpp`), and a destroy waits at its start
  while the draw holds the object or the model an animation is bound to.
- **Material animation** (`CalcMat`) writes a model's texture SRT, colours
  and texture patterns in place; 1.6% of those writes at the beach happen
  while a draw is in flight, and a draw that captures such a material
  afterwards uses the next step's value (a sea scroll differing by 0.002 in
  30 of 450,000 checked draws). This is accepted.
- **Material group re-copies**: `CalcMat` also restores the groups an
  animation wrote from the resource every frame (`CopiedMatRefresh`). The
  first copy records the disc source's provenance on the instance's group,
  and the in-place copy refused such blocks, so every later re-copy
  replaced the block and waited for the whole draw (about 22 waits a
  second in the Jhen Mohran fight). The in-place copy now takes the block
  and retires its provenance, and a replacing copy waits only when its
  destination holds provenance or live blocks the draw may use. Vita, Jhen
  Mohran fight: catalog waits 21.9 → 2.7 a second (1.03 → 0.33 ms a
  frame), game waits for the draw 1.56 → 0.88 ms a frame, 50.3 → 51.4 fps.

## Why

The game thread was the Vita's limit. Devkit, Alatreon, the same build,
last 25 s of single runs:

| | Inline | Overlapped |
| --- | --- | --- |
| Logical fps | 36.2 | 42.9 |
| Game thread, logic + draw lap | 11.3 + 14.0 ms | 11.9 + 8.8 ms |
| Render packet | 20.0 ms | 17.7 ms |

With the overlap the render thread (17.7-20 ms per packet) became the
limit, with the GPU close behind; GPU skinning then cut the render thread's
time ([GPU skinning](../rendering/gpu-skinning.md)). Later steps, Vita, Alatreon, three
alternating runs each:

| Change | Logical fps | Displayed fps | Game thread |
| --- | --- | --- | --- |
| Scene graph walk on the draw thread | 53.1 → 55.1 | 53.1 → 53.8 | draw section 4.4 → 3.6 ms |
| World matrix pass split with the guest worker ([guest worker](guest-worker.md)) | 54.1 → 56.4 | 52.4 → 54.0 | logic 12.60 → 11.67 ms, draw section 3.56 → 3.91 ms |

Spinning instead of parking moved the walk's start from 0.67 to 0.54 ms
after its launch and the join's wait from 1.32 to 1.24 ms per frame. The Pi 5
ran the village, Moga (1060), the sandship (14002), The Decisive Battle
(2006) and Alatreon for 120 s each with the walk on the draw thread without
a failure of the recording checks. Conflicts that remain (materials and
animation results read a step late, effect data the logic changes beside
the effect draw) showed no visible effect so far.

## Example

The conflict groups found in the fight (the page trace above), condensed:

| Group | Logic writers | Draw readers | Plan |
| --- | --- | --- | --- |
| Model world matrices and state | CalcWorld 0x80073830, the models' scene task 0x8007DE60 | skinning, view passes | do those passes at the handoff |
| HUD state | 0x8029A3BC (clears and rebuilds 20 pages) | the translated HUD parts | keep the HUD on the game thread |
| System flags and timers | the frame body, TickSystemTimers | various | accepted: at worst one frame newer |

## Applying it to another Wii game

Generic: the page-level conflict trace, the write watch, the draw thread's
runtime support (shared with the [guest worker](guest-worker.md)) and the
staged approach: first run the draw on its own thread but serialized, so
every assumption about which thread draws shows up with unchanged output;
then separate flows one at a time; then overlap.

Game-specific: everything the trace finds. Expect conflicts until the end
of the draw (on the Pi, the median last use of a conflicting page was
3.5 ms into a draw of about 5 ms, the UI sprites and font flush last), so
there is no safe point to start the next step early; each
flow needs a snapshot or must stay on the game thread. A copy-on-write view
of RAM for the draw thread was considered and rejected: translated code would
need a per-thread base on every access, and the logic writes about 140
pages a frame.
