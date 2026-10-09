#pragma once

// Native AX worker startup only. This does not admit guest RAM or enable mixing.
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace AxDspHle {

constexpr int kMixWorkerCore0Mask = 0x00010000;

enum class MixWorkerStartupStatus : uint8_t {
    Pending,
    Ready,
    InvalidApis,
    ThreadIdFailed,
    SetAffinityFailed,
    ReadAffinityFailed,
    AffinityMismatch,
    ReadCoreFailed,
    CoreMismatch,
};

struct MixWorkerStartupProof {
    MixWorkerStartupStatus status = MixWorkerStartupStatus::Pending;
    bool core0Required = true;
    int threadId = -1;
    int setResult = -1;
    int affinityMask = -1;
    int cpuId = -1;

    bool Ready() const { return status == MixWorkerStartupStatus::Ready; }
};

struct MixWorkerStartupApis {
    int (*threadId)();
    int (*setAffinity)(int threadId, int mask);
    int (*readAffinity)(int threadId);
    int (*cpuId)();
};

inline MixWorkerStartupProof CheckMixWorkerCore0(const MixWorkerStartupApis& apis) {
    MixWorkerStartupProof proof;
    if (!apis.threadId || !apis.setAffinity || !apis.readAffinity || !apis.cpuId) {
        proof.status = MixWorkerStartupStatus::InvalidApis;
        return proof;
    }
    proof.threadId = apis.threadId();
    if (proof.threadId < 0) {
        proof.status = MixWorkerStartupStatus::ThreadIdFailed;
        return proof;
    }
    proof.setResult = apis.setAffinity(proof.threadId, kMixWorkerCore0Mask);
    if (proof.setResult < 0) {
        proof.status = MixWorkerStartupStatus::SetAffinityFailed;
        return proof;
    }
    proof.affinityMask = apis.readAffinity(proof.threadId);
    if (proof.affinityMask < 0) {
        proof.status = MixWorkerStartupStatus::ReadAffinityFailed;
        return proof;
    }
    if (proof.affinityMask != kMixWorkerCore0Mask) {
        proof.status = MixWorkerStartupStatus::AffinityMismatch;
        return proof;
    }
    proof.cpuId = apis.cpuId();
    if (proof.cpuId < 0) {
        proof.status = MixWorkerStartupStatus::ReadCoreFailed;
        return proof;
    }
    proof.status = proof.cpuId == 0 ? MixWorkerStartupStatus::Ready : MixWorkerStartupStatus::CoreMismatch;
    return proof;
}

inline const char* MixWorkerStartupStatusName(MixWorkerStartupStatus status) {
    switch (status) {
    case MixWorkerStartupStatus::Pending:
        return "pending";
    case MixWorkerStartupStatus::Ready:
        return "ready";
    case MixWorkerStartupStatus::InvalidApis:
        return "invalid APIs";
    case MixWorkerStartupStatus::ThreadIdFailed:
        return "thread ID query failed";
    case MixWorkerStartupStatus::SetAffinityFailed:
        return "affinity set failed";
    case MixWorkerStartupStatus::ReadAffinityFailed:
        return "affinity query failed";
    case MixWorkerStartupStatus::AffinityMismatch:
        return "affinity mask mismatch";
    case MixWorkerStartupStatus::ReadCoreFailed:
        return "CPU query failed";
    case MixWorkerStartupStatus::CoreMismatch:
        return "executing CPU mismatch";
    }
    return "unknown startup status";
}

// The owner and startup thread use the mixer's existing mutex/idle condition.
// Pending/spurious notifications cannot admit a job. A failure wakes the owner
// too, so it can join the failed thread and report the error before signalling.
inline MixWorkerStartupProof WaitForMixWorkerStartup(std::mutex& mutex, std::condition_variable& idle, const MixWorkerStartupProof& shared) {
    std::unique_lock<std::mutex> lock(mutex);
    idle.wait(lock, [&] { return shared.status != MixWorkerStartupStatus::Pending; });
    return shared;
}

inline void PublishMixWorkerStartup(std::mutex& mutex, std::condition_variable& idle, MixWorkerStartupProof& shared, const MixWorkerStartupProof& result) {
    {
        std::lock_guard<std::mutex> lock(mutex);
        shared = result;
    }
    idle.notify_all();
}

} // namespace AxDspHle
