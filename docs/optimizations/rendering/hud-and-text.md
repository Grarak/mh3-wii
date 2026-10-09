# The HUD and the debug font

## What the original does

The frame body's `DrawScene` call (0x80049308) wraps the 3D scene in 2D
layers (0x8004925C, layers 0-1, the scene, then 1-11) and the UI sprites.
Each layer runs the draw functions of its active entries; the field HUD is
one of them (0x802E55C0: gauges, clock, map, items). Text such as the
online notices goes through the game's debug font (`flfnt`): for every
character `flKnjMsgNumPtr` (0x8005B8DC) and its siblings walk the 228-code
glyph table at 0x80580818 from the start (0x8005BF84), and `FontPutsText`
(0x8005C040) queues a quad per glyph (0x8005BB58).

## What the port does instead

- **Native frame draw, layers and HUD** (`runtime/src/frame_draw.cpp`,
  `hud_draw.cpp`), ported from the decompiles, so `MH3_PROFILE=split` can
  time each layer entry and each HUD part (the `MH3 layer split` and
  `MH3 hud split` lines).
- **UI shapes bind their texture once**: `UIShapeSetup` bound each textured
  shape's texture twice, clamped and then with its wrap modes (resolve,
  store into the guest texture object, bind); it binds once with the wrap
  modes, which leaves the same state.
- **No self-moves**: a recorded copy that replaced as many provenance
  ranges as it overlapped (the item bar's per-frame string copy) still
  moved the whole range tail onto itself; zero-shift moves are skipped.
- **The debug font on a glyph map**: `FontGlyphIndex` answers from a
  code-to-index map built from the table on first use; `flKnjMsgNumPtr`,
  the column measures (0x8005B83C, 0x8005BFCC), `FontPutsText` and the
  glyph writer are precise natives that read the UTF-8 text through host
  pointers.

## Why

The field HUD's layers took 2.05 ms a frame of the Vita game thread (Tundra,
HUD on screen), about two thirds of its draw part. Binding once and the
skipped moves took them to 1.82 ms. The rest of that cost is spread thin
over the HUD's `DrawShape` calls.

On the online notice screen `flKnjMsgNumPtr` alone took 37.7% of the Vita
game thread. With the glyph map and the native queueing: game frame 13.00 →
5.72 ms on average (14.89 → 6.00 ms at most), 58.5 → 59.9 fps with the
frame limit on. Port checks matched every call on the host (FontGlyphIndex
524,288 calls, flKnjMsgNumPtr 8,192).

## Example

The glyph lookup (`runtime/src/debug_font.cpp`) replaces a linear walk of
the table per character with one map built from the same table, so the
result is the original's index for every code.

## Applying it to another Wii game

Generic: time the 2D layers apart from the 3D scene before optimizing
either; UI code often repeats state setup per element, and text paths
often search tables per character.

Game-specific: the layer table and the font. `flfnt` is MH3's own; other
games have their own font code with similar per-character lookups.
