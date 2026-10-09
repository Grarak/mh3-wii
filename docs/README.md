# Notes

Start with [HANDOVER.md](../HANDOVER.md): the current state, open work, and
how to build, run (including the Vita devkit) and debug.
[flags.md](flags.md) lists every environment variable.

## Renderer

- [native-renderer-status.md](native-renderer-status.md): the architecture, each platform's state, the culling and material A/B, GXM rules, shaders and memory limits. Start here.
- [native-render-plan.md](native-render-plan.md): the fixed shading models, frame packets, draw state owners, deferred model preparation and the remaining steps.
- [frame-state-slot-exchange.md](frame-state-slot-exchange.md): the three-slot exchange the frame packets use.
- [native-material-gpu.md](native-material-gpu.md): material uniforms in GPU memory.
- [native-material-state.md](native-material-state.md): the typed material state, its writers, group publication, revisions and the shading-model classification.
- [vita-shader-specializations.md](vita-shader-specializations.md): exact shader variants (linear fog, identity texgen, texture × vertex, destination alpha one) and their checks.
- [vita-shader-bank.md](vita-shader-bank.md): every GXM program compiled offline at build time and embedded; why the set is complete; the development miss check and audit.
- [vita-runtime-shaders.md](vita-runtime-shaders.md): the devkit's runtime Cg compiler and cache, now only a development fallback for bank misses.
- [vita-fixed-shader-abi.md](vita-fixed-shader-abi.md), [vita-selector-preflight.md](vita-selector-preflight.md): GXP reflection checks and the hardware readback fixtures behind the hunter and tint fixes.
- [vita-triple-buffering.md](vita-triple-buffering.md): presentation, VSync and triple buffering on GXM.
- [vita-widescreen.md](vita-widescreen.md): the game's own 16:9 mode on the 960×544 surface, and the visibility-test audit.
- [vita-native-cdram-capacity.md](vita-native-cdram-capacity.md): the CDRAM budget and its use in Alatreon.

## Models and textures

- [native-model-preparation-recipes.md](native-model-preparation-recipes.md): structural recipes and per-model draw tables.
- [native-model-preparation-diagnostics.md](native-model-preparation-diagnostics.md): opt-in recipe and source epoch counters.
- [native-model-draw-dispatch.md](native-model-draw-dispatch.md): the scene draw passes and the authored sort in C.
- [native-resident-model-rendering.md](native-resident-model-rendering.md): resident mesh storage and the mesh draw path.
- [native-gpu-palette-preparation.md](native-gpu-palette-preparation.md), [native-texture-srt-preparation.md](native-texture-srt-preparation.md): palettes written straight into GPU memory; reading only the consumed texture SRT fields.
- [native-model-assets.md](native-model-assets.md): the three model sources (load-time, index, single pack), budgets, eviction and M3IX.
- [vita-model-conversion.md](vita-model-conversion.md), [vita-indexed-model-packs.md](vita-indexed-model-packs.md): the M3MP/M3MT format, the offline converter and indexed palette batches.
- [native-preparation-model-assembler.md](native-preparation-model-assembler.md), [native-preparation-corners.md](native-preparation-corners.md), [native-preparation-writer.md](native-preparation-writer.md): the C model assembler that load-time conversion runs.
- [native-model-resource-archives.md](native-model-resource-archives.md): the effect resource archives and the texture inventory walker.
- [vita-model-inspection.md](vita-model-inspection.md): the offline model parser and the native resource accessors.
- [vita-equipment-model-coverage.md](vita-equipment-model-coverage.md): converter coverage of player and equipment models.
- [vita-native-texture-formats.md](vita-native-texture-formats.md), [native-texture-binding.md](native-texture-binding.md): converted texture packs and formats; how binds reach them.
- [vita-texture-format-evaluation.md](vita-texture-format-evaluation.md), [vita-pvrt-evaluation.md](vita-pvrt-evaluation.md): the evaluations behind the offline format policies.
- [native-model-view-floats.md](native-model-view-floats.md), [owned-static-scene-provider.md](owned-static-scene-provider.md): an unused prototype that draws a static scene from owned inputs.

## Game families ported to C

- [native-gx-coverage.md](native-gx-coverage.md): the GX caller audit; per family [startup](native-startup-coverage.md), [system UI](native-system-ui-coverage.md) and [effects](native-effects-coverage.md).
- [native-scene-queue.md](native-scene-queue.md): scene queues and the game-side scene culling.
- [native-ui-port.md](native-ui-port.md), [native-system-ui-port.md](native-system-ui-port.md): application UI, system UI and HUD.
- [native-effects-port.md](native-effects-port.md), [native-particle-draw-state.md](native-particle-draw-state.md): particles, the THP movie draw and effect textures.
- [native-scene-feedback.md](native-scene-feedback.md), [native-feedback-target-reuse.md](native-feedback-target-reuse.md): scene feedback and glare, and their target pool.
- [native-resource-lifetime.md](native-resource-lifetime.md): heap, fill and copy producers and resource retirement.
- [native-field-resource-loading.md](native-field-resource-loading.md): field resource scheduling and invalid areas.
- [native-dispatch-freshness.md](native-dispatch-freshness.md): how native registrations reach the dispatch tables.

## Vita platform

- [vita-platform.md](vita-platform.md): the Vita build, threads, clocks, memory, guest RAM views, files and device checks.
- [vita-launcher.md](vita-launcher.md): the launcher, reading the disc image in place, load-time conversion, custom quests, devkit and host runs.
- [rom-decoder.md](rom-decoder.md): the native ISO/RVZ/WIA decoder, its check against nod, and the Vita runtime helpers the nod archive used to supply.
- [vita-controls.md](vita-controls.md): the Classic Controller the game sees and how the Vita, SDL gamepads and the keyboard map onto it.
- [wpad-bridge.md](wpad-bridge.md): the WPAD bridge that feeds the game its controller, and scripted input.
- [vita-tls-lookup-optimization.md](vita-tls-lookup-optimization.md): the SDK TLS fast paths.

## Audio

- [native-audio.md](native-audio.md): the game's sound engine, MIX and voices in C, the mixer on core 0, stream extraction from the disc image, port checks and measurements.

## Profiling and capture

- [profiling.md](profiling.md): the statistics lines, Vita CPU profiles (flat or with stack snapshots), offline callers, build setup, Pi and VM profiling.
- [vita-razor.md](vita-razor.md): Vita GPU captures with Sony Razor and the last measurement.
- [vita-rust-async-cfi.md](vita-rust-async-cfi.md): audited frame rules for the Rust memory helpers in snapshot unwinding.
- [hdmi-audio-capture.md](hdmi-audio-capture.md): the devkit's HDMI video and audio capture.
- [native-active-gameplay.md](native-active-gameplay.md), [native-model-transform-check.md](native-model-transform-check.md): host capture and transform-check tools (last used on the October 2 renderer).

## Simulation and runtime

- [variable-step.md](variable-step.md): what is scaled by the step length, what still counts whole ticks, and the input path.
- [native-fsqrt-warning-trace.md](native-fsqrt-warning-trace.md): the open FSqrt NaN in the sway chains.
- [module-inventory.md](module-inventory.md): the disc's RSO modules and field layout.
- [slow-path-access.md](slow-path-access.md): the memory-access survey behind the flat guest memory.
- [aurora-dependency.md](aurora-dependency.md): the maintained Aurora copy.

## Bring-up notes

From the macOS/Metal build, which no longer exists; the mechanisms are still
in use: [NAND](nand-bringup.md), [RSO](rso-bringup.md),
[function discovery](function-discovery.md), [switch tables](switch-bringup.md),
[USB HID](usb-hid-bringup.md), [village startup](village-startup.md),
[gameplay debugging](gameplay-debugging.md).
