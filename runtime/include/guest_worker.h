// Host threads that run guest code beside the game thread: translated
// functions and native ports, each on its own CPU context and guest stack.
// The guest worker runs the deferred effect calculation (effect_step.cpp) and
// the split world pass (scene_world.cpp); on the Vita it runs on core 3,
// which the port otherwise leaves idle. The draw thread runs the frame's draw
// (docs/pipelined-draw.md).
//
// Work is handed over whole: a thread submits a job and later waits for it.
// While a job runs, the submitter must not touch the guest state the job
// uses; everything else (the recompiler's dispatch memos, the current-context
// records of ppc_isa_thread_state.h) is safe to share.
#pragma once

#include <atomic>
#include <cstdint>
#include <pthread.h>

#include "isa/ppc_isa_context.h"
#include "spin_signal.h"

class GuestThread {
  public:
    using Job = void (*)(CpuContext* cpu, void* data);

    // Development statistics: the job time and the submitter's waits.
    struct Statistics {
        uint64_t jobs = 0, jobNs = 0, waits = 0, waitNs = 0;
    };

    // role: its PpcThread record (Worker or Draw); core: the Vita core it
    // asks for (core 0 when that one is not available).
    GuestThread(const char* name, unsigned role, unsigned core) : name_(name), role_(role), core_(core) {}

    GuestThread(const GuestThread&) = delete;
    GuestThread& operator=(const GuestThread&) = delete;

    // Boot: the guest stack is [base, base + bytes).
    void ReserveGuestStack(uint32_t base, uint32_t bytes) {
        stackBase_ = base;
        stackBytes_ = bytes;
    }

    // Boot: its copy of the scratch the g3d matrix passes share (0x8066D300
    // on the game thread, g3d_calc_world.cpp), kWorkAreaBytes at base.
    void ReserveWorkArea(uint32_t base) { workArea_ = base; }

    uint32_t WorkArea() const { return workArea_; }

    // Whether jobs can run (a guest stack and a work area were reserved).
    bool Available() const { return stackBase_ != 0 && workArea_ != 0; }

    // Hands `job` over, starting the thread on first use, with a copy of
    // `seed`'s context (its special registers, small-data bases and FPSCR) on
    // the thread's own guest stack. One job at a time: the previous one must
    // have been waited for.
    void Submit(const CpuContext& seed, Job job, void* data);

    // Waits (spinning) until the submitted job has finished; returns at once
    // when none is pending.
    void Wait();

    // True from Submit until the job finishes.
    bool Pending() const;

    Statistics TakeStatistics();

  private:
    static void* Run(void* self);
    void Loop();
    void Start();

    const char* name_;
    unsigned role_, core_;
    uint32_t stackBase_ = 0, stackBytes_ = 0, workArea_ = 0;
    bool started_ = false;
    pthread_t thread_{};

    // Written by the submitter before it publishes a submission, read by the
    // thread after it sees one.
    CpuContext seed_{};
    Job job_ = nullptr;
    void* data_ = nullptr;

    // Jobs handed over (the thread waits on it) and jobs done (the submitter
    // waits on it).
    SpinSignal submitted_, finished_;

    // The thread's host stack, which the submitter publishes (PpcThread).
    std::atomic<uintptr_t> hostLow_{0}, hostHigh_{0};
    std::atomic<bool> hostKnown_{false};
    std::atomic<uint64_t> jobCount_{0}, jobTime_{0};
    uint64_t waitCount_ = 0, waitTime_ = 0;
};

namespace GuestWorker {
using Job = GuestThread::Job;
using Statistics = GuestThread::Statistics;

// Guest stack bytes, reserved at the top of the MEM1 arena at boot (main.cpp).
inline constexpr uint32_t kGuestStackBytes = 32 * 1024;

// The scratch copy's size, reserved at the top of the MEM2 arena at boot.
inline constexpr uint32_t kWorkAreaBytes = 0x18000;

// The guest worker's thread.
GuestThread& Thread();

inline void ReserveGuestStack(uint32_t base) {
    Thread().ReserveGuestStack(base, kGuestStackBytes);
}

inline void ReserveWorkArea(uint32_t base) {
    Thread().ReserveWorkArea(base);
}

inline uint32_t WorkArea() {
    return Thread().WorkArea();
}

inline bool Available() {
    return Thread().Available();
}

inline void Submit(const CpuContext& game, Job job, void* data) {
    Thread().Submit(game, job, data);
}

inline void Wait() {
    Thread().Wait();
}

inline bool Pending() {
    return Thread().Pending();
}

inline Statistics TakeStatistics() {
    return Thread().TakeStatistics();
}

#ifdef MH3_DEVELOPMENT
// Development: called by paths that use host state without a lock (resource
// provenance, the locked cache, the effect call list); the first use of each
// on the worker or the draw thread is reported (MH3 guest worker: unsafe ...).
void NoteShared(const char* what);
#endif
} // namespace GuestWorker

namespace DrawThread {
// Guest stack bytes for the draw, reserved below the worker's at boot.
inline constexpr uint32_t kGuestStackBytes = 64 * 1024;

// The draw thread.
GuestThread& Thread();
} // namespace DrawThread
