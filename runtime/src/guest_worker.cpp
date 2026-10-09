// The guest worker (guest_worker.h): one host thread with its own CPU context
// and guest stack, running jobs the game thread hands it.
#include "guest_worker.h"

#include "isa/ppc_isa_context.h"
#include "memory.h"
#include "spin_signal.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <pthread.h>
#include <stdexcept>
#include <thread>
#ifdef MH3_DEVELOPMENT
#include <mutex>
#include <vector>
#endif

#ifdef __vita__
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#endif

namespace GuestWorker {
namespace {
uint32_t stackBase = 0, workArea = 0;
bool started = false;
pthread_t thread;

// Written by the game thread before it publishes a submission, read by the
// worker after it sees one.
CpuContext seed;
Job job = nullptr;
void* jobData = nullptr;

// Jobs handed over (the worker waits on it) and jobs done (the game thread
// waits on it).
SpinSignal submitted, finished;

// The worker's host stack, which the game thread publishes (PpcThread).
std::atomic<uintptr_t> hostLow{0}, hostHigh{0};
std::atomic<bool> hostKnown{false};
#ifdef MH3_DEVELOPMENT
std::atomic<uint64_t> jobCount{0}, jobTime{0};
uint64_t waitCount = 0, waitTime = 0;
#endif

uint64_t NowNs() {
#ifdef __vita__
    return sceKernelGetProcessTimeWide() * 1000;
#else
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
}

[[noreturn]] void Fatal(const char* reason) {
    std::fprintf(stderr, "MH3 guest worker: %s\n", reason);
    std::abort();
}

void* Run(void*) {
#ifdef __vita__
    // Core 3 (the devkit's cap unlocker opens it); core 0 otherwise.
    if (sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), SCE_KERNEL_CPU_MASK_USER_0 << 3) < 0) {
        std::printf("MH3 guest worker: core 3 is not available, running on core 0\n");
        sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), SCE_KERNEL_CPU_MASK_USER_0);
    }

    SceKernelThreadInfo info{};
    info.size = sizeof(info);

    if (sceKernelGetThreadInfo(sceKernelGetThreadId(), &info) < 0)
        Fatal("no thread information");

    hostLow.store(uintptr_t(info.stack), std::memory_order_relaxed);
    hostHigh.store(uintptr_t(info.stack) + info.stackSize, std::memory_order_relaxed);
#else
    pthread_attr_t attributes;
    void* address = nullptr;
    size_t size = 0;

    if (pthread_getattr_np(pthread_self(), &attributes) != 0 || pthread_attr_getstack(&attributes, &address, &size) != 0)
        Fatal("no stack range");

    pthread_attr_destroy(&attributes);
    hostLow.store(uintptr_t(address), std::memory_order_relaxed);
    hostHigh.store(uintptr_t(address) + size, std::memory_order_relaxed);
#endif
    hostKnown.store(true, std::memory_order_release);

    // Created once, the worker stays for the whole process, waiting on
    // `submitted` between jobs. Its context stays current from the first job
    // on: leaving the scope would restore the FP control register the thread
    // started with, and with it the NI mirror the game thread shares.
    uint32_t seen = submitted.AwaitChange(0, 0);
    CpuContext cpu = seed;
    CpuContextScope scope(&cpu);

    for (;;) {
#ifdef MH3_DEVELOPMENT
        const uint64_t start = NowNs();
#endif
        // Fresh registers for every job, on the worker's own guest stack with
        // an empty back chain.
        cpu = seed;
        cpu.gpr[1] = (stackBase + kGuestStackBytes - 16) & ~0xFu;
        Memory::Write32(cpu.gpr[1], 0);
        job(&cpu, jobData);
#ifdef MH3_DEVELOPMENT
        jobTime.fetch_add(NowNs() - start, std::memory_order_relaxed);
        jobCount.fetch_add(1, std::memory_order_relaxed);
#endif
        // Jobs come once a frame: park at once (no spin) until the next.
        finished.Advance();
        seen = submitted.AwaitChange(seen, 0);
    }
}

void Start() {
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, 1u << 20);
    const int failed = pthread_create(&thread, &attributes, Run, nullptr);
    pthread_attr_destroy(&attributes);

    if (failed)
        throw std::runtime_error("Cannot start the guest worker");

    while (!hostKnown.load(std::memory_order_acquire))
        std::this_thread::yield();

    // Published by the game thread while it runs no guest code, so its own
    // context lookups never see half of the range. The worker reads them only
    // after a submission, which orders them.
    PpcThread::workerStackLow = hostLow.load(std::memory_order_relaxed);
    PpcThread::workerStackHigh = hostHigh.load(std::memory_order_relaxed);
    started = true;
}
} // namespace

void ReserveGuestStack(uint32_t base) {
    stackBase = base;
}

void ReserveWorkArea(uint32_t base) {
    workArea = base;
}

uint32_t WorkArea() {
    return workArea;
}

bool Available() {
    return stackBase != 0 && workArea != 0;
}

void Submit(const CpuContext& game, Job work, void* data) {
    if (!stackBase)
        throw std::runtime_error("Guest worker: no guest stack");

    if (Pending())
        throw std::runtime_error("Guest worker: a job is still pending");

    if (!started)
        Start();

    seed = game;
    job = work;
    jobData = data;
    submitted.Advance();
}

void Wait() {
    const uint32_t target = submitted.Load();

    if (finished.Load() == target)
        return;
#ifdef MH3_DEVELOPMENT
    const uint64_t start = NowNs();
#endif
    // A job is a part of the frame the game thread would otherwise run
    // itself: spin, which also skips a condition variable's wake latency.
    for (uint32_t done = finished.Load(); done != target;)
        done = finished.SpinForChange(done);
#ifdef MH3_DEVELOPMENT
    waitTime += NowNs() - start;
    ++waitCount;
#endif
}

bool Pending() {
    return finished.Load() != submitted.Load();
}

#ifdef MH3_DEVELOPMENT
void NoteShared(const char* what) {
    if (PpcThread::CurrentThread() != PpcThread::Worker)
        return;

    static std::mutex lock;
    static std::vector<const char*> reported;
    std::lock_guard guard(lock);

    if (std::find(reported.begin(), reported.end(), what) != reported.end())
        return;

    reported.push_back(what);
    std::printf("MH3 guest worker: unsafe %s on the worker\n", what);
}
#endif

Statistics TakeStatistics() {
    Statistics out;
#ifdef MH3_DEVELOPMENT
    out.jobs = jobCount.exchange(0, std::memory_order_relaxed);
    out.jobNs = jobTime.exchange(0, std::memory_order_relaxed);
    out.waits = waitCount;
    out.waitNs = waitTime;
    waitCount = waitTime = 0;
#endif
    return out;
}
} // namespace GuestWorker
