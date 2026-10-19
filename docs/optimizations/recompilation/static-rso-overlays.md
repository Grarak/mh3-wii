# Relocatable modules translated ahead of time

## What the original does

Much of MH3's code is not in `main.dol` but in 21 RSO modules on the disc
(`files/01/*.rso`):
the menus, lobby and title (`com_data`, `lobby_data`, `demo_data`), the
quest and monster code (`quest_data`, `em_data`, 211 KB and 325 KB of
code), and one small overlay per map (`map00`-`map11`, `map_village`). The
SDK's RSO loader (0x804DB0A4, with the SEL initializer 0x804DA544) reads a
module into a heap block, applies its relocations against the DOL's
exports, runs its prolog, and later unlinks it. `LoadFieldModuleSet`
(0x80040BFC) loads common, quest and enemy code for the field;
`LoadMapOverlayById` (0x80040E4C) swaps the map overlay. Code that exists
only after a runtime load cannot be translated ahead of time without
knowing where it will be.

## What the port does instead

MH3 loads each module at a fixed address for a given game state, so each
module is linked offline at that address and translated like the DOL:

- **Boot modules** (`hbm_data`, `com_data`, `lobby_data`, `demo_data`,
  `quest_data` at their boot bases) are linked into an analysis copy of the
  DOL by `scripts/prepare_rso_translation.py` and translated with it.
- **Field and map overlays** are 15 separate translation profiles
  (`scripts/translate_rso_overlays.py`): `map_village`, `quest_field`
  (`quest_data` relocated to 0x808C76A0, where the field puts it),
  `enemy_field` (0x80972F40) and `map00_field`-`map11_field` (all at
  0x80AA4500). Each profile has its own host symbol prefix
  (`mh3_<profile>_...`), because the same guest addresses hold different
  code in different profiles.
- **The game's own loader still runs.** It reads the file, relocates the
  data and calls the prolog. The prolog is a native wrapper that checks the
  module's base address and a checksum of its linked text before entering the
  translated code; a wrong file or address stops the game. Unlinking marks
  the profile inactive, and a call into an inactive module's code is
  rejected.
- Each module's source file is pinned by SHA-256 in the profile table, and
  the main DOL by its hash in `recomp.yml`.

## Why

Static recompilation needs every executed instruction at build time. With
no interpreter or JIT in this runtime, a module the translator has not seen
cannot run. Translating overlays at their observed addresses was the
smallest change that kept the original loading code (and its memory layout,
which the game's heap and pointers depend on) unchanged.

## Example

The profile table of `scripts/translate_rso_overlays.py` (hashes shortened
here):

```python
PROFILES = {
    'map_village': ('map_village', 0x8097D080, 'aeddd064...', 'MapVillageBinding'),
    'quest_field': ('quest_data', 0x808C76A0, 'aed47cf7...', 'QuestFieldBinding'),
    'enemy_field': ('em_data', 0x80972F40, 'bbb8a300...', 'EnemyFieldBinding'),
    'map01_field': ('map01', 0x80AA4500, 'f93ed630...', 'Map01FieldBinding'),
    # map00_field and map02_field ... map11_field at 0x80AA4500 as well
}
```

The build's translation step (`scripts/translate.py`,
[translation](../../translation.md)) runs the profiles whose inputs changed
in parallel after the main translation.

## Applying it to another Wii game

Generic: `scripts/link_rso.py` (an RSO linker following Dolphin's `RSO.h`),
the analysis-DOL approach, per-profile symbol isolation, and the runtime's
prolog verification and lifetime tracking.

Game-specific: the module list and the address each module is loaded at in
each state. MH3's addresses follow from file sizes and 32-byte alignment in
a fixed load order; a game whose heap layout varies (modules loaded in a
different order, or into a heap with other allocations) needs one profile
per base address, or a translation that relocates at run time. Find the
loader and its callers first, record the bases in a few states (MH3's were
checked against MEM1 snapshots taken after the original loader ran), and pin
each file by hash so that a different disc revision fails loudly.
