#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <poll.h>
#include <sys/socket.h>
#endif

namespace NetworkPollContract {

constexpr size_t kMaxDescriptors = 24;

#ifdef _WIN32
using NativeSocket = SOCKET;
using NativePollFd = WSAPOLLFD;
constexpr NativeSocket kInvalidSocket = INVALID_SOCKET;
#else
using NativeSocket = int;
using NativePollFd = pollfd;
constexpr NativeSocket kInvalidSocket = -1;
#endif

struct CopiedDescriptor {
    uint32_t wiiFd = 0;
    NativeSocket nativeFd = kInvalidSocket;
    uint64_t socketGeneration = 0;
    short events = 0;
    short revents = 0;
};

// A zero-timeout SO_POLL is a pure readiness probe. Running it directly on
// the emulation thread is safe because ProbeNow always passes timeout zero to
// the host API; putting the guest IOS caller to sleep until the next scheduler
// pump only adds a needless context switch to every GameSpy update tick.
inline bool RequiresSchedulerWait(int64_t timeoutMilliseconds) {
    return timeoutMilliseconds != 0;
}

inline short WiiEventsToNative(uint32_t events) {
    int native = 0;
    if (events & 0x0001u) native |= POLLRDNORM;
    if (events & 0x0002u) native |= POLLRDBAND;
    if (events & 0x0004u) native |= POLLPRI;
    if (events & 0x0008u) native |= POLLWRNORM;
    if (events & 0x0010u) native |= POLLWRBAND;

    // ERR/HUP/NVAL are return-only. Winsock's WSAPoll also rejects the
    // priority and write-band inputs which Dolphin masks on Windows.
    native &= ~(POLLERR | POLLHUP | POLLNVAL);
#ifdef _WIN32
    native &= ~(POLLPRI | POLLWRBAND);
#endif
    return static_cast<short>(native);
}

inline uint32_t NativeEventsToWii(short events) {
    uint32_t wii = 0;
    if (events & POLLRDNORM) wii |= 0x0001u;
    if (events & POLLRDBAND) wii |= 0x0002u;
    if (events & POLLPRI) wii |= 0x0004u;
    if (events & POLLWRNORM) wii |= 0x0008u;
    if (events & POLLWRBAND) wii |= 0x0010u;
    if (events & POLLERR) wii |= 0x0020u;
    if (events & POLLHUP) wii |= 0x0040u;
    if (events & POLLNVAL) wii |= 0x0080u;
    return wii;
}

class Timeout {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    static Timeout FromMilliseconds(int64_t milliseconds, TimePoint now = Clock::now()) {
        Timeout timeout;
        if (milliseconds < 0) {
            timeout.m_infinite = true;
            timeout.m_deadline = TimePoint::max();
            return timeout;
        }

        using Milliseconds = std::chrono::milliseconds;
        const int64_t maximum = std::chrono::duration_cast<Milliseconds>(TimePoint::max() - now).count();
        timeout.m_deadline = milliseconds >= maximum
            ? TimePoint::max()
            : now + Milliseconds(milliseconds);
        return timeout;
    }

    bool IsExpired(TimePoint now = Clock::now()) const {
        return !m_infinite && now >= m_deadline;
    }

    bool ShouldRemainPending(int nativeResult, TimePoint now = Clock::now()) const {
        return nativeResult == 0 && !IsExpired(now);
    }

    bool IsInfinite() const { return m_infinite; }
    TimePoint Deadline() const { return m_deadline; }

private:
    bool m_infinite = false;
    TimePoint m_deadline{};
};

// Probes only descriptors whose copied socket identity is still live. A dead identity
// (SOClose/SOCleanup/slot reuse) reports POLLNVAL and counts toward readiness like IOS/Dolphin;
// skipping it silently would return 0 forever and strand an infinite-timeout SO_POLL parked
// during socket teardown mid-WFC-connect.
template <typename IsStillValid>
int ProbeNow(std::vector<CopiedDescriptor>& descriptors, IsStillValid&& isStillValid) {
    if (descriptors.size() > kMaxDescriptors) {
        return -1;
    }

    std::array<NativePollFd, kMaxDescriptors> active{};
    std::array<size_t, kMaxDescriptors> originalIndices{};
    size_t activeCount = 0;
    int invalidCount = 0;
    for (size_t i = 0; i < descriptors.size(); ++i) {
        CopiedDescriptor& descriptor = descriptors[i];
        descriptor.revents = 0;
        if (!isStillValid(descriptor)) {
            descriptor.revents = POLLNVAL;
            ++invalidCount;
            continue;
        }
        active[activeCount].fd = descriptor.nativeFd;
        active[activeCount].events = descriptor.events;
        active[activeCount].revents = 0;
        originalIndices[activeCount] = i;
        ++activeCount;
    }

    if (activeCount == 0) {
        return invalidCount;
    }

#ifdef _WIN32
    const int result = WSAPoll(active.data(), static_cast<ULONG>(activeCount), 0);
#else
    const int result = poll(active.data(), activeCount, 0);
#endif
    if (result >= 0) {
        for (size_t i = 0; i < activeCount; ++i) {
            descriptors[originalIndices[i]].revents = active[i].revents;
        }
        return result + invalidCount;
    }
    return invalidCount > 0 ? invalidCount : result;
}

} // namespace NetworkPollContract
