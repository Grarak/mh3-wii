// MH3-local producer fence, based on Aurora's synchronous EFB-copy flush path.
// Submit recorded work and resume the same frame without presentation or clearing.
bool mh3_aurora_flush_draws() {
#ifdef AURORA_ENABLE_GX
  aurora::wait_for_frame_worker();
  aurora::gx::mark_frame_interpolation_replay_unsafe();
  aurora::gx::fifo::drain();
  const wgpu::CommandEncoderDescriptor descriptor{.label = "MH3 GXDrawDone encoder"};
  auto encoder = aurora::webgpu::g_device.CreateCommandEncoder(&descriptor);
  aurora::gfx::end_batch(encoder);
  aurora::gfx::render(encoder);
  const auto command = encoder.Finish();
  {
    std::lock_guard submitLock(aurora::g_queueSubmitMutex);
    aurora::webgpu::g_queue.Submit(1, &command);
  }
  aurora::gfx::after_submit();
  return aurora::gfx::resume_frame();
#else
  return false;
#endif
}
