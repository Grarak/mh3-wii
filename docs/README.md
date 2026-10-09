# Notes

Start with [HANDOVER.md](../HANDOVER.md). It covers the current state and how to
build, run and debug.

Current:

- [pi5-cortex-a9-benchmark.md](pi5-cortex-a9-benchmark.md): native ARM32 Pi measurements, game thread CPU time, and the model draw regression boundary.
- [vita-bringup.md](vita-bringup.md): native GXM port constraints and validated guest RAM layout.
- [aurora-dependency.md](aurora-dependency.md): the Aurora copy and its GLES renderer.
- [module-inventory.md](module-inventory.md): the disc's RSO modules and field layout.
- [slow-path-access.md](slow-path-access.md): the memory-access survey behind the flat guest memory.
- [variable-step.md](variable-step.md): what runs by the step's length and what still on whole ticks.

Historical bring-up notes. These were written on the earlier macOS/Metal
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
