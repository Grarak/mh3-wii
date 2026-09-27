#include "controller_host.h"
#include "abi_bridge.h"
#include "ios_host.h"
#include <cstring>
#include <stdexcept>
extern "C" void MH3_WPADProbe(CpuContext*);
extern "C" void MH3_WPADSetDataFormat(CpuContext*);
extern "C" void MH3_WPADRead(CpuContext*);
extern "C" void MH3_WPADGetInfoAsync(CpuContext*);
extern "C" void MH3_WPADControlDpd(CpuContext*);

void RunControllerSelfTests(CpuContext* cpu) {
    const auto saved = *cpu;
    constexpr uint32_t table = 0x8075D790, block = 0x80012000;
    constexpr uint32_t buffer = 0x80014000, output = 0x80015000;
    const auto oldBlock = Memory::Read32(table);
    cpu->gpr[13] = 0x8079B2E0;
    const auto initialized = cpu->gpr[13] - 0x3700;
    const auto oldInitialized = Memory::Read32(initialized);
    Memory::Write32(initialized, 1);
    Memory::Write32(table, block);
    std::memset(Memory::GetPointer(block, 0x990), 0, 0x990);
    ClassicControllerSample sample{true, 0x8010, -512, 511, -33, 127, 17, 201};
    SetControllerTestSample(sample);
    cpu->gpr[3] = 0; cpu->gpr[4] = output;
    MH3_WPADProbe(cpu);
    if (cpu->gpr[3] || Memory::Read32(output) != 2) throw std::runtime_error("Classic probe failed");
    cpu->gpr[3] = 0; cpu->gpr[4] = 8;
    MH3_WPADSetDataFormat(cpu);
    if (cpu->gpr[3] || Memory::Read32(block + 0x8B8) != 8) throw std::runtime_error("Classic format failed");
    std::memset(Memory::GetPointer(output, 0x40), 0xCC, 0x40);
    cpu->gpr[3] = 0; cpu->gpr[4] = output;
    MH3_WPADRead(cpu);
    if (Memory::Read8(output + 0x28) != 2 || Memory::Read8(output + 0x29) ||
        Memory::Read16(output + 0x2A) != 0x8010 || Memory::Read16(output + 0x2C) != uint16_t(-512) ||
        Memory::Read16(output + 0x2E) != 511 || Memory::Read16(output + 0x30) != uint16_t(-33) ||
        Memory::Read16(output + 0x32) != 127 || Memory::Read8(output + 0x34) != 17 ||
        Memory::Read8(output + 0x35) != 201 || Memory::Read8(output + 0x36) != 0xCC)
        throw std::runtime_error("Classic raw report layout/bounds failed");
    cpu->gpr[3] = 0; cpu->gpr[4] = 5;
    MH3_WPADSetDataFormat(cpu);
    if (int32_t(cpu->gpr[3]) != -4 || Memory::Read32(block + 0x8B8) != 8)
        throw std::runtime_error("Classic accepted incompatible format");
    Memory::Write32(block + 0x8AC, buffer); Memory::Write32(block + 0x8B4, 3);
    Memory::Write32(block + 0x8B0, 0xFFFFFFFF);
    ServiceHostController(cpu, true);
    sample.buttons = 0; SetControllerTestSample(sample); ServiceHostController(cpu, true);
    sample.buttons = 0x40; SetControllerTestSample(sample); ServiceHostController(cpu, true);
    if (Memory::Read32(block + 0x8B0) != 2 || Memory::Read16(buffer + 0x2A) != 0x8010 ||
        Memory::Read16(buffer + 0x36 + 0x2A) || Memory::Read16(buffer + 0x6C + 0x2A) != 0x40)
        throw std::runtime_error("WPAD ring did not preserve press/release samples");
    ServiceHostController(cpu, true);
    if (Memory::Read32(block + 0x8B0) || Memory::Read16(buffer + 0x2A) != 0x40)
        throw std::runtime_error("WPAD ring wrap failed");
    Memory::Write32(block + 0x8A8, 0x800010F0);
    ServiceHostController(cpu, true);
    const auto sampling = TakeIosCompletion();
    if (!sampling || sampling->callback != 0x800010F0 || sampling->result != 0)
        throw std::runtime_error("WPAD sampling callback not queued");
    Memory::Write32(block + 0x8A8, 0);
    cpu->gpr[3] = 0; cpu->gpr[4] = output; cpu->gpr[5] = 0x800010F0;
    MH3_WPADGetInfoAsync(cpu);
    const auto info = TakeIosCompletion();
    if (cpu->gpr[3] || !info || info->result || info->argument ||
        Memory::Read32(output + 8) != 1 || Memory::Read8(output + 0x14) != 4)
        throw std::runtime_error("WPAD info/completion failed");
    cpu->gpr[3] = 0; cpu->gpr[4] = 1; cpu->gpr[5] = 0x800010F0;
    MH3_WPADControlDpd(cpu);
    const auto unsupported = TakeIosCompletion();
    if (int32_t(cpu->gpr[3]) != -4 || !unsupported || int32_t(unsupported->argument) != -4)
        throw std::runtime_error("WPAD accepted unsupported IR enable");
    sample.connected = false; SetControllerTestSample(sample);
    ServiceHostController(cpu, true);
    if (Memory::Read8(buffer + 2 * 0x36 + 0x29) != 0xFF)
        throw std::runtime_error("WPAD ring disconnect status failed");
    cpu->gpr[3] = 0; cpu->gpr[4] = output;
    MH3_WPADProbe(cpu);
    if (int32_t(cpu->gpr[3]) != -1 || Memory::Read32(output) != 0xFD)
        throw std::runtime_error("WPAD disconnect probe failed");
    cpu->gpr[3] = 4; cpu->gpr[4] = 0;
    MH3_WPADProbe(cpu);
    if (int32_t(cpu->gpr[3]) != -4) throw std::runtime_error("WPAD accepted bad channel");
    if (TakeIosCompletion()) throw std::runtime_error("Unexpected controller completion");
    SetControllerTestSample(std::nullopt);
    Memory::Write32(table, oldBlock); Memory::Write32(initialized, oldInitialized);
    *cpu = saved;
}
