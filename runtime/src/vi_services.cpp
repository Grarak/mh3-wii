#include "abi_bridge.h"
#include "video_interface.h"
#include "ios_host.h"
#include "audio_dma.h"
#include "os_alarm.h"
#include "controller_host.h"
#include <chrono>
#include <thread>
#include <cstdio>

#ifdef MH3_HAS_AURORA
void ServiceHostGraphicsEvents();
#endif

namespace {
using Clock = std::chrono::steady_clock;
auto nextRetrace = Clock::now();
}

void ServiceVideoRetrace(CpuContext* cpu) {
    ServiceHostController(cpu);
    const auto now = Clock::now();
    if (now < nextRetrace) return;
    nextRetrace = now + std::chrono::nanoseconds(VideoRetracePeriodNs());
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
}
extern "C" void MH3_VIWaitForRetrace(CpuContext* cpu) {
    if (nextRetrace <= Clock::now())
        nextRetrace = Clock::now() + std::chrono::nanoseconds(VideoRetracePeriodNs());
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
