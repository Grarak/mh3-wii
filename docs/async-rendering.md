# macOS asynchronous frame encoding

MH3 now uses Aurora's bounded frame worker on macOS. The existing Aurora worker
was disabled on Apple because it also prepared ImGui/SDL frames and presented
through AppKit. Both require the main thread.

The mh3-local source overlay separates the phases:

1. The game thread decodes GX, snapshots draw resources into Aurora's existing
   storage, and seals the completed display frame (including ImGui draw data).
2. One worker encodes and submits Metal commands from that sealed frame. It
   neither pumps SDL nor prepares or presents windows.
3. The main thread joins before touching the next frame's renderer state and
   presents completed jobs. GXDrawDone still waits for worker and GPU completion;
   EFB CPU reads/writes and texture-copy readbacks keep their coherence checks.

There is at most one outstanding frame. Release/acquire ready flags publish the
worker's completed presentation vector; the existing worker mutex/CV controls
job ownership. The renderer mutex protects encoding. Main-thread begin/update/
completion paths join before consuming results or reusing shared render state.
Shutdown joins the worker and releases sealed GPU references before teardown.

`GXCopyDisp` now seals/queues at the end of the current guest FIFO submission,
rather than waiting until the later GXDrawDone call. This permits overlap with
intervening guest CPU work. FIFO packet framing finishes before handing over the
frame. No raw guest-RAM pointer is newly handed to an asynchronous decoder.

This is **asynchronous completed-frame encoding/submission**, not a complete
threaded GX decoder. GX decoding, array scanning/uploads and mid-frame EFB
passes remain on the game thread. Moving those requires owned resource snapshots
or memory-write synchronization; simply running `GuestGxStream::Write` on another
thread would race translated stores and overlay changes.

Enabled by default on macOS. `MH3_ASYNC_RENDER=0` selects synchronous encoding
for comparison. This is a runtime backend option, not debug instrumentation.
The shared Wiicompiled/Aurora checkout is unchanged; `patch_aurora_async.py`
patches the build-local source and fails if its expected upstream anchors drift.

## Validation

- Runtime, audio and SDL controller CTests pass.
- Metal EFB readback/poke/preservation tests pass with async on and off.
- The 30-frame graphics smoke test completes and shuts the worker down cleanly.
- Post-Chief replay reaches the village and Moga Woods with correct top/left
  alignment and no runtime errors (`logs/replays/async-render`).
- `logs/async-field-sample.txt`: 10-second / 5,120-sample capture confirms
  `encode_sealed_frame` occurs on worker Thread_10313272, not the main thread.
  The startup sampling warmup is excluded from measurements.
- Final-frame completion wait in the field is roughly 1.2 ms rather than the
  prior 1.7 ms, but end-to-end frame intervals remain around 50 ms (~20 FPS).
  Process CPU remains around 42 ms/copy. Do not interpret the smaller wait as
  equivalent total GPU-time savings; submission occurs earlier now.

The next performance target is still redundant graphics-buffer copying, followed
by safely moving GX processing off the game thread. This first step establishes
worker/main-thread ownership without weakening hardware synchronization.

Same-build fallback replay (`logs/replays/async-off`, MH3_ASYNC_RENDER=0) also
reached the field correctly. The final seven steady field batches measured
median 50.05 ms wall / 45.38 ms process CPU / 1.30 ms final completion with
async enabled, versus 50.22 / 45.18 / 1.39 with it disabled. The async window
includes profiler overhead, and animation/workload varies, so these numbers do
not establish a throughput improvement. Both remain about 20 FPS. Earlier
unprofiled async batches were around 42 ms process CPU. The first-stage worker
is verified functionality, not a claim that the dominant decoding/copying cost
has moved off-thread.
