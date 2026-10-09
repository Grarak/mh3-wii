#include <aurora/gpu_completion.hpp>
#include <aurora/native_frame_gate.hpp>

#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <limits>

int main() {
  aurora::NativeFrameGate frame;
  assert(frame.drawing_allowed());
  volatile unsigned completion = 6;
  uint32_t submitted = 7;
  unsigned polls = 0, logicTicks = 0, renders = 0, skips = 0;
  std::array<uint32_t, 8> payload{};
  payload.fill(0xabc123u); // GPU still reads the previous frame's CDRAM.
  const auto previousPayload = payload;
  const auto pending = [&] {
    ++polls;
    return aurora::gpu_notification_pending(&completion, submitted);
  };

  // Many skipped game frames still advance logic. Admission polls once, with
  // no retry/wait; skipped frames cannot prepare draws or rewrite the payload.
  for (unsigned tick = 0; tick != 100; ++tick) {
    const bool render = frame.begin(true, true, pending);
    ++logicTicks;
    assert(frame.active());
    assert(frame.should_render() == render);
    assert(!frame.drawing_allowed());

    if (render) { ++renders; payload.fill(logicTicks); }
    else ++skips;
    frame.end();
    assert(!frame.active() && !frame.should_render());
  }

  assert(logicTicks == 100 && renders == 0 && skips == 100 && polls == 100);
  assert(payload == previousPayload);

  // A notification arriving during a skipped logic step does not reopen it.
  assert(!frame.begin(true, true, pending));
  completion = submitted;
  assert(!frame.should_render());
  assert(polls == 101);
  frame.end();
  assert(frame.begin(true, true, pending));
  assert(frame.should_render() && polls == 102);
  ++logicTicks;
  payload.fill(logicTicks);
  frame.end();
  assert(payload != previousPayload);

  // Demand loading may cancel an admitted frame after earlier offscreen draws.
  // Cancellation closes drawing/presentation, preserves the current slot's
  // scene lease and never reopens when the notification arrives mid-frame.
  frame.cancel(); // Inactive cancellation preserves standalone preparation.
  assert(frame.drawing_allowed());
  aurora::GpuFrameRing canceledRing;
  volatile unsigned canceledCompletion = 0;
  assert(frame.begin(true, true, [] { return false; }));
  canceledRing.record_scene(&canceledCompletion, 1);
  frame.cancel(); frame.cancel();
  assert(frame.active() && !frame.should_render() && !frame.drawing_allowed());
  assert(canceledRing.slot() == 0 && canceledRing.pending());
  canceledCompletion = 1;
  assert(!frame.should_render());
  frame.end();
  assert(frame.begin(true, true, [&] { return canceledRing.pending(); }));
  assert(canceledRing.slot() == 0);
  frame.end();

  // New offscreen work owns memory even if the old display scene completed.
  ++submitted;
  assert(!frame.begin(true, true, pending));
  frame.end();
  assert(completion == 7 && submitted == 8);

  // Compare scene serials across wrap, including a ready zero-valued word.
  completion = std::numeric_limits<uint32_t>::max();
  assert(aurora::gpu_notification_pending(&completion, 0));
  completion = 0;
  assert(!aurora::gpu_notification_pending(&completion, 0));
  assert(!aurora::gpu_notification_pending(&completion, std::numeric_limits<uint32_t>::max()));
  assert(!aurora::gpu_notification_pending(nullptr, 0));

  // Explicitly disabled frameskip still renders. A hidden window always skips
  // without invoking the backend or reading a GPU notification.
  const auto previousPolls = polls;
  completion = 6;
  assert(frame.begin(false, true, pending));
  frame.end();
  assert(!frame.begin(true, false, pending));
  frame.end();
  assert(polls == previousPolls);

  // Three mutable slots can coexist. The fourth submission must retain the
  // next slot's payload until its own notification completes, even though
  // newer frames are still pending. Skipped game steps never rotate the ring.
  aurora::GpuFrameRing ring;
  volatile unsigned retired = 0;
  std::array<std::array<uint32_t, 8>, aurora::GpuFrameRing::BufferCount> buffers{};

  for (unsigned serial = 1; serial != 4; ++serial) {
    assert(ring.slot() == serial - 1 && !ring.pending());
    buffers[ring.slot()].fill(serial);
    ring.record_scene(&retired, serial);
    ring.advance();
  }

  const auto outstanding = buffers;

  for (unsigned tick = 0; tick != 50; ++tick) {
    assert(!frame.begin(true, true, [&] { return ring.pending(); }));
    ++logicTicks;

    if (frame.should_render()) {
      buffers[ring.slot()].fill(4);
      ring.record_scene(&retired, 4);
      ring.advance();
    }

    frame.end();
  }

  assert(ring.slot() == 0 && buffers == outstanding);
  retired = 1;
  assert(!ring.pending()); // Slot 0 ready; slots 1/2 can still be in flight.
  buffers[0].fill(4);
  ring.record_scene(&retired, 4);
  ring.advance();
  assert(ring.slot() == 1 && ring.pending());
  assert(buffers[1] == outstanding[1] && buffers[2] == outstanding[2]);
  retired = 2;
  assert(!ring.pending());
  ring.record_scene(&retired, 5); // Offscreen scene extends the current lease.
  assert(ring.pending());
  retired = 5;
  assert(!ring.pending());

  // Protect the display queue too: at three pending flips (its capacity),
  // reject before any draw or queue API call. A completed callback admits
  // the next frame.
  assert(!aurora::gpu_display_queue_full(0, 0));
  assert(!aurora::gpu_display_queue_full(1, 0));
  assert(!aurora::gpu_display_queue_full(2, 0));
  assert(aurora::gpu_display_queue_full(3, 0));
  assert(aurora::gpu_display_queue_full(4, 0));
  assert(!aurora::gpu_display_queue_full(3, 1));
  assert(aurora::gpu_display_queue_full(1, std::numeric_limits<uint32_t>::max() - 1));
  assert(!aurora::gpu_display_queue_full(0, std::numeric_limits<uint32_t>::max() - 1));
  frame.end();
  assert(frame.drawing_allowed());
  std::puts("Native frameskip: logic advances, one poll, latched skips, triple buffer leases, immutable busy payloads, queue capacity and serial wrap passed");
}
