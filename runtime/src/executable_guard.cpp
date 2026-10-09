#include "memory.h"
#include "fatal.h"
#include "recomp_mod_loader.h"
#include <sstream>
#include <stdexcept>
#include <algorithm>
#include <cstring>

namespace RecompMod {
namespace {
struct Range {
    uint32_t start, end;
    uint64_t checksum = 0;
    bool verified = false;
};

std::vector<Range> ranges;

uint64_t Checksum(uint32_t start, uint32_t end) {
    // FNV-1a over 64-bit words; code ranges are word aligned.
    const uint8_t* bytes = Memory::GetPointer(start | 0x80000000u, end - start);
    uint64_t hash = 0xcbf29ce484222325ull;
    size_t i = 0;

    for (; i + 8 <= end - start; i += 8) {
        uint64_t word;
        std::memcpy(&word, bytes + i, 8);
        hash = (hash ^ word) * 0x100000001b3ull;
    }

    for (; i < end - start; ++i)
        hash = (hash ^ bytes[i]) * 0x100000001b3ull;

    return hash;
}
} // namespace

uint32_t CurrentTranslatedExecutionAddress() noexcept {
    return PpcThread::state.executionAddress;
}

void RegisterExecutableRange(uint32_t start, uint32_t end, std::string) {
    start &= 0x3FFFFFFF;
    end &= 0x3FFFFFFF;

    if (start >= end)
        MH3_THROW(std::runtime_error("Invalid executable range"));

    if (std::none_of(ranges.begin(), ranges.end(), [&](const Range& r) { return r.start == start && r.end == end; }))
        ranges.push_back({start, end});
}

void UnregisterExecutableRange(uint32_t start, uint32_t end) {
    start &= 0x3FFFFFFF;
    end &= 0x3FFFFFFF;
    const auto found = std::find_if(ranges.begin(), ranges.end(), [&](const Range& r) { return r.start == start && r.end == end; });

    if (found == ranges.end())
        MH3_THROW(std::runtime_error("Executable range is not registered"));

    ranges.erase(found);
}

void CheckExecutableWrite(uint32_t address, size_t length, uint64_t value) {
    const uint32_t physical = address & 0x3FFFFFFF;

    for (const auto& range : ranges) {
        if (physical >= range.end || uint64_t(physical) + length <= range.start)
            continue;

        const auto* bytes = Memory::GetPointer(address, length);
        bool unchanged = length <= 8;

        for (size_t i = 0; unchanged && i < length; ++i)
            unchanged = bytes[i] == uint8_t(value >> ((length - i - 1) * 8));

        if (unchanged)
            return;

        std::ostringstream message;
        message << "Untranslated executable modification at 0x" << std::hex << address << " by 0x" << CurrentTranslatedExecutionAddress();
        MH3_THROW(std::runtime_error(message.str()));
    }
}

void VerifyExecutableRanges() {
    for (auto& range : ranges) {
        const uint64_t checksum = Checksum(range.start, range.end);

        if (!range.verified) {
            range.checksum = checksum;
            range.verified = true;
        } else if (checksum != range.checksum) {
            std::ostringstream message;
            message << "Translated code at 0x" << std::hex << (range.start | 0x80000000u) << "..0x" << (range.end | 0x80000000u) << " was modified by a guest store";
            MH3_THROW(std::runtime_error(message.str()));
        }
    }
}
} // namespace RecompMod
