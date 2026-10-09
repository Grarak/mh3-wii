#include "abi_bridge.h"
#include "audio_dma.h"
#include "audio_dma_statistics.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <stdexcept>

std::vector<int16_t> DecodeWiiPcm(const uint8_t* data, uint32_t bytes) {
    if (bytes % 4)
        throw std::runtime_error("Partial Wii stereo PCM frame");

    std::vector<int16_t> samples(bytes / 2);

    for (uint32_t i = 0; i < bytes; i += 4) {
        samples[i / 2] = int16_t(uint16_t(data[i + 2]) << 8 | data[i + 3]);
        samples[i / 2 + 1] = int16_t(uint16_t(data[i]) << 8 | data[i + 1]);
    }

    return samples;
}

namespace {
AudioDmaState dma;
using Clock = std::chrono::steady_clock;
auto previous = Clock::now();
bool servicing = false;
uint64_t completed = 0;
NativeAudioDmaStatistics statistics;
Clock::time_point previousEnabledService;
bool previousEnabledServiceValid = false;
uint32_t Rate() {
    return (Memory::Read32(0xCD006C00) & 0x40) ? 32000 : 48000;
}
} // namespace

NativeAudioDmaStatistics GetNativeAudioDmaStatistics() {
    auto result = statistics;
    result.enabled = dma.enabled;
    result.lengthBytes = dma.length;
    result.currentDebtNs = dma.elapsedNs;
    result.currentPeriodNs = result.rateHz ? dma.Period(result.rateHz) : 0;
    return result;
}

extern "C" void MH3_AIInitDMA(CpuContext* cpu) {
    const auto address = cpu->gpr[3] & 0x1FFFFFE0u;
    const auto length = cpu->gpr[4] & 0xFFFE0u;

    if (!length || !Memory::Contains(address, length))
        throw std::runtime_error("AI DMA buffer is empty or outside RAM");

    dma.address = address;
    dma.length = length;
}
REGISTER_NATIVE_FUNCTION(0x8046DAF0, MH3_AIInitDMA);
extern "C" void MH3_AIStartDMA(CpuContext*) {
    if (!dma.enabled) {
        previous = Clock::now();
        dma.elapsedNs = 0;
        previousEnabledService = previous;
        previousEnabledServiceValid = true;
    }

    dma.enabled = true;
}
REGISTER_NATIVE_FUNCTION(0x8046DB70, MH3_AIStartDMA);
extern "C" void MH3_AIStopDMA(CpuContext*) {
    dma.enabled = false;
    dma.elapsedNs = 0;
    previousEnabledServiceValid = false;
}
REGISTER_NATIVE_FUNCTION(0x8046DB90, MH3_AIStopDMA);
extern "C" void MH3_AIGetDMABytesLeft(CpuContext* cpu) {
    ServiceAudioDma(cpu);
    cpu->gpr[3] = dma.BytesLeft(Rate());
}
REGISTER_NATIVE_FUNCTION(0x8046DBB0, MH3_AIGetDMABytesLeft);
extern "C" void MH3_AIGetDMAStartAddr(CpuContext* cpu) {
    cpu->gpr[3] = dma.address;
}
REGISTER_NATIVE_FUNCTION(0x8046DBC0, MH3_AIGetDMAStartAddr);
extern "C" void MH3_AIGetDMALength(CpuContext* cpu) {
    cpu->gpr[3] = dma.length;
}
REGISTER_NATIVE_FUNCTION(0x8046DBE0, MH3_AIGetDMALength);

void ServiceAudioDma(CpuContext* cpu) {
    ++statistics.serviceCalls;
    const auto now = Clock::now();

    if (dma.enabled)
        dma.elapsedNs += std::chrono::duration_cast<std::chrono::nanoseconds>(now - previous).count();

    previous = now;

    if (servicing || !dma.enabled || !dma.length)
        return;

    ++statistics.enabledServiceCalls;

    if (previousEnabledServiceValid) {
        const auto gap = std::chrono::duration_cast<std::chrono::nanoseconds>(now - previousEnabledService).count();
        statistics.maxServiceGapNs = std::max(statistics.maxServiceGapNs, uint64_t(gap));
    }

    previousEnabledService = now;
    previousEnabledServiceValid = true;
    statistics.maxDebtNs = std::max(statistics.maxDebtNs, dma.elapsedNs);
    servicing = true;
    const auto saved = *cpu;
    struct Restore {
        CpuContext* cpu;
        CpuContext saved;
        ~Restore() {
            *cpu = saved;
            servicing = false;
        }
    } restore{cpu, saved};

    // Bounded catch-up preserves a callback between each completed audio block.
    // Remaining elapsed time is retained for the next cooperative service point.
    auto ready = [&] {
        statistics.rateHz = Rate();
        return dma.Ready(statistics.rateHz);
    };

    unsigned count = 0;

    for (; count < 16 && ready(); ++count) {
        const auto rate = Rate();
        const auto period = dma.Period(rate);
        dma.elapsedNs -= period;
        const auto frames = dma.length / 4;
#ifdef MH3_HAS_AURORA
        QueueHostPcm(Memory::GetPointer(dma.address, dma.length), dma.length, rate);
#else
        throw std::runtime_error("Audio DMA output requires the Aurora/SDL build");
#endif
        cpu->msr &= ~0x8000u;
        const auto busy = saved.gpr[13] - 0x3F94;
        const auto callback = Memory::Read32(saved.gpr[13] - 0x3F60);

        if (callback) {
            Memory::Write32(busy, 1);

            try {
                InvokeIndirectCpu(callback, cpu);
                Memory::Write32(busy, 0);
            } catch (...) {
                Memory::Write32(busy, 0);
                throw;
            }

            Memory::Write32(busy, 0);
        }

        *cpu = saved;
        ++statistics.completedBlocks;
        statistics.completedFrames += frames;
        statistics.completedDurationNs += period;

        if (++completed <= 3)
            std::fprintf(stderr, "MH3 audio DMA: block %llu completed\n", static_cast<unsigned long long>(completed));
    }

    if (count == 16 && ready())
        ++statistics.capHits;

    statistics.maxDebtNs = std::max(statistics.maxDebtNs, dma.elapsedNs);
}
