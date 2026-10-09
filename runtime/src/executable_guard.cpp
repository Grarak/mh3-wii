#include "memory.h"
#include "recomp_mod_loader.h"
#include <sstream>
#include <stdexcept>
#include <utility>
#include <algorithm>

namespace RecompMod {
std::atomic<bool> g_executableWriteGuardEnabled{false};
std::atomic<uint8_t> g_executableWriteGuardPages[kExecutableWriteGuardPageCount]{};
std::atomic<uint8_t> g_executableWriteGuardCoarsePages[kExecutableWriteGuardCoarsePageCount]{};
std::atomic<uint8_t> g_executableWriteGuardMidPages[kExecutableWriteGuardMidPageCount]{};
namespace { std::vector<std::pair<uint32_t, uint32_t>> ranges; }
uint32_t CurrentTranslatedExecutionAddress() noexcept { return g_currentTranslatedExecutionAddress; }
void RegisterExecutableRange(uint32_t start, uint32_t end, std::string) {
    const auto range = std::pair{start & 0x3FFFFFFF, end & 0x3FFFFFFF};
    if (range.first >= range.second) throw std::runtime_error("Invalid executable range");
    if (std::find(ranges.begin(), ranges.end(), range) == ranges.end()) ranges.push_back(range);
    for (uint32_t alias : {0u, 0x80000000u, 0xC0000000u}) {
        for (uint32_t a = (start & 0x3FFFF000) | alias; a < ((end & 0x3FFFFFFF) | alias); a += 0x1000) {
            g_executableWriteGuardPages[a >> kExecutableWriteGuardPageShift] = 1;
            g_executableWriteGuardCoarsePages[a >> kExecutableWriteGuardCoarsePageShift] = 1;
            g_executableWriteGuardMidPages[a >> kExecutableWriteGuardMidPageShift] = 1;
        }
    }
    g_executableWriteGuardEnabled = true;
}
void UnregisterExecutableRange(uint32_t start, uint32_t end) {
    const auto range = std::pair{start & 0x3FFFFFFF, end & 0x3FFFFFFF};
    const auto found = std::find(ranges.begin(), ranges.end(), range);
    if (found == ranges.end()) throw std::runtime_error("Executable range is not registered");
    ranges.erase(found);
    // Page bits are conservative hints. Keeping them avoids clearing protection
    // for another range sharing a page; the exact range lookup is authoritative.
    g_executableWriteGuardEnabled = !ranges.empty();
}
void CheckExecutableWrite(uint32_t address, size_t length, uint64_t value) {
    if (!g_executableWriteGuardEnabled) return;
    uint32_t physical = address & 0x3FFFFFFF;
    for (const auto& [start, end] : ranges) {
        if (physical >= end || uint64_t(physical) + length <= start) continue;
        const auto* bytes = Memory::GetPointer(address, length);
        bool unchanged = length <= 8;
        for (size_t i = 0; unchanged && i < length; ++i)
            unchanged = bytes[i] == uint8_t(value >> ((length - i - 1) * 8));
        if (unchanged) return;
        std::ostringstream message;
        message << "Untranslated executable modification at 0x" << std::hex << address
                << " by 0x" << CurrentTranslatedExecutionAddress();
        throw std::runtime_error(message.str());
    }
}
bool HandleExecutableWrite(uint32_t address, size_t length, uint64_t value) {
    CheckExecutableWrite(address, length, value);
    return false;
}
}
