# Guest code on a second thread: the guest worker

## What the original does

All of MH3's logic runs on one guest thread. The particle simulation runs
inside each effect owner's update during the task pass: `effect_move`
(0x800F9984) and `EffectSpawnerUpdate` (0x800FF938) call
`nw4r::ef::Effect::Calc` through the Effect vtable. The scene's world
matrix pass (`ScnRoot::CalcWorld` 0x800832F4, run by `nwMoveEnd`
0x800D5D90) walks every model in order.

## What the port does instead

A third host thread, the guest worker (`runtime/src/guest_worker.cpp`), runs
guest code (translated functions and native ports) on jobs the game thread
hands it. On the Vita it runs on core 3 beside the audio mixer and the disc
reader ([core layout](core-layout.md)).

- **Effect calculation** (`runtime/src/effect_step.cpp`): during the task
  pass `Effect::Calc` only records the call. When the scene is drawn the
  worker runs the recorded calls in order: launched at the start of
  `DrawScene` while the game thread sets the scene up, traverses it and
  draws the opaque buckets: just before the game thread hands the scene
  graph walk to the draw thread ([pipelined draw](pipelined-draw.md)). The
  scene joins it before
  the first effect node, callback or sorted bucket. Effects therefore
  calculate after the whole task pass instead of inside each owner's
  update: an owner that reads an effect's state right after its call sees
  the previous frame's state, so an effect retires a frame later.
- **World matrices, split**: the root's children are divided between the
  worker and the game thread by their last measured time; the game thread
  takes back the worker's late models instead of waiting for them. Models whose CalcWorld callbacks read
  other models (the hunter's equipment copies bones of the body) stay on the
  game thread.
- **Runtime support**: one current-context record per thread, found by
  comparing the stack pointer with the worker's recorded stack range (no
  `thread_local`); dispatch memos that tolerate the other thread's writes; a
  copy of the game thread's FP mode; its own guest stack and g3d scratch;
  each thread's record on its own cache line.

## Why

The game thread is the limit on the Vita. In the Alatreon fight the effect
jobs take 4.5-7.4 ms per frame on the worker, and the game thread waits
0.06-0.9 ms per frame at the joins. With the draw inline on the
game thread, the split world pass saved 0.38 ms of logic but the draw lost
0.46 ms (it read the matrices the worker wrote from another core's cache).
The overlapped draw reads them on the draw thread's core either way, so the
split now pays: Vita, Alatreon, three alternating runs each, the logic
12.60 → 11.67 ms, logical 54.1 → 56.4 fps, displayed 52.4 → 54.0 fps. The Pi 5
ran the village, Moga (1060), the sandship (14002), The Decisive Battle
(2006) and Alatreon for 120 s each, splitting every frame of the field.

## Example

The thread lookup that replaces `thread_local`
(`runtime/include/isa/ppc_isa_thread_state.h`):

```cpp
inline unsigned CurrentThread() noexcept {
    const char probe = 0;
    const uintptr_t sp = reinterpret_cast<uintptr_t>(&probe);

    if (sp - workerStackLow < workerStackHigh - workerStackLow)
        return Worker;

    return sp - drawStackLow < drawStackHigh - drawStackLow ? Draw : Game;
}
```

The hot checks avoid even this: `IsCurrent(cpu)` compares the context with
each thread's record, the game thread's first.

## Applying it to another Wii game

Generic: running translated code on a second host thread needs only what
this runtime added: per-thread context records, a separate guest stack,
memos and caches that tolerate concurrent use, and flat guest memory
([flat memory](../cpu/flat-guest-memory.md)).

Game-specific: finding work that can run beside the game thread. Candidates
are passes whose inputs are fixed at a known point and whose outputs are
read only after another known point. Trace what the job reads and writes,
and who else touches it, before moving it: MH3's effect calculation could
move because the owners only set root matrices before it and read state
after it, with one exception that shifts by a frame. Shared host state
reached on the worker is reported once in development builds
(`GuestWorker::NoteShared`).
