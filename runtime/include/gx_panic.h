#pragma once

#include <cstdint>

struct CpuContext;

// Remaining Wii graphics calls are fatal during the native renderer migration.
// This reports both the guest call chain and the native stack before exiting.
[[noreturn]] void PanicLegacyGx(const char* entry, uint32_t guestAddress = 0, const CpuContext* cpu = nullptr) noexcept;
void InstallLegacyGxPanicHandler() noexcept;
