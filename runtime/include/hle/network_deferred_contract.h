#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <utility>

namespace NetworkDeferredContract {

enum class PreparationState : uint8_t {
  NotApplicable,
  Ready,
  Error,
};

// A recognized operation must not be represented by an empty optional: doing
// so makes malformed input and resource failures indistinguishable from an
// unrelated ioctl and lets the caller fall through to a blocking fallback.
template <typename Work> class Preparation {
public:
  static Preparation NotApplicable() {
    return Preparation(PreparationState::NotApplicable, 0, std::nullopt);
  }

  static Preparation Ready(Work work, int32_t failureResult) {
    return Preparation(PreparationState::Ready, failureResult,
                       std::optional<Work>(std::move(work)));
  }

  static Preparation Error(int32_t result) {
    return Preparation(PreparationState::Error, result, std::nullopt);
  }

  PreparationState State() const noexcept { return state_; }
  int32_t FailureResult() const noexcept { return failureResult_; }

  Work &&TakeWork() && { return std::move(*work_); }

private:
  Preparation(PreparationState state, int32_t failureResult,
              std::optional<Work> work)
      : state_(state), failureResult_(failureResult), work_(std::move(work)) {}

  PreparationState state_ = PreparationState::NotApplicable;
  int32_t failureResult_ = 0;
  std::optional<Work> work_;
};

enum class StartDisposition : uint8_t {
  NotApplicable,
  Started,
  ImmediateResult,
};

struct StartOutcome {
  StartDisposition disposition = StartDisposition::NotApplicable;
  int32_t result = 0;
  uint64_t token = 0;

  static constexpr StartOutcome NotApplicable() noexcept { return {}; }

  static constexpr StartOutcome Started(uint64_t token) noexcept {
    return {StartDisposition::Started, 0, token};
  }

  static constexpr StartOutcome Immediate(int32_t result) noexcept {
    return {StartDisposition::ImmediateResult, result, 0};
  }
};

// Launcher returns a token when the operation is fully installed. Token zero
// is valid for an asynchronous route because only synchronous callers consume
// it. A launcher failure or exception becomes the operation-specific immediate
// error; it can never be reinterpreted as "not applicable".
template <typename Work, typename Launcher>
StartOutcome StartPrepared(Preparation<Work> &&preparation,
                           Launcher &&launcher) {
  const PreparationState state = preparation.State();
  const int32_t failureResult = preparation.FailureResult();
  if (state == PreparationState::NotApplicable) {
    return StartOutcome::NotApplicable();
  }
  if (state == PreparationState::Error) {
    return StartOutcome::Immediate(failureResult);
  }

  try {
    const std::optional<uint64_t> token =
        std::forward<Launcher>(launcher)(std::move(preparation).TakeWork());
    return token ? StartOutcome::Started(*token)
                 : StartOutcome::Immediate(failureResult);
  } catch (...) {
    return StartOutcome::Immediate(failureResult);
  }
}

// Keep an untouched copy of the host-only work value until resolution and
// completion publication both succeed. Any worker-side exception is converted
// into one failure-publication attempt instead of escaping the thread entry and
// terminating the process.
template <typename Work, typename Resolver, typename Publish,
          typename PublishFailure>
void RunWorker(Work work, Resolver &&resolver, Publish &&publish,
               PublishFailure &&publishFailure) noexcept {
  try {
    auto completion = std::forward<Resolver>(resolver)(work);
    std::forward<Publish>(publish)(std::move(completion));
  } catch (...) {
    const std::exception_ptr error = std::current_exception();
    try {
      std::forward<PublishFailure>(publishFailure)(std::move(work), error);
    } catch (...) {
      // There is no safe blocking fallback from a detached worker. The
      // production failure publisher logs if its completion queue cannot
      // accept the already-normalized failure.
    }
  }
}

template <typename Worker, typename OnDetachFailure>
Worker *DetachOrRelease(std::unique_ptr<Worker> worker,
                        OnDetachFailure &&onDetachFailure) noexcept {
  if (!worker) {
    return nullptr;
  }

  try {
    worker->detach();
    return nullptr;
  } catch (...) {
    const std::exception_ptr error = std::current_exception();
    Worker *const runningWorker = worker.release();
    try {
      std::forward<OnDetachFailure>(onDetachFailure)(error);
    } catch (...) {
      // Diagnostics must not turn containment of a running worker into a
      // second failure path.
    }
    return runningWorker;
  }
}

// IOS encodes an IPv4 sockaddr as a two-byte length/family header followed by
// sockaddr::sa_data. Advertising any larger ai_addrlen would expose bytes that
// were never copied into the guest result.
inline constexpr size_t kWiiSockAddrHeaderBytes = 2;
inline constexpr size_t kWiiSockAddrPayloadBytes = 14;
inline constexpr size_t kWiiIpv4SockAddrBytes =
    kWiiSockAddrHeaderBytes + kWiiSockAddrPayloadBytes;

inline constexpr bool CanCopyIpv4SockAddr(int nativeFamily, size_t nativeLength,
                                          int nativeIpv4Family) noexcept {
  return nativeFamily == nativeIpv4Family &&
         nativeLength >= kWiiIpv4SockAddrBytes;
}

inline constexpr bool
AdvertisedSockAddrFits(uint32_t advertisedLength) noexcept {
  return advertisedLength <= kWiiIpv4SockAddrBytes;
}

} // namespace NetworkDeferredContract

namespace NetworkConnectContract {

// IOS presents a blocking socket to the guest while the retained host socket
// stays nonblocking. A blocking guest connect therefore waits on the guest
// OSThread, never inside WSAPoll/poll on the emulation scheduler thread.
inline constexpr int64_t kGuestBlockingTimeoutMilliseconds = 10000;

enum class ProbeDisposition : uint8_t {
    StaleSocket,
    PollError,
    SocketReady,
    TimedOut,
    Pending,
};

// Keep the ordering explicit: fd reuse invalidates the operation before any
// host syscall, readiness wins at the deadline, and only a zero-result probe
// may remain pending or time out.
inline constexpr ProbeDisposition ClassifyProbe(
    bool socketIdentityIsCurrent, int pollResult, bool deadlineExpired) noexcept {
    if (!socketIdentityIsCurrent) {
        return ProbeDisposition::StaleSocket;
    }
    if (pollResult < 0) {
        return ProbeDisposition::PollError;
    }
    if (pollResult > 0) {
        return ProbeDisposition::SocketReady;
    }
    return deadlineExpired ? ProbeDisposition::TimedOut
                           : ProbeDisposition::Pending;
}

} // namespace NetworkConnectContract
