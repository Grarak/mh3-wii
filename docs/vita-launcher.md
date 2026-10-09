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

The input stays read-only. Processing runs automatically when a complete
prepared disc is absent. A core 0 worker decodes the partition with a bounded
sector buffer while the core 2 UI draws the current stage and percentage.
The native file callback uses positioned **64-bit** reads; Rust/newlib's
32-bit file offsets are not used for the source image.

The `nod` 1.4.4 library handles compression and Wii AES/title-key decoding.
Hash reconstruction and hash validation are disabled; those paths would
create a Rayon pool. For nodtool-decrypted ISOs, the outer header's
`no_partition_encryption` byte selects the native sector reader instead,
because nod 1.4.4 assumes plain ISO sectors are encrypted.

The output is `ux0:/data/mh3-wii/disc/`. A `.processing` sibling holds work in
progress. The processor preserves all partition bytes, including padding,
extracts boot/FST/DOL/IOS metadata, inventories BRRES resources, and converts
all static texture assets. It retains authored mipmaps, resolves palettes,
and uses the host packer's verified CMPR-to-BC1/BC3 and SGX Morton layout.
No game artwork, fonts, ROM bytes, or extracted resources are embedded in the
program or committed to the repository.

The final directory is published only after `DiscImage` validation and native
pack/table extent validation succeed. A truncated partition, pack or table is
not ready. Interrupted staging is regenerated on the next attempt; an
incomplete previously published output is preserved as an `.incomplete`
sibling. Existing prepared data directly under `ux0:/data/mh3-wii/` remains
supported. Missing/broken native packs there can be regenerated from their
original partition; old generated packs are preserved as `.previous` files.

For this disc the converted partition is 4,286,328,832 bytes and the native
texture payload is 804,292,160 bytes. Leave room for both outputs in addition
to the original ROM. Recovery copies from interrupted older preparations may
require additional space.

RVZ/WIA chunks above 2 MiB are rejected before decoding. BRRES source files
are limited to 8 MiB and textures to 2048 pixels per dimension. RMHP08's
largest BRRES is approximately 5.4 MiB. Decoder buffers are released before
texture conversion. These are processing limits, not claims about measured
Vita memory use or throughput.

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

The editor creates variants of existing hunt/capture quest templates. Choosing
the template chooses its map, monsters, spawn tables, supplies and rewards.
The editor changes the quest name, hunt/capture objective and stars. The target
count follows the template's spawn configuration. It writes private `.mh3quest` settings under `saves/custom-quests/`;
several variants of one template can coexist, and editing updates the selected
variant. Creation is allowed before a hunter save exists; launching requires
the same valid save check as other quests.

At launch the settings are applied to a copy of the corresponding complete
disc quest file. The original IDs and file length stay fixed. `DiscImage`
applies the replacement to full and partial raw DVD reads, so the game loads
the edited quest objective and text. The disc is not modified. This is a
template editor, not a map/spawn-script editor. Settings that increase the
target count beyond the template's main objective are rejected because they
would require additional spawn scripting.
Capture is offered only for monster targets that appear in an authored
capture objective elsewhere on the disc. This prevents introducing capture
objectives for targets with no verified capture support.

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
```

The tests create synthetic assets in their own temporary subdirectory. They
verify MH3 save checksums/populated slots, bad save/quest rejection, full and
partial raw DVD quest overlays, exact ISO extraction, read-only ROM handling,
and rejection of a truncated native texture pack.

The production processor was also run on the actual external European RVZ
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
