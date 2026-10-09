// MH3_CALL_TRACE=PATH,movie (-DMH3_ENABLE_FUNCTION_TRACE=ON, which compiles
// everything with -finstrument-functions-after-inlining): every host function
// call while a movie plays and for 5 s after its last frame. PATH gets each
// second's count of every callee and call site, with the threads that made
// the calls; PATH.seq.WINDOW gets the game thread's calls and returns in order
// (pairs of native words: callee or callee | 1 for a return, then call site;
// callee 0 marks a monotonic time in ns), from about 2M before the window's
// last movie frame to 14M after it: the skip and the player's cleanup.
// scripts/function_trace.py symbolizes and summarizes.
#include "guest_call_trace.h"

#include "dev_options.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <link.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <unistd.h>

#define NOINSTR __attribute__((no_instrument_function))

namespace {
constexpr size_t kSlots = size_t(1) << 20; // callee and call-site pairs
constexpr unsigned kThreads = 64;
constexpr size_t kRing = size_t(1) << 24; // game-thread calls and returns
constexpr uint64_t kBefore = uint64_t(1) << 21, kTail = 5'000'000'000ull;

struct Slot {
    std::atomic<uintptr_t> fn{0}, site{0};
    std::atomic<uint64_t> count{0}, threads{0};
};

struct Call {
    uintptr_t fn, site; // fn | 1: a return; fn 0: site is a steady-clock time in ns
};

std::atomic<bool> g_on{false};
std::atomic<bool> g_insert{false};
Slot* g_slots = nullptr;
uint64_t* g_reported = nullptr;
std::atomic<uintptr_t> g_threadIds[kThreads];
std::atomic<long> g_threadTids[kThreads];
std::atomic<pthread_t> g_game{0};
Call* g_ring = nullptr;
std::atomic<uint64_t> g_next{0}, g_lastFrameCall{0}, g_lastFrameNs{0};
std::FILE* g_counts = nullptr;
const char* g_path = nullptr;

NOINSTR uint64_t Now() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return uint64_t(t.tv_sec) * 1'000'000'000ull + uint64_t(t.tv_nsec);
}

NOINSTR unsigned ThreadIndex(pthread_t self) {
    const uintptr_t id = uintptr_t(self);

    for (unsigned i = unsigned((id >> 12) * 0x9E3779B97F4A7C15ull >> 58), n = 0; n < kThreads; ++n, i = (i + 1) % kThreads) {
        uintptr_t seen = g_threadIds[i].load(std::memory_order_relaxed);

        if (seen == id)
            return i;

        if (seen == 0 && g_threadIds[i].compare_exchange_strong(seen, id)) {
            g_threadTids[i].store(syscall(SYS_gettid), std::memory_order_relaxed);
            return i;
        }

        if (seen == id)
            return i;
    }

    return kThreads - 1;
}

NOINSTR void Count(uintptr_t fn, uintptr_t site, unsigned thread) {
    const uint64_t key = (uint64_t(fn) * 0x9E3779B97F4A7C15ull) ^ (uint64_t(site) * 0xC2B2AE3D27D4EB4Full);

    for (size_t i = size_t(key >> 44) & (kSlots - 1), n = 0; n < kSlots; ++n, i = (i + 1) & (kSlots - 1)) {
        Slot& slot = g_slots[i];
        uintptr_t seen = slot.fn.load(std::memory_order_acquire);

        if (seen == 0) {
            // Claim under a spin flag: site must be set before fn publishes it.
            while (g_insert.exchange(true, std::memory_order_acquire)) {
            }

            seen = slot.fn.load(std::memory_order_acquire);

            if (seen == 0) {
                slot.site.store(site, std::memory_order_relaxed);
                slot.fn.store(fn, std::memory_order_release);
                seen = fn;
            }

            g_insert.store(false, std::memory_order_release);
        }

        if (seen == fn && slot.site.load(std::memory_order_relaxed) == site) {
            slot.count.fetch_add(1, std::memory_order_relaxed);

            if (!(slot.threads.load(std::memory_order_relaxed) >> thread & 1))
                slot.threads.fetch_or(uint64_t(1) << thread, std::memory_order_relaxed);

            return;
        }
    }
}

NOINSTR void Record(uintptr_t fn, uintptr_t site) {
    const uint64_t n = g_next.load(std::memory_order_relaxed);
    const uint64_t frame = g_lastFrameCall.load(std::memory_order_relaxed);

    if (n - frame >= kRing - kBefore)
        return; // holds the calls around the last frame until another one comes
    if ((n & 4095) == 0) {
        g_ring[n % kRing] = {0, uintptr_t(Now())};
        g_ring[(n + 1) % kRing] = {fn, site};
        g_next.store(n + 2, std::memory_order_relaxed);
        return;
    }

    g_ring[n % kRing] = {fn, site};
    g_next.store(n + 1, std::memory_order_relaxed);
}

NOINSTR int WriteModule(dl_phdr_info* info, size_t, void*) {
    char self[4096] = "/proc/self/exe";
    const char* name = info->dlpi_name;

    if (!name || !*name) {
        const ssize_t length = readlink("/proc/self/exe", self, sizeof self - 1);

        if (length > 0)
            self[length] = 0;

        name = self;
    }

    std::fprintf(g_counts, "module %lx %s\n", (unsigned long)info->dlpi_addr, name);
    return 0;
}

NOINSTR void WriteThreads(std::FILE* file) {
    for (unsigned i = 0; i < kThreads; ++i) {
        const long tid = g_threadTids[i].load(std::memory_order_relaxed);

        if (!tid)
            continue;

        char path[64], name[64] = "?";
        std::snprintf(path, sizeof path, "/proc/self/task/%ld/comm", tid);

        if (std::FILE* comm = std::fopen(path, "r")) {
            if (std::fgets(name, sizeof name, comm))
                for (char* c = name; *c; ++c)
                    if (*c == '\n')
                        *c = 0;
            std::fclose(comm);
        }

        std::fprintf(file, "thread %u %ld %s%s\n", i, tid, name, g_threadIds[i].load() == uintptr_t(g_game.load()) ? " game" : "");
    }
}

NOINSTR void WriteSecond(unsigned window, unsigned second) {
    std::fprintf(g_counts, "second %u %u\n", window, second);

    for (size_t i = 0; i < kSlots; ++i) {
        const uintptr_t fn = g_slots[i].fn.load(std::memory_order_acquire);

        if (!fn)
            continue;

        const uint64_t count = g_slots[i].count.load(std::memory_order_relaxed);

        if (count == g_reported[i])
            continue;

        std::fprintf(g_counts, "%llu %lx %lx %llx\n", (unsigned long long)(count - g_reported[i]), (unsigned long)fn,
                     (unsigned long)g_slots[i].site.load(std::memory_order_relaxed),
                     (unsigned long long)g_slots[i].threads.exchange(0, std::memory_order_relaxed));
        g_reported[i] = count;
    }

    std::fflush(g_counts);
}

NOINSTR void WriteSequence(unsigned window) {
    const uint64_t end = g_next.load(std::memory_order_relaxed);
    const uint64_t frame = g_lastFrameCall.load(std::memory_order_relaxed);
    uint64_t begin = frame > kBefore ? frame - kBefore : 0;

    if (end - begin > kRing)
        begin = end - kRing;

    char path[4096];
    std::snprintf(path, sizeof path, "%s.seq.%u", g_path, window);
    std::FILE* file = std::fopen(path, "wb");

    if (!file)
        return;

    for (uint64_t n = begin; n < end; ++n)
        std::fwrite(&g_ring[n % kRing], sizeof(Call), 1, file);

    std::fclose(file);
    std::fprintf(g_counts, "sequence %u %s records %llu last_frame %llu\n", window, path, (unsigned long long)(end - begin),
                 (unsigned long long)(frame - begin));
}

NOINSTR void* Dump(void*) {
    unsigned window = 0, second = 0;

    for (;;) {
        timespec pause{1, 0};
        nanosleep(&pause, nullptr);

        if (!g_on.load(std::memory_order_relaxed))
            continue;

        WriteSecond(window, second++);

        if (Now() - g_lastFrameNs.load(std::memory_order_relaxed) < kTail)
            continue;

        g_on.store(false, std::memory_order_relaxed);
        pause = {0, 50'000'000};
        nanosleep(&pause, nullptr);
        WriteSecond(window, second);
        WriteThreads(g_counts);
        WriteSequence(window);
        std::fflush(g_counts);
        std::fprintf(stderr, "MH3 function trace: window %u written (%u s)\n", window, second);
        ++window;
        second = 0;
    }

    return nullptr;
}

NOINSTR bool Configure() {
    const DevOptions options{"MH3_CALL_TRACE", {"gate", "movie"}, DevOptions::Words::Allowed};

    if (!options.Has("movie") || options.List().empty())
        return false;

    static const std::string path = options.List().front();
    g_path = path.c_str();
    g_counts = std::fopen(g_path, "w");
    g_slots = new Slot[kSlots];
    g_reported = new uint64_t[kSlots]();
    g_ring = static_cast<Call*>(std::calloc(kRing, sizeof(Call)));

    if (!g_counts || !g_ring)
        return false;

    dl_iterate_phdr(WriteModule, nullptr);
    pthread_t dumper;
    return pthread_create(&dumper, nullptr, Dump, nullptr) == 0;
}
} // namespace

extern "C" NOINSTR void __cyg_profile_func_enter(void* fn, void* site) {
    if (!g_on.load(std::memory_order_relaxed))
        return;

    const pthread_t self = pthread_self();
    Count(uintptr_t(fn), uintptr_t(site), ThreadIndex(self));

    if (self == g_game.load(std::memory_order_relaxed))
        Record(uintptr_t(fn), uintptr_t(site));
}

extern "C" NOINSTR void __cyg_profile_func_exit(void* fn, void* site) {
    if (g_on.load(std::memory_order_relaxed) && pthread_self() == g_game.load(std::memory_order_relaxed))
        Record(uintptr_t(fn) | 1, uintptr_t(site));
}

NOINSTR void FunctionTraceMovieFrame() {
    static const bool configured = Configure();

    if (!configured)
        return;

    g_game.store(pthread_self(), std::memory_order_relaxed);
    g_lastFrameCall.store(g_next.load(std::memory_order_relaxed), std::memory_order_relaxed);
    g_lastFrameNs.store(Now(), std::memory_order_relaxed);
    g_on.store(true, std::memory_order_relaxed);
}
