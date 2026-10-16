// MH3_ENTRY_TRACE=PATH[,gate=FILE] in a -DMH3_ENABLE_ENTRY_TRACE=ON build:
// every translated function counts its entries (the translator emits
// MKW_GUEST_ENTER at each body's start). PATH gets each function's guest
// address, entries and the order of its first entry, rewritten every 5 s and
// at exit; with `gate`, counting starts once FILE exists. A native port runs
// no translated body, so a trace of a build with ports differs from one
// without only where the ports' own calls differ:
// scripts/dev/entry_trace_compare.py compares two.
#include "ppc_runtime.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace {

// MEM1, where the DOL and the RSO overlays run.
constexpr uint32_t kBase = 0x80000000;
constexpr uint32_t kSlots = 0x01800000 / 4;

struct Trace {
    std::unique_ptr<std::atomic<uint32_t>[]> entries{new std::atomic<uint32_t>[kSlots]()};
    std::unique_ptr<std::atomic<uint32_t>[]> first{new std::atomic<uint32_t>[kSlots]()};
    std::atomic<uint32_t> order{0};
    std::atomic<uint32_t> outside{0};
    std::mutex dumpLock;
    std::string path, gate;
};

std::atomic<bool> g_counting{false};
Trace* g_trace = nullptr;

void Dump() {
    Trace& trace = *g_trace;
    std::lock_guard lock(trace.dumpLock);
    const std::string temporary = trace.path + ".tmp";
    std::FILE* file = std::fopen(temporary.c_str(), "w");

    if (!file)
        return;

    std::fprintf(file, "# address entries first\n");

    for (uint32_t i = 0; i < kSlots; ++i)
        if (const uint32_t entries = trace.entries[i].load(std::memory_order_relaxed))
            std::fprintf(file, "%08x %u %u\n", kBase + 4 * i, entries, trace.first[i].load(std::memory_order_relaxed));

    if (const uint32_t outside = trace.outside.load(std::memory_order_relaxed))
        std::fprintf(file, "# %u entries outside MEM1\n", outside);

    std::fclose(file);
    std::rename(temporary.c_str(), trace.path.c_str());
}

// Opens the gate, then rewrites PATH every 5 s.
void Writer() {
    Trace& trace = *g_trace;

    while (!g_counting.load(std::memory_order_relaxed)) {
        if (std::FILE* marker = std::fopen(trace.gate.c_str(), "r")) {
            std::fclose(marker);
            g_counting.store(true, std::memory_order_relaxed);
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        Dump();
    }
}

struct Start {
    Start() {
        const char* setting = std::getenv("MH3_ENTRY_TRACE");

        if (!setting || !*setting)
            return;

        g_trace = new Trace;
        std::string value = setting;

        if (const auto comma = value.find(",gate="); comma != std::string::npos) {
            g_trace->gate = value.substr(comma + 6);
            value.resize(comma);
        }

        g_trace->path = value;
        std::atexit(Dump);

        if (g_trace->gate.empty()) {
            g_counting.store(true, std::memory_order_relaxed);
            std::thread([] {
                for (;;) {
                    std::this_thread::sleep_for(std::chrono::seconds(5));
                    Dump();
                }
            }).detach();
        } else {
            std::thread(Writer).detach();
        }

        std::fprintf(stderr, "MH3 entry trace: %s%s%s\n", g_trace->path.c_str(), g_trace->gate.empty() ? "" : " once ", g_trace->gate.c_str());
    }
} g_start;

} // namespace

extern "C" void MH3_TraceEntry(uint32_t address) {
    if (!g_counting.load(std::memory_order_relaxed))
        return;

    Trace& trace = *g_trace;
    const uint32_t offset = address - kBase;

    if (offset >= kSlots * 4) {
        trace.outside.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    if (trace.entries[offset / 4].fetch_add(1, std::memory_order_relaxed) == 0)
        trace.first[offset / 4].store(trace.order.fetch_add(1, std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}
