# Vita launcher

The normal Vita launch (no development arguments) enters a native launcher
before allocating guest RAM. `Start game` continues through MH3's ordinary
title and character menus. Quest selection, custom quest editing and cheats
are compiled into Vita release builds; they do not depend on `--quest`.

## Startup and external game data

The launcher first initializes native GXM without opening game texture packs.
It checks the installed kernel module named `kubridge` with
`_vshKernelSearchModuleByName`. A missing module produces a native
`SceMsgDialog`, with its GUI rendered through `sceCommonDialogUpdate` on the
current display buffer and sync object. No guest allocation happens before
this check succeeds.

Put exactly one European `RMHP08` disc image in `ux0:/data/mh3-wii/`:

* ISO, including a normal encrypted Wii ISO;
* RVZ, including Zstandard, Bzip2, LZMA or LZMA2 compression;
* WIA within the same decoder's format support and memory limits.

The input stays read-only and stays where it is: the game reads its partition
in place for the whole session (`RomImage`, `runtime/src/rom_image.cpp`).
The native file callback uses positioned **64-bit** reads; Rust/newlib's
32-bit file offsets are not used for the source image. Reads are serialized,
so any thread may read.

The `nod` 1.4.4 library handles compression and Wii AES/title-key decoding.
Hash reconstruction and hash validation are disabled; those paths would
create a Rayon pool. For nodtool-decrypted ISOs, the outer header's
`no_partition_encryption` byte selects the native sector reader instead,
because nod 1.4.4 assumes plain ISO sectors are encrypted. RVZ/WIA chunks
above 2 MiB are rejected before decoding.

At first start the launcher writes `ux0:/data/mh3-wii/disc/` (about 7 MB):
the boot/FST/DOL/IOS metadata and `rom.txt`, which names the image and its
size. Every start checks the metadata against the image; a moved or changed
image is processed again (the converted-asset cache is kept). A `.processing`
sibling holds work in progress, and the directory is published only after
`DiscImage` validation. A development install of a prepared disc directly in
`ux0:/data/mh3-wii/` (with `partition.bin`) is still used as is. No game
artwork, fonts, ROM bytes, or extracted resources are embedded in the program
or committed to the repository.

### Load-time texture and model conversion

Textures and models are converted while the game loads them
(`AssetCache`, `runtime/src/asset_cache.cpp`). The first game read of a
texture/model source file (every standalone BRRES, the two common and ten map
effect archives) reads the whole file, parses its textures and models, and
assigns their IDs: the next free texture and model IDs, recorded for good in
`disc/cache/index.log` with each texture's tag offset. The read returns at once
with the native texture tags patched in; the file's textures and models are
queued for three conversion workers whose affinity covers all user cores.
While a file awaits conversion its bytes stay in memory and the game's further
reads of it are served from there; past 48 MiB of pending files the game
thread waits for the workers.

Results go to `disc/cache/textures/` (one M3VT v2 pack per file; header word 4
holds the ID before its first record) and `disc/cache/models/` (M3MP/M3ID pair,
or `.skip` when the file's models cannot be converted). Files are named by the
source file's partition offset and first ID and written under a temporary name
first, so a file under its final name is complete. A texture or model consumer
that arrives before its file is converted waits for it; the loading screens
absorb that. Later launches reuse every converted file and only parse what is
new. `version.txt` names the cache format; a mismatch rebuilds the cache.

The conversions are the ones the whole-disc stages (`PrepareTextures`,
`PrepareModels`, kept for development tools) use per file: authored mipmaps
are retained, palettes resolved, CMPR converted to BC1/BC3 in the SGX Morton
layout, and model packs assembled by the bounded C assembler
(`mh3_prepare_model.c`) with validated GPU indices and palettes. Materials
bind textures of their own container only, so each file converts on its own.
BRRES source files are limited to 8 MiB and textures to 2048 pixels per
dimension; RMHP08's largest BRRES is approximately 5.4 MiB. A model
conversion workspace is about 30 MiB per worker and is released when the
queue is empty.

`10/em001/em001_shadow.brres` binds two textures (`em001_BM_01`,
`light016_SM`) that its own file does not contain; it is left out
(`.skip`), and drawing it fails at admission.

Measured on the Pi 5 (arm32 build), booting into the Alatreon quest reads
146 files (60 MiB): converting them costs about 0.35 s of CPU in total, against
about 18 s for converting the whole disc. First start went from 8 min 42 s
(decoding the 4.1 GiB partition to the SD card) to 0.25 s.

Linux uses the same code: `MH3 --rom IMAGE` writes `disc/` next to the
executable (see the README), with the ROM decoder built for the host target
by cargo.

## Navigation and menus

The UI uses a framed parchment/copper palette and a vertical selection list.
Font glyphs come from the Vita's installed Latin PVF font. CPU glyph runs are
converted directly into the admitted frame's CDRAM vertices and indices;
there is no main RAM GPU vertex buffer. The renderer uses the existing triple
buffer admission rules and skips busy GPU/display slots. It remains initialized
for the guest game's subsequent native graphics startup.

| Control | Action |
| --- | --- |
| D-pad or left stick | Move selection; held directions repeat |
| Cross / Start | Select or launch |
| Circle | Back |
| L / R | Move a page through a list |
| Left / Right in editor | Change the selected value |
| Cross on editor Name | Open the native Vita keyboard |
| Triangle in Custom quests | Edit the selected saved quest |

The main menu contains `Start game`, `Quests`, `Create custom quest` and
`Cheats`. The Quests submenu contains `Offline quests`, `Online quests`,
`Arena` and `Custom quests`. Online quests are the disc's online quest
catalogue launched locally; this menu does not establish an online service.

Before opening Quests the launcher reads MH3's actual NAND paths:

```
ux0:/data/mh3-wii/saves/nand/title/00010000/524d4850/data/nocopy/data00
...
ux0:/data/mh3-wii/saves/nand/title/00010000/524d4850/data/nocopy/data05
```

Each file must have the exact 0x14000-byte length, the in-use flag, and the
byte-sum checksum stored at +4. A selected character must have a populated
0x6000-byte record beginning at `0x48 + character * 0x6000`. All usable file
and character combinations are offered. A missing or invalid save produces
a clear message directing the user to normal `Start game` character creation
or their existing RMHP08 save. The probe never creates, repairs or overwrites
a save and does not inspect Mario Kart's `rksys.dat`.

## Custom quests and cheats

The editor starts from an existing hunt/capture quest. For ordinary quests with
one monster on the five main maps, it also offers separate map and monster
choices. A monster choice reuses a complete placement authored for that map,
along with its main objective and rewards. Map changes choose an authored base
for that map, preserving its small-monster spawn tables and supplies. The
placement source is shown in the editor. This allows new combinations of an
authored map environment, encounter, name, hunt/capture objective, time limit
and stars (1–7), without guessing monster coordinates.

The target count may be reduced within the authored objective's count. Counts
that need additional monsters are rejected. The time limit can retain the
template value or use 1–50 minutes. The editor writes private `.mh3quest`
settings under `saves/custom-quests/`;
several variants of one template can coexist, and editing updates the selected
variant. Creation is allowed before a hunter save exists; launching requires
the same valid save check as other quests.

At launch the settings are applied to a copy of the corresponding complete
disc quest file. The original IDs and file length stay fixed. `DiscImage`
applies the replacement to full and partial raw DVD reads, so the game loads
the edited quest objective and text. Both the summary objective at `+0x330` and
the live expanded objective at `+0x394` change together. When changing the
monster, old secondary objectives and their expanded records are removed.
The disc is not modified. Version 2 settings retain compatibility with the
initial version 1 template records.

Composition excludes special boss maps, quests with extra monster slots,
random invaders, and special quest modes. Those quests retain their template's
map and placements. Creation currently uses hunt/capture templates; Arena's
slay objectives are available for launching but not for composition. Arbitrary
monster combinations, coordinate editing and new spawn scripts remain outside
this initial editor's scope.
Capture requires an authored capture objective for that species with matching
placement mode bytes; targets or scripted variants without that evidence cannot
be changed to capture.

The field evidence comes from `803AE9F4` (complete eight-byte monster slot at
`+0x314`, monster/map placement lookup and spawn index), `803AEF34` (copying
three expanded objective/reward records from `+0x394`), `803AF518` (time limit
at `+0x13a`) and `803AD4E0` (eight star buckets). Composition requires the
ordinary `0x300c4/0x300c5` quest modes and preserves the base map's day/night
selection. No new game code or guessed spawn tables are generated.

Cheats currently offers two native toggles: `No damage` and
`No action stamina drain`. They call the existing gameplay ports directly and
apply to the current game launch. The stamina toggle covers the continuous
action drains implemented in `stamina.cpp`; it is not a general inventory or
save editor. Gecko code lists are not interpreted: PPC instruction patches
cannot change code that has already been statically translated. The user's
untracked `cheats.txt` and `cheats/` are untouched.

## Build and verification

The Vita build adds a Rust static library from
`runtime/vita/rom_decoder/Cargo.toml`, with its dependency versions pinned in
`Cargo.lock`. Setup needs `nightly-2025-09-28` with `rust-src` and the locked
Cargo dependencies cached. `cargo fetch --locked --manifest-path
runtime/vita/rom_decoder/Cargo.toml` can populate that cache. The build helper
uses offline Cargo, all available CPUs, and clang-21/llvm-ar-21 for codec C
sources. Existing C++ sources and GXM shaders use the port's clang/baked-GXP
build. The launcher does not compile shaders at runtime.

Focused host verification:

```
cmake --build build/linux-arm64 --target MH3LauncherTests -j$(nproc)
build/linux-arm64/MH3LauncherTests /tmp/mh3-launcher-test-data
# Optional validation against the user's external prepared game assets:
build/linux-arm64/MH3LauncherTests --quest-audit Assets/DATA
python3 scripts/test_launcher_texture_inventory.py --asset-root Assets/DATA \
  --launcher build/linux-arm64/MH3LauncherTests -v
```

The tests create synthetic assets in their own temporary subdirectory. They
verify MH3 save checksums/populated slots, bad save/quest rejection, full and
partial raw DVD quest overlays, live/summary objective consistency, compatible
authored placement transfer, smaller objective counts, incompatible-map and
scripted-mode rejection, exact ISO extraction, read-only ROM handling, and
rejection of a truncated native texture pack. The external European quest audit
passed for 107 authored base/encounter/hunt-or-capture configurations. This
checks parser and overlay consistency; it is not a gameplay completion test.

The earlier standalone-only production processor was run on the external European RVZ
and nodtool-decrypted ISO through the native C/Rust bridge. Both produced
1,522 BRRES resources and 8,394 native textures. Offline SHA-256 comparisons
matched `partition.bin`, `sys/main.dol`, `sys/fst.bin`,
`native-textures.bin` and `native-texture-map.bin` byte for byte against the
existing host pipeline. Those hashes were test comparisons only; runtime
texture binding does not use content hashes.

On the host, ISO processing took 6.94 seconds with peak RSS 31,692 KiB; RVZ
processing took 9.88 seconds with peak RSS 32,492 KiB. These measurements do
not represent Vita performance. Vita release source checks and the decoder's
Vita clang codec build passed. No launcher display/controller test on physical
hardware has been performed in this work. End-to-end gameplay remains subject
to the current native rendering migration described in
[vita-render-investigation.md](vita-render-investigation.md).

The updated production texture stage converted all 8,562 resources in an isolated
host output in 2.34 seconds with 35,000 KiB peak RSS. Its complete tag map matched
the offline inventory byte for byte. Seven focused portable parser/production
tests and the existing save/quest/ISO tests pass; independent clang 21 host and
Vita launcher objects compile. This does not measure on-device preparation or
certify the pending native model conversion stage.
