# Compressed and exact small texture formats

## What the original does

Most of MH3's textures are CMPR, GX's 4×4 block format: DXT1 with
big-endian colour endpoints, the texel indices of each row in reverse
order, and the blocks grouped in 8×8 tiles of four. GX samples it directly
from RAM at 4 bits a texel. The rest use GX's other formats: intensity (I4,
I8), intensity with alpha (IA4, IA8), colour index with a palette (CI4,
CI8), RGB565, RGB5A3 and RGBA8.

## What the port does instead

The load-time converter ([asset conversion](load-time-asset-conversion.md),
`runtime/src/launcher_textures.cpp`) converts each texture once, the first
time the game loads its file, and stores it in the file's pack in the disc
cache; later loads read the pack. It never stores a texture larger than it
needs:

- **CMPR stays block-compressed.** The endpoints are byte-swapped and each
  index row is reversed: the block becomes a BC1 block with the same
  endpoints and indices, so nothing is re-encoded. A texture with
  transparent texels (CMPR's three-colour mode with index 3 in use) becomes
  BC3 instead: an alpha block of 0 for those texels and 255 elsewhere,
  followed by the colour block. CMPR's transparent texels keep the midpoint
  colour; BC1's punch-through alpha would give them black, which bilinear
  filtering bleeds into the edges of leaves and hair. The SGX applies the
  three-colour endpoint rule to BC3's colour block too.
- **Every other format takes the smallest GXM format that samples every
  texel unchanged**, chosen from the decoded texels of all levels: `P4` or
  `P8` with a 16- or 256-entry palette in CDRAM when the texture uses that
  few colours, `U8` when the texels are grey or alpha-only (`U8_RRRR`,
  `U8_1RRR`, `U8_R000`, `U8_R111`), `U8U8_GRRR` for grey with alpha,
  `U5U6U5_RGB` for opaque 5:6:5 colours, else RGBA8. Equal sizes prefer the
  format without a palette. CI4/CI8 thus stay paletted, IA4/IA8 become
  palettes or two channels, and many RGB5A3 textures, whose few colours the
  artists painted, become palettes.
- **Swizzled formats at any size.** Block and palette formats are swizzled
  (Morton order). GXM's arbitrary swizzle
  (`sceGxmTextureInitSwizzledArbitrary`) takes sizes other than powers of
  two, addressed and stored as the size rounded up to powers of two; all of
  MH3's such textures have one level, which is the only case the layout
  allows. An 854×480 CMPR texture is 256 KiB as BC1 in a 1024×512 footprint
  instead of 1.6 MiB as RGBA8.

The format choice runs twice per texture with the same result: once to
plan the pack's layout (`TexturePayloadBytes`), so the conversion workers
can write each payload in place, and once to convert it. The texture packs
carry their own format version (`textures/format.txt` in the cache), so a
converter change rebuilds only them.

## Why

A texture fetch reads its texels from memory; at 4 bits a texel a BC1
texture moves an eighth of an RGBA8 one's bytes through the memory bus and
the texture cache, BC3 and P8 a quarter, U8 a quarter, U8U8 and RGB565
half. Smaller textures also decide whether a map fits in CDRAM at all
([CDRAM budget](../platforms/vita-cdram-budget.md)).

Every texture on the disc (8,562 in all files), converted:

| Wii format | Textures | Converted to | Before (MiB) | After (MiB) |
| --- | ---: | --- | ---: | ---: |
| CMPR | 7,284 | BC1 5,495, BC3 1,789 | 390.5 | 390.5 |
| CMPR, not a power of two | 115 | BC1 94, BC3 21 | 107.3 | 19.9 |
| CI8 | 155 | P8 | 93.5 | 30.2 |
| IA4 | 137 | P4 73, P8 59, U8U8 5 | 31.7 | 9.3 |
| IA8 | 461 | U8 250, U8U8 174, P8 37 | 31.6 | 10.6 |
| I8, I4 | 107 | U8 93, P4 14 | 13.5 | 3.1 |
| RGB565 | 23 | RGB565 13, P8 10 | 3.3 | 1.3 |
| RGB5A3 | 111 | RGBA8 75, P8 34, P4 2 | 33.2 | 28.8 |
| RGBA8 | 169 | RGBA8 153, U8U8 11, U8 5 | 64.8 | 64.2 |
| All | 8,562 | | 769.4 | 558.0 |

"Before" is BC1/BC3 for power-of-two CMPR and RGBA8 for the rest; decoding
every texture to RGBA8 would take 2.9 GiB. `MH3LauncherTests
--check-textures DISC_DIR` converts every texture, decodes each payload the
way the GPU samples it and compares it with the GX decode: the exact formats
match every texel, BC1/BC3 differ only in CMPR's in-between colours, which
GX blends 3:5 and BC1 1:2.

## Example

The format choice, `runtime/src/launcher_textures.cpp`:

```cpp
if (Compressed(s)) { // CMPR at a size GXM swizzles
    const bool alpha = has_transparency(s, bytes);
    r.format = alpha ? 3 : 2; // BC3, BC1
    out.payload = compressed(s, bytes, alpha);
} else {
    Palette palette;
    r.format = ExactFormat(s, converted.data.data(), palette); // P4, P8, U8, U8U8, RGB565 or RGBA8
    out.payload = native(s, converted.data.data(), r.format, palette);
}
```

## Applying it to another Wii game

Generic: all of it. CMPR is GX's only compressed format, and Wii games use
it for most of their colour textures: keep the blocks instead of decoding
them, since re-encoding decoded texels to BC1 loses quality and costs CPU at
load time, and use BC3 only where CMPR's transparent texels occur. For the
other formats, choose from the decoded texels rather than from the GX
format: an RGB5A3 or RGBA8 texture often has few enough colours for a
palette, and an RGBA8 one may be grey. Check the round trip against the GX
decode for every texture on the disc.

Game-specific: the share of each format (here CMPR is 86% of the textures
and 74% of the converted bytes) and the true-colour textures left in RGBA8
(here 228, 93 MiB), which only a lossy encoder would shrink.
