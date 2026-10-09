# Notes

Start with [HANDOVER.md](../HANDOVER.md): current state, next steps, and how to
build, run (including the Vita devkit) and debug.

Topic notes below are dated. Notes describing removed systems (TEV shader
generation, capture and the GXP bank audits, offline asset preparation, the
single-owner renderer and its Pi/Vita measurements, render-thread planning)
were deleted on 2026-10-03; they remain in git history.

## Renderer and platform

- [native-renderer-status.md](native-renderer-status.md): what works on each platform, the verified Vita colour/hunter fixes, shaders and memory limits. Start here.
- [native-render-plan.md](native-render-plan.md): fixed shading models, frame packets, render thread, model instances and the measured Pi game/render split.
- [frame-state-slot-exchange.md](frame-state-slot-exchange.md): the three-slot exchange the frame packets use.
- [vita-launcher.md](vita-launcher.md): launcher, reading the disc image in place and load-time texture/model conversion.
- [vita-bringup.md](vita-bringup.md): platform setup, guest RAM layout and earlier devkit checkpoints.
- [vita-native-cdram-capacity.md](vita-native-cdram-capacity.md): CDRAM budgets.
- [vita-runtime-shaders.md](vita-runtime-shaders.md): the devkit's runtime Cg compiler and cache.
- [vita-fixed-shader-abi.md](vita-fixed-shader-abi.md), [vita-selector-preflight.md](vita-selector-preflight.md): actual GXP reflection, matrix-selector and RGBA hardware fixtures behind the hunter/tint fixes.
- [vita-oct03-linear-fog.md](vita-oct03-linear-fog.md): exact linear fog specialization and optimized Sony shader cycle counts.
- [native-model-preparation-recipes.md](native-model-preparation-recipes.md), [native-gpu-palette-preparation.md](native-gpu-palette-preparation.md), [native-texture-srt-preparation.md](native-texture-srt-preparation.md): admitted structural reuse, direct final palette writes and selective native SRT decoding.
- [draw-memory-view-gate.md](draw-memory-view-gate.md), [vita-tls-lookup-optimization.md](vita-tls-lookup-optimization.md): default draw-memory TLS bypass and initialized VitaSDK TLS lookup routing.
- [vita-gxm-scene-overlap.md](vita-gxm-scene-overlap.md): audited scene dependency removal; hardware timing remains to be measured.
- [native-material-state.md](native-material-state.md), [native-material-group-publication.md](native-material-group-publication.md), [native-material-unchecked-publication.md](native-material-unchecked-publication.md), [native-material-update-origins.md](native-material-update-origins.md): typed material state and how it is published.
- [native-resident-model-rendering.md](native-resident-model-rendering.md), [native-model-assets.md](native-model-assets.md), [native-model-draw-dispatch.md](native-model-draw-dispatch.md), [vita-model-conversion.md](vita-model-conversion.md), [vita-indexed-model-packs.md](vita-indexed-model-packs.md): converted models and their draw path.
- [vita-native-assets.md](vita-native-assets.md), [vita-native-texture-formats.md](vita-native-texture-formats.md), [native-texture-processed-host.md](native-texture-processed-host.md), [native-texture-binding-origins.md](native-texture-binding-origins.md): texture formats and binding.
- [native-scene-queue.md](native-scene-queue.md), [native-ui-port.md](native-ui-port.md), [native-system-ui-port.md](native-system-ui-port.md), [native-effects-port.md](native-effects-port.md), [native-scene-feedback.md](native-scene-feedback.md): application family ports.
- [native-gx-coverage.md](native-gx-coverage.md), [native-effects-coverage.md](native-effects-coverage.md), [native-system-ui-coverage.md](native-system-ui-coverage.md), [native-startup-coverage.md](native-startup-coverage.md): caller coverage audits.
- [native-resource-lifetime.md](native-resource-lifetime.md): native copy/fill/heap producers and resource retirement.
- [owned-static-scene-provider.md](owned-static-scene-provider.md): unregistered serial scene prototype (superseded in practice by the model-instance path).

## Profiling and capture

- [vita-oct03-head-profile.md](vita-oct03-head-profile.md): latest CPU/GPU profile (`0febd79`), how emulated TLS works on the Vita and the options to replace it.
- [vita-oct03-optimized-gpu-profile.md](vita-oct03-optimized-gpu-profile.md): latest complete GPU busyness, SCNE pass attribution, CPU supply and recorded gameplay.
- [vita-oct03-fixed-render-profile.md](vita-oct03-fixed-render-profile.md): matching recorded gameplay, CPU samples with unwind coverage, GPU-only Razor/vprof and current frame-rate bottlenecks.
- [vita-native-profiling.md](vita-native-profiling.md), [vita-native-stack-profiling.md](vita-native-stack-profiling.md), [vita-razor.md](vita-razor.md): Vita CPU and GPU profiling.
- [vita-gxm-scene-trace.md](vita-gxm-scene-trace.md): bounded application scene records and required Sony LIVE settings.
- [vita-memory-helper-selection.md](vita-memory-helper-selection.md): actual EABI versus public Sony libc routing, standalone correctness and register-preservation audit.
- [vita-render-thread-statistics.md](vita-render-thread-statistics.md), [vita-profile-analysis-draft.md](vita-profile-analysis-draft.md): game/render counter scopes and offline import/unwind coverage rules.
- [pi5-arm32-callstack-unwind.md](pi5-arm32-callstack-unwind.md): recovering ARM32 callers.
- [hdmi-audio-capture.md](hdmi-audio-capture.md): the devkit HDMI video/audio capture.
- [aurora-dependency.md](aurora-dependency.md): the maintained Aurora copy.

## Audio

- [vita-native-audio-worker-design.md](vita-native-audio-worker-design.md), [ax-private-memory-cost.md](ax-private-memory-cost.md), [vita-audio-output-evaluation.md](vita-audio-output-evaluation.md), [vita-streaming-audio-tempo.md](vita-streaming-audio-tempo.md), [vita-soundtouch-streaming-experiment.md](vita-soundtouch-streaming-experiment.md): AX worker and output experiments (SoundTouch is not integrated; AX gets replaced after rendering).

## Runtime and simulation

- [module-inventory.md](module-inventory.md): the disc's RSO modules and field layout.
- [slow-path-access.md](slow-path-access.md): the memory-access survey behind the flat guest memory.
- [variable-step.md](variable-step.md): systems scaled by the step length and remaining whole-tick behaviour.
- [release-quest-selection.md](release-quest-selection.md): quest shortcuts in release builds.

## Specialised notes

Focused investigations and checks, dated:
[active gameplay](native-active-gameplay.md),
[dispatch freshness](native-dispatch-freshness.md),
[feedback target churn](native-feedback-target-churn.md) and
[reuse](native-feedback-target-reuse.md),
[field resource loading](native-field-resource-loading.md),
[finite validation](native-finite-validation.md),
[fsqrt warnings](native-fsqrt-warning-trace.md),
[GLES state checks](native-gles-state-checks.md),
[GPU texture revisions](native-gpu-texture-revisions.md),
[model resource archives](native-model-resource-archives.md),
[model transform check](native-model-transform-check.md),
[model view floats](native-model-view-floats.md),
[particle draw state](native-particle-draw-state.md),
[preparation corners](native-preparation-corners.md),
[model assembler](native-preparation-model-assembler.md),
[preparation writers](native-preparation-writer.md),
[renderer batch probes](native-renderer-batch.md),
[equipment model coverage](vita-equipment-model-coverage.md),
[model inspection](vita-model-inspection.md),
[Vita helper audit](vita-native-helper-audit.md),
[Rust async CFI](vita-rust-async-cfi.md),
[texture format evaluation](vita-texture-format-evaluation.md),
[PVRTC evaluation](vita-pvrt-evaluation.md),
[armhf Wayland dependencies](pi5-armhf-wayland-dependencies.md).

## Historical bring-up notes

Written on the earlier macOS/Metal build; mechanisms mostly still exist, but
paths, build steps and status may be out of date:
[nand](nand-bringup.md), [RSO](rso-bringup.md),
[function discovery](function-discovery.md), [switch tables](switch-bringup.md),
[graphics](graphics-bringup.md), [EFB colour](efb-color-bringup.md),
[audio](audio-bringup.md), [controller](controller-bringup.md),
[USB HID](usb-hid-bringup.md), [village startup](village-startup.md),
[gameplay debugging](gameplay-debugging.md).
