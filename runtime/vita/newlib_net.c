/* Link-time fix (-Wl,--wrap=_vita_net_init) for VitaSDK's newlib sockets. */
#include <psp2/kernel/threadmgr.h>
#include <psp2/net/net.h>
#include <psp2/sysmodule.h>
#include <stdio.h>
#include <stdlib.h>

int __real__vita_net_init(void);

/* newlib starts SceNet at its first network call (socket, inet_pton,
 * gethostbyname and others) with a pool of 0x23114 bytes, which the game's
 * sockets use up: the stack then stops taking (and acknowledging) segments
 * for minutes. On the devkit (2026-10-08) a city entry stalled on a 1 KiB
 * packet until the server dropped the client ("Client timed out"); every
 * online run had started the stack there first (NetHost's DefaultInterface
 * calls inet_pton), and sceNetInit with a larger pool then failed (EBUSY).
 * Here the larger pool comes first, whichever call starts the stack; newlib's
 * own sceNetInit then fails with EBUSY, frees its pool and goes on. */
enum { kPoolBytes = 2 << 20 };

static int state; /* 0 not started, 1 starting, 2 started */

static void Start(void)
{
    if (sceSysmoduleIsLoaded(SCE_SYSMODULE_NET) != SCE_SYSMODULE_LOADED && sceSysmoduleLoadModule(SCE_SYSMODULE_NET) < 0)
        return;

    SceNetInitParam parameters = {0};
    parameters.memory = malloc(kPoolBytes);
    parameters.size = kPoolBytes;

    if (!parameters.memory)
        return;

    const int initialized = sceNetInit(&parameters);

    if (initialized < 0) {
        fprintf(stderr, "MH3 net: sceNetInit 0x%08X\n", (unsigned)initialized);
        free(parameters.memory);
    }
}

int __wrap__vita_net_init(void)
{
    int expected = 0;

    if (__atomic_compare_exchange_n(&state, &expected, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        Start();
        __atomic_store_n(&state, 2, __ATOMIC_RELEASE);
    } else {
        while (__atomic_load_n(&state, __ATOMIC_ACQUIRE) != 2)
            sceKernelDelayThread(100);
    }

    return __real__vita_net_init();
}
