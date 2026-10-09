#!/usr/bin/env python3
"""Patch only the build-local Aurora copy: one sealed frame, main-thread UI.

The game thread prepares, seals and presents frames (SDL, X11/Wayland and
ImGui stay on the main thread); the bounded worker only encodes and submits
the sealed frame. Upstream Aurora's Linux path instead lets the worker prepare
frames and hands presentation to a presenter thread, which MH3's mid-frame GX
fences were never validated with.
"""
import sys
from pathlib import Path
p = Path(sys.argv[1])
s = p.read_text()
def replace(old, new):
    global s
    if s.count(old) != 1:
        raise SystemExit(f'Aurora async overlay no longer matches: {old[:100]}')
    s = s.replace(old, new)
replace('#include <SDL3/SDL_thread.h>', '#include <SDL3/SDL_thread.h>\n#include <SDL3/SDL_init.h>')
replace("""#if defined(__APPLE__)
    // ImGui's SDL backend may raise an SDL window from ImGui::NewFrame(). On
    // macOS that reaches AppKit, whose window operations are main-thread-only;
    // doing it on the frame worker terminates the process with EXC_BREAKPOINT.
    // Keep all SDL/ImGui work on the calling thread until the worker no longer
    // owns frame preparation on Apple platforms.
    return false;
#endif
#if defined(_WIN32)""", """    // MH3 keeps frame preparation, sealing and presentation on the main
    // thread. Only GPU encoding/submission moves to the bounded worker.
    // The opt-out retains a synchronous baseline for A/B checks.
    if (const char* enabled = std::getenv("MH3_ASYNC_RENDER"); enabled && std::strcmp(enabled, "0") == 0)
      return false;
#if defined(_WIN32)""")
# Declarations precede shutdown/update. Definitions need SealedFrameContext.
replace('void shutdown() noexcept {', '''void mh3_present_completed_frame();
void mh3_release_sealed_frame();
void shutdown() noexcept {''')
replace('''  stop_frame_worker();
#ifdef AURORA_ENABLE_GX''', '''  stop_frame_worker();
#if MH3_MAIN_THREAD_PRESENTATION && defined(AURORA_ENABLE_GX)
  mh3_present_completed_frame();
  mh3_release_sealed_frame();
#endif
#ifdef AURORA_ENABLE_GX''')
replace('''const AuroraEvent* update() noexcept {
  ZoneScoped;''', '''const AuroraEvent* update() noexcept {
  ZoneScoped;
#if MH3_MAIN_THREAD_PRESENTATION && defined(AURORA_ENABLE_GX)
  wait_for_frame_worker_private(FrameWorkerPhase::Done);
  mh3_present_completed_frame();
#endif''')
anchor='// Phase 2: encode every presentation slot.'
replace(anchor, '''#if MH3_MAIN_THREAD_PRESENTATION
// One outstanding frame: sealing and SDL/ImGui preparation belong to the
// main thread. The worker owns only immutable sealed commands and GPU encoding.
gfx::SealedFrame mh3_sealedFrame;
SealedFrameContext mh3_sealedContext;
std::vector<PresentationJob> mh3_completedPresentations;
void mh3_present_completed_frame() {
  if (!SDL_IsMainThread()) return;
  // Callers have joined the worker (or hold its initial ready state).
  for (const auto& job : mh3_completedPresentations) present_presentation_job(job);
  mh3_completedPresentations.clear();
}
void mh3_release_sealed_frame() {
  mh3_completedPresentations.clear();
  mh3_sealedFrame = {};
  mh3_sealedContext = {};
}
#endif

''' + anchor)
replace('''  ZoneScopedN("Frame worker cycle");
  webgpu::fail_if_device_lost();''', '''  ZoneScopedN("Frame worker cycle");
  webgpu::fail_if_device_lost();
#if MH3_MAIN_THREAD_PRESENTATION
  {
    std::lock_guard gpuLock(g_rendererGpuMutex);
    mh3_completedPresentations = encode_sealed_frame(mh3_sealedFrame, mh3_sealedContext);
  }
  record_frame_telemetry();
  {
    std::lock_guard lock(g_frameWorker.mutex);
    g_frameWorker.sealed.store(true, std::memory_order_release);
    g_frameWorker.ready.store(true, std::memory_order_release);
  }
  g_frameWorker.cv.notify_all();
  return true;
#endif''')
replace('''  ensure_frame_worker_started();
  // SDL needs event pumping''', '''  ensure_frame_worker_started();
#if MH3_MAIN_THREAD_PRESENTATION
  wait_for_frame_worker_private(FrameWorkerPhase::Done);
  mh3_present_completed_frame();
  const bool mh3_prepared = begin_frame_impl(true);
  {
    std::lock_guard lock(g_frameWorker.mutex);
    g_frameWorker.framePrepared = mh3_prepared;
  }
  return mh3_prepared;
#endif
  // SDL needs event pumping''')
replace('''  // Later FIFO writes belong exclusively to the next frame.
  gx::fifo::drain();''', '''  // Later FIFO writes belong exclusively to the next frame.
  gx::fifo::drain();
#if MH3_MAIN_THREAD_PRESENTATION
  mh3_present_completed_frame();
  {
    std::lock_guard gpuLock(g_rendererGpuMutex);
    seal_frame_locked(mh3_sealedFrame, mh3_sealedContext);
  }
#endif''')
replace('''void wait_for_frame_worker() noexcept { wait_for_frame_worker_private(FrameWorkerPhase::Done); }''', '''void wait_for_frame_worker() noexcept {
  wait_for_frame_worker_private(FrameWorkerPhase::Done);
#if MH3_MAIN_THREAD_PRESENTATION && defined(AURORA_ENABLE_GX)
  mh3_present_completed_frame();
#endif
}''')
s = '#define MH3_MAIN_THREAD_PRESENTATION 1\n#include <cstring>\n' + s
p.write_text(s)
