/* Initialized VitaSDK TLS lookup, with the SDK retaining allocation/cleanup. */
#include <psp2/kernel/threadmgr/thread.h>
#include <reent.h>
#include <stddef.h>
#include <string.h>

/* newlib/libc/sys/vita/threading.c stores struct _reent * in this slot. */
enum { MH3_NEWLIB_REENT_TLS_KEY = 0x89 };

extern struct _reent *__real___getreent(void);
extern void *__real_pte_osTlsGetValue(unsigned int key);

/* platform/vita/vita_osal.c: next/key/value; only the SDK writes these nodes. */
struct Mh3VitaTlsKeyEntry {
    struct Mh3VitaTlsKeyEntry *next;
    unsigned int key;
    void *value;
};

#if defined(__vita__)
_Static_assert(sizeof(void *) == 4, "VitaSDK TLS lookup requires ARM32 pointers");
_Static_assert(offsetof(struct Mh3VitaTlsKeyEntry, key) == 4, "VitaSDK key offset");
_Static_assert(offsetof(struct Mh3VitaTlsKeyEntry, value) == 8, "VitaSDK value offset");
_Static_assert(sizeof(struct Mh3VitaTlsKeyEntry) == 12, "VitaSDK TLS node size");
#endif

static inline struct _reent *current_reent(void)
{
    struct _reent **slot = sceKernelGetTLSAddr(MH3_NEWLIB_REENT_TLS_KEY);
    return *slot;
}

struct _reent *__wrap___getreent(void)
{
    struct _reent *reent = current_reent();

    if (reent)
        return reent;

    return __real___getreent();
}

void *__wrap_pte_osTlsGetValue(unsigned int key)
{
    struct _reent *reent = current_reent();

    if (!reent)
        return __real_pte_osTlsGetValue(key);

    /* The newlib reent_for_thread prefix contains tls_data_ext followed by
       pthread_data_ext immediately before reent. Copy the head pointer to
       avoid aliasing newlib's private struct. This becomes one ARM32 load. */
    struct Mh3VitaTlsKeyEntry *entry;
    memcpy(&entry, (char *)reent - 2 * sizeof(void *), sizeof(entry));

    while (entry) {
        if (entry->key == key)
            return entry->value;

        entry = entry->next;
    }

    return NULL;
}
