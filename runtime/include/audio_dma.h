#pragma once
#include <cstdint>
#include <vector>
struct CpuContext;
struct AudioDmaState {
    uint32_t address = 0, length = 0;
    bool enabled = false;
    uint64_t elapsedNs = 0;
    uint64_t Period(uint32_t rate) const { return uint64_t(length) * 1000000000 / (rate * 4); }
    bool Ready(uint32_t rate) const { return enabled && length && elapsedNs >= Period(rate); }
    uint32_t BytesLeft(uint32_t rate) const {
        if (!enabled || !length || Ready(rate))
            return 0;

        const auto consumed = elapsedNs * rate * 4 / 1000000000;
        return (length - uint32_t(consumed) + 31) & ~31u;
    }
};

void ServiceAudioDma(CpuContext* cpu);
std::vector<int16_t> DecodeWiiPcm(const uint8_t* data, uint32_t bytes);
void QueueHostPcm(const uint8_t* bigEndianRightLeft, uint32_t bytes, uint32_t rate);
