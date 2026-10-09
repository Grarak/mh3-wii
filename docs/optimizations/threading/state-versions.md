# Immutable state versions instead of per-draw copies

## What the original does

On the Wii a draw's inputs are consumed when the draw is issued: g3d
computes a model's view matrices (`CalcView`) into arrays in RAM, loads them
into the GPU's matrix memory, and may overwrite them for the next model.
Materials are register writes. Nothing has to stay valid after the draw
call returns.

## What the port does instead

With a render thread ([render thread](render-thread.md)), a recorded draw
is executed later while the game thread already changes guest memory, so a
record must own its inputs. Copying them per draw was expensive; the port
records references to **immutable versions** instead:

- **Pose versions** (`VersionNativeModelPose`, `runtime/src/model_pose.cpp`):
  each model binding's view arrays (positions, normals, environment, 132
  bytes per matrix) get a version. A draw takes the current version if the
  producer revision is unchanged (the translucent pass after the opaque
  one), or if the live arrays compare equal (16 bytes a step; a moved camera
  differs in the first matrix). Otherwise it makes a new version.
- **Material versions**: draws reference versions of the material's pixel
  and colour group fields (`VersionNativeLiveMaterialGroup`) instead of
  copying 200 bytes; a static record per material scope is shared by both
  threads ([deferred model preparation](../rendering/deferred-model-preparation.md)).
- **Texture snapshots**: a texture state equal to one of the last three
  stored snapshots is referenced instead of copying 1.25 KiB (particle
  flipbooks alternate a few textures).
- **A structure revision that moves only for structure**: the draw thread
  drops its cull records, recipes and resolved resources when the model
  structure revision changes. Every copy or retirement of tracked bytes
  used to advance it (a `sprintf` copying a string out of a loaded file
  each frame did), so at the Deserted Island beach it changed about once a
  frame and every model's cull record was rebuilt. It now advances only
  when an owner certificate changes; live material blocks have their own
  revision.
- **Lifetime without reference counts**: a replaced version is tagged with
  the packet being recorded and reused once the render thread has finished
  that packet (`NativeFramePacketsDone()`); free versions are pooled by size.

## Why

Copies (`memcpy`/`memset`) were 7.6% of the Vita game thread in Alatreon
(stack samples, 2026-10-03); the per-draw pose copy (`CapturePose`) was
1.0%. On the Pi 5 the change was within the run-to-run spread.

The structure revision (Vita, Deserted Island beach, camera turning): draw
thread 12.49 → 10.16 ms a frame, game frame 13.06 → 10.74 ms, 68.6 → 72.3
logical fps, in two steps (owners and live blocks only, then owners only).

A development check compared every reused version with the live arrays and
checked that no version was reused before its item rendered: 11,962,391
reuses in 175 s of Alatreon, no mismatch and no lifetime error.

## Example

The rule for a pose capture:

1. same producer revision (`CalcView` stamps every view slot it writes):
   take the version as it is;
2. new revision, equal contents: keep the version;
3. otherwise copy once into a new version; tag the replaced one with the
   packet being recorded, free it when that packet has rendered.

## Applying it to another Wii game

Generic: revision stamps on producers, content compares before copies, and
retirement by packet sequence work for any recorded renderer.

Game-specific: which producers can stamp revisions (MH3's `CalcView` is a
native port, so it can), and how often contents repeat. Measure first: in
MH3's fight nearly every pose capture follows a moved camera, so most
captures still copy once; the gain is in removing the second and later
copies and the zero-filling around them.
