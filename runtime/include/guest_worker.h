// A second host thread that runs guest code beside the game thread: translated
// functions and native ports, on its own CPU context and guest stack. On the
// Vita it runs on core 3, which the port otherwise leaves idle.
//
// Work is handed over whole: the game thread submits a job and later waits for
// it. While a job runs, the game thread must not touch the guest state the job
// uses; everything else (the recompiler's dispatch memos, the current-context
// records of ppc_isa_thread_state.h) is safe to share.
#pragma once

#include <cstdint>

struct CpuContext;

namespace GuestWorker {
using Job = void (*)(CpuContext* cpu, void* data);

// Guest stack bytes, reserved at the top of the MEM1 arena at boot (main.cpp).
inline constexpr uint32_t kGuestStackBytes = 32 * 1024;

// Boot: the guest stack is [base, base + kGuestStackBytes).
void ReserveGuestStack(uint32_t base);

// Whether jobs can run on the worker (a guest stack was reserved and the
// thread started or can start).
bool Available();

// Hands `job` to the worker, starting it on first use with a copy of `game`'s
// context (its special registers, small-data bases and FPSCR) on the worker's
// own guest stack. One job at a time: the previous one must have been waited for.
void Submit(const CpuContext& game, Job job, void* data);

// Waits until the submitted job has finished; returns at once when none is pending.
void Wait();

// True from Submit until Wait returns.
bool Pending();

// Development statistics: the worker's job time and the game thread's waits.
struct Statistics {
    uint64_t jobs = 0, jobNs = 0, waits = 0, waitNs = 0;
};

Statistics TakeStatistics();
} // namespace GuestWorker
