#pragma once
#include <cstdint>

struct CpuContext;

// CPU entry and diagnostic address scopes belong to the same host thread.
// One constant-initialized TLS record lets a dispatch resolve their storage
// once. Values still change independently and each scope restores its own.
namespace PpcThread {
struct State {
    CpuContext* context = nullptr;
    uint32_t executionAddress = 0;
};
inline thread_local constinit State state{};
} // namespace PpcThread
