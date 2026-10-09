/* gettimeofday and clock_gettime without a system call. VitaSDK's goes to the kernel
 * (sceKernelLibcGettimeofday, about 6 us), and libstdc++'s steady_clock and
 * system_clock read it: the guest time base (mftb, OSGetTime), the frame
 * pacing and the statistics call them every frame. The process time is a
 * 0.7 us microsecond counter; the wall time at startup plus the process time
 * elapsed since gives the same clock.
 *
 * clock_gettime reads the same clock: pthread-embedded turns the absolute
 * deadline of every timed wait (libstdc++'s wait_for and wait_until, from
 * gettimeofday) into a timeout with ftime, which reads clock_gettime. The
 * process time stops while the Vita suspends the game, the kernel's wall
 * time does not: after a resume each deadline lay the suspension's length in
 * the past on the kernel's clock, every timed wait returned at once and its
 * loop spun, four threads at 100% on cores 0-2 (the resume freeze). */
#include <psp2/kernel/processmgr.h>
#include <stdint.h>
#include <sys/time.h>
#include <time.h>

extern int __real_gettimeofday(struct timeval *tv, void *tz);
extern int __real_clock_gettime(clockid_t clock, struct timespec *ts);

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

static int64_t now_us(void)
{
    return start_wall_us + (int64_t)(sceKernelGetProcessTimeWide() - start_process_us);
}

int __wrap_gettimeofday(struct timeval *tv, void *tz)
{
    if (!started || tz || !tv)
        return __real_gettimeofday(tv, tz);

    const int64_t us = now_us();
    tv->tv_sec = (time_t)(us / 1000000);
    tv->tv_usec = (suseconds_t)(us % 1000000);
    return 0;
}

int __wrap_clock_gettime(clockid_t clock, struct timespec *ts)
{
    if (!started || !ts || (clock != CLOCK_REALTIME && clock != CLOCK_MONOTONIC))
        return __real_clock_gettime(clock, ts);

    const int64_t us = now_us();
    ts->tv_sec = (time_t)(us / 1000000);
    ts->tv_nsec = (long)(us % 1000000) * 1000;
    return 0;
}
