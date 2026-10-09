# Notes

Start with [HANDOVER.md](../HANDOVER.md): current state, next steps, and how to
build, run (including the Vita devkit) and debug. [flags.md](flags.md) lists
every runtime environment variable.

Topic notes below are dated. Notes describing removed systems (TEV shader
generation, capture and the GXP bank audits, offline asset preparation, the
single-owner renderer and its Pi/Vita measurements, render-thread planning)
were deleted on 2026-10-03; they remain in git history.

## Renderer and platform

- [native-renderer-status.md](native-renderer-status.md): the architecture, each platform's state, the culling and material A/B, GXM rules, shaders and memory limits. Start here.
- [native-render-plan.md](native-render-plan.md): the fixed shading models, frame packets, draw state owners, deferred model preparation and the remaining steps.
- [frame-state-slot-exchange.md](frame-state-slot-exchange.md): the three-slot exchange the frame packets use.
- [vita-widescreen.md](vita-widescreen.md): the game's own 16:9 mode on the Vita's whole 960×544 display (`MH3_WIDESCREEN`), how the anamorphic picture is stretched, devkit numbers.
- [vita-launcher.md](vita-launcher.md): launcher, reading the disc image in place and load-time texture/model conversion.
- [vita-bringup.md](vita-bringup.md): platform setup, guest RAM layout and earlier devkit checkpoints.
- [vita-native-cdram-capacity.md](vita-native-cdram-capacity.md): the CDRAM budget and its use in Alatreon.
- [vita-shader-bank.md](vita-shader-bank.md): every GXM program compiled offline at build time and embedded; why the set is complete; the development miss check and audit.
- [vita-runtime-shaders.md](vita-runtime-shaders.md): the devkit's runtime Cg compiler and cache, now only a development fallback for bank misses.
- [vita-fixed-shader-abi.md](vita-fixed-shader-abi.md), [vita-selector-preflight.md](vita-selector-preflight.md): actual GXP reflection, matrix-selector and RGBA hardware fixtures behind the hunter/tint fixes.
- [vita-shader-specializations.md](vita-shader-specializations.md): exact shader variants (linear fog, identity texgen, texture × vertex, destination alpha one) and their checks.
- [native-material-gpu.md](native-material-gpu.md), [vita-triple-buffering.md](vita-triple-buffering.md): material uniforms in GPU memory; presentation and triple buffering.
- [native-model-preparation-recipes.md](native-model-preparation-recipes.md), [native-gpu-palette-preparation.md](native-gpu-palette-preparation.md), [native-texture-srt-preparation.md](native-texture-srt-preparation.md): admitted structural reuse, direct final palette writes and selective native SRT decoding.
- [native-material-state.md](native-material-state.md): the typed material state, its writers, group publication, revisions and the shading-model classification.
- [native-model-preparation-diagnostics.md](native-model-preparation-diagnostics.md): opt-in recipe and source epoch counters.
- [native-resident-model-rendering.md](native-resident-model-rendering.md), [native-model-assets.md](native-model-assets.md), [native-model-draw-dispatch.md](native-model-draw-dispatch.md), [vita-model-conversion.md](vita-model-conversion.md), [vita-indexed-model-packs.md](vita-indexed-model-packs.md): converted models and their draw path.
- [vita-native-texture-formats.md](vita-native-texture-formats.md), [native-texture-binding.md](native-texture-binding.md): converted texture packs and formats; how binds reach them and the remaining Wii metadata.
- [native-scene-queue.md](native-scene-queue.md), [native-ui-port.md](native-ui-port.md), [native-system-ui-port.md](native-system-ui-port.md), [native-effects-port.md](native-effects-port.md), [native-scene-feedback.md](native-scene-feedback.md): application family ports.
- [native-gx-coverage.md](native-gx-coverage.md), [native-effects-coverage.md](native-effects-coverage.md), [native-system-ui-coverage.md](native-system-ui-coverage.md), [native-startup-coverage.md](native-startup-coverage.md): caller coverage audits.
- [native-resource-lifetime.md](native-resource-lifetime.md): native copy/fill/heap producers and resource retirement.
- [owned-static-scene-provider.md](owned-static-scene-provider.md): an unused prototype that draws a static scene from owned inputs.

## Profiling and capture

- [profiling.md](profiling.md): the statistics lines, Vita CPU profiles (flat or with stack snapshots, offline callers, build setup, helper provenance), Pi and VM profiling.
- [vita-razor.md](vita-razor.md): Vita GPU captures with Sony Razor: live job metrics, scene records, `.rzlv`/`.sgx` formats, the last measurement.
- [vita-rust-async-cfi.md](vita-rust-async-cfi.md), [vita-tls-lookup-optimization.md](vita-tls-lookup-optimization.md): audited frame rules for the Rust memory helpers; the SDK TLS fast paths.
- [hdmi-audio-capture.md](hdmi-audio-capture.md): the devkit HDMI video/audio capture.
- [aurora-dependency.md](aurora-dependency.md): the maintained Aurora copy.

## Audio

- [native-audio.md](native-audio.md): the game's sound engine, MIX and voices in C, the mixer on core 0, stream extraction from the disc image, port checks and measurements.

## Runtime and simulation

- [vita-controls.md](vita-controls.md): the Classic Controller the game sees and how the Vita, SDL gamepads and the keyboard map onto it; stick dead zones and curve.
- [module-inventory.md](module-inventory.md): the disc's RSO modules and field layout.
- [slow-path-access.md](slow-path-access.md): the memory-access survey behind the flat guest memory.
- [variable-step.md](variable-step.md): systems scaled by the step length and remaining whole-tick behaviour.

## Specialised notes

Focused investigations and checks, dated:
[active gameplay](native-active-gameplay.md),
[dispatch freshness](native-dispatch-freshness.md),
[feedback target reuse](native-feedback-target-reuse.md),
[field resource loading](native-field-resource-loading.md),
[fsqrt warnings](native-fsqrt-warning-trace.md),
[model resource archives](native-model-resource-archives.md),
[model transform check](native-model-transform-check.md),
[model view floats](native-model-view-floats.md),
[particle draw state](native-particle-draw-state.md),
[preparation corners](native-preparation-corners.md),
[model assembler](native-preparation-model-assembler.md),
[preparation writers](native-preparation-writer.md),
[equipment model coverage](vita-equipment-model-coverage.md),
[model inspection](vita-model-inspection.md),
[texture format evaluation](vita-texture-format-evaluation.md),
[PVRTC evaluation](vita-pvrt-evaluation.md).

## Historical bring-up notes

Written on the earlier macOS/Metal build; mechanisms mostly still exist, but
paths, build steps and status may be out of date:
[nand](nand-bringup.md), [RSO](rso-bringup.md),
[function discovery](function-discovery.md), [switch tables](switch-bringup.md),
[graphics](graphics-bringup.md), [EFB colour](efb-color-bringup.md),
[controller](controller-bringup.md),
[USB HID](usb-hid-bringup.md), [village startup](village-startup.md),
[gameplay debugging](gameplay-debugging.md).
