// A counter that one thread advances and one other thread waits on, for
// threads that hand each other work (the render thread's packets, the guest
// worker's jobs, the game thread limiter). Waiting can spin first, which
// catches the next of a burst at once, then the waiter parks until the
// counter moves: on the Vita on a kernel semaphore, which counts its signals
// and so cannot lose a wakeup (the pthread condition variable there can),
// elsewhere on the counter's futex, or for a wait with a time limit
// (AwaitChangeFor) on a condition variable. A wait that is known to be short
// spins only (SpinForChange). Spinning reads no clock: a millisecond is a
// spin count measured once, at the first wait.
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>

#ifdef __vita__
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#else
#include <condition_variable>
#include <mutex>
#endif

class SpinSignal {
  public:
    uint32_t Load() const noexcept { return value_.load(std::memory_order_acquire); }

    // Advances the counter and wakes the waiter if it is parked.
    void Advance() noexcept {
        value_.fetch_add(1, std::memory_order_seq_cst);
#ifdef __vita__
        if (sleeping_.exchange(false, std::memory_order_seq_cst))
            sceKernelSignalSema(sema_, 1);
#else
        value_.notify_one();

        // A wait with a time limit parks on the condition variable.
        if (sleeping_.load(std::memory_order_seq_cst) && sleeping_.exchange(false, std::memory_order_seq_cst)) {
            std::lock_guard lock(mutex_);
            wake_.notify_one();
        }
#endif
    }

    // Waits until the counter is no longer `seen`, spinning for about spinUs
    // microseconds before parking; returns its value. One waiter at a time.
    uint32_t AwaitChange(uint32_t seen, uint32_t spinUs = 1000) {
        if (const uint32_t now = Spin(seen, spinUs); now != seen)
            return now;
#ifdef __vita__
        return Park(seen, UINT64_MAX);
#else
        for (uint32_t now = Load();; now = Load()) {
            if (now != seen)
                return now;

            value_.wait(seen, std::memory_order_acquire);
        }
#endif
    }

    // AwaitChange that gives up after about timeoutUs microseconds: returns
    // the counter, which is still `seen` when it timed out.
    uint32_t AwaitChangeFor(uint32_t seen, uint32_t spinUs, uint32_t timeoutUs) {
        if (const uint32_t now = Spin(seen, spinUs); now != seen)
            return now;
#ifdef __vita__
        return Park(seen, NowUs() + timeoutUs);
#else
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(timeoutUs);
        std::unique_lock lock(mutex_);

        for (;;) {
            // Announce, then re-check, as on the Vita. The notifier takes the
            // lock, which this thread holds until the wait releases it, so
            // its notification cannot come between the check and the wait.
            sleeping_.store(true, std::memory_order_seq_cst);

            if (const uint32_t now = value_.load(std::memory_order_seq_cst); now != seen) {
                sleeping_.store(false, std::memory_order_relaxed);
                return now;
            }

            if (wake_.wait_until(lock, deadline) == std::cv_status::timeout) {
                sleeping_.store(false, std::memory_order_relaxed);
                return Load();
            }
        }
#endif
    }

    // Spins until the counter is no longer `seen`, without parking: for a wait
    // known to be short, the other thread already being on it.
    uint32_t SpinForChange(uint32_t seen) const noexcept {
        for (;;) {
            if (const uint32_t now = Load(); now != seen)
                return now;

            Pause();
        }
    }

  private:
    // Spins for about spinUs microseconds while the counter is `seen`;
    // returns its value.
    uint32_t Spin(uint32_t seen, uint32_t spinUs) const noexcept {
        for (uint64_t spins = uint64_t(SpinsPerMillisecond()) * spinUs / 1000; spins; --spins) {
            if (const uint32_t now = Load(); now != seen)
                return now;

            Pause();
        }

        return seen;
    }
#ifdef __vita__
    // Parks on the semaphore until the counter is no longer `seen` or the
    // process time deadlineUs passed (UINT64_MAX: never); returns the counter.
    uint32_t Park(uint32_t seen, uint64_t deadlineUs) {
        // Created by the waiter before it first announces a sleep, which
        // orders the id before any notifier reads it.
        if (sema_ < 0)
            sema_ = sceKernelCreateSema("SpinSignal", 0, 0, 1 << 20, nullptr);

        for (;;) {
            // Announce, then re-check: a notifier either sees the announcement
            // and signals, or advanced before the check. A signal that arrives
            // after the check saw the change stays counted and wakes the next
            // park at once, which re-checks and parks again.
            sleeping_.store(true, std::memory_order_seq_cst);

            if (const uint32_t now = value_.load(std::memory_order_seq_cst); now != seen) {
                sleeping_.store(false, std::memory_order_relaxed);
                return now;
            }

            // Bounded, as every wait on the Vita: the loop re-checks.
            SceUInt timeout = 100000;

            if (deadlineUs != UINT64_MAX) {
                const uint64_t now = NowUs();

                if (now >= deadlineUs) {
                    sleeping_.store(false, std::memory_order_relaxed);
                    return seen;
                }

                timeout = SceUInt(std::min<uint64_t>(deadlineUs - now, timeout));
            }

            sceKernelWaitSema(sema_, 1, &timeout);
        }
    }
#endif
    // How many spins of AwaitChange's loop take a millisecond, timed over
    // 64Ki spins of the same loop once per process. The probe is shared state,
    // like the counter, so the compiler keeps its load in the loop.
    static uint32_t SpinsPerMillisecond() noexcept {
        static const uint32_t spins = [] {
            constexpr uint64_t Probe = 1u << 16;
            const uint64_t start = NowUs();

            for (uint64_t i = 0; i < Probe; ++i) {
                if (calibration_.load(std::memory_order_acquire) == UINT32_MAX)
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

    static inline std::atomic<uint32_t> calibration_{0};
    std::atomic<uint32_t> value_{0};
    std::atomic<bool> sleeping_{false};
#ifdef __vita__
    SceUID sema_ = -1;
#else
    std::mutex mutex_;
    std::condition_variable wake_;
#endif
};
