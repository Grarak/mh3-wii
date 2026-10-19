# Every GXM program compiled ahead of time

## What the original does

The Wii's GPU has no programmable shaders: TEV and XF are fixed-function
units configured by registers, so the game never compiles anything. A port
that turns TEV configurations into shaders has to produce shader programs
somewhere. The first versions of this port generated GLSL from the TEV
state at run time and, on the Vita, compiled it on the console.

## What the port does instead

The Vita build ships every program it can bind, compiled by Sony's
`psp2cgc` at build time and embedded in the executable:

- The fixed shading programs ([fixed shading](fixed-shading-models.md))
  are a function of a small variant struct. Each stage's Cg source is a
  function of its stage key alone, and `NativeShaderVertexKeys()` /
  `NativeShaderFragmentKeys()` (`runtime/src/native_shader_source.cpp`)
  enumerate every value of every field. The bank is that whole domain plus
  the fixed GLSL programs (texture copy and palette conversion, depth peek,
  scene feedback), translated to Cg by the runtime's own converter, and 8
  hand-written utility programs: 4,222 vertex and 898 fragment stages in
  all (the skinned variants doubled the mesh vertex programs).
- `cmake/GxmShaderBank.cmake` builds a host generator that writes the Cg,
  compiles each stage with `psp2cgc` under Wine in Docker (cached by a hash
  of compiler, options and source), and packs the result: a zstd
  dictionary trained on the bank, then one zstd frame per program. The
  5,120-stage bank is about 28 MB of GXP, embedded as 2.1 MB (the build's
  `shader_bank.json`). A program is decompressed the first time it is bound.
- Release builds link no shader compiler; a miss stops the game.
  Development builds report a miss and fall back to the runtime compiler
  (`aurora-main/lib/gxm/runtime_shader.cpp`, through vitaShaRK and the
  user's `libshacccg.suprx`).

## Why

- **Retail consoles cannot compile.** The on-device compiler
  (`libshacccg.suprx`, loaded through vitaShaRK) is not part of the system;
  homebrew users extract it themselves. On the Vita it cost seconds per
  program at first sight.
- **The domain is small enough.** The first bank took about 8 minutes for
  a cold compile on an arm64 build host and 1 MB in the executable, and
  costs nothing at run time but a decompress per program a quest binds (a
  few dozen, some 200 KiB).
- **The game's materials could not be enumerated instead.** Spot light
  counts depend on the lights present at draw time, fog form on the scene,
  target-alpha certification on the render target; effects and UI take their
  state from code. Enumerating the program domain is the only set that is complete by
  construction.

## Example

A new variant field must reach the key domain; the compiler enforces it
(`runtime/src/native_shader_source.h`):

```cpp
// A new field must reach a stage key, the key domain and the shader bank.
static_assert(sizeof(NativeShaderVariant) == 20, "update NativeShader*Key, NativeShader*Keys() and docs/optimizations/rendering/offline-shader-bank.md");
```

The fragment domain is a loop over every model, texture count and flag
combination:

```cpp
for (unsigned model = 0; model < NativeShaderModelCount; ++model)
    for (unsigned textures = 0; textures <= NativeShaderMaxTextures; ++textures)
        for (unsigned flags = 0; flags < 1024; ++flags) {
            v.overlay = flags & 1;
            v.alpha_test = flags & 2;
            ...
            const auto key = NativeShaderFragmentKeyOf(v);
```

## Applying it to another Wii game

Generic: the bank format, the CMake function, the Docker image and the
runtime lookup (`aurora-main/lib/gxm/shader_bank.cpp`) do not depend on
MH3. Any renderer whose programs are a function of a finite key can use
them. The web build does the same for WebGPU (`cmake/WebShaderBank.cmake`):
it converts the key domain's GLSL and the fixed programs, 4,187 vertex and
604 fragment stages, to WGSL with glslang and naga and embeds 65 MB of WGSL
as 1.6 MB ([web build](../platforms/web-build.md)).

Requirement: Sony's `psp2cgc` (a Windows program from the official SDK),
which this project cannot distribute. Without it the options are an own
USSE emitter (the approach the azahar Vita port took) or the on-device
compiler as a cache-backed fallback, which retail users would have to
install.

Game-specific: the key domain. Keep it small by making shaders depend on a
few classified fields instead of raw TEV state, and canonicalize keys
(clear fields a stage ignores) so equal programs share an entry. Some
`-O3` compiles fail in `psp2cgc`; the bank compiles those stages at `-O2`
and lists them in `shader_bank.json`. Which stages fail changes with the
sources (none in the current bank, up to ten in earlier ones).
