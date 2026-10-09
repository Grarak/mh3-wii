/* Link-time fixes (-Wl,--wrap) for VitaSDK's pthread-embedded. */
#include <errno.h>
#include <psp2/kernel/threadmgr.h>

/* platform/vita/pte_types.h, pte_generic_osal.h */
typedef SceUID pte_osSemaphoreHandle;
enum { PTE_OS_OK = 0, PTE_OS_TIMEOUT = 3 };
typedef int pte_osResult;
extern pte_osResult __real_pte_osSemaphorePost(pte_osSemaphoreHandle handle, int count);
extern pte_osResult pte_osSemaphorePend(pte_osSemaphoreHandle handle, unsigned int *timeoutMsecs);

/* pte's normal mutex leaks kernel semaphore counts: a contended lock that
 * the slow path then takes without waiting (pthread_mutex_lock exchanges the
 * lock word to -1 and finds it free) still makes the next unlock post the
 * mutex's semaphore, and no waiter consumes the token. Mutexes taken often
 * under near-misses, such as the asset cache's with its workers' 20 ms timed
 * waits, reach the kernel's limit within minutes. Every later post then fails
 * with SCE_KERNEL_ERROR_SEMA_OVF, which pte ignores, but the devkit's kernel
 * prints a backtrace for each failure: the flood blocks the console, so the
 * game's own printf stalls and the game hangs (the title movie, 2026-10-04).
 *
 * Posts above Cap tokens are dropped. pthread semaphores (sem_post,
 * pthread_once, condition variables) post only for registered waiters, far
 * fewer than Cap, and a mutex with Cap stale tokens still wakes every
 * waiter: each pass of its lock loop takes one and retries the lock. */
enum { Cap = 64 };

pte_osResult __wrap_pte_osSemaphorePost(pte_osSemaphoreHandle handle, int count)
{
    SceKernelSemaInfo info;
    info.size = sizeof info;
    if (sceKernelGetSemaInfo(handle, &info) >= 0 && info.currentCount + count > Cap) {
        count = Cap - info.currentCount;
        if (count <= 0)
            return PTE_OS_OK;
    }
    return __real_pte_osSemaphorePost(handle, count);
}

/* sem_wait and sem_timedwait, so every condition variable wait, go through
 * pte_cancellable_wait. With cancellation enabled (every thread's default)
 * it polls: a 500 us semaphore wait and a cancel-flag check in a loop, 2000
 * wakeups a second per waiting thread. The asset cache's three idle workers
 * alone took about 5% of the game thread's core and a quarter of the render
 * thread's. Nothing here cancels threads, so wait in the kernel as pte does
 * for threads with cancellation disabled. */
int __wrap_pte_cancellable_wait(pte_osSemaphoreHandle handle, unsigned int *timeoutMsecs)
{
    switch (pte_osSemaphorePend(handle, timeoutMsecs)) {
    case PTE_OS_OK:
        return 0;
    case PTE_OS_TIMEOUT:
        return ETIMEDOUT;
    default:
        return EINVAL;
    }
}
