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
| Emscripten SDK | 6.0.11, with emdawnwebgpu v20260423.175430 | `git clone https://github.com/emscripten-core/emsdk.git ~/emsdk`, then `./emsdk install 6.0.11 && ./emsdk activate 6.0.11` there (`EMSDK` overrides the path) |
| glslangValidator | 15.1.0 | `apt install glslang-tools` |
| naga | naga-cli 30.0.1 | `cargo install naga-cli` (found in `~/.cargo/bin`) |
| zstd | any | `apt install zstd` |
| Liberation Sans or DejaVu Sans | any | `apt install fonts-liberation`: the launcher's font, preloaded into `MH3.data` |

The translated sources (`generated/`) are the same as for the other targets.

```sh
scripts/build-web.sh                 # build/web: MH3.js, MH3.wasm, MH3.data, index.html, mh3-web.js
scripts/serve-web.py build/web       # http://localhost:8080/
```

`build-web.sh` configures `runtime/` with `cmake/toolchains/web.cmake`
(Emscripten's toolchain plus `-pthread -fwasm-exceptions` for every object)
as a development build (`MH3_DEVELOPMENT=ON`: statistics, no LTO);
`MH3_BUILD_DIR`, `BUILD_JOBS` and `MH3_EXTRA_CMAKE_ARGS` work as in
`build-aurora.sh`. A release configuration (`MH3_DEVELOPMENT=OFF`, ThinLTO) has
not been tried with Emscripten. A build from warm objects links in about a
minute; `MH3.wasm` is 50 MB.

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

`serve-web.py` listens on `localhost` only. The README's
[hosting](../README.md#hosting-it) section gives nginx and Caddy
configurations for a server, checked with nginx 1.31 and Caddy 2.11 in
Docker: both send the headers, `application/wasm` and compressed files, and
headless Chromium started the launcher and the game through nginx. On a
plain-HTTP LAN address Chromium has no secure context (no WebGPU, no OPFS),
even with `--unsafely-treat-insecure-origin-as-secure`; Caddy's `tls
internal` on that address, with `default_sni`, gives the page all of them
once the browser accepts the certificate.

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

### The page's options

The setup panel's options are kept in the browser's local storage and start
the runtime with these environment variables (an `env=` value replaces one):

| Option | Environment |
| --- | --- |
| Fill the browser window | `MH3_WEB_FILL=1`: SDL's fill-document mode; the canvas covers the page and follows the window's size |
| Render at the display's resolution | `MH3_RESOLUTION=native`: the canvas's pixel size, device pixel ratio included |
| Use the window's aspect | `MH3_ASPECT=window`: the picture takes the window's aspect, the HUD's parts its edges ([widescreen](optimizations/rendering/widescreen.md)) |
| Camera speed | `MH3_MOUSE_SENSITIVITY` |
| Invert up and down | `MH3_MOUSE_INVERT_Y=1` |

Show a frame counter is the page's own (F2 toggles it during play): every
half second the page reads the frames the render loop drew and showed
(`mh3_web_frames_shown`) and the frames the game published to draw
(`mh3_web_frames_made`), and shows the rate in the canvas's top right
corner, with the game's own rate beside it where it makes more frames than
the browser shows (Unlimited, or a cap above the display's refresh rate).
Loading screens, the intro movie's included, are left out of the game's
count: their frames run faster than they are drawn, and the render loop
shows only the newest (the intro gave 76 against 60 shown under VSync). SDL's
fill mode moves the page's elements into a hidden one, so the counter is kept
directly in the body.

Mouse look needs the pointer lock: a click on the picture asks for it (the
page on `mousedown`, SDL's relative mode on the next click as well), Esc
gives it back to the browser. SDL keeps its relative mode when the browser
drops the lock, so the page reports the lock (`mh3_web_pointer_locked`) and
the runtime turns the camera and takes the buttons as attacks only while it
holds; with Alt held the click stays a Wii Remote pointer click. Chromium
reports the cursor's jump into the lock (minus the cursor's position on the
page, a turn of 100° or more) as the movement of the first locked move, of
the capturing click's release and of some later moves: the page drops the
first locked move and every move with that movement before SDL's listener
sees them, and the runtime takes no motion until the frame after the release.

### The launcher

Start without arguments runs `MH3 --launcher`: the Vita's launcher menus
(`runtime/vita/launcher.cpp`) in the canvas, then the game as they start it,
with its settings in `/opfs/mh3/disc/settings.txt`. Everything but online
play: the City and the online settings are left out (the online quests,
played offline, and the custom quests stay). Beside the Vita:

- The keys act by what they do in the game's menus: Enter or F selects, Space
  or Backspace goes back, Q and Shift page, E edits a custom quest; the
  footer names them. A gamepad is the Classic Controller (east selects, south
  goes back, north or west edits). `MH3 --launcher` on Linux reads them the
  same way.
- The mouse (on Linux too): the row under it is selected, a click accepts
  it, the arrows on a row that takes a value change it, the right button
  goes back, the wheel moves through the rows and the footer's hints are
  buttons. The menus watch SDL's mouse events themselves and turn mouse look
  off while they run (`SetMouseLook`), so a click neither captures the mouse
  nor asks the page for the pointer lock.
- Names (custom quests, the character name cheat) are typed into the
  browser's prompt, which the launcher opens on the page's thread
  (`MAIN_THREAD_EM_ASM`) and waits for.
- SSAA instead of MSAA, which the web build leaves out: the WebGPU backend
  has no multisampled targets, so its own `SSAA` row (`ssaa` in the
  settings, `MH3_SSAA` for runs without the launcher) renders the target at
  1.41 and 2 times the width and height (`aurora_set_native_super_sample`) at
  2x and 4x, and the present pass's linear filter brings it down to the
  canvas (at 4x on a target that fills the canvas, an exact 2×2 average). It
  applies when the game starts, as upscaling does.
- `Framelimit` has a PC game's choices: VSync (a frame for each of the
  browser's animation frames, at the display's refresh rate), Half VSync
  (each frame shown for two), caps of 30, 60, 120, 144, 165 and 240 FPS,
  Unlimited, and Custom (Enter types 10 to 1000 FPS). A cap runs unpaced with
  the game thread timing its frames; the browser shows the newest at each
  refresh. Browsers show frames at the display's refresh rate, full screen
  or not; Chromium started with `--disable-frame-rate-limit
  --disable-gpu-vsync` drops that limit for all its pages (VSync off, with
  tearing): VSync then ran 200 frames a second in headless Chromium
  (`web-check.mjs --browser-args`).
- The arguments field replaces the launcher with the command line.

### Saves

The setup panel imports a save into `/opfs/mh3/user`: a folder (Dolphin's
`Wii/title/00010000/524d4850`, this port's `saves` from Linux or the Vita) or
its files. The page takes the save's files by name (`banner.bin`, and
`system`, `network`, `data00` to `data05` into `nocopy/`), skips other games'
title folders and refuses files of another size, a set without the system
file and one without a character (the launcher's `ValidSave`: file in use,
byte sum, record filled). A save replaces the whole save directory in the
browser, after a confirmation when that one has characters. Custom quests
(`*.mh3quest`) are added to `user/custom-quests`.

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
  Animated 5, View 6, and the loose uniforms of the vertex 6 and fragment 7
  stage, which the native programs, the only ones with a View block, lack).
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

### The compatibility level

The runtime asks for the adapter at WebGPU's compatibility feature level
(`gpu.cpp`), as the page does before it starts. Where the adapter has
`core-features-and-limits` the device takes it and nothing changes; where it
has not (WebGPU on OpenGL ES, such as the Raspberry Pi 5's V3D) the device
stays at the compatibility level, and the log line `Using the compatibility
feature level` says so. `MH3_WEBGPU_COMPAT=1` keeps a core adapter at that
level, which lets a desktop browser check the restricted path.

The renderer needed nothing else: every pipeline has one color target, the
textures are 2D with binding views that span all their mips, there is no depth
bias, multisampling or depth copy, and the WGSL bank uses no flat or linear
interpolation, sample built-ins or storage buffers and at most 13 inter-stage
variables. The 16 KiB uniform bindings and 8 dynamic uniform buffers are the
level's default limits. A smaller texture limit (4096 on the Pi) is met by
the framebuffer clamp of `lib/gl/device.cpp`. `AURORA_WEBGPU_TRACE=1` logs
each queue write of 256 KiB or more.

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
- The launcher (`--keys` without `--args`): the cheat list, a custom quest
  named through the prompt and saved, the hunter choice, the online quests'
  list and a quest start; the save import through the folder picker (two
  characters and a custom quest). SSAA 4x renders a 2000x1800 target for a
  1000x900 canvas through the quest, with the heap steady at 923 of 2048 MiB
  (`MH3 web heap`). One of four such runs stopped on the main thread asking
  for more than the heap's free 1.1 GB at once; the repeats with whole stacks
  did not.
- The launcher by mouse: the row under the pointer selected, the wheel
  scrolling, the frame limit and SSAA rows' arrows, a cheat toggled, back by
  the right button and by the footer's hint, Start game; no pointer lock in
  the menus, mouse look again in the game; 2x SSAA chosen there gives the game
  a 1414x796 target for the 1000x563 canvas.
- The compatibility level (`MH3_WEBGPU_COMPAT=1`, SwiftShader): the
  launcher, the village and the Alatreon quest with no validation error, the
  pictures as at the core level; Dawn's OpenGL ES backend
  (`--browser-args "--use-webgpu-adapter=opengles"`, on ANGLE's SwiftShader)
  ran the village too.
- The frame counter: 60 FPS in the launcher and the intro movie; in the
  village 12 to 18 shown with the game's 47 to 72 beside them, under VSync
  and Unlimited alike, as the runtime's statistics count them (logical 68,
  submitted 17 a second). There SwiftShader draws fewer frames than the game
  makes even under VSync, which should hold the game to the frames drawn: not
  explained yet. F2 hid the counter and showed it again.

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

Keyboard input and mouse look reach the game (the controls of
[the Classic Controller mapping](vita-controls.md)): `--mouse 30:click,40:300:0`
locks the pointer and turns the camera (under the lock headless Chromium
reports the protocol's moves as no movement, so the check dispatches pointer
events with the movement). `--options` and `--viewport` set the page's options
and the window, for example `--options ultrawide=1 --viewport 1600x600`.
Without `--args` the launcher starts, and `--keys` drives it, or `--mouse`
with `click:FX:FY`, `right:FX:FY`, `move:FX:FY` (fractions of the canvas)
and `wheel:DY`; `--save DIR` imports a save folder through the page's picker
and `--prompt TEXT` answers the launcher's text prompts. With `--options
fps=1` the log takes the frame counter's text at each screenshot, and
`--page-shots 1` also saves the page's own screenshots (the page's elements;
the canvas stays blank in them). `--cdp http://localhost:9222` drives a
desktop browser instead, started with `--remote-debugging-port=9222` on
another machine, with that port and the page's server forwarded over SSH
(`ssh -R 8090:localhost:8090 -L 9222:localhost:9222 HOST`, so the page is
`localhost` there too). Its shots are the page's screenshots, which show the
canvas, and saves go in with `--nand` (the folder picker cannot reach files
on this machine).

On the Raspberry Pi 5 (no monitor: labwc on wlroots' headless backend gave
the browser a 60 Hz screen) a windowed browser does not run the game yet:

- Debian's Chromium 154 creates no WebGPU context ("Failed to create WebGPU
  Context Provider"), not even on SwiftShader or OpenGL ES. Its requests also
  hung until it ran with `--password-store=basic`: the cookie store waited
  for the desktop keyring.
- Chrome for Testing 153 (Playwright's arm64 build) has WebGPU. Dawn turns
  the V3D's Vulkan driver down (it lacks `shaderUniform*ArrayDynamicIndexing`)
  and offers the V3D through OpenGL ES at the compatibility level, which
  Chrome hands out with `--enable-unsafe-webgpu --use-webgpu-adapter=opengles`
  (without them it offers no adapter; with only the first, SwiftShader). As
  an X11 client (Xwayland) its GPU process crashed on Dawn's OpenGL ES
  backend; as a Wayland client a minimal canvas ran 30 s at 60 frames a
  second, and the game ran on the V3D: the launcher at 62 FPS, the loading
  screens. 7 seconds into the village load, every time, the page dropped its
  WebGPU context ("A valid external Instance reference no longer exists"):
  in Chrome's trace the GPU process destroyed the page's WebGPU command
  buffer on the page's request, right after the commands of two new
  pipelines. Not the cause: the render size (960x544 too), the shaders
  first used then (built alone, they link), large writes (64 MiB buffer and
  16 MiB texture writes alone pass; the game writes 1 MiB at most there),
  memory and the 512 GiB address space of the Pi's 39-bit kernel (the
  renderer stays at 230 GiB), Chrome's GPU caches; no GPU process crash, no
  Mesa, Dawn backend-validation or kernel message. With SwiftShader in the
  window the device was lost after 4 to 37 seconds the same way.
Gamepads go through SDL's Gamepad API support and have not been tried.

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
