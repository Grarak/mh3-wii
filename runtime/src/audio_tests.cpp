#include "abi_bridge.h"
#include "audio/ax_dsp.h"
#include "audio/ax_internal.h"
#include "audio_dma.h"
#include <array>
#include <iostream>
#include <stdexcept>

extern "C" void MH3_AXOutInitDSP(CpuContext*);
void RunAudioSelfTests(CpuContext* cpu) {
    AudioDmaState timing;
    timing.enabled = true;
    timing.length = 384;
    if (timing.Period(32000) != 3000000 || timing.Period(48000) != 2000000)
        throw std::runtime_error("AI DMA block durations failed");
    timing.elapsedNs = 2000000;
    if (timing.Ready(32000) || timing.BytesLeft(32000) != 128 || !timing.Ready(48000))
        throw std::runtime_error("AI DMA partial block timing failed");
    timing.elapsedNs = 6500000;
    unsigned blocks = 0;
    while (timing.Ready(32000)) { timing.elapsedNs -= timing.Period(32000); ++blocks; }
    if (blocks != 2 || timing.BytesLeft(32000) != 320)
        throw std::runtime_error("AI DMA carry between blocks failed");
    timing.enabled = false;
    if (timing.Ready(32000) || timing.BytesLeft(32000))
        throw std::runtime_error("Stopped AI DMA remained active");
    cpu->gpr[13] = 0x8079B2E0;
    MH3_AXOutInitDSP(cpu); // validates the actual DOL's DSP program and task binding
    if (Memory::Read32(0x8079B2E0 - 0x3EB0) != 1 ||
        Memory::Read32(0x806FDA60 + 0x28) != 0x804707B0 ||
        !AxDspHle::MixResolveRange(0x90000000, 64 * 1024 * 1024) ||
        AxDspHle::MixResolveRange(0x94000000, 1))
        throw std::runtime_error("MH3 AX task/MEM2 binding failed");
    if (!AxDspHle::CheckMailFromDSP() || AxDspHle::ReadMailFromDSP() != 0xDCD10000)
        throw std::runtime_error("AX startup mail missing");
    // A real command list through the mailbox: distinct L/R ramps exercise
    // sample generation, endian conversion, MEM2 output and Wii's R/L ordering.
    constexpr uint32_t setup = 0x90010000, output = 0x90011000;
    constexpr uint32_t surround = 0x90012000, commands = 0x90013000;
    for (uint32_t i = 0; i < 120; ++i) Memory::Write8(setup + i, 0);
    Memory::Write32(setup, 1000);
    Memory::Write16(setup + 4, 2);
    Memory::Write32(setup + 6, uint32_t(-2000));
    Memory::Write16(setup + 10, 3);
    const std::array<uint16_t, 10> words = {
        0, uint16_t(setup >> 16), uint16_t(setup),
        0xB, 0x8000, uint16_t(surround >> 16), uint16_t(surround),
        uint16_t(output >> 16), uint16_t(output), 0xE};
    for (size_t i = 0; i < words.size(); ++i) Memory::Write16(commands + i * 2, words[i]);
    AxDspHle::SendMailToDSP(0xBABE0000 | words.size());
    AxDspHle::SendMailToDSP(commands);
    AxDspHle::JoinMixWorker();
    const auto hostPcm = DecodeWiiPcm(Memory::GetPointer(output, 384), 384);
    for (int i = 0; i < 96; ++i) {
        if (hostPcm[i * 2] != 1000 + i * 2 || hostPcm[i * 2 + 1] != -2000 + i * 3 ||
            int16_t(Memory::Read16(output + i * 4)) != -2000 + i * 3 ||
            int16_t(Memory::Read16(output + i * 4 + 2)) != 1000 + i * 2 ||
            Memory::Read32(surround + i * 4) != 0)
            throw std::runtime_error("AX native command-list PCM output mismatch");
    }
    if (AxDspHle::ReadMailFromDSP() != 0xDCD10004 ||
        AxDspHle::ReadMailFromDSP() != 0xDCD10002 || AxDspHle::CheckMailFromDSP())
        throw std::runtime_error("AX command-list completion mail missing");
    AxDspHle::Stop();
    std::cout << "MH3 AX self-test passed: 96 stereo frames mixed through DSP mailbox\n";
}
