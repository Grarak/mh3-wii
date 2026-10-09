#include "efb_host.h"
#include "gx_panic.h"
#include "recomp_mod_loader.h"
#include "isa/ppc_isa_context.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <stdexcept>
#include <tuple>

namespace {
// MH3_TRACE_EFB=1: group CPU EFB accesses by (read/write, translated function,
// guest LR) and print counts, coordinate bounds and sample values every 5 s.
struct EfbTraceEntry {
    uint64_t count = 0;
    uint16_t minX = 1023, minY = 1023, maxX = 0, maxY = 0;
    uint32_t firstValue = 0, lastValue = 0;
};
void TraceEfbAccess(bool write, uint16_t x, uint16_t y, uint32_t value) {
    static const bool enabled = std::getenv("MH3_TRACE_EFB") != nullptr;
    if (!enabled)
        return;
    using Clock = std::chrono::steady_clock;
    static std::map<std::tuple<bool, uint32_t, uint32_t>, EfbTraceEntry> entries;
    static auto last = Clock::now();
    const auto* cpu = TryGetCpuContext();
    auto& entry = entries[{write, RecompMod::g_currentTranslatedExecutionAddress, cpu ? cpu->lr : 0}];
    if (!entry.count++)
        entry.firstValue = value;
    entry.lastValue = value;
    entry.minX = std::min(entry.minX, x);
    entry.maxX = std::max(entry.maxX, x);
    entry.minY = std::min(entry.minY, y);
    entry.maxY = std::max(entry.maxY, y);
    if (Clock::now() - last < std::chrono::seconds(5))
        return;
    last = Clock::now();
    std::fprintf(stderr, "MH3 EFB trace (last 5 s):\n");
    for (const auto& [key, e] : entries)
        std::fprintf(stderr, "  %s func 0x%08x lr 0x%08x: %llu accesses, x %u..%u y %u..%u, values 0x%08x .. 0x%08x\n", std::get<0>(key) ? "WRITE" : "READ ", std::get<1>(key), std::get<2>(key),
                     static_cast<unsigned long long>(e.count), e.minX, e.maxX, e.minY, e.maxY, e.firstValue, e.lastValue);
    entries.clear();
}
} // namespace

bool DecodeEfbColorAddress(uint32_t address, uint16_t& x, uint16_t& y) {
    if ((address >> 30) == 1 || (address & 0x3FC00003u) != 0x08000000u)
        return false;
    x = (address >> 2) & 1023;
    y = (address >> 12) & 1023;
    return true;
}
uint32_t ConvertEfbArgb(uint32_t color, unsigned pixelFormat, unsigned alphaMode) {
    unsigned a = color >> 24, r = (color >> 16) & 255, g = (color >> 8) & 255, b = color & 255;
    auto six = [](unsigned c) {
        return (c & 252) | (c >> 6);
    };
    auto five = [](unsigned c) {
        return (c & 248) | (c >> 5);
    };
    if (pixelFormat == 1) {
        a = six(a);
        r = six(r);
        g = six(g);
        b = six(b);
    } else if (pixelFormat == 2) {
        a = 255;
        r = five(r);
        g = six(g);
        b = five(b);
    } else if (pixelFormat == 0)
        a = 255;
    else
        throw std::runtime_error("Unsupported EFB peek pixel format");
    if (alphaMode == 0)
        a = 0;
    else if (alphaMode == 1)
        a = 255;
    else if (alphaMode != 2)
        throw std::runtime_error("Invalid PE alpha-read mode");
    return (a << 24) | (r << 16) | (g << 8) | b;
}
bool ReadEfbColor32(uint32_t address, uint32_t& value) {
    uint16_t x, y;
    if (!DecodeEfbColorAddress(address, x, y))
        return false;
    PanicLegacyGx("GX CPU EFB color read", address);
    // The address encoding has ten bits per coordinate; the physical EFB is smaller.
    value = x < 640 && y < 528 ? ReadHostEfbColor(x, y) : 0;
    TraceEfbAccess(false, x, y, value);
    return true;
}
bool WriteEfbColor32(uint32_t address, uint32_t value) {
    uint16_t x, y;
    if (!DecodeEfbColorAddress(address, x, y))
        return false;
    PanicLegacyGx("GX CPU EFB color write", address);
    TraceEfbAccess(true, x, y, value);
    if (x < 640 && y < 528)
        WriteHostEfbColor(x, y, value);
    return true;
}
#ifndef MH3_HAS_AURORA
void WriteHostEfbColor(uint16_t, uint16_t, uint32_t) {
    throw std::runtime_error("EFB color writes require the Aurora-linked build");
}
uint32_t ReadHostEfbColor(uint16_t, uint16_t) {
    throw std::runtime_error("EFB color reads require the Aurora-linked build");
}
#endif
