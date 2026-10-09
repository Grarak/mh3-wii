# Compressed textures kept compressed

## What the original does

Most of MH3's textures are CMPR, GX's 4×4 block format: DXT1 with
big-endian colour endpoints, the texel indices of each row in reverse
order, and the blocks grouped in 8×8 tiles of four. GX samples it directly
from RAM at 4 bits a texel.

## What the port does instead

CMPR textures stay block-compressed. The load-time converter
([asset conversion](load-time-asset-conversion.md),
`runtime/src/launcher_textures.cpp`) rewrites each block for the Vita's
GPU instead of decoding it:

- The endpoints are byte-swapped and each index row is reversed: the
  block becomes a BC1 block with the same endpoints and indices, so the
  conversion is lossless and does not re-encode anything.
- The blocks move from GX's 8×8 tiles into the SGX's Morton (swizzled)
  order, which GXM's `UBC1_ABGR`/`UBC3_ABGR` formats expect.
- A texture with transparent texels (CMPR's three-colour mode with index 3
  in use) becomes BC3 instead: an alpha block of 0 for those texels and 255
  elsewhere, followed by the colour block. CMPR's transparent texels keep
  the midpoint colour; BC1's punch-through alpha would give them black,
  which bilinear filtering bleeds into the edges of leaves and hair. The
  SGX applies the three-colour endpoint rule to BC3's colour block too, so
  the colours stay CMPR's.
- Only power-of-two CMPR textures are compressed (GXM's block formats need
  them); the rest, and every other GX format, become RGBA8.

## Why

A texture fetch reads its texels from memory; at 4 bits a texel a BC1
texture moves an eighth of an RGBA8 one's bytes through the memory bus and
the texture cache, and BC3 a quarter. Compressed textures also decide
whether a map fits in CDRAM at all
([CDRAM budget](../platforms/vita-cdram-budget.md)).

The textures in one development build's cache, converted from the files the
game loaded over many runs (2,668 textures in 288 files):

| Wii format | Textures | Converted to | MiB |
| --- | --- | --- | --- |
| CMPR, opaque | 1,633 | BC1 | 99.6 (796 as RGBA8) |
| CMPR, transparent texels | 634 | BC3 | 45.7 (183 as RGBA8) |
| CMPR, not a power of two | 25 | RGBA8 | 5.0 |
| IA8, IA4 | 247 | RGBA8 | 21.2 |
| CI8 | 26 | RGBA8 | 17.9 |
| RGBA8, RGB5A3 | 63 | RGBA8 | 14.2 |
| I8, I4, RGB565 | 40 | RGBA8 | 6.3 |

Keeping CMPR compressed holds these textures at 210 MiB instead of about
1,040 MiB. The rest leaves room: exact smaller GXM formats for the other GX
formats (`P8` with a CDRAM palette for CI8, `U8U8` for IA4/IA8, `U8` for
I4/I8, `U5U6U5` for RGB565) would take 59.6 MiB down to about 31 MiB with
the same texels. The load-time converter does not produce them yet; the
offline packer `scripts/vita_texture_pack.cpp` (`MH3VitaTexturePack`, for a
whole-disc set read through `MH3_NATIVE_ASSET_DIR`) takes them from a
per-texture policy file and refuses any choice that changes a sampled
texel.

## Example

The format choice, `runtime/src/launcher_textures.cpp`:

```cpp
if (s.format == GX_TF_CMPR && std::has_single_bit(s.width) && std::has_single_bit(s.height)) {
    const bool alpha = has_transparency(s, bytes.data());
    r.format = alpha ? 3 : 2; // 3 BC3, 2 BC1, 1 RGBA8
    out.payload = compressed(s, bytes.data(), alpha);
} else
    out.payload = linear(s, converted.data.data());
```

## Applying it to another Wii game

Generic: the CMPR-to-BC1/BC3 rewrite applies to every Wii game, since CMPR
is GX's only compressed format, and Wii games use it for most of their
colour textures. Keep the blocks instead of decoding them; re-encoding
decoded texels to BC1 loses quality and costs CPU at load time. Use BC3
only where CMPR's transparent texels occur, and swizzle into the target
GPU's block order.

Game-specific: the share of CMPR (here 86% of the textures and 72% of the
converted bytes), and whether the remaining formats are worth their exact
GXM equivalents: count them per map before adding formats to the converter.
