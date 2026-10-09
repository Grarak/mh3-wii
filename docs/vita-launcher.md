# Vita launcher

The normal Vita launch (no development arguments) enters a native launcher
before allocating guest RAM. `Start game` continues through MH3's ordinary
title and character menus. Quest selection, custom quest editing and cheats
are compiled into Vita release builds; they do not depend on `--quest`.

## Renderer and hand-off to the game

The launcher uses the renderer the way the game does
(`runtime/src/launcher_screen.cpp`). `StartScreen` creates the 960×544
native target without opening game assets and starts the render thread. The
launcher, on the game thread, records each frame as a packet of UI geometry
(`AllocateNativeGeometry`/`SubmitNativeGeometry`, untextured, vertex colour).
The render thread copies it into the frame's GPU slot, draws and presents.
Recording never waits for the GPU.

A Vita frame has 1 MiB of native vertex memory and a frame that does not fit
is canceled, so the launcher draws at most 9,000 rectangles (four 28-byte
vertices each) per frame. Glyph runs of the PVF font are merged vertically,
so a stem or bar is one rectangle.

System dialogs (`SceMsgDialog`, `SceImeDialog`) start on the game thread.
Aurora's GXM present calls `sceCommonDialogUpdate` on the displayed buffer
while its dialog flag is set. That flag belongs to the render thread: the
launcher switches it with a render-thread task (`RunOnRenderThread`) and
keeps presenting the menu behind the dialog. The dialog is terminated only
after the switch-off task ran, so no present can update a terminated dialog.

`Start game`, a quest or a custom quest ends the launcher (`StartGame`):

1. It presents a last "Starting Monster Hunter Tri" frame.
2. It stops the render thread, which hands the graphics context back to the
   game thread, and opens the native assets of `MH3_DISC_DIR`.
3. The game's own graphics startup (`NativeGraphicsInit`, `0x8003FD30`)
   calls `InitializeNativeGraphics` at the render mode's size. Aurora is
   already initialized, so this only reconfigures the target (resized at the
   next admitted frame) and starts a new render thread. The frame packet
   sequence continues across the restart.

Until then the last launcher frame stays on the display. Errors after the
hand-off go to `main` instead of a dialog. Native audio starts later, from the
game (`StartNativeAudio`); the launcher does not touch it.

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
* WIA with the same compressions (not Purge).

The input stays read-only and stays where it is: the game reads its partition
in place for the whole session (`RomImage`, `runtime/src/rom_image.cpp`).
The native file callback uses positioned **64-bit** reads; newlib's 32-bit
file offsets are not used for the source image. Reads are serialized, so any
thread may read.

Without an image or prepared game data the launcher shows `Game data is
missing. Copy your European Monster Hunter Tri (RMHP08) disc image (ISO, RVZ
or WIA) to ux0:/data/mh3-wii, then start Monster Hunter Tri again.` and
exits after OK. Two images give `There is more than one disc image in ...`,
and an image that cannot be decoded `The disc image could not be read:
<reason>`.

The native decoder ([disc image decoder](rom-decoder.md),
`runtime/src/rom_decoder.cpp`) handles compression and Wii AES/title-key
decoding; partition hashes are neither rebuilt nor validated. For
nodtool-decrypted ISOs, the outer header's `no_partition_encryption` byte
selects `RomImage`'s own sector reader. RVZ/WIA chunks above 2 MiB are
rejected before decoding.

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
`disc/cache/index.log` with each texture's tag offset. The read gets the bytes
with the native texture tags patched in; the file's textures and models are
queued for three conversion workers whose affinity covers all user cores (on
the Vita only one of them converts models at a time). While a file awaits
conversion its bytes stay in memory and further reads of it are served from
there; past 16 MiB (Vita) or 48 MiB (other hosts) of pending files a read
waits for the workers. The disc's audio streams are copied to plain files in
`disc/cache/files/` the same way ([native audio](native-audio.md#streams-from-the-disc-image)).

The game thread does not do this work. The DVD library reads through
`IOS_IoctlAsync` (DI 0x71); those reads run on a disc reader thread
(`DiscReader`, `runtime/src/ios.cpp`, core 0 on the Vita), as a Wii drive
reads while the game runs, and their replies reach the library's callback at
a later pump. A read is answered only once the files it touches are
converted (`AssetCache::Converted`), so nothing that looks their packs up
later waits: not the loading screen's draw-table preparation, not model or
texture binding. The game's file loader (`load_file_req` 0x800CEB14, served
once a frame by 0x800CE704) issues a read and polls for its reply each
frame, so its loading screens keep animating through the conversions. Its
synchronous loads (`load_file` 0x800CECC8) spin in `WaitDvd` (0x80435EA8,
native) until the drive is idle, as on the Wii; while the drive is busy the
port delivers device work and sleeps until the reader's next reply, since
the retrace that refreshes the drive status it polls is only delivered at
pump points.

Results go to `disc/cache/textures/` (one M3VT v2 pack per file; header word 4
holds the ID before its first record) and `disc/cache/models/` (M3MP/M3ID pair,
or `.skip` when the file's models cannot be converted). Files are named by the
source file's partition offset and first ID and written under a temporary name
first, so a file under its final name is complete. A texture or model consumer
that still arrives before its file is converted (a read outside the DVD
library) waits for it; development builds log waits over 0.5 s. Later
launches reuse every converted file and only parse what is new. `version.txt` names the cache format; a mismatch rebuilds the cache.

The conversions are the ones the whole-disc stages (`PrepareTextures`,
`PrepareModels`, kept for development tools) use per file: authored mipmaps
are retained, palettes resolved, CMPR converted to BC1/BC3 in the SGX Morton
layout, and model packs assembled by the bounded C assembler
(`mh3_prepare_model.c`) with validated GPU indices and palettes. Materials
bind textures of their own container only, so each file converts on its own.
BRRES source files are limited to 8 MiB and textures to 2048 pixels per
dimension; RMHP08's largest BRRES is approximately 5.4 MiB. A model
conversion workspace is about 30 MiB and is released when the queue is
empty.

`10/em001/em001_shadow.brres` binds two textures (`em001_BM_01`,
`light016_SM`) that its own file does not contain; it is left out
(`.skip`), and drawing it fails at admission.

Measured on the Pi 5 (arm32 build), booting into the Alatreon quest reads
146 files (60 MiB): converting them costs about 0.35 s of CPU in total, against
about 18 s for converting the whole disc. First start went from 8 min 42 s
(decoding the 4.1 GiB partition to the SD card) to 0.25 s.

Linux uses the same code: `MH3 --rom IMAGE` writes `disc/` next to the
executable (see the README), with the same decoder.

## Navigation and menus

The UI uses a framed parchment/copper palette and a vertical selection list.
Font glyphs come from the Vita's installed Latin PVF font, drawn as solid
rectangles (see [the renderer section](#renderer-and-hand-off-to-the-game)).
Every page has the same layout: the title bar, up to seven rows, a detail
line about the selected row, and the page's buttons in the footer. A list
longer than seven rows shows the position (`12 / 28`) in the title bar and
marks above and below the rows when more are hidden there. The messages and
progress screen before the menu draw over the same empty page.

| Control | Action |
| --- | --- |
| D-pad or left stick | Move selection; held directions repeat |
| Cross / Start | Select, toggle or launch |
| Circle | Back to the previous page, on the row it was entered from |
| L / R (L1/R1 or L2/R2) | Move seven rows (a page) through a list |
| Left / Right in editor | Change the selected value |
| Cross on editor Name | Open the native Vita keyboard |
| Triangle in Custom quests | Edit the selected saved quest |

The pad is read with `sceCtrlPeekBufferPositive2`, port 1 first and port 0
when that fails, as the game's own input reads it (SDL's Vita driver, and
the Classic Controller mapping's `vita_controller.cpp`): a PS TV controller
or the devkit harness's CTRLP pad drives the menus too. With this call the
handheld's L/R report as L1/R1.

The main menu contains `Start game`, `Quests`, `Create custom quest`,
`Cheats` and `Widescreen`. `Widescreen` toggles the game's 16:9 mode for this
launch (on by default; `MH3_WIDESCREEN=0` starts it off) through
`SetWidescreenSetting`; the game reads it once, at `RenderInit`, after the
hand-off. Off, the game runs in 4:3 on a pillarboxed 777×544 target
([vita-widescreen.md](vita-widescreen.md)). The Quests submenu contains `Offline quests`, `Online quests`,
`Arena` and `Custom quests`. Online quests are the disc's online quest
catalogue launched locally; this menu does not establish an online service.
The row reads `Widescreen: ON (16:9)` or `Widescreen: OFF (4:3)` and changes
at each press; on, off and on again starts the title in 16:9 (checked on the
host with `widescreen-toggle-start.tsv`).

`Start game` goes through the game's own intro movie and title. Only the
Classic Controller's A (the Vita's Circle) skips the intro: Plus (Start) does
not, which is the game's own behaviour. On the host three Plus presses during
the intro left it playing, one A press went to the title at once.

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

The editor's rows are Name, Map, Monster, Objective, Target count (`1 of 3`:
the authored count is the most), Time (`as the template` or 1–50 minutes),
Stars and Save quest. A template that cannot be composed shows its map and
monster as `(fixed)`. Of the European disc's 28 offline hunt/capture
templates 11 can be composed (the first in the list is `Save Our Boat`), of
the 64 online ones 13. A new quest is named `Custom <template>`; until the
name is typed over it follows the quest the placement comes from. The editor
title reads `Create custom quest` or, from Triangle in the custom quest list,
`Edit custom quest`; Save quest and Circle return to the list it was opened
from (Save quest of a new quest to the main menu). The custom quest list's
detail line summarizes the selected quest (`Sandy Plains: Hunt 1 Diablos,
4*.`).

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

Cheats lists the codes of the RMHE08 (US) Gecko code lists, ported to this
EU disc, in groups by the lists' author tags: Basic (`No damage`,
`No action stamina drain`, `All items in the item box` and
`All equipment in the equipment box`, the launcher's own), Stuff (43),
Seth@WiiPlaza (10), Bully@Wiiplaza (30), Skiller (80), Argon (6) and
Doudley (88), 257 codes and the four Basic entries. The
Cheats page shows how many codes of each group are on;
a group's page lists its codes with On/Off and, where a code has a value,
the value in brackets. X toggles the selected code and Left/Right changes its
value: an option (an item, a multiplier, a range), the buttons of a code
that left them to the user (L+R or SELECT with a d-pad direction, or
`Always` where the code also runs without them), or for the name code the
name, typed on the system keyboard. The main menu's
detail line names the active codes. Toggles apply to the current launch, in
development and release builds. `MH3 --cheat <name>[=<value>]` (repeatable,
with any other option) turns one on from the command line: letters and
digits of the name count, `Group:name` picks between lists that share a
name, and the value is an option's label, a number or the name.

No Gecko code is interpreted at runtime: PPC instruction patches cannot
change code that has already been statically translated. Each code is native
code instead (`runtime/src/cheat_*.cpp`, the table in `cheats.cpp`): writes
to the game's data at the EU addresses once per logic step, as the code
handler wrote them once per frame; hooks in the native ports of the patched
functions (health, stamina, timers, the quest clock, the jump and run
actions); or wrappers registered at a translated function's address that
change its arguments, a small-data constant it loads, or its result. The 20
codes that only work online (cities, lobby, chat, friends, servers, quest
board) and Bully's button activator template are not in the table. The
user's untracked `cheats.txt` and `cheats/` are only read while porting;
the build does not use them.

`All items in the item box` fills the box once a launch, when a character
first loads (PlLoadUserData, 0x80049FE0). The box is the save data's
(u16 item, s16 count) slots from +0x180, 100 a page (ItemBoxSlots,
0x8004AED4). BoxPages (0x8004AE6C) gives 4 pages, 6 once the Organizer
Guide (item 0x244) was obtained and 2 more with the Pack Rat Guide (0x245):
the obtained bits at the save data's +0x4AD8 (ItemObtained 0x8004D38C,
ItemObtainedSet 0x8004D318), which adding an item to the pouch or the box
sets. The cheat sets both bits, as getting the two guides does, so the box
has its largest 8 pages (800 slots, up to the equipment box at +0xE00); it
empties them and stores every item through the game's ItemBoxAdd
(0x8004BF08: at most 99 a slot, the item marked obtained), the count
chosen (99, 10 or 1) each. The item table (GetItemData 0x8029F740,
ItemName 0x8029F68C) has items 1 to 0x2EA; ItemNotStorable (0x8004B098)
refuses 223 No Item, 438 BBQ with Mask and 439 No Mask, and 30 entries
named `ERR<n>` are placeholders, which leaves 713 items in slots 0 to 712.

`All equipment in the equipment box` fills that box at the same point. The
equipment box is the save data's 12-byte records from +0xE00 (u8 kind, u8 an
armor piece's upgrade level or a talisman's slots, u16 id, u16 an armor
piece's pigment or a talisman's skill points, u16[3] the decorations in the
slots, a talisman's skills after its slots), as many as the item box has
slots (EquipBoxSlots 0x8004AEFC, the same BoxPages): the cheat sets the two
guides' bits too, for 800 slots. That is the most the box can have: its
records end at the 20 equipment sets (+0x3380, the slots of a set's pieces)
and the bowgun sets (+0x36C8) that follow it in the save data's 0x6000
bytes, and about 60 translated functions (the box's own, the box menus, the
sets, the smithy) address a slot as +0xE00 plus 12 times its index. Kinds: 1
chest, 2 arms, 3 waist, 4 legs, 5 head, 6 talisman, 7 great sword, 8 sword
and shield, 9 hammer, 10 lance, 11 bowgun frame, 12 barrel, 13 stock, 14
long sword, 15 switch axe; each kind's id count is at 0x80570E80. What the
hunter wears (the slots at +0x8C to +0x9C) stays; every other record goes
through EquipBoxRemove (0x8004C744), which frees a slot as selling does and
clears the sets that use it. The weapons and armor then come from the game's
EquipBoxAdd (0x8004C578, what the smithy, the shops and quest rewards call:
level 0, no decorations, the piece marked obtained) and the talismans from
TalismanToBox (0x8004C620). Every armor piece (643) counts, and every weapon
but the 137 named `ダミー` (dummy), frame 16 (`Dummy`) and barrels and stocks
17 to 19, which belong to the three frames that take no parts (384). For
talismans, the generator (TalismanGenerate 0x8027F7DC) gives at most +10 a
skill, and slots and skills share three entries; its skill lists
(0x805C6B88) hold 80 skills, so the box gets a Queen Talisman with each
skill at +10 and two slots. That is 1107 records for at most 800 slots: the
talismans come first, then the pieces by rarity (Get_equip_rare 0x8027FDE0),
the highest first, armor before weapons at the same rarity, and the box gets
those that fit in kind and id order (armor, talismans, weapons). With
nothing but a weapon worn, that is every armor piece of rarity 3 to 7 (RARE
as the box shows it; 487), the 80 talismans and 232 weapons: rarity 4 to 7
of every kind and rarity 3 of the great swords, swords and hammers.

The lists' authors copied each other's codes, so 31 of the 288 ported codes
did the same for the player as another code (the same patched instruction,
the same memory write, or the same visible effect). Each such set is one
entry, with the implementation that hooks the game's own check, offers a
value or works without a precondition; a code that always runs and its
copy switched or held by buttons became one entry whose buttons have an
`Always` choice (the default):

| Entry (group) | Merged into it |
| --- | --- |
| No damage (Basic) | Invincible players (Doudley) |
| Infinite health (Skiller, was Inf HP) | Infinite health (online code) (Seth) |
| Health never changes (Seth, was Toggle infinite health) | Infinite health (Bully; it also lowered the maximum to the health) |
| Sharpness stays full (Skiller, was Weapon always sharp) | Weapons always sharp (Bully) |
| Set unlimited traps (Bully) | Set unlimited traps (Argon) |
| Set unlimited barrel bombs (Bully) | Set unlimited barrel bombs (Argon) |
| Show all monsters on the map (Stuff) | Show all monsters on the map (Doudley) |
| Action repeater (Bully; a `START` choice) | Action repeater (Doudley) |
| Enable armor pigment for all armor (Stuff) | Enable armour pigment for all armors (Doudley) |
| Equip blade/gunner armor (Stuff) | Enable both armor type for all armors (Doudley) |
| Never stunned (Skiller, was Never stund) | Never stun (Skiller) |
| No knockback (Stuff) | Unknockable players (Doudley) |
| No reload (Skiller) | Infinite ammo (Bully) |
| Gain/use items/ammo get 10 (Skiller) | Gain/use items/ammo get 10 (buttons on/off) (Skiller) |
| Infinite time arena and quests (Argon) | Inf time arena/quests (Skiller) |
| 1 hit kills (Skiller) | (Alt) 1 hit kills (buttons on/off), (Alt) 1 hit kills enemys, Enable 1 hit kills (enemy) (Skiller) |
| Enemy has 1 HP (Skiller) | Enable 1 HP (enemy capture/cutting parts mode), Normal enemy HP (the buttons switch both off) (Skiller) |
| Unlimited hot drink, Unlimited cold drink (Argon) | Hot drink lasts forever, Cold drink lasts forever (Bully) |
| Autofail, Autoclear (main and subs) (Stuff) | Autofail at will, Autoclear at will (Stuff) |
| Rapidfire all ammo (Doudley) | Rapidfire all ammo (hold shoot button while firing) (Doudley) |
| Range for all bowgun barrels (Doudley) | Range 3.00 for all bowgun barrels (Doudley) |
| Infinite money (Skiller, was Max/inf money) | Gain/use money > max money (Skiller), Infinite funds (Bully) |
| Infinite stamina (Skiller, was Inf stamina) | Infinite stamina (Bully) |
| Switch axe power gauge always full (Doudley) | Thunder weapons always charged (Bully; it filled the same gauge) |
| Character name change (Bully, was Profile name modifier) | Name change (Skiller) |

Look-alikes that differ stay apart: No damage (no damage taken), Infinite
health (refilled each step), Health never changes (no damage and no
healing) and Alternate infinite health (a hit that would empty the health
still does); Seth's Weapons always sharp (best version) (keeps the sharp
levels set when the weapon is drawn, the sharpness still wears) and Weapons
ultra sharp; Inf ammo (the pouch) and No reload (the loaded rounds); the two
freeze codes (Freeze/unfreeze monsters stops their step, switched; Freeze
monsters slows their next motion while held); the speed codes (other
values, buttons or places); Infinite ressources and Max res on converting
(when shown and on a change); Item pouch items x10 and Gain/use items get
10 (when listed and on a change); All boxes have 8 pages (the menus) and
Item/equip boxes have 8 pages (the box's slots); Infinite items and
Infinite bombs in pouch; Increase oxygen bar and Inf air.

## Build and verification

The disc image decoder is C++ with vendored C codecs, built like the rest of
the game; no Rust toolchain is needed. The C++ sources and GXM shaders use the
port's clang build and the offline [shader bank](vita-shader-bank.md). The
launcher does not compile shaders at runtime.

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
rejection of a truncated native texture pack. `--quest-audit` checks the
parser and quest overlays against every authored base/encounter/hunt-or-capture
configuration of the external disc (107 on the European disc).

## Devkit testing

`scripts/dev/devkit-heartbeat.sh RUN --launcher` launches `host0:/game/eboot.bin` without
arguments. Without `MH3_DISC_DIR`, the launcher checks `ux0:/data/mh3-wii`
itself; the devkit still holds an older prepared install there
(`partition.bin`, `native-textures.bin`) that this check would use as is, so
test runs set `MH3_DISC_DIR=ux0:/data/mh3-wii/disc`. Menu input comes from
`MH3_LAUNCHER_INPUT` (development builds): lines `FRAME BUTTONS`, each holding
its buttons from that launcher frame on (names joined by `+`, `none`, or
`SCE_CTRL` bits). While a script runs, dialogs close after 2.5 s. For example,
Quests > Online quests > three pages down > quest 10057:

```text
120 down
124 none
170 cross
174 none
220 down
224 none
270 cross
274 none
320 r
324 none
370 r
374 none
420 r
424 none
480 cross
484 none
```

The launcher logs its pages, inputs, dialogs and
`starting the game (...) after N launcher frames`; the harness counts readiness
frames from that point. `--pad-script` sends timed CTRLP `pad` states, which the
game sees (Circle, the Classic Controller's A, skips the intro movie:
`scripts/launcher-input/skip-intro.pad`); pad entries not yet sent when the
readiness count is reached are dropped. On the host, `MH3 --launcher-screen-check`
draws launcher frames through the render thread, hands off, restarts the
renderer at the game's 640×456 render mode (764×544 on the 960×544 display) and
checks pixels and the target size.

Verified on the devkit: the menu at full screen, script navigation with L/R
paging, the no-save message dialog over the menu, the hand-off into a quest
(10057, replayed movement) and into Start game's intro and title, and
Widescreen off in the menu giving a 4:3 pillarboxed title. The render-thread
restart and target resize work; there is no hand-off stall. Two things seen
in those runs belong to the game:

- **Shader program creation.** At a quest's first model draws the render
  thread creates about 25 GXM programs. When they were compiled at runtime
  and cached as files, the screen held the loading screen for 20–25 s. They
  now come from the embedded [shader bank](vita-shader-bank.md), which needs
  no compile or file access (`oct03-head-bank-1`: none at runtime).
- **The THP intro** decodes on the game thread at about 320 ms per step (about
  3 logical fps). Its colours were fixed afterwards (3e4fabc: the YUV planes
  are converted to RGB when the texture is made); the devkit has not shown the
  intro since. Running the vprof register profiler during the intro stopped
  the process's output, so profile after it: skip it with Circle
  (`--pad-script scripts/launcher-input/skip-intro.pad`) and set
  `--ready-copies` past it (1100 frames after the hand-off).

Development builds report a render thread that makes no progress for two
statistics intervals: `MH3 render thread: no progress for N reports in draw
of packet P, record R kind K` (or the task stage). A slow but moving freeze
does not trigger it.

Input scripts are in `scripts/launcher-input/`:

| Script | Route |
| --- | --- |
| `quest-10057.tsv` | Quests > Online quests > three pages (R) > quest 10057 |
| `no-save-then-start.tsv` | Quests without a save (message), then Start game |
| `widescreen-off-start.tsv` | Widescreen off, Start game (4:3 title) |
| `widescreen-toggle-start.tsv` | Widescreen off, on again, Start game (16:9 title) |
| `tour-custom-quest.tsv` | Cheats on, a custom quest from `Save Our Boat` with another map and monster (keyboard opened on Name), saved, edited from the custom quest list and saved again, launched; needs exactly one hunter |
| `name-keyboard.tsv` | Ends on the editor's Name with the keyboard open, for `name-keyboard.pad` |
| `hold-dialogs.tsv` | Nothing; only keeps dialogs closing by themselves (missing-disc message) |
| `skip-intro.pad` | CTRLP: Circle (A) after the hand-off skips the intro |
| `ctrlp-quest-10057.pad` | CTRLP: `quest-10057.tsv`'s route with the pad alone |
| `name-keyboard.pad` | CTRLP: types into the keyboard, confirms with Start, saves, closes the message |

## Development hosts: `MH3 --launcher`

Development builds on Linux (`MH3_DEVELOPMENT=ON`, with Aurora) run the same
launcher code (`runtime/vita/launcher.cpp`) with `MH3 --launcher`, then the
game as the launcher starts it. `MH3_DISC_DIR` is the launcher's directory as
on the Vita: prepared game data (such as the `disc/` that `--rom` writes), or
a directory with one disc image, prepared into its `disc/`; an empty
directory gives the missing-disc message. `MH3_USER_DIR` holds the saves and
`custom-quests/` (default `saves`). Differences from the Vita:

- the font is a system TrueType font (Liberation Sans or DejaVu Sans) through
  imgui's `stb_truetype`, at the same 18-pixel em as the PVF font;
- messages draw as a panel over the page and close with Cross, Circle or
  Start (after 2.5 s while an input script runs);
- the keyboard (quest names, the name cheat) is the Vita's: on the host
  the name keeps its value;
- no kubridge check.

Input is `MH3_LAUNCHER_INPUT`, or the host's controls read as the game's
Classic Controller (`ReadLiveController`), by button position: B is Cross, A
Circle, X Triangle, Y Square, Plus Start, L/R L/R (keyboard: arrows, Left
Shift for Cross, Space for Circle, Enter for Start, Q/R for L/R;
[vita-controls.md](vita-controls.md)). For example, on the VM:

```
MH3_BIN=build/linux-arm64-dev/MH3 MH3_ARGS=--launcher CAPTURE_INTERVAL=60 \
  scripts/dev/run-heartbeat.sh tour 75 MH3_AUDIO_NULL=1 XAUTHORITY=... \
  MH3_DISC_DIR=$PWD/build/disc MH3_LAUNCHER_INPUT=$PWD/scripts/launcher-input/tour-custom-quest.tsv
```

Frame dumps count frame packets, which keep counting after the hand-off: the
intro and title run thousands of packets per second on the VM, so keep such
runs short. Results (VM, llvmpipe, October 2026):

| Run | Result |
| --- | --- |
| `--launcher-screen-check` | Passes (target 960×544, then 764×544 after the game's startup). |
| Empty `MH3_DISC_DIR`, `hold-dialogs.tsv` | The missing-disc message over the empty page, closed after 2.5 s; exit 0. |
| Directory with only the RVZ | `disc/` written (under a second), then the menu. |
| `tour-custom-quest.tsv` | Cheats toggled, the editor changed map (Flooded Forest to Sandy Plains) and monster (Barroth to Diablos), saved, edited and saved again (`0-1027-1.mh3quest`), launched: the quest loads into Sandy Plains with the HUD. |
| `widescreen-toggle-start.tsv` | Row text OFF (4:3), then ON (16:9); the intro and the title in 16:9 on the whole display. Plus during the intro: no skip; A: skip. |

## Devkit checklist

Not run yet on hardware: the custom quest tour (editor, IME keyboard, save,
saved quest launch, cheats), the missing-disc message, the launcher reading
CTRLP, re-enabling Widescreen, and a release build of the launcher. Run each
through `scripts/dev/devkit-heartbeat.sh` (main session only), in a new
directory `build/vita-devkit/RUN` staged like this:

```
build/vita-devkit/RUN/host0/game/eboot.bin           # build/vita/eboot.bin
build/vita-devkit/RUN/host0/game/runtime.env
build/vita-devkit/RUN/host0/game/launcher-input.tsv  # the run's script
build/vita-devkit/RUN/host0/game/test-saves/nand/title/00010000/524d4850/data/nocopy/data00
                                                      # fixtures/post-chief-data00.bin (one hunter)
```

with `runtime.env`:

```
MH3_STATS=1
MH3_DISC_DIR=ux0:/data/mh3-wii/disc
MH3_USER_DIR=host0:/game/test-saves
MH3_LAUNCHER_INPUT=host0:/game/launcher-input.tsv
```

Build: `scripts/build-vita.sh` (development, the default). Release check:
`MH3_VITA_BUILD_DIR=build/vita-release MH3_DEVELOPMENT=OFF scripts/build-vita.sh`
(the release eboot has no input script code).

1. **Tour** (`launcher-tour-1`): `tour-custom-quest.tsv`;
   `scripts/dev/devkit-heartbeat.sh build/vita-devkit/launcher-tour-1 --launcher --timeout 600`.
   Expect in the log: `cheat No damage on`, `cheat No action stamina drain on`,
   `quest-name keyboard open` with the keyboard over the editor on HDMI for
   2.5 s, `custom quest saved: Custom The Horned Dragon (template 1027,
   encounter 1046, 4 stars)` twice, each with the `saved` message dialog,
   then `starting the game (custom quest Custom The Horned Dragon on template
   1027, File 1 / Character 1)`, and after the GXM program freeze Sandy
   Plains. `host0/game/test-saves/custom-quests/0-1027-1.mh3quest` holds the
   quest. Check the menu pages on HDMI: footer, detail line, `n / 28` in the
   template list.
2. **Keyboard** (`launcher-keyboard-1`): `name-keyboard.tsv` and
   `--pad-script scripts/launcher-input/name-keyboard.pad --timeout 120`.
   The script ends with the keyboard open, so the keyboard stays until the
   pad confirms. Expect `quest name: ...` (typed name) and `custom quest
   saved: <that name>`, and the message closing on the pad's Cross. If the
   keyboard opened later than 45 s after the launch (`quest-name keyboard
   open` in `events.jsonl`), shift the pad script. If the IME or the message
   ignores CTRLP, the log stops at `quest-name keyboard open` or at the
   message: report it; system dialogs then need the touch screen or a person.
3. **CTRLP** (`launcher-ctrlp-1`): no `MH3_LAUNCHER_INPUT` line in
   `runtime.env`; `--pad-script scripts/launcher-input/ctrlp-quest-10057.pad
   --timeout 600`. Expect `input frame N buttons 0x40` (down), `0x4000`
   (cross) and `0x800` (CTRLP `r`, R1) lines and `starting the game (quest
   10057 ...)`. If `menu ready` comes after 30 s, shift the pad script.
4. **Missing disc** (`launcher-nodisc-1`): `MH3_DISC_DIR=host0:/game/empty`
   (stage the empty directory), `hold-dialogs.tsv`, `--launcher --timeout 60`.
   Expect `message: Game data is missing. Copy your European Monster Hunter
   Tri (RMHP08) disc image (ISO, RVZ or WIA) to host0:/game/empty, then start
   Monster Hunter Tri again.` over the empty parchment page, the dialog
   closing after 2.5 s and the process ending (no hand-off).
5. **Widescreen on again** (`launcher-widescreen-1`):
   `widescreen-toggle-start.tsv`, `--pad-script
   scripts/launcher-input/skip-intro.pad --ready-copies 1100`. Expect
   `widescreen off`, `widescreen on`, and the title at 960×544 without bars.
6. **Release** (`launcher-release-1`, optional): the release eboot, no
   `MH3_LAUNCHER_INPUT`, `--pad-script
   scripts/launcher-input/ctrlp-quest-10057.pad --timeout 240`. A release
   build logs nothing, so the harness sees no hand-off: judge by HDMI (menu,
   navigation, quest 10057's loading screen).
