# Barriers off the per-call paths (ARM32)

## What the original does

The Wii's Broadway has no memory barriers on the paths in question: the
game's code is single-threaded where it matters and its flags are plain
loads.

## What the port does instead

The port's own C++ around the translated code put barriers there without
looking like it. On ARMv7 an acquire load or a sequentially consistent
atomic is `ldr` plus `dmb ish`, and so is the guard test of every
function-local `static` with a dynamic initializer
(`static const bool check = f();`), on every pass; a `std::shared_ptr`
copy or release is an atomic read-modify-write between two `dmb`s. The
Cortex-A9 waits at each `dmb` for its earlier memory accesses.

Three hot ones were removed:

- **Port-check flags** were function-local statics in every checked
  native. `PortCheck::Set()` now caches "is `MH3_PORT_CHECK` set" in a
  relaxed atomic byte that the natives test inline before any slow path.
- **The base-translation verdict** (`IsBaseTranslatedCpuTargetActive`),
  tested for each function of a region on every state-free translated
  call, was an acquire load. The verdict is the whole value and the
  registry it comes from is immutable once published (a thread that still
  sees 0 recomputes the same answer), so the load is relaxed. Translated
  code fell from 7,431 to 2,988 `dmb` instructions.
- **`ResourceMemory()`**, read by every NW4R resource accessor, returned a
  function-local static; the guest views are fixed host addresses, so it is
  a namespace-scope constant. The indirect-call memo tests its slot before
  the acquire of the publication flag (a slot is only filled after the flag
  was seen), so memo hits skip the barrier.

## Why

The Pi 5's Cortex-A76 hides barriers: on it the changes stayed within the
run-to-run spread (port-check functions 0.27 → 0.10% of samples,
`ResourceMemory` 0.27 → 0.01%). The A9 does not, so barrier work is judged
by counts in the binary and by the Vita profile, not by Pi timings.
[Profiling](../../profiling.md#barriers-on-arm32) has the census command
and the barriers still on hot paths.

## Example

Before, each checked native kept its own flag (a `dmb` on every call, the
guard test of a function-local static):

```cpp
static const bool checked = PortCheck::Enabled("Name");
```

After, `runtime/include/port_check.h`:

```cpp
inline std::atomic<int8_t> g_set{-1};
bool SetSlow();
inline bool Set() {
    const int8_t set = g_set.load(std::memory_order_relaxed);

    if (set >= 0) [[likely]]
        return set != 0;
    return SetSlow();
}
```

## Applying it to another Wii game

Generic: any runtime around a translated game on ARMv7. Keep flags in
namespace-scope variables or relaxed atomics, keep guarded statics and
`shared_ptr` copies out of per-call paths, and count `dmb`s per function
in the binary against the profile's hot functions.

Game-specific: none; the hot paths are the runtime's.
