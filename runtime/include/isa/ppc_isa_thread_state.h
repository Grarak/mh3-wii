#pragma once
#include <cstdint>

struct CpuContext;

// The current CPU context and diagnostic execution address of each host thread
// that runs guest code: the game thread and the guest worker (guest_worker.h).
// No thread_local: the worker runs on a host stack whose range is recorded
// here, and a thread is the worker exactly when its stack pointer is inside it.
// Values change independently and each scope restores its own. Each record
// has its own cache line: the dispatch writes its thread's execution address
// on every call, and the two threads run on two cores.
namespace PpcThread {
struct alignas(64) State {
    CpuContext* context = nullptr;
    uint32_t executionAddress = 0;
};

enum : unsigned { Game, Worker, Threads };
inline constinit State state[Threads]{};

// The worker's host stack, [low, high); empty until the worker starts.
inline constinit uintptr_t workerStackLow = 0, workerStackHigh = 0;

inline unsigned CurrentThread() noexcept {
    const char probe = 0;
    const uintptr_t sp = reinterpret_cast<uintptr_t>(&probe);
    return sp - workerStackLow < workerStackHigh - workerStackLow ? Worker : Game;
}

inline State& Current() noexcept {
    return state[CurrentThread()];
}

// The record of the thread `cpu` is current on (the game thread's when it
// is current on neither): one compare, without finding the calling thread.
inline State& Of(const CpuContext* cpu) noexcept {
    return state[cpu == state[Worker].context ? Worker : Game];
}

// Whether `cpu` is the current context of the calling thread. A context runs
// on one thread only, so being current on either thread answers it without
// finding the caller: on the game thread the first compare decides, as the
// single record did.
inline bool IsCurrent(const CpuContext* cpu) noexcept {
    return state[Game].context == cpu || state[Worker].context == cpu;
}
} // namespace PpcThread
