# Static RSO bring-up

Five boot modules are included in static C++ translation with fixed-address
bindings for the verified RMHP08 files. Alternate addresses and other modules
still require explicit support.

| Module | Base | Prolog |
| --- | --- | --- |
| hbm_data | `0x808196E0` | `0x8081A8A4` |
| com_data | `0x80868E40` | `0x8086907C` |
| lobby_data | `0x808C76A0` | `0x808C7ED8` |
| demo_data | `0x8097D080` | `0x80992FC8` |
| quest_data | `0x809D3DC0` | `0x809D60C4` |
| map_village (overlay) | `0x8097D080` | `0x8097D22C` |

Every initialized section of each offline-linked image matched a snapshot after
the original guest linker finished. The first module used `rso-boot-mem1.bin`;
the remaining four used `rso-next-mem1.bin`, captured at the com prolog before
constructors executed. Each source file and the original DOL are hash-pinned.
No snapshot is needed for a normal build or run.

The format is documented by [Dolphin's RSO declarations](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/Core/Debugger/RSO.h).
The RMHP08 loader at `0x804DB0A4` supplies the relocation behavior, and its SEL
initializer at `0x804DA544` supplies section addresses, including BSS. The script
checks the main DOL hash before applying those addresses. It rejects unsupported
relocations and unresolved imports rather than leaving them as zero.

`scripts/prepare_rso_translation.py` builds an analysis-only DOL with the linked
RSO sections and discovers function seeds from exports, standard prologues, and
constructor/destructor tables. The five executable sections share bytes with one encompassing analysis data
span, keeping within the DOL section limit. The runtime data initializer still comes from
the original DOL; the module is not copied into guest memory at startup. The
module's native prolog wrapper verifies its base and linked text checksum before
entering compiled code, then enables executable-write protection for its text.

To repeat the comparison using the retained pre-constructor snapshot:

```sh
python3 scripts/link_rso.py Assets/DATA/files/01/hbm_data.rso \
  --base 0x808196e0 --output build/hbm-linked.bin \
  --verify-mem1 build/rso-boot-mem1.bin
python3 scripts/test_rso_linker.py
```

`MH3_MISSING_TARGET_MEM1_DUMP=path` writes MEM1 only when dispatch reaches a missing target.
Comparison requires a snapshot before constructors mutate initialized data;
a later boot boundary may no longer satisfy that condition. Local linked images,
analysis DOLs, generated code and snapshots stay under ignored build directories.

The Metal boot verifies and executes all five compiled prologs, prints media
version `1002121503`, and passes the former disc-padding boundary at partition byte
`968250889` with the preserved partition image. Both native and Metal runtime/audio tests pass. Translation emits
20,639 functions; this remains incomplete game coverage. Evidence is in
`logs/boot-rsos-run.log`.

## Village transition: module lifetime boundary

After the HID shutdown fix, `logs/hid-close-boot.log` reaches the original
module unload path and stops at executable write `0x808692C2`. The recorded LR
`0x804DAE18` identifies the relocation call inside SDK import unlinking
(`0x804DAD48`), not an arbitrary game patch. It replaces the com module's
resolved import with its unresolved handler (`0x808694CC`).

The high-level SDK unlink routine `0x804DB614` removes a module from the linked
list, unlinks imports in both directions, and reverses its section addresses.
The game's unload routine `0x800409B8` runs module epilogs before invoking that
SDK unlink routine. The current fixed-address bindings protect text indefinitely,
so they cannot yet support this legitimate unload. The next implementation must
track module lifetime, reject stale translated execution, and reverify linked
text on reload; disabling executable protection globally would be incorrect.

## Module lifetime handling

The host now wraps the original SDK unlink routine. For a verified, linked
module it checks the current text checksum, marks the binding inactive, and
removes only that module's exact executable-write range before running the
original unlink code. Other executable ranges remain protected. Dispatch to
inactive module code is rejected; the verification prolog remains reachable so
a reloaded image can be checked before its constructors execute. Same-address
reload restores protection only after the full linked-text checksum matches.

The pinned boot modules have no imports from one another, so unlinking one
does not require changing another active module's text. Arbitrary new modules
and alternate load addresses still require their own translation/binding.
Generated internal calls run within the active module's synchronous call tree;
this is not a concurrent module-unload implementation.

Range tests cover unaligned ranges crossing pages, physical/cached/uncached
aliases, duplicate registration, neighboring protected code on a shared page,
unload/reload writes, and rejection of stale RSO dispatch.

Validation: both build variants pass CTest; linker and native-discovery tests
also pass. The Metal run `logs/rso-lifetime-boot.log` unloads com/lobby/demo/quest,
then reloads com and lobby at their original addresses and verifies their full
text checksums again. It stops at the next module's untranslated prolog
`0x8097D22C`, with base `0x8097D080`; the optional MEM1 snapshot is retained at
`logs/rso-lifetime-mem1.bin` for identifying that new binding.

## Village map overlay

`map_village.rso` loads at the demo module's former base. Its initialized
sections, including all 388 text bytes, match the offline-linked image in
`logs/rso-lifetime-mem1.bin`. The source image is SHA-256 pinned.

`scripts/translate_rso_overlays.py` runs the same strict PPC-to-C++ translator
against a separate analysis DOL, then gives the six generated bootstrap
functions unique host symbols. It does not change guest addresses or initialize
module RAM; the original guest loader still supplies all code/data and runs the
prolog. Generated source remains ignored, alongside the other translations.
The generator checks metadata/source hashes, rejects unexpected function seeds
and cross-profile ABI dependencies, and requires zero unsupported instructions
and zero invalid SSA functions. Run it as part of `scripts/translate.sh`.

Indirect calls/jumps consult the active overlay before boot dispatch. The
overlay cannot replace an active demo binding. Its prolog verifies linked text
before activation, unknown targets inside active overlay text cannot fall
through to demo code, and SDK unlink deactivates the correct profile even when
two profiles share a base. Generator tests cover host-symbol isolation and
rejection of unsafe cross-profile calls; runtime tests exercise overlap
ownership, wrong-image rejection, and the actual generated object-chain leaf.

Validation: overlay-emission/linker/native-discovery tests and both CTest suites
pass. In `logs/map-village-boot.log`, the original loader unloads the boot
modules, reloads com/lobby, verifies the village overlay, and executes its
prolog. Loading then stops at an independent main-DOL indirect jump target
`0x800ACBF8`; playable Village rendering is not yet verified.

## Offline field and Moga Woods profiles

`LoadFieldModuleSet` (`0x80040BFC`) loads common, quest and enemy modules.
Quest is relocated from its boot base `0x809D3DC0` to `0x808C76A0` when
entering the field, so calling the boot translation at the new address is
incorrect. Enemy follows at `0x80972F40`. `LoadMapOverlayById` (`0x80040E4C`)
then selects map01 for Moga Woods at `0x80AA4500`. The full file inventory and
static filename/selection tables are documented in `module-inventory.md`.

The overlay generator now supports `quest_field`, `enemy_field`, and
`map01_field` profiles in addition to the village profile. Each uses a pinned
source hash and independently relocated analysis image. It emits unique host
symbols and includes the closure of typed callees from the same translation
pass, avoiding assumptions about the boot translation's optimized ABI.
CPU-context calls use lifetime-aware runtime dispatch. Generated guest
addresses and loader behavior are unchanged. The runtime selects the active
profile, checks its linked text at prolog entry, and retains the existing
unlink/write-protection checks. Overlay records are sorted for binary lookup.

A large quest export (`pl_effect_sub`) exceeds the translator's default
8,192-instruction disassembly budget. The CLI now accepts explicit
`--max-instructions` and `--max-bytes` limits; overlay generation requests
32,768 instructions and 262,144 bytes. Budget overruns, unsupported
instructions and invalid SSA still fail. Defaults for other translations
are unchanged. The field profiles emit 351 quest, 84 enemy and 6 map
entries with zero unsupported instructions and zero invalid SSA functions.

`python3 scripts/replay_village.py --name woods-check --moga-woods` reuses an
isolated copy of the post-Chief save, follows the dock route, and selects the
exit. This is input automation, not a savestate or an in-memory scene patch.

Validation: `logs/replays/woods-map01` verifies all four active field modules,
renders Moga Woods with its HUD/minimap and first-entry Chief/tutorial pages,
and accepts movement after those pages close (captures 1860 and 5700).
The runtime self-test includes field-versus-lobby ownership and wrong-image
rejection; it passes. Six linker tests, three overlay-emission tests, native
replacement-discovery regression, and five translator command/disassembly
tests pass. Regenerating map01 produces identical function output (21,739
unchanged files). Loading time is a separate optimization item in `TODO.md`.

Further traversal in `logs/replays/woods-map01` reaches the Area 1 lookout
introduction and its stamina/camera tutorials. Area 1 → Area 3 loads
successfully (transition capture 16380; rendered Area 3 capture 16800),
accepts movement, and returns to Area 1 (capture 17340) without a missing
translated target or runtime stop. The top/left dark-band rendering report
is tracked separately in `TODO.md`; viewport/fog misalignment is a hypothesis,
not a verified cause.
