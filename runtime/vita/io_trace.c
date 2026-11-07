/* Development builds: the memory card IO the game does while it plays
 * (-Wl,--wrap of the sceIo calls newlib, the disc image, the asset cache and
 * the packs use). MH3_TRACE=io makes the game thread print, once a second,
 * the IO of that second outside loading screens (operations, bytes, time
 * spent in them, the threads), and every operation over 20 ms at once. */
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

SceUID __real_sceIoOpen(const char *file, int flags, SceMode mode);
int __real_sceIoClose(SceUID fd);
SceSSize __real_sceIoRead(SceUID fd, void *buf, SceSize nbyte);
int __real_sceIoPread(SceUID fd, void *data, SceSize size, SceOff offset);
SceSSize __real_sceIoWrite(SceUID fd, const void *buf, SceSize nbyte);
int __real_sceIoPwrite(SceUID fd, const void *data, SceSize size, SceOff offset);
int __real_sceIoRemove(const char *file);
int __real_sceIoRename(const char *oldname, const char *newname);
int __real_sceIoGetstat(const char *file, SceIoStat *stat);
int __real_sceIoMkdir(const char *dir, SceMode mode);

enum { Read, Write, Meta, Kinds };

enum { Files = 64, PathBytes = 80, Threads = 8, Slow = 16 };

static const char *const kind_names[Kinds] = {"read", "write", "open/close/stat/remove/rename"};

/* Descriptors opened through the wrapper: their path. Others (the TTYs
 * stdout and stderr write to) are not counted. */
static struct {
    SceUID fd;
    char path[PathBytes];
} files[Files];

struct Totals {
    uint32_t ops[Kinds];
    uint64_t bytes[Kinds], us[Kinds];
    uint32_t max_us;
    char max_path[PathBytes];
    struct {
        SceUID id;
        uint32_t ops;
        uint64_t us;
    } threads[Threads];
};

/* This second's IO outside loading screens, the slow operations not printed
 * yet, and whether one is up (the game thread's view, a frame old). */
static struct Totals totals;
static struct {
    char line[160];
} slow[Slow];

static unsigned slow_count, slow_dropped;
static int held, on, loading;

static void Lock(void)
{
    while (__atomic_exchange_n(&held, 1, __ATOMIC_ACQUIRE))
        while (__atomic_load_n(&held, __ATOMIC_RELAXED))
            ;
}

static void Unlock(void)
{
    __atomic_store_n(&held, 0, __ATOMIC_RELEASE);
}

static int Slot(SceUID fd)
{
    for (int i = 0; i < Files; ++i)
        if (files[i].fd == fd)
            return i;

    return -1;
}

static void Note(int kind, const char *path, uint64_t bytes, uint64_t start, const char *what)
{
    const uint32_t us = (uint32_t)(sceKernelGetProcessTimeWide() - start);
    const int in_loading = __atomic_load_n(&loading, __ATOMIC_RELAXED);
    const SceUID thread = sceKernelGetThreadId();
    Lock();

    if (!in_loading) {
        totals.ops[kind]++;
        totals.bytes[kind] += bytes;
        totals.us[kind] += us;

        if (us > totals.max_us) {
            totals.max_us = us;
            snprintf(totals.max_path, PathBytes, "%s %s", what, path);
        }

        for (int i = 0; i < Threads; ++i)
            if (totals.threads[i].id == thread || !totals.threads[i].id) {
                totals.threads[i].id = thread;
                totals.threads[i].ops++;
                totals.threads[i].us += us;
                break;
            }
    }

    if (us > 20000) {
        if (slow_count < Slow)
            snprintf(slow[slow_count++].line, sizeof slow[0].line, "%s %s %llu bytes %.1f ms thread 0x%x%s", what, path, (unsigned long long)bytes, us / 1000.0,
                     (unsigned)thread, in_loading ? " (loading screen)" : "");
        else
            ++slow_dropped;
    }

    Unlock();
}

static void PathOf(SceUID fd, char *out)
{
    Lock();
    const int i = Slot(fd);
    strcpy(out, i >= 0 ? files[i].path : "");
    Unlock();
}

SceUID __wrap_sceIoOpen(const char *file, int flags, SceMode mode)
{
    if (!__atomic_load_n(&on, __ATOMIC_RELAXED))
        return __real_sceIoOpen(file, flags, mode);

    const uint64_t start = sceKernelGetProcessTimeWide();
    const SceUID fd = __real_sceIoOpen(file, flags, mode);

    if (fd >= 0) {
        Lock();
        const int i = Slot(0);

        if (i >= 0) {
            files[i].fd = fd;
            snprintf(files[i].path, PathBytes, "%s", file);
        }

        Unlock();
    }

    Note(Meta, file, 0, start, (flags & (SCE_O_WRONLY | SCE_O_RDWR)) ? "open for writing" : "open");
    return fd;
}

int __wrap_sceIoClose(SceUID fd)
{
    char path[PathBytes];
    PathOf(fd, path);
    const uint64_t start = sceKernelGetProcessTimeWide();
    const int result = __real_sceIoClose(fd);

    if (!path[0])
        return result;

    Lock();
    const int i = Slot(fd);

    if (i >= 0)
        files[i].fd = 0;

    Unlock();
    Note(Meta, path, 0, start, "close");
    return result;
}

SceSSize __wrap_sceIoRead(SceUID fd, void *buf, SceSize nbyte)
{
    char path[PathBytes];
    PathOf(fd, path);
    const uint64_t start = sceKernelGetProcessTimeWide();
    const SceSSize result = __real_sceIoRead(fd, buf, nbyte);

    if (path[0])
        Note(Read, path, result > 0 ? (uint64_t)result : 0, start, "read");

    return result;
}

int __wrap_sceIoPread(SceUID fd, void *data, SceSize size, SceOff offset)
{
    char path[PathBytes];
    PathOf(fd, path);
    const uint64_t start = sceKernelGetProcessTimeWide();
    const int result = __real_sceIoPread(fd, data, size, offset);

    if (path[0])
        Note(Read, path, result > 0 ? (uint64_t)result : 0, start, "pread");

    return result;
}

SceSSize __wrap_sceIoWrite(SceUID fd, const void *buf, SceSize nbyte)
{
    char path[PathBytes];
    PathOf(fd, path);
    const uint64_t start = sceKernelGetProcessTimeWide();
    const SceSSize result = __real_sceIoWrite(fd, buf, nbyte);

    if (path[0])
        Note(Write, path, result > 0 ? (uint64_t)result : 0, start, "write");

    return result;
}

int __wrap_sceIoPwrite(SceUID fd, const void *data, SceSize size, SceOff offset)
{
    char path[PathBytes];
    PathOf(fd, path);
    const uint64_t start = sceKernelGetProcessTimeWide();
    const int result = __real_sceIoPwrite(fd, data, size, offset);

    if (path[0])
        Note(Write, path, result > 0 ? (uint64_t)result : 0, start, "pwrite");

    return result;
}

int __wrap_sceIoRemove(const char *file)
{
    const uint64_t start = sceKernelGetProcessTimeWide();
    const int result = __real_sceIoRemove(file);

    if (__atomic_load_n(&on, __ATOMIC_RELAXED))
        Note(Meta, file, 0, start, "remove");

    return result;
}

int __wrap_sceIoRename(const char *oldname, const char *newname)
{
    const uint64_t start = sceKernelGetProcessTimeWide();
    const int result = __real_sceIoRename(oldname, newname);

    if (__atomic_load_n(&on, __ATOMIC_RELAXED))
        Note(Meta, newname, 0, start, "rename");

    return result;
}

int __wrap_sceIoGetstat(const char *file, SceIoStat *stat)
{
    const uint64_t start = sceKernelGetProcessTimeWide();
    const int result = __real_sceIoGetstat(file, stat);

    if (__atomic_load_n(&on, __ATOMIC_RELAXED))
        Note(Meta, file, 0, start, "stat");

    return result;
}

int __wrap_sceIoMkdir(const char *dir, SceMode mode)
{
    const uint64_t start = sceKernelGetProcessTimeWide();
    const int result = __real_sceIoMkdir(dir, mode);

    if (__atomic_load_n(&on, __ATOMIC_RELAXED))
        Note(Meta, dir, 0, start, "mkdir");

    return result;
}

/* Game thread, once a frame: whether a loading screen is up, and the report
 * once a second. Descriptors opened before the trace was switched on are not
 * counted. */
void mh3_io_trace_frame(int enabled, int loading_screen)
{
    static uint64_t reported;

    if (!enabled)
        return;

    __atomic_store_n(&on, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&loading, loading_screen, __ATOMIC_RELAXED);
    const uint64_t now = sceKernelGetProcessTimeWide();
    struct Totals t;
    unsigned count, dropped;
    static char lines[Slow][sizeof slow[0].line];
    Lock();
    count = slow_count;
    dropped = slow_dropped;
    memcpy(lines, slow, count * sizeof slow[0]);
    slow_count = slow_dropped = 0;
    const int due = now - reported >= 1000000;

    if (due) {
        t = totals;
        memset(&totals, 0, sizeof totals);
    }

    Unlock();

    for (unsigned i = 0; i < count; ++i)
        printf("MH3 io slow: %.3f s %s\n", now / 1e6, lines[i]);

    if (dropped)
        printf("MH3 io slow: %u more\n", dropped);

    if (!due)
        return;

    reported = now;

    if (!t.ops[Read] && !t.ops[Write] && !t.ops[Meta])
        return;

    char threads[160] = "";
    size_t used = 0;

    for (int i = 0; i < Threads && t.threads[i].id && used < sizeof threads; ++i) {
        SceKernelThreadInfo info;
        info.size = sizeof info;
        const char *name = sceKernelGetThreadInfo(t.threads[i].id, &info) >= 0 ? info.name : "?";
        used += (size_t)snprintf(threads + used, sizeof threads - used, " %s %u/%.1fms", name, (unsigned)t.threads[i].ops, t.threads[i].us / 1000.0);
    }

    printf("MH3 io: %.3f s outside loading screens: %s %u %.1f KiB %.1f ms, %s %u %.1f KiB %.1f ms, %s %u %.1f ms; longest %.1f ms (%s); threads%s\n", now / 1e6, kind_names[Read],
           (unsigned)t.ops[Read], t.bytes[Read] / 1024.0, t.us[Read] / 1000.0, kind_names[Write], (unsigned)t.ops[Write], t.bytes[Write] / 1024.0, t.us[Write] / 1000.0,
           kind_names[Meta], (unsigned)t.ops[Meta], t.us[Meta] / 1000.0, t.max_us / 1000.0, t.max_path, threads);
}
