# Thread and core layout on the Vita

## What the original does

The Wii has one CPU core. The game's logic, its draw pass, the sound
engine's callbacks and the DVD library's completion work all share it,
interleaved by the OS's thread scheduler and by interrupts.

## What the port does instead

The Vita has four Cortex-A9 cores; an application gets cores 0-2, and core
3 is the system's. With the CapUnlocker kernel plugin a game may run
threads on core 3 too, beside the system's work. The port pins each thread
by role (`aurora::pin_current_thread`, `aurora-main/include/aurora/thread_role.hpp`):

| Core | Threads |
| --- | --- |
| 0 | the render thread (GXM, uniform writes, model preparation) |
| 1 | the draw thread (the 3D scene of the previous step) |
| 2 | the game thread, alone |
| 3 | workers: the audio mixer, the guest worker, the disc reader, the RVZ decode worker, the display callback |

- Every other pthread starts on cores 0-1: a `--wrap` of
  `pte_osThreadCreate` (`runtime/vita/pthread_embedded.c`) overrides the
  Vita's default affinity (any user core), and `ThreadRole::Any` (the asset
  conversion workers) means cores 0-1.
- Threads are created for their final core; changing a started thread's
  affinity had no effect.
- Without CapUnlocker the workers fall back to core 0.
- No thread on core 3 may busy-wait: the system's network stack runs there.

## Why

The game thread is the bottleneck, so nothing else may take its core's
time: before the wrap, the asset conversion workers and other pthreads took
samples on core 2 during loads. Afterwards all of core 2's samples in a
Jhen Mohran profile were the game thread's (3,412 of 3,412).

The draw thread ran on core 3 at first. Busy most of a frame, it starved the
system's work there: during an online login, pings took 4.5-8.4 s and the
login stalled until the server's 30 s keep-alive. With the draw thread off
core 3 the login took 13 s instead of about two minutes, pings returned
within about 2 ms (median, wired path), and the workers on core 3 (audio,
disc reads, the effect job) had no underruns.

## Example

`aurora-main/include/aurora/thread_role.hpp` (condensed):

```cpp
const unsigned core = role == ThreadRole::Game ? 2 : role == ThreadRole::Worker ? 3 : 0; // Render: core 0
int result = sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(),
    role == ThreadRole::Any ? SCE_KERNEL_CPU_MASK_USER_0 | SCE_KERNEL_CPU_MASK_USER_1 : SCE_KERNEL_CPU_MASK_USER_0 << core);
```

## Applying it to another Wii game

Generic: give the game thread a core of its own and keep every helper thread
off it, including threads the C library or SDK create for you; pin threads
at creation; keep threads that spin away from the system's core.

Game-specific: how many helpers the port has and how busy they are. Check
with a per-core profile which threads actually run on the game thread's
core during loads and fights.
