#include "abi_bridge.h"
#include "video_interface.h"
#include "ios_host.h"
#include "audio_dma.h"
#include "os_alarm.h"
#include "controller_host.h"
#include <chrono>
#include <thread>
#include <cstdio>
#include <stdexcept>

#ifdef MH3_HAS_AURORA
void ServiceHostGraphicsEvents();
void ReportHostSpeed(uint64_t retraces, double nominalHz);
#endif

namespace {
using Clock = std::chrono::steady_clock;
auto nextRetrace = Clock::now();
Clock::time_point AdvanceRetraceDeadline(Clock::time_point deadline,
                                         Clock::time_point now, Clock::duration period) {
    if (now < deadline) return deadline;
    return deadline + period * ((now - deadline) / period + 1);
}
}

void ServiceVideoRetrace(CpuContext* cpu) {
    ServiceHostController(cpu);
    const auto now = Clock::now();
    if (now < nextRetrace) return;
    // Keep the VI phase fixed. The old "now + period" reset inserted a fresh
    // full retrace delay after every slow CPU frame, worsening both FPS and
    // pacing; missed retraces should be skipped, not accumulated or restarted.
    nextRetrace = AdvanceRetraceDeadline(nextRetrace, now,
        std::chrono::nanoseconds(VideoRetracePeriodNs()));
    const auto saved = *cpu;
    const uint32_t before = Memory::Read32(saved.gpr[13] - 0x379C);
    BeginVideoRetrace();
    try {
        cpu->msr &= ~0x8000u;
        cpu->gpr[3] = 24;
        cpu->gpr[4] = Memory::Read32(0x800000D4);
        if (!cpu->gpr[4] || !Memory::Contains(cpu->gpr[4], 0x2C8))
            throw std::runtime_error("VI retrace has no valid guest OS context");
        InvokeIndirectCpu(0x804E6F30, cpu);
    } catch (...) { EndVideoRetrace(); *cpu = saved; throw; }
    EndVideoRetrace();
    *cpu = saved;
    const uint32_t after = Memory::Read32(saved.gpr[13] - 0x379C);
    if (after != before + 1) throw std::runtime_error("VI handler did not deliver a retrace");
    if (after <= 3) std::fprintf(stderr, "MH3 VI retrace delivered: %u\n", after);
#ifdef MH3_HAS_AURORA
    ReportHostSpeed(after, 1e9 / VideoRetracePeriodNs());
#endif
}
extern "C" void MH3_VIWaitForRetrace(CpuContext* cpu) {
    std::this_thread::sleep_until(nextRetrace);
#ifdef MH3_HAS_AURORA
    ServiceHostGraphicsEvents();
#endif
    PumpIosCompletions(cpu);
    ServiceAudioDma(cpu);
    ServiceOsAlarms(cpu);
    ServiceVideoRetrace(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x804E8050, MH3_VIWaitForRetrace, "MH3_VIWaitForRetrace");

void RunVideoTimingSelfTests() {
    const auto start = Clock::time_point{};
    const auto period = std::chrono::milliseconds(20);
    const auto deadline = start + period;
    if (AdvanceRetraceDeadline(deadline, start + std::chrono::milliseconds(19), period) != deadline ||
        AdvanceRetraceDeadline(deadline, deadline, period) != start + period * 2 ||
        AdvanceRetraceDeadline(deadline, start + std::chrono::milliseconds(105), period) != start + period * 6)
        throw std::runtime_error("VI deadline phase/catch-up regression");
}
