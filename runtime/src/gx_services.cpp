#include "abi_bridge.h"

extern "C" void func_804B67E0(CpuContext*);
#ifdef MH3_HAS_AURORA
void CompleteHostGraphics();
#endif

extern "C" void MH3_GXDrawDone(CpuContext* cpu) {
#ifdef MH3_HAS_AURORA
    const auto saved = *cpu;
    try {
        const auto gx = Memory::Read32(cpu->gpr[2] - 0x9B0);
        if (Memory::Read32(gx + 0x5FC)) InvokeIndirectCpu(0x804B6C70, cpu);
        CompleteHostGraphics();
        Memory::Write16(gx + 2, 0);
        Memory::Write8(saved.gpr[13] - 0x3C08, 1);
        cpu->msr &= ~0x8000u; // completion callback runs with external interrupts masked
        const auto callback = Memory::Read32(saved.gpr[13] - 0x3C04);
        if (callback) InvokeIndirectCpu(callback, cpu);
        cpu->gpr[3] = saved.gpr[13] - 0x3C10;
        InvokeIndirectCpu(0x804D51A0, cpu); // wake the SDK's draw-done wait queue
    } catch (...) { *cpu = saved; throw; }
    *cpu = saved;
#else
    func_804B67E0(cpu);
#endif
}
REGISTER_NATIVE_FUNCTION_AS(0x804B67E0, MH3_GXDrawDone, "MH3_GXDrawDone");
