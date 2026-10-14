/* Link-time fix (-Wl,--wrap=__sfp) for VitaSDK's newlib stdio. */
#include <psp2/kernel/threadmgr.h>
#include <reent.h>
#include <stdio.h>

FILE *__real___sfp(struct _reent *reent);

/* fopen, fdopen and the other openers take their FILE in __sfp: the first
 * entry of the global table whose flags are 0, which it then reserves. The
 * SDK's newlib builds its lock (__sfp_lock_acquire) empty, so two threads
 * opening files at once could reserve the same FILE, and one's open or close
 * broke the other's stream. The render thread reads a mesh pack while the
 * asset workers write the converted packs of an area entered the first time:
 * its read ended early ("Native mesh payload is truncated or unreadable",
 * not on the next launch, the packs then converted). The
 * reservation takes this lock; a close frees its FILE with a single store. */
static int sfp_held;

static void Pause(unsigned tries)
{
    if (tries >= 65536)
        sceKernelDelayThread(50);
}

FILE *__wrap___sfp(struct _reent *reent)
{
    unsigned tries = 0;

    while (__atomic_exchange_n(&sfp_held, 1, __ATOMIC_ACQUIRE))
        do
            Pause(tries++);
        while (__atomic_load_n(&sfp_held, __ATOMIC_RELAXED));

    FILE *file = __real___sfp(reent);
    __atomic_store_n(&sfp_held, 0, __ATOMIC_RELEASE);
    return file;
}
