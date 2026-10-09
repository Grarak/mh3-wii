/* gettimeofday without a system call. VitaSDK's goes to the kernel
 * (sceKernelLibcGettimeofday, about 6 us), and libstdc++'s steady_clock and
 * system_clock read it: the guest time base (mftb, OSGetTime), the frame
 * pacing and the statistics call them every frame. The process time is a
 * 0.7 us microsecond counter; the wall time at startup plus the process time
 * elapsed since gives the same clock. */
#include <psp2/kernel/processmgr.h>
#include <stdint.h>
#include <sys/time.h>

extern int __real_gettimeofday(struct timeval *tv, void *tz);

static int64_t start_wall_us;
static SceUInt64 start_process_us;
static int started;

/* Before main, while there is one thread. */
__attribute__((constructor(101))) static void start_clock(void)
{
    struct timeval now;

    if (__real_gettimeofday(&now, 0) != 0)
        return;

    start_process_us = sceKernelGetProcessTimeWide();
    start_wall_us = (int64_t)now.tv_sec * 1000000 + now.tv_usec;
    started = 1;
}

int __wrap_gettimeofday(struct timeval *tv, void *tz)
{
    if (!started || tz || !tv)
        return __real_gettimeofday(tv, tz);

    const int64_t us = start_wall_us + (int64_t)(sceKernelGetProcessTimeWide() - start_process_us);
    tv->tv_sec = (time_t)(us / 1000000);
    tv->tv_usec = (suseconds_t)(us % 1000000);
    return 0;
}
