#pragma once
// Debug-only tracer for host accesses to GX texture-copy destinations.
#include <cstdint>

namespace EfbCopyTrace {
void BeforeCopy(uintptr_t host, uint32_t bytes);
void AfterCopy(uintptr_t host, uint32_t guest, uint32_t bytes, uint32_t width, uint32_t height, uint32_t format);
void EndFrame();
// Host code that reads guest texture bytes on purpose (texture hashing): lift
// protection for the span and restore it afterwards, so guest reads stay visible.
void BeginHostRead(uintptr_t host, uint32_t bytes);
void EndHostRead();
} // namespace EfbCopyTrace
