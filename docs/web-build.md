# Web build (WebAssembly + WebGPU)

A third renderer backend beside GLES 3.0 (`aurora-main/lib/gl`) and GXM
(`aurora-main/lib/gxm`): the game compiled to WebAssembly with Emscripten,
drawing through WebGPU (`aurora-main/lib/webgpu`). The GLES and GXM builds do
not compile any of it: the backend, the web platform files and the shader bank
exist only when CMake runs with Emscripten's toolchain (`AURORA_WEBGPU`
defaults to `${EMSCRIPTEN}`), and the shared sources that know about the web
do so only under `__EMSCRIPTEN__`/`__wasm__` conditions.

## Build

Tools (the versions it was built with):

| Tool | Version | Install |
| --- | --- | --- |
| Emscripten SDK | 6.0.11, with emdawnwebgpu v20260423.175430 | `curl -L https://github.com/emscripten-core/emsdk/archive/refs/heads/main.tar.gz \| tar -xz`, then `./emsdk install latest && ./emsdk activate latest` in `~/emsdk` (`EMSDK` overrides the path) |
| glslangValidator | 15.1.0 | `apt install glslang-tools` |
| naga | naga-cli 30.0.1 | `cargo install naga-cli` (found in `~/.cargo/bin`) |
| zstd | any | `apt install zstd` |

The translated sources (`generated/`) are the same as for the other targets.

```sh
scripts/build-web.sh                 # build/web: MH3.js, MH3.wasm, index.html, mh3-web.js
scripts/serve-web.py build/web       # http://localhost:8080/
```

`build-web.sh` configures `runtime/` with `cmake/toolchains/web.cmake`
(Emscripten's toolchain plus `-pthread -fwasm-exceptions` for every object)
as a development build (`MH3_DEVELOPMENT=ON`: statistics, no LTO);
`MH3_BUILD_DIR`, `BUILD_JOBS` and `MH3_EXTRA_CMAKE_ARGS` work as in
`build-aurora.sh`. A release configuration (`MH3_DEVELOPMENT=OFF`, ThinLTO) has
not been tried with Emscripten. A build from warm objects links in about a
minute; `MH3.wasm` is 66 MB.

## Serving

The game runs on several threads (Emscripten pthreads, which need
`SharedArrayBuffer`), so the page must be cross-origin isolated. Every
response needs

```
Cross-Origin-Opener-Policy: same-origin
Cross-Origin-Embedder-Policy: require-corp
```

(`scripts/serve-web.py` also sends `Cross-Origin-Resource-Policy:
same-origin`). WebGPU and the Origin Private File System need a secure context:
`localhost`, or HTTPS anywhere else. A browser with WebGPU is required
(Chrome/Edge 113+; Firefox and Safari where WebGPU is enabled).

## The disc image

Nothing of the disc is in the build or the repository. The page copies the
user's own European image (ISO, RVZ or WIA) into the site's Origin Private
File System once, streaming it (2.4 GB for the RVZ), and the game reads it in
place from there through WasmFS (`/opfs/mh3/<image>`), as the native builds
read it from disk. The first start passes `--rom` so the runtime writes
`disc/` (metadata and the load-time asset cache, `/opfs/mh3/disc`); saves go
to `/opfs/mh3/user` (`MH3_USER_DIR`). All of it stays in the browser profile
between visits (`runtime/web/web_files.cpp`).

Like the native executables, `MH3.wasm` holds none of the DOL's bytes: the
runtime reads the DOL's sections from `disc/sys/main.dol` at boot
(`runtime/src/dol_sections.cpp`). The translated code is still derived from
the game's executable.

Page parameters (`runtime/web/mh3-web.js`):

| Parameter | Meaning |
| --- | --- |
| `disc=URL` | fetch the image from the serving host instead of the file picker (`serve-web.py --disc IMAGE` offers it at `/disc/NAME`) |
| `args=...` | the program's arguments, as on the command line (`--quest 19001 --area 1 --no-damage`) |
| `env=NAME=VALUE,...` | environment variables (`MH3_STATS=1`, `MH3_AUDIO_NULL=1`) |
| `autostart=1` | start without the button once an image is stored |

## Threads

WebGPU objects belong to the JavaScript realm that created them, and a page
shows a canvas frame only when its thread returns to the event loop. So:

- `main()` runs on a pthread (`-sPROXY_TO_PTHREAD`): the game thread, with
  the draw thread, the guest worker, the disc reader and the asset converters
  as further pthreads, as on Linux.
- The browser's main thread owns the canvas, the WebGPU device and the render
  loop. `StartNativeRenderThread` starts a `requestAnimationFrame` loop there
  instead of a thread (`native_frame_packet.cpp`, web only): each animation
  frame replays the next published packet exactly as `RenderPackets` does, and
  its present appears when the callback returns. The game thread's limiter
  therefore paces the game to the display's refresh rate.
- Before the loop starts (and after it stops) the game thread owns the
  context, as on GLES; the backend runs each of its calls on the main thread
  with a synchronous proxy (`gpu.hpp`, `on_gpu`), during which the main thread
  counts as the owner.
- The device is requested asynchronously on the main thread while the game
  thread waits in `gl::initialize`.
- SDL's DOM event callbacks are queued to the thread that registered them;
  `aurora::window::poll_events` runs that queue on the web.

## The WebGPU backend

### What it restores

From Aurora's Dawn renderer as it was before 03132238
(`03132238^:aurora-main/lib/webgpu/gpu.cpp`, `wgpu.hpp`) and its
WiiCompiled original: the device setup (instance, adapter and device requests
with the uncaptured-error and device-lost callbacks, which stop the game as
there), the surface configuration and the letterboxed present blit, rebuilt on
Emscripten's emdawnwebgpu (`webgpu/webgpu_cpp.h`, the same Dawn C++ API).
Dawn native, its toggles, the blob cache and the backend fallback loop have no
browser counterpart and are not restored.

What the old backend drew with, the GX/TEV pipeline cache and the TEV WGSL
generator, no longer exists in the runtime (no runtime GX/TEV since the frame
packets and baked materials), so the old code could not serve the current
interface: `renderer.cpp` is new, an implementation of `aurora-main/lib/gl/gl.hpp` (the
interface `lib/gl` and `lib/gxm` implement), modelled on the GXM backend's
structure (names mapped to backend objects, state recorded until a draw).

### How the GL-style interface maps onto WebGPU

- State calls record state; a draw opens its target's render pass on first use
  (a clear before it becomes the pass's load operation, other clears draw a
  quad that honours the scissor and masks), then finds or creates its pipeline
  (program, vertex layout, raster state, target formats).
- Group 0 holds the uniform blocks: eight 16 KiB windows into one uniform
  buffer with dynamic offsets (Draw 0, Scene 1, Palette 2, Shade 3, Material 4,
  Animated 5, and the loose uniforms of the vertex 6 and fragment 7 stage).
  Per-draw blocks, vertices and indices are copied to staging memory, written
  to their GPU buffers once per submission. Persistent material blocks live
  past the staging range of the same buffer. Group 1 holds the program's
  textures and samplers; bind groups are cached by texture and sampler.
- Mesh vertex inputs are bound per draw as on GLES (`ShadedMeshInputs`); inputs
  a draw leaves unbound read (0, 0, 0, 1) from a zero-stride buffer, and the
  raw U8 matrix selectors and skin rows are unsigned integer inputs.
- Formats without a WebGPU counterpart load as RGBA8: RGB565 and the U8
  swizzles expand on upload (WebGPU has no texture swizzle), block-compressed
  and paletted packs decode as on GLES. Mesh strides must be multiples of four.
- A texture upload after draws that sampled it in the same submission submits
  those draws first, so they read the old contents, as GL orders it.
- GL keeps its rows bottom-up and draws everything vertically flipped; the
  converted vertex stages undo the flip (and map z from [-w, w] to [0, w]),
  rows stay in the same order (texture row 0 is the top row in both), so
  viewports and scissors carry over unchanged and front faces are clockwise.

### Shaders: the WGSL bank

The runtime still hands every backend GLSL ES (`native_shader_source.cpp` and
the fixed programs of `fixed_shader_sources()`). As the Vita's shader bank
does for Cg, the build converts every stage the game can bind to WGSL and
embeds the result (`cmake/WebShaderBank.cmake`):

1. `scripts/web_shader_bank.cpp` (host) writes the GLSL of the complete native
   key domain and the fixed programs, keyed by XXH3 of the source text the
   backend receives: 4,187 vertex and 604 fragment stages.
2. `scripts/web-shader-bank.py` preprocesses each stage with glslang, rewrites
   its declarations into Vulkan GLSL (fixed bindings, one location per varying
   name, separate textures and samplers, loose uniforms in a block, the clip
   space wrapper), compiles it to SPIR-V with glslangValidator and writes WGSL
   with naga. Converted stages are cached in `~/.cache/mh3-web-shader-bank`
   (a cold conversion takes about 80 s with 6 jobs). 65 MB of WGSL embed as
   1.6 MB (zstd with a shared dictionary).
3. `aurora-main/lib/webgpu/shader_bank.cpp` finds a program's stages by key
   and reads the bindings from the comment lines the converter puts first.

A program the bank lacks fails with `WebGPU shader bank has no program`.

### Not implemented

- `read_pixels`: WebGPU reads back asynchronously and the main thread cannot
  wait, so it returns zeros. Frame captures (`MH3_CAPTURE`) and the renderer
  self-checks that read pixels do not work; the page's asynchronous capture
  (`mh3CaptureFrame()`, below) stands in for the captures.
- The GX command executor's entry points (`draw_indexed`,
  `draw_native_material_geometry`, `draw_native_mesh`) belong to the removed
  FIFO path and only report an error.
- Depth-format texture copies; texture copies between formats.
- Wii NAND metadata: WasmFS has no extended attributes, so every NAND entry has
  the title's default owner and permissions.
- Floating-point flush-to-zero (the guest's FPSCR[NI]): WebAssembly always
  keeps denormals. Fused multiply-adds are a multiply and an add, as on the
  Cortex-A9.
- Fault reports for stray guest accesses (WebAssembly has no signals); native
  backtraces.

## Platform port

The web build follows the arm32 port: wasm32 is a 32-bit target, and the ISA
headers take the arm32 paths (scalar paired-single lanes, no fused
multiply-add) wherever they test `__arm__`, except for the FP control register,
which is a software word (`ppc_isa_fpenv.h`). Guest RAM cannot be mapped twice
in linear memory, so the cached, uncached and physical aliases fold onto one
backing as on the Vita (`guest_flat_memory.h`); the backing is a fixed range
below the static data (`-sGLOBAL_BASE`, 16 to 336 MiB).

## Status

Checked on arm64 Linux with Playwright 1.63's headless Chromium 153
(WebGPU on SwiftShader), `scripts/web-check.mjs`:

- Boot, disc preparation into OPFS (`--rom` on the first start), the intro
  movie and the title menu (Space skips the intro) render correctly.
- `--quest 19001 --area 1 --no-damage` with a staged post-Chief save reaches
  the Alatreon fight: Sacred Pinnacle, the skinned hunter and Alatreon, lava
  effects and the HUD render as on GLES. All 23 native shader variants the
  run used came from the bank; no WebGPU validation error.
- About 30 logical fps in the fight and 20–25 in the title menu, with every
  frame rendered on SwiftShader (CPU) on the browser's main thread;
  `requestAnimationFrame` caps it at 60.
- Audio plays through SDL's ScriptProcessorNode (`MH3_AUDIO_NULL=1` turns it
  off).

Chromium on that machine, headless or on the desktop, does not show WebGPU canvas
contents (neither the canvas, page screenshots nor `drawImage` see them): a
bare page with a 2D, a WebGL 2 and a WebGPU canvas shows the first two and a
blank third, so this is the browser's WebGPU presentation here, not the build.
With its default Vulkan compositing a canvas present also destroys the device:
run it with `--use-angle=swiftshader`. `web-check.mjs` therefore takes its screenshots
from the renderer itself: the page calls `aurora_webgpu_capture_frame()`, the
next present copies its source texture to a buffer and `Module.onFrameCapture`
receives the pixels (`mh3CaptureFrame()` in `mh3-web.js`).

```sh
npm install playwright && npx playwright install chromium
scripts/serve-web.py build/web --port 8090 --disc "<your RMHP08 image>"
PLAYWRIGHT_DIR=<dir with node_modules> node scripts/web-check.mjs \
  --url http://localhost:8090/ --disc "/disc/<image file name>" \
  --profile <profile dir> --shots <screenshot dir> --env MH3_STATS=1 \
  --args "--quest 19001 --area 1 --no-damage" \
  --nand saves/nand --data00 <your data00 save> --seconds 300
```

Keyboard input reaches the game (the controls of
[the Classic Controller mapping](vita-controls.md)); gamepads go through SDL's
Gamepad API support and have not been tried.

### Frame pacing

The backend reports no pending GPU frame (`gpu_frame_pending`): the browser
runs `requestAnimationFrame` at the rate it can show frames, which paces the
render loop and, through the packet limiter, the game. Queue completion
(`onSubmittedWorkDone`) cannot be used: with a canvas texture in a
submission, headless Chromium rejects it ("A valid external Instance reference
no longer exists").

## Merging GLES and WebGPU later

The two backends implement one interface from the same GLSL, so a merge has a
clear seam:

1. Move the backend-neutral parts of `gl.cpp` and `renderer.cpp` (the state
   record, the uniform staging, the program and sampler tables, the per-draw
   attribute mapping) into a common core that records draws into a small
   command list.
2. Keep two thin executors: GLES calls and WebGPU passes and pipelines. The
   GLES executor keeps its state cache; the WebGPU one keeps its pipeline and
   bind group caches.
3. Generate WGSL directly in `native_shader_source.cpp` next to GLSL and Cg
   (the key domain is shared), which would retire the glslang/naga step, or
   keep the bank and let GLES take its GLSL from the same keyed bank.
4. Native (non-web) WebGPU through Dawn would then be the same executor on
   another device, which is how Aurora started.

WebGL 2 would have been the shorter way into browsers (Emscripten maps GLES
3.0 onto it, so `lib/gl` would run nearly as is), at WebGL's higher cost per
draw. The merge above would make either a matter of which executor a target
links.
