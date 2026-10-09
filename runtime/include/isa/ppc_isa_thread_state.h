#pragma once
#include <cstdint>

struct CpuContext;

// The current CPU context and diagnostic execution address. Guest code runs
// only on the game thread, so this is one plain record. Values change
// independently and each scope restores its own.
namespace PpcThread {
struct State {
    CpuContext* context = nullptr;
    uint32_t executionAddress = 0;
};
inline constinit State state{};
} // namespace PpcThread
