# Notes

Start with [HANDOVER.md](../HANDOVER.md). It covers the current state and how to
build, run and debug.

## Current renderer and platform references

- [native-renderer-status.md](native-renderer-status.md): current implementation, pending integration, threads, waits/copies, shader compilation and memory accounting. Start here for rendering status.
- [native-gx-coverage.md](native-gx-coverage.md): remaining-caller audit scope; registration does not prove semantic or game coverage.
- [native-renderer-batch.md](native-renderer-batch.md): native backend probes, shader estimates and dated integration checkpoints.
- [native-resident-model-rendering.md](native-resident-model-rendering.md), [vita-model-conversion.md](vita-model-conversion.md): compact external models and prepared typed metadata; GPU/live-producer integration remains separate.
- [vita-native-assets.md](vita-native-assets.md), [vita-texture-format-evaluation.md](vita-texture-format-evaluation.md), [vita-pvrt-evaluation.md](vita-pvrt-evaluation.md): installed asset support and offline format assessments. Assessment bytes are not runtime residency or proof of installation.
- [native-material-state.md](native-material-state.md): persistent material mapping and explicit unsupported semantics.
- [native-resource-lifetime.md](native-resource-lifetime.md): native copy/fill/heap producers, source retirement, exact address coverage and remaining inline-write boundaries.
- [native-scene-queue.md](native-scene-queue.md), [native-ui-port.md](native-ui-port.md), [native-system-ui-port.md](native-system-ui-port.md), [native-effects-port.md](native-effects-port.md), [native-scene-feedback.md](native-scene-feedback.md): application family ports and focused evidence.
- [vita-bringup.md](vita-bringup.md): platform setup, guest RAM layout and historical DevKit checkpoints.
- [vita-launcher.md](vita-launcher.md): launcher, ROM processing and startup UI scope.
- [vita-render-investigation.md](vita-render-investigation.md), [vita-razor.md](vita-razor.md), [vita-active-play-capture.md](vita-active-play-capture.md): investigation/profiling tools and source-specific captures; older GX gameplay is historical.
- [pi5-cortex-a9-benchmark.md](pi5-cortex-a9-benchmark.md): historical source-identified playable snapshots; Pi 5 executes the Cortex-A9 build on a Cortex-A76 CPU.
- [aurora-dependency.md](aurora-dependency.md): maintained GLES/GXM copy and dependency setup.

## Runtime and simulation references

- [module-inventory.md](module-inventory.md): the disc's RSO modules and field layout.
- [slow-path-access.md](slow-path-access.md): the memory-access survey behind the flat guest memory.
- [variable-step.md](variable-step.md): simulation systems scaled by step length and remaining whole-tick behavior. Its gameplay probes need a playable source snapshot.

## Historical bring-up notes

These were written on the earlier macOS/Metal
build; the mechanisms they describe mostly still exist, but paths, build
steps and status may be out of date:

- [nand-bringup.md](nand-bringup.md): NAND save storage.
- [rso-bringup.md](rso-bringup.md): static RSO binding. All overlays are now translated (`scripts/translate.sh`).
- [function-discovery.md](function-discovery.md) and [switch-bringup.md](switch-bringup.md): translator discovery.
- [graphics-bringup.md](graphics-bringup.md) and [efb-color-bringup.md](efb-color-bringup.md): GX and EFB.
- [audio-bringup.md](audio-bringup.md): audio.
- [controller-bringup.md](controller-bringup.md) and [usb-hid-bringup.md](usb-hid-bringup.md): input.
- [village-startup.md](village-startup.md): the `--village` shortcut.
- [gameplay-debugging.md](gameplay-debugging.md): input replay and the Dolphin reference setup.
