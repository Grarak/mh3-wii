# GX replaced by native draw producers

## What the original does

The game draws through the Wii SDK's GX library, which writes commands to
the GPU's FIFO at `0xCC008000`: register loads for the TEV combiner stages,
the transform unit (XF: matrices, lights, texture coordinate generation),
blending and depth (BP), vertex formats, then vertex data, either immediate
(`GXPosition3f32` ...) or display lists. MH3's own code and the NW4R
libraries call GX from many places: the g3d model draws (MDL0 shapes as
display lists), the effect strategies (immediate quads), the 2D and font
paths, the HUD, the movie player, and the scene feedback passes that copy
the framebuffer into textures (EFB copies).

## What the port does instead

There is no GX emulation at runtime. Every application caller of a GX entry
point was ported to C that produces typed native draws:

- **Producers in C** (`runtime/native/*.c`) build vertices, indices and
  typed material state from the game's records; thin adapters
  (`runtime/src/native_*.cpp`) read guest memory and call them. They write
  into frame allocations of the native renderer
  (`aurora_allocate_native_geometry` and the model, material and texture
  APIs) and never write FIFO bytes or wait for the GPU.
- **Families** ported: startup textures and display utilities, the scene
  queue and buckets, model draws and their material setup
  ([deferred model preparation](deferred-model-preparation.md)), UI, fonts
  and HUD, NW4R effects ([particle batching](particle-batching.md)), the
  THP movie, scene feedback and glare ([scene feedback](scene-feedback.md)).
  `docs/native-gx-original-calls.json` is the inventory of the original GX
  callers.
- **The GX SDK itself stops the game.** `runtime/src/gx_services.cpp`
  (generated once from the GX entry table) holds 201 natives covering the SDK's GX
  interval (0x804B3BE0-0x804BB8A0) and named GX leaves outside it, each a
  `PanicLegacyGx` with a guest and host backtrace. The translated FIFO
  writer is trapped separately. An unported path fails loudly instead of
  drawing wrong.
- **Shading** no longer follows TEV stages: materials are classified into
  fixed shading models ([fixed shading](fixed-shading-models.md)).
- **The movie** (THP) is decoded natively (on the Vita by the hardware
  JPEG decoder, [movies](../assets/hardware-movie-decoding.md)) and its YUV
  planes converted to an RGB texture with NEON in eight-row bands.

The renderer started from Aurora, a GX-to-WebGPU compatibility layer. Its
FIFO decoder and TEV shader generator still exist in `aurora-main/lib/gx`
but are not reachable; Aurora's own `GX*` functions also panic
(`aurora-main/lib/dolphin/gx/`).

## Why

- **Cost.** Decoding a register stream, generating a TEV shader per
  combiner configuration and caching it, and hashing textures to find them
  again all ran per draw or per frame on the CPU. The TEV-generated programs
  cost 46-175 GPU cycles per fragment and 271-848 per vertex in
  psp2shaderperf, against 10 for the fixed MODULATE program with one
  texture (commit "Shade native draws with fixed hand-written programs
  instead of TEV").
- **The Vita cannot compile shaders in a release.** Generated TEV programs
  would need a compiler on the console ([shader bank](offline-shader-bank.md)).
- **EFB access.** GX code reads the framebuffer back (EFB peeks, copies to
  RAM). The native producers keep feedback on the GPU.
- **Ownership.** Typed state can be recorded on the game thread and drawn
  on a render thread ([render thread](../threading/render-thread.md)); a FIFO stream ties
  both to one order of register writes.

## Example

A generated guard (`runtime/src/gx_services.cpp`):

```cpp
extern "C" void MH3_DisallowedGX_804B4DD0(CpuContext* cpu) {
    PanicLegacyGx("GXSetCPUFifo", 0x804B4DD0, cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x804B4DD0, MH3_DisallowedGX_804B4DD0, "GXSetCPUFifo");
```

Two UI entries, `80041470` and `8005ac3c`, became
coloured or textured screen quads with source alpha blending and depth
ALWAYS without depth writes, written straight into the frame allocation.

## Applying it to another Wii game

This is the largest and most game-specific part of the port. Generic parts:
the native renderer's APIs (typed geometry, materials, textures, render
targets), the panic generator for the SDK's GX interval, and the audit
approach.

Game-specific: the list of GX callers and what each one means. The order
that worked here:

1. Build a call inventory: every translated function that calls a GX entry
   (this repository's is `docs/native-gx-original-calls.json`, 213 callers),
   grouped into families by what they draw.
2. Make the GX SDK fatal first, so every unported caller shows up with a
   backtrace instead of rendering wrong.
3. Port family by family, from the decompilation, keeping the game's order
   of draws and its state changes; compare frame dumps with the previous
   renderer while it still exists.
4. Model draws are the bulk of the work: decode the game's model format
   (here NW4R MDL0) offline or at load time instead of replaying its
   display lists ([asset conversion](../assets/load-time-asset-conversion.md)).

A game with its own engine on GX (not NW4R) has the same structure: find
its draw entry points and the state it sets before each draw.
