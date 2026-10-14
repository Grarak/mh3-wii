# Textures and models converted while the game loads them

## What the original does

MH3's textures and models live in NW4R resource files (BRRES) and TPLs on
the disc. The game reads a file into RAM and NW4R parses it in place; GX
then reads textures straight from RAM in Wii formats (I4, I8, IA4, IA8,
RGB565, RGB5A3, RGBA8, the paletted CI4/CI8 with separate palettes, and
CMPR, a DXT1 variant), all in 4×4 or 8×4 tiles. Models (MDL0) keep their
vertex attributes in separate arrays, each with its own format and index,
and draw through display lists that GX decodes.

## What the port does instead

Nothing the GPU reads comes from guest RAM. Each texture or model file is
converted once into a native pack, on worker threads, the first time the
game reads it (`AssetCache`, `runtime/src/asset_cache.cpp`):

- **Textures** become one M3VT version 2 pack per file in
  `disc/cache/textures/`: BC1/BC3 for CMPR, and for the other formats the
  smallest GXM format that samples every texel unchanged (P4/P8 with CDRAM
  palettes, U8, U8U8, RGB565, else RGBA8;
  [compressed textures](compressed-textures.md)); authored mips kept;
  block and palette formats in the GPU's Morton order
  (`scripts/vita_texture_pack.cpp` can also apply lossy BC4/BC5 per
  texture offline).
- **Models** become an M3MP pack with its M3ID identity file in
  `disc/cache/models/`, assembled by the bounded C converter
  (`runtime/native/mh3_prepare_model.c` and its corner, skin and writer
  parts): one interleaved vertex layout and U16 index buffer per shape,
  validated matrix palettes, typed materials, and skinned shapes for
  [GPU skinning](../rendering/gpu-skinning.md).
- **Identification**: disc reads patch each texture header's last word with
  the tag `0xA0000000 | asset ID`, in memory only, so the renderer finds the
  converted texture from the header the game binds. IDs are recorded in
  `disc/cache/index.log` and stay fixed.
- **Ordering**: three conversion workers (cores 0-1 on the Vita); a disc
  read is answered only once the files it touches are converted
  ([disc access](disc-access.md)). A consumer that still arrives early waits.
  While a file awaits conversion its bytes stay in memory (up to 16 MiB of
  pending files on the Vita, 48 MiB elsewhere). Idle workers help with a
  file's textures: each texture's place in the pack is known before it is
  converted, so they convert in parallel and the pack is byte for byte the
  serial one. Later launches reuse every converted file.
- **Model packs read ahead.** Once a pack is written and the game reads its
  file, a worker reads and validates its metadata; the loading screen's
  certification then claims the read pack instead of reading it on the game
  thread. The disc reader answers the file's read only once its pack is read,
  and certification skips a pack not read yet until a later frame, so the
  card reads happen during Now Loading, not in a frame of it. A second
  worker reads the pack's GPU data, and the render thread only copies it
  into GPU memory, while the area loads, not at the model's first draw.
  Both read it unbuffered (see Why); a GPU read at a draw is logged.
- **On the Vita** packs land in CDRAM (models within their own reservation),
  with no decode, staging copy or content hash. Textures are never spilled
  to main RAM.
- **Retirement**: a cached texture is retired when the guest heap frees its
  source, so area changes do not accumulate dead textures.

## Why

- The CPU does no texture decoding, hashing or display-list parsing while
  drawing, and GXM samples native formats directly.
- Converting at load time instead of converting the whole disc once:
  booting into the Alatreon quest reads 146 files (60 MiB), about 0.35 s of
  conversion CPU on the Pi 5, against about 18 s for the whole disc; first
  start went from 8 min 42 s to 0.25 s.
- Reading packs on workers: a model pack read on the render thread at first
  draw held one packet 270 ms in the Alatreon fight; made resident during
  the load, the fight's longest render packets fell from 275 to at most
  50 ms. Moving the GPU data reads off the render thread took a Vita loading
  screen from 11.28 to 10.85 s and the game thread's waits for packet slots
  from 2.4 s to 26 ms.
- Reading the GPU data unbuffered: through newlib's stream buffer the Vita
  read a 1.5 MiB payload in 5.6-6.6 s. The next area's stage model, loaded
  while the hunter was still in the previous area, was still being read
  when the area first drew it; the render thread read it again from the
  card, and the screen stayed black for 5.95 s after Now Loading (devkit,
  island quest area 1 to 2). Unbuffered: 0.23 s, the fade.
- No converted asset is distributed: everything is made from the user's
  disc on their device.

## Example

The M3VT version 2 record (`aurora-main/lib/gfx/native_texture_layout.hpp`
is shared by the writers and the GXM allocator):

```text
u32 id, format, width, height, mips, flags, bytes, palette_offset
u64 payload_offset
u32 image_bytes, palette_bytes
i32 original_palette_minus_image_address
u32 original_palette_entries, original_palette_format, original_Wii_format
```

## Applying it to another Wii game

Generic: GX texture decoding and the CMPR-to-BC conversion apply to every
Wii game; the BRRES parser and the MDL0 model converter apply to every game
that uses NW4R resources; the asset cache's read hook, worker pool and
on-disk cache are independent of the format.

Game-specific: which files hold textures and models (MH3: standalone
BRRES files and two common plus ten map effect archives), the containers
around them, and how the game's binding code finds a texture (here the TEX0
header, which made the header tag possible). A game that builds textures
at run time (render targets, CPU-generated images) needs those kept on the
CPU path or produced on the GPU ([scene feedback](../rendering/scene-feedback.md)).
