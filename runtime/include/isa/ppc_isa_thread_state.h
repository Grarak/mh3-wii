#pragma once
#include <cstdint>

struct CpuContext;

// The current CPU context and diagnostic execution address of each host thread
// that runs guest code: the game thread, the guest worker and the draw thread
// (guest_worker.h). No thread_local: the worker and the draw thread run on
// host stacks whose ranges are recorded here, and a thread is one of them
// exactly when its stack pointer is inside its range. Values change
// independently and each scope restores its own. Each record has its own
// cache line: the dispatch writes its thread's execution address on every
// call, and the threads run on separate cores.
namespace PpcThread {
struct alignas(64) State {
    CpuContext* context = nullptr;
    uint32_t executionAddress = 0;
};

enum : unsigned { Game, Worker, Draw, Threads };
inline constinit State state[Threads]{};

// The worker's and the draw thread's host stacks, [low, high); empty until
// each starts.
inline constinit uintptr_t workerStackLow = 0, workerStackHigh = 0;
inline constinit uintptr_t drawStackLow = 0, drawStackHigh = 0;

inline unsigned CurrentThread() noexcept {
    const char probe = 0;
    const uintptr_t sp = reinterpret_cast<uintptr_t>(&probe);

    if (sp - workerStackLow < workerStackHigh - workerStackLow)
        return Worker;

    return sp - drawStackLow < drawStackHigh - drawStackLow ? Draw : Game;
}

inline State& Current() noexcept {
    return state[CurrentThread()];
}

// The record of the thread `cpu` is current on (the game thread's when it
// is current on none): compares, without finding the calling thread.
inline State& Of(const CpuContext* cpu) noexcept {
    if (cpu == state[Worker].context)
        return state[Worker];

    return state[cpu == state[Draw].context ? Draw : Game];
}

// Whether `cpu` is the current context of the calling thread. A context runs
// on one thread only, so being current on any thread answers it without
// finding the caller: on the game thread the first compare decides, as the
// single record did.
inline bool IsCurrent(const CpuContext* cpu) noexcept {
    return state[Game].context == cpu || state[Worker].context == cpu || state[Draw].context == cpu;
}
} // namespace PpcThread
