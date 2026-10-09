# Model preparation on the render thread, through draw tables

## What the original does

NW4R g3d draws a model by walking its MDL0 draw stream (a byte code of
material, shape and node IDs). For each item it resolves the material and
shape through the model's dictionaries, then runs the preparation in
order: the material scope steps (0x80076220: textures, palettes, TEV
pipeline, pixel and colour state, channels, texgen), the primitive setup
(0x800879DC) and the matrix rows that select the shape's matrices
(0x80087B34), each writing GX registers and g3d's CPU caches, and finally
sends the shape's display list. All of it runs on the game thread.

## What the port does instead

- **Capture on the game thread, prepare on the render thread**
  (`runtime/src/model_render_prep.{h,cpp}`, on by default). Per dispatch the
  game thread captures only what the three steps read (`ModelPrepInputs`):
  a shared immutable record of the material's static inputs, the genmode,
  depth flag and channels, pointers to immutable versions of the material's
  pixel and colour fields, the selected lights' masks and ambient, the
  shape's cache record, its matrix inputs and the pose version. The render
  thread replays the original step order with the portable C producers
  against a host mirror of g3d's CPU caches; it reads no guest memory.
  Fog and light-set selection stay on the game thread. The selection checks
  the set the light state already holds before decoding all 128 lights
  (Pi 5 Alatreon model material preparation 0.55-0.59 → 0.47-0.49 ms a
  frame), and reselecting it updates only the masks and the ambient colour
  in place and compares those alone, instead of copying the 480-byte slots
  twice and comparing all eight lights (Vita: light selection 34 → 12 of
  about 330 material preparation stacks).
- **Per-model draw tables** (`model_scope.cpp`): each certified model gets a
  table indexed by the draw stream's material, shape and node IDs, holding
  what the dictionary lookups and scope resolution return. A draw finds its
  table once by MDL0 address and dispatches every item by index. Loading
  screens build the tables of every loaded model file; their
  address-independent part is cached in `disc/cache/models/draw-tables.bin`.
- **Compiled materials**: a material record runs its steps once into an
  image and later dispatches draw from it
  ([compiled dispatch materials](compiled-dispatch-materials.md)).
- **Checks** (development, removed once they ran clean): the game-thread
  path ran beside the capture and replayed the captured inputs on a third,
  logical-only owner, comparing every result (0 mismatches over 10.5
  million dispatches when it was added, and again over 27 million items in
  three scenes with the draw tables); a second check resolved every indexed
  item by address as well.

## Why

The model draw pass was the largest part of the game thread's draw.
Measured:

- Moving preparation to the render thread: Vita desert quest 20.1 → 24.8
  fps, game frame 46.3 → 37.5 ms, model drawing 20.6 → 12.7 ms (the render
  replay grew 18.2 → 20.6 ms); Pi 5 Alatreon moving 5.35 → 4.94 ms CPU per
  frame.
- Draw tables: Vita desert quest model drawing 5.7 → 4.5 ms per frame; on
  arm64 Linux the model scene draw fell from 15.8% to 11.0% of the game thread.

## Example

The capture's contract: the game thread caches one material record per material scope and reuses
it while the live node revision, the frozen blocks' generations and the
object bytes are unchanged; a record is never reference counted, and a
replaced one is deleted by a render-thread task after the packet that was
being recorded. Invalidations of g3d's caches (`G3DState::Invalidate`)
travel with the next item.

## Applying it to another Wii game

Generic: the capture-and-replay structure, the draw tables and the
shadow-owner check. The preparation steps are NW4R g3d code (0x80087B34 is
g3d's `LoadResShpPrimitive`), so other g3d games have them at other
addresses; confirm each one by decompilation before reusing the C
producers, since g3d versions differ between games.

Game-specific: the addresses, and what the game changes between draws.
MH3's UI, effects, view passes and callbacks change state between model
draws, so the material and shape publications still run on every draw and
only the structural lookups are cached. Find every reader of the state a
deferred step needs before moving it: a missed input shows up as a
mismatch in the shadow replay, not as a crash.
