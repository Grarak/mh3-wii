// A counter that one thread advances and one other thread waits on, for
// threads that hand each other work in bursts (the render thread's packets,
// the guest worker's jobs). Waiting spins for a millisecond, which catches the
// next of a burst at once, then sleeps on a condition variable until the
// counter moves. The Vita's pthread-embedded condition variable can lose a
// wakeup, so there the sleep also re-checks every millisecond. A wait that is
// known to be short spins only (SpinForChange). Spinning reads no clock: the
// millisecond is a spin count measured once, at the first wait.
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

#ifdef __vita__
#include <psp2/kernel/processmgr.h>
#endif

class SpinSignal {
  public:
    uint32_t Load() const noexcept { return value_.load(std::memory_order_acquire); }

    // Advances the counter and wakes the waiter if it sleeps.
    void Advance() {
        std::lock_guard guard(lock_);
        value_.fetch_add(1, std::memory_order_release);

        if (sleeping_)
            changed_.notify_one();
    }

    // Waits until the counter is no longer `seen`; returns its value.
    uint32_t AwaitChange(uint32_t seen) {
        for (uint32_t spins = SpinsPerMillisecond(); spins; --spins) {
            if (const uint32_t now = Load(); now != seen)
                return now;

            Pause();
        }

        std::unique_lock guard(lock_);
        sleeping_ = true;

        while (value_.load(std::memory_order_acquire) == seen) {
#ifdef __vita__
            changed_.wait_for(guard, std::chrono::milliseconds(1));
#else
            changed_.wait(guard);
#endif
        }

        sleeping_ = false;
        return value_.load(std::memory_order_acquire);
    }

    // Spins until the counter is no longer `seen`, without sleeping: for a
    // wait known to be short, the other thread already being on it.
    uint32_t SpinForChange(uint32_t seen) const noexcept {
        for (;;) {
            if (const uint32_t now = Load(); now != seen)
                return now;

            Pause();
        }
    }

  private:
    // How many spins of AwaitChange's loop take a millisecond, timed over
    // 64Ki spins of the same loop once per process.
    static uint32_t SpinsPerMillisecond() noexcept {
        static const uint32_t spins = [] {
            constexpr uint64_t Probe = 1u << 16;
            const std::atomic<uint32_t> probe{0};
            const uint64_t start = NowUs();

            for (uint64_t i = 0; i < Probe; ++i) {
                if (probe.load(std::memory_order_acquire) == UINT32_MAX)
                    break;

                Pause();
            }

            const uint64_t us = NowUs() - start;
            return uint32_t(us ? std::min<uint64_t>(Probe * 1000 / us, UINT32_MAX) : Probe * 1000);
        }();

        return spins;
    }

    static uint64_t NowUs() noexcept {
#ifdef __vita__
        return sceKernelGetProcessTimeWide();
#else
        return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
    }

    static void Pause() noexcept {
#if defined(__arm__) || defined(__aarch64__)
        __asm__ __volatile__("yield");
#elif defined(__x86_64__)
        __builtin_ia32_pause();
#endif
    }

    std::atomic<uint32_t> value_{0};
    std::mutex lock_;
    std::condition_variable changed_;
    bool sleeping_ = false;
};
