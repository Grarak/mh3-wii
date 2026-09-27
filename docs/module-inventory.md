# Disc module inventory and field layout

All 21 RSO modules are discoverable from the extracted disc, without gameplay.
`python3 scripts/inspect_disc.py` records their hashes, sections, exports,
imports and relocations in `build/disc-inventory.json`.

| Module | Code bytes | File bytes | BSS bytes |
| --- | ---: | ---: | ---: |
| `com_data.rso` | 4,420 | 380,416 | 6,748 |
| `demo_data.rso` | 92,068 | 352,480 | 3,140 |
| `em_data.rso` | 325,200 | 1,121,440 | 129,304 |
| `hbm_data.rso` | 175,144 | 310,304 | 15,144 |
| `lobby_data.rso` | 100,644 | 666,496 | 77,384 |
| `map00.rso` | 388 | 3,712 | 2,096 |
| `map01.rso` | 396 | 20,768 | 3,080 |
| `map02.rso` | 396 | 20,384 | 3,080 |
| `map03.rso` | 396 | 18,688 | 3,080 |
| `map04.rso` | 396 | 14,976 | 3,080 |
| `map05.rso` | 396 | 17,248 | 3,128 |
| `map06.rso` | 396 | 6,496 | 3,176 |
| `map07.rso` | 396 | 9,568 | 3,080 |
| `map08.rso` | 396 | 6,240 | 3,080 |
| `map09.rso` | 396 | 6,176 | 3,080 |
| `map10.rso` | 388 | 4,544 | 2,056 |
| `map11.rso` | 396 | 5,760 | 3,080 |
| `map_town.rso` | 388 | 7,168 | 2,376 |
| `map_village.rso` | 388 | 4,704 | 2,176 |
| `net_data.rso` | 19,568 | 100,896 | 70,700 |
| `quest_data.rso` | 211,560 | 659,360 | 43,264 |

The main-DOL filename pointer table starts at `0x8057D198` and lists common,
lobby, quest, enemy, demo, map00–map11, town, village and network modules.
Home Button Menu is listed separately. `LoadFieldModuleSet` (`0x80040BFC`)
loads common, quest and enemy modules in order, links all three, and invokes
their prologs. `LoadMapOverlayById` (`0x80040E4C`) uses the byte lookup table
at `0x8057D1E8` to select the map filename, replacing the previous map overlay.

For the observed offline field layout, common starts at `0x80868E40`, quest
at `0x808C76A0`, enemy at `0x80972F40`, and the next aligned free address is
`0x80AA4500`. Those addresses can be calculated from each file length and
BSS size with 32-byte alignment; the original game loader still performs
all actual loading and relocation. Offline-linked quest and enemy text are
verified against the live image before translated code runs. Loading order
and coexistence, rather than module discovery, are the remaining variables
for other modes such as network play. An inventory is not a claim that every
module or layout has been translated and tested.

The observed Moga Woods map selector is entry 6 (`map01.rso`); its bootstrap
is compiled at `0x80AA46AC`, with linked text at `0x80AA45E0` (396 bytes).
The completed native replay verifies the quest, enemy and map01 checksums,
reaches the field, and accepts movement after the first-entry tutorial.
