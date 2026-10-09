// Development-only (MH3_DEVELOPMENT): lists the translated functions whose
// guest loads and stores miss the inline page-bias lookup and take the slow
// memory path. MH3_SLOW_PATH_TRACE=file arms it; every 10 s and at exit the
// file is rewritten with, per region (MMIO block, GX FIFO, EFB, locked cache,
// RAM, unmapped) and per translated function (named by dladdr from the
// host return address, so a function the compiler inlined shows as its
// caller), the read/write counts, access sizes and the addresses touched.
// Accesses the translator lowered to GX_HLE_FIFO_* calls never reach the slow
// path and are not listed; scripts/ can find those statically.
#include "slow_path_trace.h"

#ifdef __vita__
#include "abi_bridge.h"
#else
#include <dlfcn.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace MemoryInline {
bool g_slowPathTrace = false;

namespace {

const char* Region(uint32_t address) {
    if (address >= 0xE0000000u && address < 0xE0004000u)
        return "locked cache";
    uint32_t p = address & 0x3FFFFFFFu;
    // RAM that missed the inline lookup: a 1 MiB page holding code (stores),
    // or the last MiB of MEM1/MEM2, which the fast path's 7-byte overrun
    // margin keeps unpublished.
    if ((address >> 30) != 1 && (p < 0x01800000u || (p >= 0x10000000u && p < 0x14000000u)))
        return (address >> 30) == 2 ? "RAM (cached 0x8...)" : "RAM (uncached 0xC...)";
    if ((address >> 30) == 1 || (address >> 30) == 2)
        return "unmapped";
    // Wii mirrors of the legacy DI/SI/EXI/AI blocks at 0x0D006000.
    if (p >= 0x0D006000u && p < 0x0D007000u)
        p -= 0x01000000u;
    if (p >= 0x08000000u && p < 0x09000000u)
        return "EFB";
    if ((p & 0xFFFFFF00u) == 0x0C008000u)
        return "GX FIFO";
    if (p >= 0x0C000000u && p < 0x0C001000u)
        return "CP";
    if (p >= 0x0C001000u && p < 0x0C002000u)
        return "PE";
    if (p >= 0x0C002000u && p < 0x0C003000u)
        return "VI";
    if (p >= 0x0C003000u && p < 0x0C004000u)
        return "PI";
    if (p >= 0x0C004000u && p < 0x0C005000u)
        return "MI";
    if (p >= 0x0C005000u && p < 0x0C006000u)
        return "DSP";
    if (p >= 0x0C006000u && p < 0x0C006400u)
        return "DI";
    if (p >= 0x0C006400u && p < 0x0C006800u)
        return "SI";
    if (p >= 0x0C006800u && p < 0x0C006C00u)
        return "EXI";
    if (p >= 0x0C006C00u && p < 0x0C007000u)
        return "AI";
    if (p >= 0x0D000000u && p < 0x0D000010u)
        return "IPC";
    if (p >= 0x0D000010u && p < 0x0D008000u)
        return "Hollywood";
    if (p >= 0x0D800000u && p < 0x0D808000u)
        return "Hollywood (mirror)";
    if (p >= 0x0C000000u && p < 0x0E000000u)
        return "other MMIO";
    return "unmapped";
}

struct Counts {
    uint64_t reads = 0, writes = 0;
    uint32_t sizes = 0; // bit n: an access of n bytes
};
// host caller, guest address
std::map<std::pair<const void*, uint32_t>, Counts> accesses;
std::mutex mutex;
std::string path;
const auto start = std::chrono::steady_clock::now();
auto lastReport = start;

std::string Describe(const void* pc) {
#ifdef __vita__
    if (auto entry = TranslatedFunctionRegistry::FindByHostAddress(reinterpret_cast<uintptr_t>(pc)))
        return entry->name;
#else
    Dl_info info{};
    if (dladdr(pc, &info) && info.dli_sname)
        return info.dli_sname;
#endif
    char text[32];
    std::snprintf(text, sizeof(text), "%p", pc);
    return text;
}

struct FunctionCounts {
    Counts total;
    std::map<uint32_t, Counts> addresses;
};

void Report() {
    // region -> function -> counts
    std::map<std::string, std::map<std::string, FunctionCounts>> byRegion;
    std::map<const void*, std::string> names;
    for (const auto& [key, c] : accesses) {
        auto& name = names[key.first];
        if (name.empty())
            name = Describe(key.first);
        auto& f = byRegion[Region(key.second)][name];
        f.total.reads += c.reads;
        f.total.writes += c.writes;
        f.total.sizes |= c.sizes;
        auto& a = f.addresses[key.second];
        a.reads += c.reads;
        a.writes += c.writes;
        a.sizes |= c.sizes;
    }
    FILE* out = std::fopen(path.c_str(), "w");
    if (!out)
        return;
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::fprintf(out, "# MH3 slow-path access trace after %.0f s (GX_HLE_FIFO_* lowered writes excluded; RAM addresses per 32-byte line)\n", seconds);
    for (const auto& [region, functions] : byRegion) {
        uint64_t reads = 0, writes = 0;
        for (const auto& [name, f] : functions)
            reads += f.total.reads, writes += f.total.writes;
        std::fprintf(out, "\n## %s: %llu functions, %llu reads, %llu writes\n", region.c_str(), static_cast<unsigned long long>(functions.size()), static_cast<unsigned long long>(reads),
                     static_cast<unsigned long long>(writes));
        std::vector<std::pair<uint64_t, const std::string*>> order;
        for (const auto& [name, f] : functions)
            order.push_back({f.total.reads + f.total.writes, &name});
        std::sort(order.rbegin(), order.rend());
        for (const auto& [count, name] : order) {
            const auto& f = functions.at(*name);
            std::fprintf(out, "%-28s %10llu R %10llu W  sizes", name->c_str(), static_cast<unsigned long long>(f.total.reads), static_cast<unsigned long long>(f.total.writes));
            for (int n : {1, 2, 4, 8})
                if (f.total.sizes & (1u << n))
                    std::fprintf(out, " %d", n);
            std::fprintf(out, "  %llu addresses:", static_cast<unsigned long long>(f.addresses.size()));
            size_t shown = 0;
            for (const auto& [address, c] : f.addresses) {
                if (++shown > 12) {
                    std::fprintf(out, " ...");
                    break;
                }
                std::fprintf(out, " %08x%s%s", address, c.reads ? "r" : "", c.writes ? "w" : "");
            }
            std::fprintf(out, "\n");
        }
    }
    std::fclose(out);
}

struct Arm {
    Arm() {
        if (const char* p = std::getenv("MH3_SLOW_PATH_TRACE")) {
            path = p;
            g_slowPathTrace = true;
        }
    }
    ~Arm() {
        if (!g_slowPathTrace)
            return;
        std::lock_guard lock(mutex);
        g_slowPathTrace = false;
        Report();
    }
} arm;

} // namespace

void RecordSlowPathAccess(uint32_t address, uint32_t size, bool isWrite, const void* hostCaller) {
    std::lock_guard lock(mutex);
    if (!g_slowPathTrace)
        return;
    // RAM is listed per 32-byte line so a store loop over a code page stays small.
    const char* region = Region(address);
    const bool ram = region[0] == 'R';
    auto& c = accesses[{hostCaller, ram ? address & ~31u : address}];
    (isWrite ? c.writes : c.reads)++;
    c.sizes |= 1u << size;
    const auto now = std::chrono::steady_clock::now();
    if (now - lastReport > std::chrono::seconds(10)) {
        lastReport = now;
        Report();
    }
}

} // namespace MemoryInline
