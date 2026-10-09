#pragma once

// A bounded, alias-preserving AX epoch. Only the owner touches destination
// pointers: the worker resolves addresses exclusively into the private bytes.
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace AxDspHle {

#ifdef MH3_AX_PRIVATE_JOB_TEST
// The native timing fixture records the actual admitted range/write stream.
// No observer or observer branch is present in production builds.
void PrivateMixTestRangeHook(uint32_t addr, size_t bytes, uint8_t* destination, bool writable);
void PrivateMixTestWriteHook(uint32_t addr, size_t bytes);
#endif

inline bool NormalizeMixAddress(uint32_t addr, size_t bytes, uint32_t& physical) {
    constexpr uint32_t mem1Size = 24u << 20, mem2Size = 64u << 20;
    if (!bytes || bytes > mem2Size || uint64_t(addr) + bytes > uint64_t(UINT32_MAX) + 1)
        return false;
    for (uint32_t base : {0u, 0x80000000u, 0xc0000000u}) {
        const uint32_t offset = addr - base;
        if (offset < mem1Size && bytes <= mem1Size - offset) {
            physical = offset;
            return true;
        }
    }
    for (uint32_t base : {0x10000000u, 0x90000000u, 0xd0000000u}) {
        const uint32_t offset = addr - base;
        if (offset < mem2Size && bytes <= mem2Size - offset) {
            physical = 0x10000000u + offset;
            return true;
        }
    }
    return false;
}

class PrivateMixMemory {
public:
    static constexpr size_t kCapacity = 512u << 10;
    static constexpr size_t kMaxSpans = 768;

    void Reset() {
        m_spanCount = 0;
        m_writeCount = 0;
        m_bytes = 0;
        m_sealed = false;
        ++m_generation;
    }

    bool Add(uint32_t addr, size_t bytes, uint8_t* destination, bool writable = false) {
        uint32_t begin;
        if (m_sealed || !destination || !NormalizeMixAddress(addr, bytes, begin)) return false;
        const uint32_t end = begin + static_cast<uint32_t>(bytes);
        if (writable) {
            if (m_writeCount == m_writes.size()) return false;
            m_writes[m_writeCount++] = {begin, end};
        }
        // Sorted, disjoint spans make admission lookups bounded by a binary
        // search. Merge all aliases and adjacency before copying so writes
        // still forward through the complete intra-command byte view.
        uint32_t mergedBegin = begin, mergedEnd = end;
        uint8_t* mergedDestination = destination;
        size_t lo = 0, hi = m_spanCount;
        if (m_spanCount && m_spans[m_spanCount - 1].end < begin) lo = hi;
        while (lo < hi) {
            const size_t mid = lo + (hi - lo) / 2;
            if (m_spans[mid].end < begin) lo = mid + 1;
            else hi = mid;
        }
        size_t next = lo;
        while (next < m_spanCount && m_spans[next].begin <= mergedEnd) {
            const Span& span = m_spans[next++];
            if (span.begin < mergedBegin) {
                mergedBegin = span.begin;
                mergedDestination = span.destination;
            }
            mergedEnd = std::max(mergedEnd, span.end);
        }
        if (next == lo && m_spanCount == m_spans.size()) return false;
        const size_t tail = m_spanCount - next;
        if (tail && next != lo + 1)
            std::memmove(m_spans.data() + lo + 1, m_spans.data() + next, tail * sizeof(Span));
        m_spanCount = lo + 1 + tail;
        m_spans[lo] = {mergedBegin, mergedEnd, 0, mergedDestination, false};
#ifdef MH3_AX_PRIVATE_JOB_TEST
        PrivateMixTestRangeHook(addr, bytes, destination, writable);
#endif
        return true;
    }

    bool Seal() {
        m_bytes = 0;
        for (size_t i = 0; i < m_spanCount; ++i) {
            Span& span = m_spans[i];
            const size_t bytes = span.end - span.begin;
            if (bytes > m_data.size() - m_bytes) return false;
            span.offset = static_cast<uint32_t>(m_bytes);
            std::memcpy(m_data.data() + m_bytes, span.destination, bytes);
            m_bytes += bytes;
        }
        std::sort(m_writes.begin(), m_writes.begin() + m_writeCount,
                  [](const Range& a, const Range& b) { return a.begin < b.begin; });
        size_t writes = 0;
        for (size_t i = 0; i < m_writeCount; ++i) {
            if (writes && m_writes[writes - 1].end >= m_writes[i].begin)
                m_writes[writes - 1].end = std::max(m_writes[writes - 1].end, m_writes[i].end);
            else m_writes[writes++] = m_writes[i];
        }
        m_writeCount = writes;
        m_lastSpan = m_lastWrite = 0;
        std::fill_n(m_dirty.data(), (m_bytes + 7) / 8, uint8_t(0));
        m_sealed = true;
        return true;
    }

    uint8_t* Resolve(uint32_t addr, size_t bytes, bool write = false) {
        uint32_t physical;
        if (!m_sealed || !NormalizeMixAddress(addr, bytes, physical)) return nullptr;
        Span* span = Find(physical);
        if (!span || bytes > span->end - physical) return nullptr;
        const size_t offset = span->offset + physical - span->begin;
        if (write) {
            if (!Writable(physical, bytes)) return nullptr;
            MarkDirty(offset, bytes);
            span->written = true;
#ifdef MH3_AX_PRIVATE_JOB_TEST
            PrivateMixTestWriteHook(addr, bytes);
#endif
        }
        return m_data.data() + offset;
    }

    bool Window(uint32_t addr, uint32_t& begin, uint32_t& end, const uint8_t*& host) {
        uint32_t physical;
        if (!m_sealed || !NormalizeMixAddress(addr, 1, physical)) return false;
        Span* span = Find(physical);
        if (!span) return false;
        begin = addr - (physical - span->begin);
        // A normalized span never crosses a memory region or its alias boundary.
        end = begin + (span->end - span->begin);
        host = m_data.data() + span->offset;
        return true;
    }

    void Publish() {
        if (!m_sealed) return;
        for (size_t i = 0; i < m_spanCount; ++i) {
            Span& span = m_spans[i];
            if (!span.written) continue;
            const size_t begin = span.offset, end = begin + span.end - span.begin;
            size_t pos = begin;
            while (pos < end) {
                const size_t run = NextDirtyBit<true>(pos, end);
                pos = NextDirtyBit<false>(run, end);
                if (pos != run)
                    std::memcpy(span.destination + run - begin, m_data.data() + run, pos - run);
            }
            span.written = false;
        }
        // Exactly once even when a caller joins again after CPU aux processing.
        m_sealed = false;
    }

    size_t Bytes() const { return m_bytes; }
    size_t SpanCount() const { return m_spanCount; }
    uint32_t Generation() const { return m_generation; }

private:
    struct Range { uint32_t begin, end; };
    struct Span {
        uint32_t begin, end, offset;
        uint8_t* destination;
        bool written;
    };
    Span* Find(uint32_t physical) {
        if (m_lastSpan < m_spanCount && physical >= m_spans[m_lastSpan].begin &&
            physical < m_spans[m_lastSpan].end) return &m_spans[m_lastSpan];
        size_t lo = 0, hi = m_spanCount;
        while (lo < hi) {
            const size_t mid = lo + (hi - lo) / 2;
            if (m_spans[mid].begin <= physical) lo = mid + 1;
            else hi = mid;
        }
        if (!lo || physical >= m_spans[lo - 1].end) return nullptr;
        m_lastSpan = lo - 1;
        return &m_spans[m_lastSpan];
    }
    bool Writable(uint32_t physical, size_t bytes) {
        auto contains = [&](const Range& range) {
            return physical >= range.begin && physical < range.end && bytes <= range.end - physical;
        };
        if (m_lastWrite < m_writeCount && contains(m_writes[m_lastWrite])) return true;
        size_t lo = 0, hi = m_writeCount;
        while (lo < hi) {
            const size_t mid = lo + (hi - lo) / 2;
            if (m_writes[mid].begin <= physical) lo = mid + 1;
            else hi = mid;
        }
        if (!lo || !contains(m_writes[lo - 1])) return false;
        m_lastWrite = lo - 1;
        return true;
    }
    template <bool dirty>
    size_t NextDirtyBit(size_t offset, size_t end) const {
        // Skip eight clean/dirty guest bytes with one bitmap-byte load. Mixed
        // bytes retain exact partial-write runs, including unaligned span ends.
        while (offset < end) {
            const uint8_t mask = m_dirty[offset / 8];
            const unsigned bits = (dirty ? mask : uint8_t(~mask)) >> (offset % 8);
            if (bits) return std::min(end, offset + std::countr_zero(bits));
            offset = (offset | size_t(7)) + 1;
        }
        return end;
    }
    void MarkDirty(size_t offset, size_t bytes) {
        const size_t end = offset + bytes;
        while (offset < end && offset % 8) {
            m_dirty[offset / 8] |= uint8_t(1u << (offset % 8));
            ++offset;
        }
        const size_t wholeBytes = (end - offset) / 8;
        std::fill_n(m_dirty.data() + offset / 8, wholeBytes, uint8_t(0xff));
        offset += wholeBytes * 8;
        while (offset < end) {
            m_dirty[offset / 8] |= uint8_t(1u << (offset % 8));
            ++offset;
        }
    }
    std::array<Span, kMaxSpans> m_spans{};
    std::array<Range, kMaxSpans> m_writes{};
    std::array<uint8_t, kCapacity> m_data{};
    std::array<uint8_t, kCapacity / 8> m_dirty{};
    size_t m_spanCount = 0, m_writeCount = 0, m_bytes = 0;
    size_t m_lastSpan = 0, m_lastWrite = 0;
    uint32_t m_generation = 0;
    bool m_sealed = false;
};

} // namespace AxDspHle
