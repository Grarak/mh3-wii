#pragma once

#include <cstdint>

struct CpuContext;

namespace AxDspHle {

void Init();
void InitForAXOut(CpuContext* ctx);
void Stop();

uint32_t CheckInit();
uint32_t AddTask(uint32_t taskPtr);
void SendMailToDSP(uint32_t mail);
uint32_t CheckMailToDSP();
uint32_t CheckMailFromDSP();
uint32_t ReadMailFromDSP();
uint32_t AssertTask(uint32_t taskPtr);
void ServiceDeferredCallbacks();

// Publishes the private epoch before a guest audio consumer or mutation.
// Audio DMA calls this before queueing PCM and invoking the AI callback; native
// PB synchronization, reset, mode switching and memory teardown also drain.
// Cheap when the mix already finished (the normal case).
void JoinMixWorker();

// Joins and tears down the mix worker thread. Call before the guest memory map
// is destroyed or re-created.
void ShutdownMixWorker();

void SetMixWorkerEnabled(bool enabled);

// Owner diagnostics; durations include admission/copy, numeric work and the
// completion wait/publication separately. No guest callbacks run on the worker.
struct MixWorkerStats {
    uint64_t submitted = 0, completed = 0, published = 0, rejected = 0, failed = 0, serial = 0;
    uint64_t prepareNs = 0, workerNs = 0, waitNs = 0, publishNs = 0;
    uint64_t maxPrepareNs = 0, maxWorkerNs = 0, maxWaitNs = 0;
    uint64_t lastJobBytes = 0, maxJobBytes = 0, totalJobBytes = 0, storageBytes = 0;
    uint32_t lastActivePbs = 0;
    int workerThread = 0, workerCore = -1, workerAffinity = 0;
    bool workerRunning = false, core0Checked = false;
};
MixWorkerStats GetMixWorkerStats();

void InitAram();

} // namespace AxDspHle
