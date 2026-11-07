#pragma once

#ifdef __vita__
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <cstdio>
#endif

namespace aurora {
// Any: the user cores but the game thread's (0-1); the scheduler places the
// thread. Core 2 is the game thread's alone, core 0 the render thread's and
// core 1 the draw thread's (runtime/src/guest_worker.cpp). Workers run on
// core 3 (the cap unlocker opens it), core 0 without it. Reader (the DVD
// reader): cores 0 and 3, so it need not wait for the guest worker's core.
enum class ThreadRole { Game, Render, Worker, Reader, Any };
inline void pin_current_thread(ThreadRole role) noexcept {
#ifdef __vita__
  const unsigned core = role == ThreadRole::Game ? 2 : role == ThreadRole::Worker ? 3 : 0; // Render: core 0
  const int mask = role == ThreadRole::Any      ? SCE_KERNEL_CPU_MASK_USER_0 | SCE_KERNEL_CPU_MASK_USER_1
                   : role == ThreadRole::Reader ? SCE_KERNEL_CPU_MASK_USER_0 | SCE_KERNEL_CPU_MASK_USER_0 << 3
                                                : SCE_KERNEL_CPU_MASK_USER_0 << core;
  int result = sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), mask);

  if (result < 0 && role == ThreadRole::Worker)
    result = sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), SCE_KERNEL_CPU_MASK_USER_0);

  if (result < 0) {
    std::printf("MH3 Vita: thread affinity mask 0x%x failed: 0x%08x\n", unsigned(mask), unsigned(result));

    // An unpinned game/render thread violates the port's scheduling contract.
    sceKernelExitProcess(1);
  }
#else
  (void)role;
#endif
}
} // namespace aurora
