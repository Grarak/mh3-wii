#include "guest_call_trace.h"

#include "dev_options.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {
// MH3_CALL_TRACE=PATH[,gate=MARKER]: call edges as CSV; with a gate, only once
// the marker file exists.
struct TraceState {
    const DevOptions options{"MH3_CALL_TRACE", {"gate"}, DevOptions::Words::Allowed};
    std::FILE* file = nullptr;
    const char* gate = options.Value("gate");
    std::atomic<unsigned> gateChecks{0};
    std::atomic<bool> gateOpen{gate == nullptr};
    std::atomic<unsigned> count{0};

    TraceState() {
        if (!options.List().empty())
            file = std::fopen(options.List().front().c_str(), "w");
    }

    ~TraceState() {
        if (file)
            std::fclose(file);
    }
};

TraceState& State() {
    static TraceState state;
    return state;
}
} // namespace

void MkwTraceCall(uint32_t from, uint32_t to, uint32_t site) {
    TraceState& state = State();

    if (!state.file)
        return;

    if (!state.gateOpen.load(std::memory_order_relaxed)) {
        if ((state.gateChecks.fetch_add(1, std::memory_order_relaxed) & 255u) == 255u) {
            if (std::FILE* marker = std::fopen(state.gate, "r")) {
                std::fclose(marker);
                state.gateOpen.store(true, std::memory_order_relaxed);
            }
        }

        if (!state.gateOpen.load(std::memory_order_relaxed))
            return;
    }

    const unsigned n = state.count.fetch_add(1, std::memory_order_relaxed);

    if (n >= 500000)
        return;

    std::fprintf(state.file, "%08x,%08x,%08x\n", from, to, site);

    if ((n & 255u) == 255u)
        std::fflush(state.file);
}
