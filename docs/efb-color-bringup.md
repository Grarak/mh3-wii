# CPU framebuffer color reads

> Platform note: results in this document were recorded on the earlier macOS
> ARM64/Metal build. The port now targets arm64 Linux with Vulkan; see
> [arm64 Linux and Vulkan](linux-arm64.md) for what has been re-verified.

Selecting New Game calls MH3's `GXPeekARGB` at `0x804B6A10` from the luminance
readback loop near `0x80057708`. The SDK reads `0xC8000000 | x << 2 | y << 12`
after `GXDrawDone`. Previously this reached the unmapped-MMIO guard.

The memory boundary now decodes aligned 32-bit EFB color reads through the
physical, cached, and uncached aliases. It reads actual Aurora Metal framebuffer
pixels, cached until more GX bytes arrive. A synchronous GPU copy/map runs once
per snapshot; subsequent CPU pixel reads use the cache. Readback respects GPU
row pitch, BGRA/RGBA ordering, and framebuffer scaling. The original translated
SDK method and luminance calculation remain intact.

Color conversion implements RGB8, RGBA6, and RGB565 quantization and the PE
alpha-read modes (zero, opaque, or unchanged). These rules follow
[Dolphin's EFB interface](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/VideoCommon/EFBInterface.cpp)
and [PE register definitions](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/VideoCommon/PixelEngine.h).

The same loop then calls `GXPokeARGB` at `0x804B6A30` to write its computed
luminance back before `GXCopyTex`. Pokes update the coherent CPU snapshot and
batch the changed rectangle into a Metal texture upload before subsequent GX
work. The supported path applies color/alpha update masks, alpha comparison,
destination alpha, and color precision. Blending/logic operations and depth
comparisons other than Always remain explicit errors.

These accesses currently require a completed GXDrawDone boundary. Peeks or
pokes inside an unfinished host frame, depth access, MSAA pokes, and other pixel
formats remain explicit errors. It uses Aurora's framebuffer and VI internal
headers from the configured Aurora checkout. `scripts/aurora-efb.cmake` builds
a guarded local overlay of `common.cpp`, adding a one-shot instruction to
preserve the EFB on the next host frame after CPU writes. The shared Aurora
checkout remains unchanged.

Validation:

- Runtime tests cover address aliases, coordinate limits, invalid addresses,
  out-of-bounds reads, color precision and alpha modes.
- `build/linux-arm64/MH3 --efb-check` uploads two known color patterns to the actual
  Metal framebuffer and verifies both snapshots, row offsets, aliases, and
  alpha-mode changes. The extended check also verifies poke/peek coherence,
  dirty-rectangle upload, preservation of neighboring pixels, and retention
  across the next host frame (`logs/efb-poke-check.log`).
- The Metal runtime/audio/SDL CTest suite passes.

The real boot (`logs/efb-poke-ready.log`) completes a Metal color snapshot and
CPU pokes with the SDK's default PE state (`f038/0700/001f/0000`). It proceeds
to the next independent boundary: GX display-list opcode `0x40` while preparing
the character scene. Native runtime/audio tests also pass.
