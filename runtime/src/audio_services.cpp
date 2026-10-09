#include "abi_bridge.h"
#include "audio/ax_dsp.h"
#include <iostream>
#include <stdexcept>

extern "C" void MH3_DSPInit(CpuContext*) {
    AxDspHle::Init();
}
REGISTER_NATIVE_FUNCTION(0x804A5600, MH3_DSPInit);
extern "C" void MH3_DSPAddTask(CpuContext* cpu) {
    cpu->gpr[3] = AxDspHle::AddTask(cpu->gpr[3]);
}
REGISTER_NATIVE_FUNCTION(0x804A56D0, MH3_DSPAddTask);
extern "C" void MH3_DSPAssertTask(CpuContext* cpu) {
    cpu->gpr[3] = AxDspHle::AssertTask(cpu->gpr[3]);
}
REGISTER_NATIVE_FUNCTION(0x804A5780, MH3_DSPAssertTask);
extern "C" void MH3_DSPSendMail(CpuContext* cpu) {
    AxDspHle::SendMailToDSP(cpu->gpr[3]);
}
REGISTER_NATIVE_FUNCTION(0x804A55E0, MH3_DSPSendMail);
extern "C" void MH3_DSPCheckMailTo(CpuContext* cpu) {
    cpu->gpr[3] = AxDspHle::CheckMailToDSP();
}
REGISTER_NATIVE_FUNCTION(0x804A55A0, MH3_DSPCheckMailTo);
extern "C" void MH3_DSPCheckMailFrom(CpuContext* cpu) {
    cpu->gpr[3] = AxDspHle::CheckMailFromDSP();
}
REGISTER_NATIVE_FUNCTION(0x804A55B0, MH3_DSPCheckMailFrom);
extern "C" void MH3_DSPReadMail(CpuContext* cpu) {
    cpu->gpr[3] = AxDspHle::ReadMailFromDSP();
}
REGISTER_NATIVE_FUNCTION(0x804A55C0, MH3_DSPReadMail);
extern "C" void MH3_AXOutInitDSP(CpuContext* cpu) {
    // This HLE supports MH3's known AX Wii program. Do not silently choose a
    // fallback layout for an unknown DSP program.
    const auto length = Memory::Read16(cpu->gpr[13] - 0x50FC);
    uint32_t hash = 0;
    const auto* code = Memory::GetPointer(0x80611240, length);
    for (uint32_t i = 0; i < length; ++i) {
        hash ^= code[i];
        hash = (hash << 3) | (hash >> 29);
    }
    if (length != 0x2000 || hash != 0x4CC52064)
        throw std::runtime_error("Unsupported MH3 AX DSP program");
    AxDspHle::InitForAXOut(cpu);
    std::cerr << "MH3 AX: initialized native mixer for DSP program 4cc52064\n";
}
REGISTER_NATIVE_FUNCTION(0x80470840, MH3_AXOutInitDSP);
