#pragma once

#include <cstdint>

// The temporary generated-shard probes compile to nothing in release builds.
#if defined(MH3_ENABLE_GUEST_CALL_TRACE)
void MkwTraceCall(uint32_t from, uint32_t to, uint32_t site);
#define MKW_GUEST_TRACE_CALL(from, to, site) MkwTraceCall(from, to, site)
#else
#define MKW_GUEST_TRACE_CALL(from, to, site) ((void)0)
#endif
