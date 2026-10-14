# The web build: WebAssembly and WebGPU

Building and serving it: [web build guide](../../web-build.md).

## What the original does

The game runs on the Wii: PowerPC code, GX for graphics, the disc drive and
the NAND flash. There is no browser version.

## What the port does instead

The same translated code and runtime compile to WebAssembly with Emscripten
(commit "Add a WebAssembly build that renders through WebGPU"):

- **A third Aurora backend** (`aurora-main/lib/webgpu`), beside GLES and GXM:
  the renderer interface of `lib/gl` implemented on WebGPU through
  Emscripten's emdawnwebgpu. The device setup is restored from Aurora's Dawn
  backend, which commit "Replace Aurora's WebGPU/Dawn renderer with native
  OpenGL ES 3.0" removed; the draw path is new. Draws open
  render passes lazily, pipelines are cached, per-draw uniform blocks share
  one buffer with dynamic offsets, and per-draw data is staged and written
  once per submission.
- **A WGSL shader bank** (`cmake/WebShaderBank.cmake`): the build converts
  every GLSL ES stage the backends can receive (the native key domain and the
  fixed programs, 4,187 vertex and 604 fragment stages) to WGSL with glslang
  and naga, keyed like the GLSL. 65 MB of WGSL embed as 1.6 MB (zstd with a
  shared dictionary). This is the Vita's offline bank
  ([offline shader bank](../rendering/offline-shader-bank.md)) for another shading
  language.
- **The arm32 paths**: wasm32 is a 32-bit target, so the ISA headers take the
  arm32 code (scalar paired-single lanes, a multiply and an add instead of a
  fused multiply-add; [Cortex-A9 floating point](../recompilation/cortex-a9-floating-point.md)).
  The FP control register is a software word, and WebAssembly keeps
  denormals, so FPSCR[NI] flushes nothing.
- **Guest RAM folded as on the Vita**: linear memory cannot map a block
  twice, so the address aliases fold onto one backing below the static data
  (`-sGLOBAL_BASE`; MEM1 at 16 MiB, MEM2 at 272 MiB, ending at 336 MiB;
  [flat memory](../cpu/flat-guest-memory.md)).
- **Threads**: `main()` runs on a pthread (the game thread), with the draw
  thread, the guest worker, the disc reader and the converters as further
  pthreads. WebGPU objects belong to the browser's main thread, which runs the
  render loop from `requestAnimationFrame` instead of a render thread; other
  threads reach the device through a synchronous proxy (`on_gpu`).
- **Files**: the page copies the user's disc image into the site's Origin
  Private File System once, and the game reads it in place through WasmFS
  (`/opfs/mh3`), with `disc/` and the saves next to it.

The GLES and GXM builds compile none of it: the shared changes are
conditions on `__EMSCRIPTEN__`, `__wasm__` or `AURORA_WEBGPU`.

## Why

To run the same game in a browser. It is not a performance target. WebGL 2
would have been the shorter way (Emscripten maps GLES 3.0 onto it, so
`lib/gl` would run nearly as is), at WebGL's higher cost per draw; the build
follows the request for WebGPU. Measured on arm64 Linux in headless
Chromium with WebGPU on SwiftShader (CPU rendering): about 30
logical fps in the Alatreon fight and 20-25 in the title menu; every native
shader variant the fight used came from the bank, with no WebGPU validation
error ([web build](web-build.md)).

The GLES and GXM builds are unchanged by it: all GLSL and Cg sources of the
key domain stream out byte-identical, the changed sources preprocess
identically for arm32 Linux and the Vita, and three alternating Alatreon runs
per build on the Pi 5 were equal within noise (121.7 against 126.7 logical
fps median; commit "Record that the web build leaves GLES and GXM
unchanged").

## Example

Every renderer call runs on the browser's main thread
(`aurora-main/lib/webgpu/gpu.hpp`, condensed):

```cpp
template <typename F>
void on_gpu(F&& function) {
  if (on_gpu_thread()) {
    function();
    return;
  }

  detail::Proxied<std::remove_reference_t<F>> call{&function};

  if (!emscripten_proxy_sync(emscripten_proxy_get_system_queue(), emscripten_main_runtime_thread_id(), &decltype(call)::run, &call))
    fatal("Cannot proxy a renderer call to the browser's main thread");
}
```

## Applying it to another Wii game

Generic: the WebGPU backend, the WGSL bank and the web platform files
(`runtime/web/`) depend on the runtime, not on MH3. A port that already has
a 32-bit build and a renderer with a finite program set needs little more:
a toolchain file, a folded guest memory layout and a bank conversion step.

Limits to expect: `read_pixels` returns zeros (the main thread cannot wait
for an asynchronous readback), Wii NAND metadata cannot be stored (WasmFS has
no extended attributes), stray guest accesses are not reported (no signals),
and the browser paces the frames through `requestAnimationFrame`. The page
must be served cross-origin isolated (COOP/COEP headers) for pthreads, and
from `localhost` or HTTPS for WebGPU and the Origin Private File System.
