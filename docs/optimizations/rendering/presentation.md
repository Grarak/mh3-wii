# Presentation: frame slots, triple buffering and frame skipping

## What the original does

The Wii renders into the EFB, copies the finished frame to an external
framebuffer (XFB) in RAM, and hands that to the video interface with
`VISetNextFrameBuffer` and `VIFlush`, which switches at the next retrace.
MH3's main loop (0x800401A8) counts VI retraces and runs one frame per
budget of retraces (two at 60 Hz, so the game logic runs at 30 frames per
second). The CPU and GPU share one FIFO, so the CPU can only run ahead as far
as the FIFO and the game's waits allow.

## What the port does instead

On the Vita (`aurora-main/lib/gxm/device.cpp`,
`aurora-main/include/aurora/gpu_completion.hpp`):

- **Three display buffers**, each with its own GXM sync object, used for its
  scene and its queued flip.
- **Three GPU frame slots** of 3.5 MiB of CDRAM each (1 MiB vertices,
  2 MiB uniforms, 512 KiB U16 indices). Every scene that uses the current
  slot records its notification; only a presented frame advances the ring.
- **Admission**: before the render thread prepares a packet it polls
  whether the slot still has GPU work or three flips are queued
  (`Device::frame_pending`). With the frame limit on, a presenting packet
  waits on the render thread until the slot frees (polled every 200 µs, at
  most 100 ms), so every packet is drawn; with the limit off it skips the
  packet's drawing before touching the slot's memory, and the packet's tasks
  still run. The game thread never waits for the GPU
  ([render thread](../threading/render-thread.md)).
- **The display callback** (a worker core) flips. It sets each buffer for
  the next VBlank and waits for it there, so frames do not tear; the wait is
  never on the game or render thread. With the frame limit off VSync is off
  too and flips take effect at once. GXM's display queue allows four
  entries for three admitted flips: the callback counts a flip as done just
  before libgxm retires its entry.
- **The present pass** (GXM) copies the finished 960×544 frame into the
  display buffer with a utility program. At the source's own size it samples
  with point filtering, which equals linear filtering at texel centres
  (commit "Point-sample the present when it copies at the source's size").
- **Linux/GLES** requests swap interval 0 and admits a frame the same way,
  polling a GL fence with a zero timeout (`gl::gpu_frame_pending`,
  `aurora-main/lib/gl/gl.cpp`).
- **Display-locked steps** (Vita, frame limit on): frames reach the
  screen a VBlank apart through the packet ring and the display queue, but
  a step measured as the game thread's time between frame starts varied
  with slot waits and the just-in-time start (12-44 ms), so camera turns
  stuttered on screen while the game's values were smooth. A shown frame's
  step is one VBlank plus the VBlanks the display kept a frame for because
  the next was late (`sceDisplayGetVcount` at each flip), paid back an
  eighth of what is owed each frame (past 4 retraces, at once). Loading
  screens, the limit off and GL keep the host time
  (`DisplayStep`, `runtime/src/frame_pacer.cpp`).
- **Pacing** comes from the game thread limiter
  ([render thread](../threading/render-thread.md)) and the variable step
  ([variable step](../simulation/variable-step.md)), not from the display (except on the
  web, above).

## Why

A renderer that waits for the GPU or for a flip stalls the thread that
waits. Admission keeps both CPU threads busy and lets the display rate fall
where the GPU and render thread put it. Admitting a frame below three
pending flips instead of two took the Vita's displayed rate in the
Alatreon fight from about 30-32 to 41-42 fps: at two, the render thread had
skipped 14-23 of about 50 packets per second because the frame two back
was often still queued. The present pass is bound by its memory traffic:
point sampling took it from 0.61 to 0.58 ms per frame (GPU capture).

Display-locked steps, Vita, the same scripted camera turns: the difference
between a flip's interval and its frame's game advance went from 1.93 to
0.42 ms a flip; in a 10 s turn the flips off the turn speed by more than
10% went from 158 of 555 to 24, by more than 25% from 76 to 7.

## Example

`aurora-main/include/aurora/gpu_completion.hpp`:

```cpp
inline bool gpu_notification_pending(const volatile unsigned* notification, uint32_t submitted) noexcept {
  return notification && uint32_t(*notification - submitted) >= 0x80000000u;
}

inline bool gpu_display_queue_full(uint32_t queued, uint32_t completed) noexcept {
  return uint32_t(queued - completed) >= GpuFrameRing::BufferCount;
}
```

`Device::end()` writes `m_sceneSerial + 1` into the notification region at
the end of every scene; a slot is free when the region has passed the
slot's last serial.

## Applying it to another Wii game

Generic: the slot ring, the notification serials, the three buffers with
sync objects and the admission rule are libgxm usage, independent of the
game ([libgxm guide](libgxm-for-wii-ports.md)).

Game-specific: the frame rate the game logic expects and how it measures
time. MH3 counts retraces; with a variable step it measures the host
clock instead. A game that ties logic to presented frames must keep
presenting at its rate, or skip drawing without skipping logic, as here.
