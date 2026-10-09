// Guest threads (guest_worker.h): host threads with their own CPU context and
// guest stack, running jobs another thread hands them.
#include "guest_worker.h"

#include "fatal.h"
#include "memory.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
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

namespace {

uint64_t NowNs() {
#ifdef __vita__
    return sceKernelGetProcessTimeWide() * 1000;
#else
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
}

[[noreturn]] void Fatal(const char* name, const char* reason) {
    std::fprintf(stderr, "MH3 %s: %s\n", name, reason);
    std::abort();
}

} // namespace

void* GuestThread::Run(void* self) {
    static_cast<GuestThread*>(self)->Loop();
    return nullptr;
}

void GuestThread::Loop() {
#ifdef __vita__
    if (sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), SCE_KERNEL_CPU_MASK_USER_0 << core_) < 0) {
        std::printf("MH3 %s: core %u is not available, running on core 0\n", name_, core_);
        sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), SCE_KERNEL_CPU_MASK_USER_0);
    }

    SceKernelThreadInfo info{};
    info.size = sizeof(info);

    if (sceKernelGetThreadInfo(sceKernelGetThreadId(), &info) < 0)
        Fatal(name_, "no thread information");

    hostLow_.store(uintptr_t(info.stack), std::memory_order_relaxed);
    hostHigh_.store(uintptr_t(info.stack) + info.stackSize, std::memory_order_relaxed);
#else
    pthread_attr_t attributes;
    void* address = nullptr;
    size_t size = 0;

    if (pthread_getattr_np(pthread_self(), &attributes) != 0 || pthread_attr_getstack(&attributes, &address, &size) != 0)
        Fatal(name_, "no stack range");

    pthread_attr_destroy(&attributes);
    hostLow_.store(uintptr_t(address), std::memory_order_relaxed);
    hostHigh_.store(uintptr_t(address) + size, std::memory_order_relaxed);
#endif
    hostKnown_.store(true, std::memory_order_release);

    // Created once, the thread stays for the whole process, waiting on
    // `submitted_` between jobs. Its context stays current from the first job
    // on: leaving the scope would restore the FP control register the thread
    // started with, and with it the NI mirror the game thread shares.
    uint32_t seen = submitted_.AwaitChange(0, 0);
    CpuContext cpu = seed_;
    CpuContextScope scope(&cpu);

    for (;;) {
        const uint64_t start = NowNs();

        // Fresh registers for every job, on the thread's own guest stack with
        // an empty back chain.
        cpu = seed_;
        cpu.gpr[1] = (stackBase_ + stackBytes_ - 16) & ~0xFu;
        Memory::Write32(cpu.gpr[1], 0);
        job_(&cpu, data_);
        jobTime_.fetch_add(NowNs() - start, std::memory_order_relaxed);
        jobCount_.fetch_add(1, std::memory_order_relaxed);

        // Jobs come once a frame: park at once until the next, unless the
        // thread spins through its idle time (SetIdleSpin).
        finished_.Advance();
        seen = submitted_.AwaitChange(seen, idleSpinUs_.load(std::memory_order_relaxed));
    }
}

void GuestThread::Start() {
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, 1u << 20);
    const int failed = pthread_create(&thread_, &attributes, Run, this);
    pthread_attr_destroy(&attributes);

    if (failed)
        MH3_THROW(std::runtime_error(std::string("Cannot start the ") + name_));

    while (!hostKnown_.load(std::memory_order_acquire))
        std::this_thread::yield();

    // Published by the submitter while it runs no guest code, so its own
    // context lookups never see half of the range. The thread reads them only
    // after a submission, which orders them.
    const uintptr_t low = hostLow_.load(std::memory_order_relaxed), high = hostHigh_.load(std::memory_order_relaxed);

    if (role_ == PpcThread::Worker) {
        PpcThread::workerStackLow = low;
        PpcThread::workerStackHigh = high;
    } else {
        PpcThread::drawStackLow = low;
        PpcThread::drawStackHigh = high;
    }

    started_ = true;
}

void GuestThread::Submit(const CpuContext& seed, Job job, void* data) {
    if (!stackBase_)
        MH3_THROW(std::runtime_error(std::string(name_) + ": no guest stack"));

    if (Pending())
        MH3_THROW(std::runtime_error(std::string(name_) + ": a job is still pending"));

    if (!started_)
        Start();

    seed_ = seed;
    job_ = job;
    data_ = data;
    lastSubmitWoke_ = submitted_.Advance();
}

// Whether the counter of finished jobs is still short of `target` (both count
// up and wrap).
static bool Behind(uint32_t done, uint32_t target) {
    return int32_t(done - target) < 0;
}

void GuestThread::Wait() {
    const uint32_t target = submitted_.Load();

    if (finished_.Load() == target)
        return;

    const uint64_t start = NowNs();

    // A job is a part of the frame the submitter would otherwise run itself:
    // spin, which also skips a condition variable's wake latency.
    for (uint32_t done = finished_.Load(); Behind(done, target);)
        done = finished_.SpinForChange(done);

    waitTime_ += NowNs() - start;
    ++waitCount_;
}

// Another thread than the submitter waits here (the draw thread for the
// effect job): between its reads of the two counters the submitter may hand
// the thread a further job, and the thread finish both, so `finished_` can
// pass the target. Waiting for it to equal the target then spun for good,
// the draw thread and with it the game thread (seen under host load).
void GuestThread::Await() const {
    const uint32_t target = submitted_.Load();

    for (uint32_t done = finished_.Load(); Behind(done, target);)
        done = finished_.SpinForChange(done);
}

bool GuestThread::Pending() const {
    return finished_.Load() != submitted_.Load();
}

GuestThread::Statistics GuestThread::TakeStatistics() {
    Statistics out;
    out.jobs = jobCount_.exchange(0, std::memory_order_relaxed);
    out.jobNs = jobTime_.exchange(0, std::memory_order_relaxed);
    out.waits = waitCount_;
    out.waitNs = waitTime_;
    waitCount_ = waitTime_ = 0;
    return out;
}

namespace GuestWorker {
GuestThread& Thread() {
    // Core 3 (the devkit's cap unlocker opens it); core 0 otherwise.
    static GuestThread worker("guest worker", PpcThread::Worker, 3);
    return worker;
}

#ifdef MH3_DEVELOPMENT
void NoteShared(const char* what) {
    const unsigned thread = PpcThread::CurrentThread();

    if (thread == PpcThread::Game)
        return;

    static std::mutex lock;
    static std::vector<std::pair<const char*, unsigned>> reported;
    std::lock_guard guard(lock);

    if (std::find(reported.begin(), reported.end(), std::pair{what, thread}) != reported.end())
        return;

    reported.emplace_back(what, thread);
    std::printf("MH3 guest worker: unsafe %s on the %s\n", what, thread == PpcThread::Worker ? "worker" : "draw thread");
}
#endif
} // namespace GuestWorker

namespace DrawThread {
GuestThread& Thread() {
    static GuestThread draw("draw thread", PpcThread::Draw, 2);
    return draw;
}
} // namespace DrawThread
