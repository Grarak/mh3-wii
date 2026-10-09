// Debug-only (MH3_WRITE_WATCH build): find the guest code that writes an
// address range. MH3_WRITE_WATCH=START,LENGTH (hex) makes every inline guest
// store into the range call NoteWatchedWrite, which records the host return
// address: a translated func_XXXXXXXX (the build links
// with -rdynamic so dladdr can name it). Every 5 s (the ms=N option) the
// writers are listed per (function, guest lr, offset in the range), with a
// count and the last value.
//
// MH3_WRITE_WATCH=list=FILE watches the words listed instead (one hex address
// per line, as the first field: a step probe's output works as is) and
// reports each by its address.
#include "dev_options.h"
#include "memory_access.h"
#include "isa/ppc_isa_context.h"

#include <dlfcn.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace MemoryInline {
namespace {

struct Writes {
    uint64_t count = 0;
    uint64_t last = 0;
    uint32_t size = 0;
};
using Key = std::tuple<const void*, uint32_t, uint32_t>; // host caller, guest lr, offset
std::map<Key, Writes> writes;
std::chrono::steady_clock::time_point lastReport = std::chrono::steady_clock::now();
std::chrono::milliseconds interval{5000};
std::vector<uint8_t> words; // list= bitmap
bool reporting = false;

std::string Describe(const void* pc) {
    Dl_info info{};
    if (dladdr(pc, &info) && info.dli_sname)
        return info.dli_sname;
    char text[32];
    std::snprintf(text, sizeof(text), "%p", pc);
    return text;
}

void Report() {
    std::vector<std::pair<Key, Writes>> sorted(writes.begin(), writes.end());
    std::fprintf(stderr, "MH3 write watch: %llu writers of [%08x, +%x)\n", static_cast<unsigned long long>(sorted.size()), g_writeWatchStart, g_writeWatchLength);
    for (const auto& [key, w] : sorted) {
        const auto& [caller, lr, offset] = key;
        float asFloat;
        const uint32_t bits = uint32_t(w.last);
        std::memcpy(&asFloat, &bits, 4);
        std::fprintf(stderr,
                     words.empty() ? "MH3 write watch:   +%04x %s lr %08x: %llu writes of %u bytes, last %llx (%g)\n" : "MH3 write watch:   %08x %s lr %08x: %llu writes of %u bytes, last %llx (%g)\n",
                     words.empty() ? offset : g_writeWatchStart + offset, Describe(caller).c_str(), lr, static_cast<unsigned long long>(w.count), w.size, static_cast<unsigned long long>(w.last),
                     w.size == 4 ? asFloat : 0.0);
    }
    writes.clear();
}

struct Arm {
    Arm() {
        const DevOptions options("MH3_WRITE_WATCH", {"ms", "list"}, DevOptions::Words::Allowed);
        interval = std::chrono::milliseconds(options.Number("ms", 5000));
        if (const char* list = options.Value("list")) {
            std::vector<uint32_t> listed;
            std::ifstream in(list);
            for (std::string line; std::getline(in, line);) {
                char* end = nullptr;
                const uint32_t address = uint32_t(std::strtoul(line.c_str(), &end, 16));
                if (end != line.c_str() && line[0] != '#')
                    listed.push_back(address & ~3u);
            }
            if (listed.empty())
                return;
            const auto [low, high] = std::minmax_element(listed.begin(), listed.end());
            g_writeWatchStart = *low;
            g_writeWatchLength = *high - *low + 4;
            words.assign(g_writeWatchLength / 32 + 1, 0);
            for (uint32_t a : listed)
                words[(a - *low) >> 5] |= uint8_t(1u << (((a - *low) >> 2) & 7));
            g_writeWatchWords = words.data();
            std::fprintf(stderr, "MH3 write watch: armed %llu words in [%08x, +%x)\n", static_cast<unsigned long long>(listed.size()), g_writeWatchStart, g_writeWatchLength);
            return;
        }
        const auto& range = options.List();
        if (range.empty())
            return;
        g_writeWatchStart = uint32_t(std::strtoul(range[0].c_str(), nullptr, 16));
        g_writeWatchLength = range.size() > 1 ? uint32_t(std::strtoul(range[1].c_str(), nullptr, 16)) : 4;
        std::fprintf(stderr, "MH3 write watch: armed [%08x, +%x)\n", g_writeWatchStart, g_writeWatchLength);
    }
} arm;

} // namespace

void RecordWatchedWrite(uint32_t address, uint32_t size, uint64_t value, const void* hostCaller) {
    if (reporting)
        return;
    const CpuContext* cpu = TryGetCpuContext();
    auto& w = writes[{hostCaller, cpu ? cpu->lr : 0, address - g_writeWatchStart}];
    ++w.count;
    w.last = value;
    w.size = size;
    const auto now = std::chrono::steady_clock::now();
    if (now - lastReport > interval) {
        lastReport = now;
        reporting = true;
        Report();
        reporting = false;
    }
}

void NoteWatchedWrite(uint32_t address, uint32_t size, uint64_t value) {
    // Out of line, so the return address is the translated store's function.
    RecordWatchedWrite(address, size, value, __builtin_return_address(0));
}

} // namespace MemoryInline
