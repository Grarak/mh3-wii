#pragma once

#ifdef __vita__
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <cstdio>
#endif

namespace aurora {
// Any: every user core (0-2); the scheduler places the thread.
enum class ThreadRole { Game, Render, Worker, Any };
inline void pin_current_thread(ThreadRole role) noexcept {
#ifdef __vita__
  const unsigned core = role == ThreadRole::Game ? 2 : role == ThreadRole::Render ? 1 : 0;
  const int result = sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), role == ThreadRole::Any ? SCE_KERNEL_CPU_MASK_USER_ALL : SCE_KERNEL_CPU_MASK_USER_0 << core);
  if (result < 0) {
    std::printf("MH3 Vita: thread affinity core %u failed: 0x%08x\n", core, unsigned(result));
    // An unpinned game/render thread violates the port's scheduling contract.
    sceKernelExitProcess(1);
  }
#else
  (void)role;
#endif
}
} // namespace aurora
