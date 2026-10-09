#include "abi_bridge.h"
#include "audio_dma.h"
#include "audio/native_audio.h"
#include "os_alarm.h"
#include "ios_host.h"
#include "timebase_contract.h"
#include "video_interface.h"
#include "scope_exit.h"
#include "self_test.h"
#include <algorithm>
#include <chrono>
#include <thread>

#ifdef MH3_HAS_AURORA
void ServiceHostGraphicsEvents();
#endif

// OSSleepTicks, RMHP08. Host timing replaces its stack-allocated hardware
// alarm. Keep the native call stack alive while delivering device callbacks.
// Switching to another runnable guest thread requires a fiber scheduler.
namespace {
template <class Pump>
void SleepTicks(CpuContext* cpu, Pump pump) {
    using Clock = std::chrono::steady_clock;
    const auto saved = *cpu;
    const auto ticks = (uint64_t(cpu->gpr[3]) << 32) | cpu->gpr[4];
    const auto duration = TimeBaseContract::TicksToDuration(ticks);

    if (ticks && duration == std::chrono::nanoseconds::zero())
        throw std::runtime_error("OSSleepTicks duration exceeds host clock range");

    const auto start = Clock::now();

    if (duration > Clock::time_point::max() - start)
        throw std::runtime_error("OSSleepTicks deadline exceeds host clock range");

    const auto deadline = start + duration;
    ScopeExit restore([&] { *cpu = saved; });

    do {
        if (Memory::Read32(saved.gpr[13] - 14984))
            throw std::runtime_error("OSSleepTicks requires runnable guest thread switching");

        pump();
        *cpu = saved;

        if (Clock::now() >= deadline)
            break;

        std::this_thread::sleep_until(std::min(deadline, Clock::now() + std::chrono::milliseconds(1)));
    } while (true);
}
} // namespace

// Delivers pending device work: host window events, IOS completions, audio
// DMA, OS alarms and VI retrace callbacks.
void PumpDevices(CpuContext* cpu) {
#ifdef MH3_HAS_AURORA
    ServiceHostGraphicsEvents();
#endif
    PumpIosCompletions(cpu);
    ServiceAudioDma(cpu);
    NativeAudio::ServiceNativeAudio(cpu);
    ServiceOsAlarms(cpu);
    ServiceVideoRetrace(cpu);
}

extern "C" void MH3_OSSleepTicks(CpuContext* cpu) {
    SleepTicks(cpu, [&] { PumpDevices(cpu); });
}
REGISTER_NATIVE_FUNCTION(0x804D5320, MH3_OSSleepTicks);

void RunSleepSelfTests(CpuContext* cpu) {
    const auto original = *cpu;
    const auto pending = cpu->gpr[13] - 14984;
    const auto oldPending = Memory::Read32(pending);
    Memory::Write32(pending, 0);
    cpu->gpr[3] = 0;
    cpu->gpr[4] = TimeBaseContract::NanosecondsToTicks(5'000'000);
    cpu->gpr[14] = 0x1234;
    cpu->lr = 0x5678;
    unsigned calls = 0;
    const auto start = std::chrono::steady_clock::now();
    SleepTicks(cpu, [&] {
        ++calls;
        cpu->gpr[14] = 0;
        cpu->lr = 0;
    });

    if (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(5) || calls < 2 || cpu->gpr[14] != 0x1234 || cpu->lr != 0x5678)
        throw std::runtime_error("Sleep timing/pumping/CPU preservation failed");

    const auto failing = [&] {
        SleepTicks(cpu, [&] {
            cpu->lr = 0;
            throw std::runtime_error("test callback failure");
        });
    };

    if (CheckRejections && (!Rejects(failing) || cpu->lr != 0x5678))
        throw std::runtime_error("Sleep exception restoration failed");

    Memory::Write32(pending, 1);

    if (CheckRejections && !Rejects([&] { SleepTicks(cpu, [&] { ++calls; }); }))
        throw std::runtime_error("Sleep skipped a runnable thread");

    Memory::Write32(pending, oldPending);
    *cpu = original;
}
