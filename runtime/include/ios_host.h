#pragma once
#include <cstdint>
#include <optional>

struct IosCompletion {
    uint32_t callback;
    int32_t result;
    uint32_t argument;
};
// The host scheduler must deliver queued callbacks at a guest-safe boundary.
std::optional<IosCompletion> TakeIosCompletion();
void QueueIosCompletion(IosCompletion completion);
void TriggerStmEvent(uint32_t event);
struct CpuContext;
void PumpIosCompletions(CpuContext* cpu);
