# Render thread and frame packets

## What the original does

On the Wii the CPU writes GX commands into a FIFO and the GPU consumes them
in parallel; the CPU only waits at synchronization points (`GXDrawDone`,
the display copy). Within one frame the game thread runs its logic, then
its draw pass, which walks the scene and issues GX state and draws in
order. The GPU's command processor does the rest.

## What the port does instead

The draw pass no longer talks to a graphics API. It records the frame, and
a render thread draws it:

- **Frame packets** (`runtime/src/native_frame_packet.cpp`): the game thread
  records model items (captured preparation inputs and pose versions),
  material geometry (effects, UI, feedback), full-target depth clears and
  scene feedback operations. Geometry is written once into the packet's
  arenas (2 MiB vertices, 512 KiB indices).
- **State by component and revision**: draw, material, colours, lighting,
  post matrices and textures are captured only when that component's
  revision changed and its bytes differ (`BytesEqual`, NEON), and the render
  thread restores only changed components. Setters advance a revision only
  when a value changes.
- **Owners, not thread identity**: the logical state machines exist per
  owner, `NativeOwner::Game` (records) and `NativeOwner::Render` (replays
  and shades) (`runtime/include/native_owner.h`); the draw thread's records
  use a third copy, `NativeOwner::Draw`, which the Game owner maps to on that
  thread. Code names an owner, not a thread, and there is no `thread_local`
  in the runtime.
- **The render thread** owns the GL or GXM context (Vita core 0; the game
  thread has core 2 alone, see [core layout](core-layout.md)), replays
  packets in order, prepares models
  ([deferred model preparation](../rendering/deferred-model-preparation.md)), writes
  uniforms into the GPU frame slot, draws and presents. On the web the
  browser's main thread does this from a `requestAnimationFrame` loop
  instead of a thread, because WebGPU objects belong to it
  ([web build](../platforms/web-build.md)).
- **The 3D scene is recorded on a draw thread** (Vita core 1) beside the
  next logic step, in every build since the commit "Overlap the draw in
  release builds and fix their compile", into the same packet
  ([pipelined draw](pipelined-draw.md)).
- **A ring of four packets with a limiter**: packet `s` uses slot `s % 4`.
  Before recording packet `s`, the game thread waits until the render thread
  consumed packet `s - 2` (submitted its frame to the GPU;
  `PaceNativeFramePacket`): the game thread records `s` while the render
  thread prepares `s - 1` and the GPU draws `s - 2`. A presenting packet
  waits on the render thread for a free GPU frame slot instead of skipping
  its draw, so the game steps once per frame shown.
- **Just-in-time start**: the render thread notes how long each presenting
  packet waited for its GPU slot; the game thread, once its slot is free,
  sleeps the smallest such wait of the last 32 packets less 2 ms (at most
  12 ms) before it records, so it reads its input that much later.
- **The wait** spins about 50 µs, then parks on a counter the render thread
  advances after each take and completion (`SpinSignal`,
  `runtime/include/spin_signal.h`): on the Vita a kernel semaphore, which
  counts its signals, elsewhere a condition variable. The park re-checks
  at least every 100 ms.
- **With the frame limit off** (the launcher's `Framelimit` row) the game
  thread waits only for its slot, the render thread takes the newest packet
  and skips older ones, and the Vita turns VSync off.
- **Loading screens** run their frames at up to 240 Hz without the limiter,
  and the render thread draws them at 60 Hz: part of a load is counted in
  frames.
- **Tasks**: resource work (publishing generated materials, retiring model
  packs, statistics) is queued by packet sequence and runs before or after
  that packet's draws, so a skipped packet never loses it.
- **No GPU wait on the game thread**: GPU memory is fenced per frame slot
  ([presentation](../rendering/presentation.md)).

## Why

With drawing and presentation on one thread, the draw pass included the
driver's time. Moving it to a render thread fed by packets took the Pi 5
(Cortex-A9 build, Alatreon, moving) from 49 to 145 game frames per second,
the game-thread draw pass from 15.7 to 3.0 ms per frame (commit "Move
drawing and present to a render thread fed by frame packets"). On the Vita
the GPU driver calls and uniform writes moved off the game thread's core.

The ring and limiter (Vita, Alatreon, last 25 s, against the build before
the ring): logical fps 43.2 → 53.7 and the game's wait for the render
thread 132 → 8 ms per second. The limiter keeps the game running at the
rate frames are shown, so the [variable step](../simulation/variable-step.md)
samples game time evenly.

Waiting for `s - 2` to be consumed instead of `s - 1` to be taken (Vita,
Alatreon): before, the game made 93.5 logical frames a second for 56.3
shown, skipping 38.8 draws a second; after, 55.6 for 56.5 shown, no skips,
and the game thread's CPU use fell from 81% to 55%. Input to flip rose
from 58.6 to 74.9 ms; the just-in-time start took it back to 74.0 ms.
Recording `s` once `s - 1` was consumed cut input to flip to 56.0 ms but
displayed only 48.9 fps, and was not kept.

The parked wait instead of 100 µs sleeps: the game thread's CPU use 8.3%
→ 4.8% at the same frame rate (arm64, render-bound).

## Example

The limiter's condition (`runtime/src/native_frame_packet.cpp`,
`PaceNativeFramePacket`):

```cpp
const uint64_t next = sequence + 1;
const auto released = [next, unpaced] { return unpaced ? SlotReleased(next) : completed.load(std::memory_order_acquire) + 2 >= next; };
const auto ready = [&] { return released() && SlotFree(next); };
```

The Vita's pthread condition variable (pthread-embedded) can lose
broadcasts, which is why the park uses a kernel semaphore there and every
wait re-checks its condition on a timeout.

## Applying it to another Wii game

Generic: the packet format, the component snapshots, the owner split and
the task queues belong to the native renderer, not to MH3. Any game whose
draws go through native producers ([GX replacement](../rendering/gx-replacement.md))
can record into packets.

Game-specific: what a draw reads at replay time. A record must carry every
input it needs, because the game thread is already changing guest memory
for the next frame. MH3's model draws needed captured preparation inputs,
pose versions and immutable material records
([state versions](state-versions.md)); a draw that reads live guest memory
on the render thread is a data race. A development mode that replays the
same inputs on a shadow owner and compares the results finds the inputs a
record misses (this port had one until every draw passed it).
