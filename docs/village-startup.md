# Village startup shortcut

> Platform note: results in this document were recorded on the earlier macOS
> ARM64/Metal build. The port now targets arm64 Linux with Vulkan; see
> [arm64 Linux and Vulkan](linux-arm64.md) for what has been re-verified.

Run from any directory:

```sh
/Users/willi/Projects/mh3/scripts/run.sh --village
```

The shortcut enters File 1 / character 1 automatically. Normal boot, menu
transitions, module relocation, and loading still execute; no manual menu
navigation is needed. It returns input control when the verified village RSO
activates. The opening NPC conversation remains available for the player.

It requires an existing populated first character. Before boot it validates
the `data00` file size, character marker/name, and the game's additive checksum.
The save location honors `MH3_USER_DIR`; by default it is under `saves/nand`.
Missing, empty, or invalid saves produce an error before automated input starts.

A button or substantial stick movement cancels automatic navigation. A
three-minute timeout also releases input control. Omit `--village` for ordinary
startup. No savestate format is introduced: the shortcut loads the game's save
through its original code.

## Validation

Native and Metal runtime tests cover input edges, cancellation, timeout,
completion, checksum validation, and short/empty saves. Both runtime suites
pass. The launcher also passes its self-test when invoked from `/private/tmp`,
and the missing-save invocation fails explicitly.

The live run in `logs/village-shortcut-boot.log` reaches the verified village
module with no `MH3_INPUT_FILE` and no external input automation. Frame 1414
shows the village and the Chief's first dialogue page, still awaiting player
input after the shortcut has finished. The preceding
`logs/large-switch-boot.log` run verifies opening dialogue and free movement;
`logs/village-movement.png` shows the character after moving away from the Chief.
Quest progression and full-game coverage remain unverified.
