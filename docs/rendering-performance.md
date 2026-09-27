# Viewport correction and performance investigation

> Platform note: results in this document were recorded on the earlier macOS
> ARM64/Metal build. The port now targets arm64 Linux with Vulkan; see
> [arm64 Linux and Vulkan](linux-arm64.md) for what has been re-verified.

## Rendering

MH3 programs a 640×448 viewport in PAL mode, displayed as 640×528 by the native
capture path. Before the fix, Aurora decoded its origin as (2,2), while the
scissor was (0,0). GX viewport and scissor coordinates use the same 342-pixel
hardware bias; Aurora's FIFO decoder and native GX encoder instead used 340.
Dolphin computes viewport origin minus twice the BP scissor offset
(`VideoCommon/VertexManagerBase.cpp`). The mh3-local CMake overlay changes both
Aurora endpoints to 342 without changing the shared Aurora source checkout.

Controlled fog disabling changed the mist but left the dark edges intact.
After correcting the viewport, diagnostic logs report (0,0,640,448), and the
black top/left borders disappear in both the village and Moga Woods entrance.
Evidence: `logs/replays/fog-native/frame-1170.bmp` versus
`logs/replays/fog-fixed/frame-1170.bmp` (village), and native frame 6990 versus
fixed frame 2880 (field). Animation and mist move between captures; these are
visual comparisons, not assertions of pixel identity.

The Dolphin launcher now sets both output aspect ratio and Wii Widescreen=False.
Its default Wii SYSCONF was widescreen even when output was forced to 4:3.
Background screenshots work through debug pipe X; A/B save/load slot 1.
Completed reference runs were stopped.

Renderer isolation is compiled only with `MH3_ENABLE_GX_DIAGNOSTICS=ON` (default
OFF). `MH3_GX_DEBUG_CONTROL` names a text file: 0 normal, 1 disable GX fog,
2 skip orthographic draws, 3 log one frame on transition to this mode. Mode 2
also removes cached scene images and UI, so use it only for debugging.

## Initial CPU profile

`logs/fog-fixed-cpu-sample2.txt` contains 2,786 main-thread samples over five
seconds at the field entrance. The first sample attempt was delayed by system
sampling authorization and yielded only two samples; it is not usable evidence.
The initial frame logs show about 57 ms process CPU per display copy and
1–2 ms graphics completion. Process CPU includes other threads; graphics
completion is a wait measurement, not a GPU timestamp duration.

The hottest rendering call chain is 80076A58 → 80087B34 → 8009A0DC → guest FIFO
→ display-list processing → Aurora draw submission. Much of its inclusive time
is host vertex/uniform copying rather than guest arithmetic. The 8009A0DC
wrapper emits the 0x40 call-display-list command through inlined 8009A064.
80475880 is a separate three-channel float/filter callback in the audio path;
its exact SDK identity is not yet established. Observational labels are in
`symbols.txt`; they do not claim recovered original names.

The RAM fast-read bias table was declared and consumed by translated code but
never initialized. Ordinary guest reads therefore took MMIO/device dispatch and
checked pointer resolution. Initialize biases only for full contiguous RAM
pages with seven bytes of safe lookahead; terminal pages and MMIO retain checked
access. Resolved-range fallbacks now reuse scalar fast reads/writes. Writes still
check executable guard pages. Regression coverage includes cross-page reads,
all RAM aliases, invalid/MMIO addresses and allocation endpoints, in addition
to existing executable registration/unregistration protections.

## Validation and remaining work

The final build keeps `-O1 -ffp-contract=off`; call tracing, collision checking
and GX diagnostics are OFF. Runtime/audio/SDL controller CTests pass, as does
`MH3 --efb-check` (readback, poke, preservation, completion fencing). Diagnostic
control/log strings are absent from the final executable.

RAM fast paths preserve every translated function's arithmetic. This is a
measured host-runtime optimization, not a hand rewrite of guest behavior.
Next investigate repeated vertex-array uploads / uniform copies in Aurora,
then re-profile audio filter 80475880 before attempting a reference-checked
readable rewrite. Shader/texture loading and map-transition latency remain
separate outstanding work; this change does not claim those are solved.

Final release replay: `logs/replays/perf-release` (same post-Chief save, same
4:3 route and 30-copy capture interval). Village steady samples were about
35 ms process CPU / 50 ms wall per copy, versus 55 / 67 before. At the Moga
Woods entrance, seven steady 50-copy samples after clearing the dialog were
42.1–42.8 ms CPU and 49.9–50.1 ms wall, versus about 57 / 67 before. This is
roughly 26% less field CPU time and 25% shorter frame intervals (15→20 FPS),
not full-speed playback. These are local end-to-end measurements, with moving
mist/animation rather than deterministic identical frame workloads. Field
movement was verified again at frame 2700 with no top/left borders.

The extra final-binary sample attempt stalled while the game was paused. It
completed just before cancellation, with only two samples, and is not used in
the analysis. The game resumed; the completed pre-optimization sample and frame
logs above are the evidence for this pass.
