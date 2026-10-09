# The Vita's CDRAM budget

## What the original does

The Wii's GPU reads textures and vertex data from MEM1 and MEM2, the same
memory the CPU uses, and renders into its 2 MiB embedded framebuffer. The
game manages that memory itself, through its heaps.

## What the port does instead

All GPU data lives in the Vita's 128 MiB of CDRAM (video memory); textures
are never spilled to main memory. The device
(`aurora-main/lib/gxm/device.cpp`) queries the free CDRAM after GXM, the
context and the shader patcher are set up, then splits it:

| User | Size | Notes |
| --- | ---: | --- |
| Frame slots | 3 × 3.5 MiB | per slot: 1 MiB vertices, 2 MiB uniforms, 512 KiB U16 indices (`aurora-main/lib/gxm/native_capacity.hpp`) |
| Auxiliary blocks | 3 × 256 KiB | the numeric-ID stream, the pixel-clip rectangle, the utility rectangle |
| Producer reservation | 12 MiB | the two rows above, rounded up to MiB |
| Converted models | 16 MiB | separately allocated blocks, including models awaiting retirement |
| Driver headroom | 8 MiB | |
| Texture/display arena | the rest, about 47 MiB | display buffers, render and feedback targets, textures, the material uniform blocks |

Command rings, shader patcher and program storage and driver allocations use
CDRAM too, so the 128 MiB are never all available.

- **Textures** take the smallest GXM format that keeps their texels
  ([compressed textures](../assets/compressed-textures.md)): BC1/BC3 for
  CMPR, palettes, U8, U8U8 or RGB565 for the rest.
- **Model packs** stay within their reservation; a pack that does not fit
  evicts older ones first ([asset conversion](../assets/load-time-asset-conversion.md)).
- **At capacity**, a draw allocation that exceeds a slot's span or the U16
  vertex limit cancels the whole logical frame (no flip, no wait, no
  replacement allocation); a texture allocation that does not fit fails.
- **Per-frame data** (uniforms, generated geometry) is written into CDRAM
  by the CPU; it is never read back.

## Why

The GPU reads CDRAM fastest; main memory is far slower for it, which made
spilled textures visibly expensive. CPU writes, on the other hand, cost the
same in CDRAM as in the uncached main memory a GPU buffer would otherwise
use. Measured on the Vita, MB/s:

| Kind | memcpy (784 B blocks) | float stores | memset | reads |
| --- | ---: | ---: | ---: | ---: |
| CDRAM | 653 | 554 | 504 | 289 |
| Uncached main | 666 | 664 | 666 | 367 |
| Phycont | 666 | 673 | 659 | 367 |
| Cached main | 667 | 1415 | 886 | 1012 |

So per-draw uniforms stay in CDRAM, and every block is built on the stack
and copied once instead of being cleared in place and then written or
OR-ed field by field (reads from CDRAM are what costs).

Giving models a fixed 16 MiB and textures the rest raised the texture arena
from 31 to 47 MiB; in the Alatreon fight the arena held 27.7 MiB, textures
21.3 MiB of it, models 4.5 MiB.

## Example

`aurora-main/lib/gxm/device.cpp`:

```cpp
constexpr unsigned modelReserve = 16u << 20;
constexpr unsigned driverReserve = 8u << 20;
```

## Applying it to another Wii game

Generic: the slot layout, the capacity rules and the measurements. Every
Wii game's assets were sized for 88 MiB of shared memory, so converted
textures at their original sizes usually fit; convert CMPR to BC1 rather
than RGBA8, and keep palettes where the GPU can use them.

Game-specific: the split between models and textures, and the peak of each
map. Log the arena's use per area and keep a margin; a texture allocation
failure is fatal.
