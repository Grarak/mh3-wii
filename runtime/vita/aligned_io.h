#pragma once

// Sony's file access notes (Kernel Overview, "Notes on the Implementation of
// File Access Processing"): sceIoPread wants 64-byte aligned buffers ("other
// alignment results in a dramatic decline in performance") and offsets that
// are multiples of 512; above 512 bytes, larger requests are faster. A read
// that breaks either rule goes through an aligned bounce buffer instead, in
// requests of up to 512 KiB covering whole 512-byte blocks.
#include <psp2/io/fcntl.h>

#include <malloc.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>

namespace VitaIo {
constexpr size_t BufferAlign = 64, OffsetAlign = 512, BounceBytes = 512u << 10;

// Reads count bytes at `at` (fewer at the end of the file); the byte count or
// a negative SCE error.
inline int64_t Pread(SceUID fd, uint8_t* out, size_t count, uint64_t at) {
    const bool aligned = reinterpret_cast<uintptr_t>(out) % BufferAlign == 0 && at % OffsetAlign == 0 && count % OffsetAlign == 0;
    size_t done = 0;

    if (aligned) {
        while (done < count) {
            const int n = sceIoPread(fd, out + done, count - done, SceOff(at + done));

            if (n < 0)
                return n;

            if (n == 0)
                break;

            done += size_t(n);
        }

        return int64_t(done);
    }

    // One per reading thread (the loader, the stream reader, the asset cache).
    thread_local uint8_t* bounce = static_cast<uint8_t*>(memalign(BufferAlign, BounceBytes));

    if (!bounce)
        return -1;

    while (done < count) {
        const uint64_t want = at + done, start = want & ~uint64_t(OffsetAlign - 1);
        const size_t skip = size_t(want - start);
        const size_t span = std::min<size_t>(BounceBytes, (skip + (count - done) + OffsetAlign - 1) & ~(OffsetAlign - 1));
        const int n = sceIoPread(fd, bounce, span, SceOff(start));

        if (n < 0)
            return n;

        if (size_t(n) <= skip)
            break;

        const size_t copy = std::min(size_t(n) - skip, count - done);
        std::memcpy(out + done, bounce + skip, copy);
        done += copy;

        if (size_t(n) < span)
            break;
    }

    return int64_t(done);
}

// A suspend (the PS button, or the power button's standby) invalidates the
// descriptors of files open under ux0:data: after the resume every operation
// on them but sceIoClose fails with ENODEV (0x80010013, Sony's Development
// Process Overview, "Invalidation of File Descriptors"). A reader that keeps
// a file open for the session reads through this: a failed read opens `path`
// again in place of `fd` (once per stale descriptor, under `lock`, for the
// threads that share it) and is retried once.
inline int64_t PreadReopening(std::atomic<int>& fd, std::mutex& lock, const std::string& path, uint8_t* out, size_t count, uint64_t at) {
    const int stale = fd.load(std::memory_order_acquire);
    const int64_t n = Pread(stale, out, count, at);

    if (n >= 0)
        return n;

    {
        std::lock_guard guard(lock);

        // Another reader may have opened it again already.
        if (fd.load(std::memory_order_relaxed) == stale) {
            const int file = sceIoOpen(path.c_str(), SCE_O_RDONLY, 0);

            if (file < 0)
                return n;

            sceIoClose(stale);
            fd.store(file, std::memory_order_release);
        }
    }

    return Pread(fd.load(std::memory_order_acquire), out, count, at);
}
} // namespace VitaIo
