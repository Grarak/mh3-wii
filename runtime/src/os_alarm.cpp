#include "abi_bridge.h"
#include "os_alarm.h"
#include "scope_exit.h"
#include <cstdio>
#include <cstring>

// The original SDK still owns alarm creation, insertion, cancellation and
// periodic deadline calculation. Replace only decrementer interrupt delivery.
void ServiceOsAlarms(CpuContext* cpu) {
    static bool servicing = false;
    static unsigned delivered = 0;

    if (servicing)
        return;

    const auto saved = *cpu;
    const auto queue = saved.gpr[13] - 0x3B10;
    servicing = true;
    ScopeExit finish([&] {
        *cpu = saved;
        servicing = false;
    });

    for (unsigned count = 0; count < 64; ++count) {
        const auto alarm = Memory::Read32(queue);

        if (!alarm)
            break;

        if (!Memory::Contains(alarm, 0x30))
            throw std::runtime_error("Invalid OS alarm queue entry");

        InvokeIndirectCpu(0x804D5400, cpu); // __OSGetSystemTime, same clock as InsertAlarm
        const uint64_t now = (uint64_t(cpu->gpr[3]) << 32) | cpu->gpr[4];
        const uint64_t due = Memory::Read64(alarm + 8);
        *cpu = saved;

        if (static_cast<int64_t>(now) < static_cast<int64_t>(due))
            break;

        const auto next = Memory::Read32(alarm + 0x14);
        const auto callback = Memory::Read32(alarm);

        if (!callback)
            throw std::runtime_error("Queued OS alarm has no callback");

        Memory::Write32(queue, next);

        if (next)
            Memory::Write32(next + 0x10, 0);
        else
            Memory::Write32(queue + 4, 0);

        Memory::Write32(alarm, 0);

        if (static_cast<int64_t>(Memory::Read64(alarm + 0x18)) > 0) {
            cpu->gpr[3] = alarm;
            cpu->gpr[5] = cpu->gpr[6] = 0;
            cpu->gpr[7] = callback;
            InvokeIndirectCpu(0x804CBB30, cpu); // InsertAlarm chooses the next future period
            *cpu = saved;
        }

        const auto oldContext = Memory::Read32(0x800000D4);
        const auto oldPhysical = Memory::Read32(0x800000C0);
        const auto scheduler = saved.gpr[13] - 14992;
        const auto disabled = Memory::Read32(scheduler);
        const auto stack = (saved.gpr[1] - 0x300) & ~15u;
        const auto temporaryContext = stack + 16;
        std::memset(Memory::GetPointer(temporaryContext, 0x2C8), 0, 0x2C8);
        Memory::Write32(stack, saved.gpr[1]);
        Memory::Write32(scheduler, disabled + 1);
        Memory::Write32(0x800000D4, temporaryContext);
        Memory::Write32(0x800000C0, temporaryContext & 0x3FFFFFFF);
        auto restore = [&] {
            Memory::Write32(scheduler, disabled);
            Memory::Write32(0x800000D4, oldContext);
            Memory::Write32(0x800000C0, oldPhysical);
            *cpu = saved;
        };

        {
            ScopeExit restoring(restore);
            cpu->gpr[1] = stack;
            cpu->msr &= ~0x8000u;
            cpu->gpr[3] = alarm;
            cpu->gpr[4] = oldContext;
            InvokeIndirectCpu(callback, cpu);
        }

        if (++delivered <= 3)
            std::fprintf(stderr, "MH3 OS alarm: callback %08x delivered\n", callback);
    }
}
