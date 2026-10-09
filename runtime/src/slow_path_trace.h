// Development-only (MH3_DEVELOPMENT): slow memory path trace. See slow_path_trace.cpp.
#pragma once
#include <cstdint>

namespace MemoryInline {
extern bool g_slowPathTrace;
void RecordSlowPathAccess(uint32_t address, uint32_t size, bool isWrite, const void* hostCaller);
}
