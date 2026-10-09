# The PS Vita platform layer

What the port adds for the Vita besides the renderer, which is in
[libgxm for Wii ports](../rendering/libgxm-for-wii-ports.md).

## What the original does

The game runs on the Wii: one PowerPC core at 729 MHz, 24 MiB MEM1 and
64 MiB MEM2, the GX GPU, IOS on a separate ARM core, and the Wii's video,
audio and input hardware.

## What the port does instead

- **Toolchain**: Clang 21 and lld instead of VitaSDK's GCC, with VitaSDK's
  sysroot, libraries and packaging (`cmake/toolchains/vita-clang.cmake`,
  adapted from Azahar), a linker script derived from VitaSDK's
  (`runtime/vita/ldscript.ld`) and `--pic-veneer` (without relative
  veneers, lld's absolute Thumb call thunks are not relocated by the Vita
  loader). Release builds use ThinLTO and PGO
  ([PGO and ThinLTO](../cpu/pgo-and-thinlto.md)) and no exceptions or
  unwind tables.
- **Guest memory**: MEM1, MEM2 and the locked cache are fixed views of one
  backing block reserved through the kubridge kernel plugin, with the Wii's
  address aliases folded in `GuestFlat::Host`
  ([flat memory](../cpu/flat-guest-memory.md)).
- **Cores**: the game thread alone on core 2, the render thread on core 0,
  the draw thread on core 1, the workers on core 3 (opened by the
  CapUnlocker plugin; core 0 otherwise), see
  [core layout](../threading/core-layout.md). Clocks: CPU 444, bus 222,
  GPU 222, crossbar 166 MHz.
- **Kernel plugins**: kubridge 0.3.1 or newer (the guest memory views) and
  CapUnlocker (core 3). The launcher checks that both are loaded and says
  how to install them otherwise.
- **Memory**: newlib heap 112 MiB; guest backing 88 MiB + 16 KiB; main RAM
  kept below the retail limit of 365 MB (checked once a second; exceeding it
  stops the game); CDRAM split between three frame slots, models, textures
  and driver headroom. Textures never spill to main RAM: the GPU reads it
  too slowly.
- **SDK fixes by link wrap** (`runtime/vita/`):
  - `fast_tls.c`: initialized newlib and pthread TLS lookups read the kernel
    slot directly (TLS lookups);
  - `fast_clock.c`: `gettimeofday` and `clock_gettime` add the process time
    to a wall time taken once, instead of a system call;
  - `pthread_embedded.c`: caps the mutex semaphore count that VitaSDK's
    pthread-embedded leaks, and waits in the kernel instead of polling;
  - `aeabi_memory.c`: the EABI memory helpers as a C port of
    compiler-builtins;
  - `newlib_sfp.c`: newlib's `__sfp` under a lock (VitaSDK's newlib has no
    stdio locks; two threads opening files got the same `FILE`);
  - `newlib_reent.c`: an exited thread's reclaimed reent no longer closes
    descriptors 0-2;
  - `newlib_net.c`: the network stack's first start takes a larger pool.
- **printf**: newlib has no C99 length modifiers (`%zu` shifts the
  arguments, and a later `%s` faults); `scripts/check-vita-printf.py`
  (a test) rejects them.
- **File descriptors**: 50 per process; the asset cache caps its open
  files.
- **No `thread_local`** in the runtime; per-thread guest state is found by
  stack range ([guest worker](../threading/guest-worker.md)). Waits that must not lose a
  wakeup use timed re-checks or kernel semaphores, because the pthread
  condition variable can lose broadcasts there.
- **Files**: the disc image is read with `sceIoPread` and 64-bit offsets;
  64-byte aligned buffers and 512-byte multiple offsets (the SDK's
  guidance); NAND attributes live in a journal (exFAT has no extended
  attributes); logs go to the console's TTY. Large reads through stdio go
  unbuffered (`setvbuf(file, nullptr, _IONBF, 0)`): through newlib's stream
  buffer a 1.5 MiB `fread` took 5.6-6.6 s on the devkit, and the audio
  streams' extracted copies read in 34 ms instead of 15 ms a request
  ([asset conversion](../assets/load-time-asset-conversion.md)).
- **Input**: `sceCtrl` mapped to the Classic Controller
  ([controller input](../system/controller-input.md)).
- **Audio**: one output thread on core 3, blocking in `sceAudioOutOutput`
  ([native audio](../system/native-audio.md)).

## Why

Each fix came from a Vita measurement or failure:

- `gettimeofday` is a system call of about 6 µs that the guest time base,
  the frame pacing and the statistics call every frame; the wrap took the
  game thread's kernel samples from 3.1% to 1.8% (commit "Read gettimeofday
  from the process time on the Vita").
- The leaking semaphore made `sceKernelSignalSema` fail with `SEMA_OVF`; the
  Vita kernel printed a backtrace per failure and the flood stalled the
  game. The polling waits had three idle asset workers taking about 40% of
  core 0 and a quarter of core 1 (commit "Fix pthread-embedded's leaking
  mutex semaphores and polling waits").
- Removing `thread_local` shrank per-thread TLS from 73 KiB to 52 bytes
  (commit "Remove thread_local from the runtime").
- Logging through blocking host file writes froze the game when the debug
  link stalled; the TTY does not.

## Example

`runtime/CMakeLists.txt`, the link wraps of the Vita build:

```cmake
# pthread-embedded fixes: leaking mutex semaphores, polling waits, new
# threads off the game thread's core.
target_sources(MH3 PRIVATE vita/pthread_embedded.c)
target_link_options(MH3 PRIVATE "-Wl,--wrap=pte_osSemaphorePost" "-Wl,--wrap=pte_cancellable_wait"
  "-Wl,--wrap=pte_osThreadCreate")

# newlib: concurrent opens could take the same FILE (no stdio lock).
target_sources(MH3 PRIVATE vita/newlib_sfp.c)
target_link_options(MH3 PRIVATE "-Wl,--wrap=__sfp")

# gettimeofday from the process time instead of a system call (6 us).
target_sources(MH3 PRIVATE vita/fast_clock.c)
target_link_options(MH3 PRIVATE "-Wl,--wrap=gettimeofday" "-Wl,--wrap=clock_gettime")
```

## Applying it to another Wii game

All of this is game-independent: any port built on this runtime gets the
toolchain, memory views, thread placement, link wraps and file handling.
What a new game changes is the memory budget (guest RAM is fixed at
88 MiB; the rest of the 365 MB main RAM limit and the CDRAM split depend on
the game's assets) and which jobs can use the extra cores. The wraps depend
on VitaSDK's private newlib and pthread layouts: review them when upgrading
VitaSDK.
