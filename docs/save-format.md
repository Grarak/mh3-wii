# Save file format

How Monster Hunter Tri (RMHP08) lays out its saves, as the port reads and
writes them. The files are the game's own NAND files, stored as plain host
files (see [saves on the NAND](optimizations/system/saves-nand.md)); all
numbers are big-endian.

## Files

Under the user directory (`saves/` on Linux, `ux0:/data/mh3-wii/saves/` on
the Vita, `MH3_USER_DIR` to override):

| Path | Size | Contents |
| --- | --- | --- |
| `nand/title/00010000/524d4850/data/nocopy/data00` .. `data05` | 0x14000 | save files, three characters each |
| `nand/title/00010000/524d4850/data/nocopy/system` | 0x4000 | the game's system data |
| `nand/title/00010000/524d4850/data/nocopy/network` | 0x4000 | the online data |
| `nand/title/00010000/524d4850/data/banner.bin` | 61600 | the Wii save banner |
| `nand-metadata.bin` (Vita) | | the NAND files' owners, permissions and attributes |
| `custom-quests/*.mh3quest` | | the launcher's custom quests |

The title menu's "File 1" is `data00`; `--village` and `--city` start its
first character.

## Save file (`data00` .. `data05`)

| Offset | Type | Field |
| --- | --- | --- |
| 0x00 | u32 | flags: bit 0 set when the file is in use |
| 0x04 | u32 | checksum: the sum of every byte from 0x08 to the end |
| 0x08 | | (0x40 bytes) |
| 0x48 | 0x6000 bytes | character 1 |
| 0x6048 | 0x6000 bytes | character 2 |
| 0xC048 | 0x6000 bytes | character 3 |

The launcher lists a character when the file is in use, the checksum
matches and the first word of its record is not 0. The game refuses a file
whose checksum does not match (LoadCharacter, 0x80442088): after editing any
byte, write the new sum back to 0x04.

## Character record

When a character is loaded its record is copied to the working character
(system work +0x95C, `get_userdata` 0x8004D184); the offsets below are the
same in both. Known fields:

| Offset | Type | Field |
| --- | --- | --- |
| 0x0000 | u8 | sex |
| 0x0001 | u8 | face |
| 0x0002 | u8 | hair style |
| 0x0003 | char[16] | name (with its terminator) |
| 0x0014 | u8[3] | hair colour (red, green, blue) |
| 0x0018 | s32 | money (zenny) |
| 0x001C | u32 | character id |
| 0x009E | u8 | clothing |
| 0x009F | u8 | voice |
| 0x0180 | {u16 item, s16 count}[] | item box |
| 0x0E00 | 12-byte records | equipment box (byte 0 the kind, 0: free) |
| 0x3920 | u16[32] | random seeds; the first four are the game's random streams when the character was stored |
| 0x3960 | u32[4], u8 | known combinations |
| 0x39BC | u8[4] | clothing colour (red, green, blue, alpha) |
| 0x3DE4 | u16 | the points the quest result screen adds (the Hunter Rank cheat sets it) |
| 0x3DE6 | u8 | eye colour |
| 0x3DE7 | u8 | face features |
| 0x3E00 | u8 | time of day in the village: 0 morning, 1 evening |
| 0x3E02 | u8 | hunting fleet ships available |
| 0x3E03 | u8 | Cha-Cha: bit 7 set when he comes along on quests, bits 0-6 his mask |
| 0x3F04 | s32 | Moga Woods resource points |
| 0x3F0D | u8 | 1 once the Loc Lac movie has played (first trip online) |
| 0x3F38 | 3 × 12 bytes | hunting fleet: per ship u8 catch, +2 u8 stars, +4 u8 voyage length |
| 0x4654 | u8[4] | farm field levels |
| 0x465C | 3 × 8 bytes | farm Felyne helpers: u8 work left first |
| 0x4670 | u8 | farm Felyne helpers hired |
| 0x47DA | u16[] | story flags: flag n is bit n % 16 of the word n / 16 |
| 0x4850 | u8 | the forecast: tomorrow's two large monsters, the day after's at +8 |
| 0x48DA | u16[36] | large monsters hunted, per monster |
| 0x5180 | u8 per mask, 4 apart | Cha-Cha's mask records |
| 0x51A0 | u16, u16, u8, u8, then u16 per mask | Cha-Cha's levels and experience (copied to the quest departure) |
| 0x52E2 | u16 | arena challenge bits |
| 0x53A4 | u32 | the Chamberlyne's prize money |

The story flags are read with 0x8004D2E0 and set with 0x8004D274 (their
argument is the flag number n). Examples: the village's event cutscenes set
flags 352 to 368 when they end (352: the farm's first visit, which plays
when flag 25 is set and 352 is not).

## Cha-Cha on a quest

The quest counter's accept (0x8033C5D4) copies the character's 0x3E03 and
Cha-Cha's levels into the departure block (0x8065B534; 0x3E03 goes to +8)
through 0x8033C410; the village then sets the system's Cha-Cha byte
(system work +0x7CC) from bit 7, and the field spawns him (0x802C2764) when
that byte is set. `--quest` departs through the same copy for village
quests, so a save with bit 7 of 0x3E03 set brings Cha-Cha along.

## Editing a field

```python
import struct
path = 'saves/nand/title/00010000/524d4850/data/nocopy/data00'
data = bytearray(open(path, 'rb').read())
record = 0x48                       # character 1
data[record + 0x3E03] |= 0x80       # Cha-Cha comes along
struct.pack_into('>I', data, 4, sum(data[8:]) & 0xFFFFFFFF)
open(path, 'wb').write(data)
```
