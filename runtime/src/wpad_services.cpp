#include "abi_bridge.h"
#include "controller_host.h"
#include "village_boot.h"
#include "ios_host.h"
#include <array>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <iostream>

// WUDInit: the native controller bridge owns discovery rather than the Wii USB
// Bluetooth stack. Preserve the SDK state queried by WPADGetStatus and setters.
// WPAD's original initialization still creates all four channel control blocks.
extern "C" void MH3_WUDInit(CpuContext* cpu) {
    const auto initialized = cpu->gpr[13] - 0x3700;

    if (Memory::Read32(initialized)) {
        cpu->gpr[3] = 0;
        return;
    }

    constexpr uint32_t state = 0x80760F60;

    for (uint32_t offset : {0u, 4u, 8u, 0x6ECu, 0x6F0u})
        Memory::Write32(state + offset, 0);

    Memory::Write8(state + 0x10, 1);
    Memory::Write8(state + 0x708, 3); // WUD library ready, no connected remotes yet
    Memory::Write8(cpu->gpr[13] - 0x36CC, 0);
    Memory::Write32(initialized, 1);
    cpu->gpr[3] = 1;
    std::cerr << "MH3 WPAD: host controller transport initialized\n";
}
REGISTER_NATIVE_FUNCTION(0x804FD450, MH3_WUDInit);

// The RMHP08 SDK still owns callback registration and sampling-buffer metadata.
// Host input replaces Bluetooth report delivery, probe and control transactions.
namespace {
constexpr uint32_t channelTable = 0x8075D790;
uint32_t ControlBlock(uint32_t channel) {
    if (channel >= 4)
        return 0;

    const auto block = Memory::Read32(channelTable + channel * 4);
    return block && Memory::Contains(block, 0x990) ? block : 0;
}

int32_t ControllerStatus(uint32_t channel, const ClassicControllerSample& sample) {
    if (channel >= 4)
        return -4;

    return channel == 0 && sample.connected ? 0 : -1;
}

std::array<uint8_t, 0x36> Report(const ClassicControllerSample& sample, int32_t status) {
    std::array<uint8_t, 0x36> bytes{};
    const auto word = [&](size_t offset, uint16_t value) {
        bytes[offset] = uint8_t(value >> 8);
        bytes[offset + 1] = uint8_t(value);
    };

    bytes[0x28] = status ? 0xFD : 2;
    bytes[0x29] = uint8_t(status);

    if (status)
        return bytes;

    word(2, 512);
    word(4, 512);
    word(6, 612); // stationary remote, one g on Z
    for (unsigned i = 0; i < 4; ++i) {
        word(8 + i * 8, 0x3FF);
        word(10 + i * 8, 0x3FF);
    }

    word(0x2A, sample.buttons);
    word(0x2C, sample.leftX);
    word(0x2E, sample.leftY);
    word(0x30, sample.rightX);
    word(0x32, sample.rightY);
    bytes[0x34] = sample.triggerL;
    bytes[0x35] = sample.triggerR;
    return bytes;
}

uint32_t ReportSize(uint32_t format) {
    if (format <= 2)
        return 0x2A;

    if (format >= 6 && format <= 8)
        return 0x36;

    throw std::runtime_error("Host Classic Controller encountered unsupported WPAD format");
}

void CopyReport(uint32_t address, const std::array<uint8_t, 0x36>& report, uint32_t size) {
    auto* destination = Memory::GetPointer(address, size);
    RecompMod::CheckExecutableWrite(address, size, 0);
    std::memcpy(destination, report.data(), size);
}

void CompleteController(uint32_t callback, uint32_t channel, int32_t result) {
    // The common deferred callback pump supplies r3/r4 and preserves CPU state.
    QueueIosCompletion({callback, static_cast<int32_t>(channel), static_cast<uint32_t>(result)});
}
} // namespace

extern "C" void MH3_WPADProbe(CpuContext* cpu) {
    const auto channel = cpu->gpr[3];
    const auto sample = ReadGameController();
    const auto status = ControllerStatus(channel, sample);

    if (cpu->gpr[4] && channel < 4)
        Memory::Write32(cpu->gpr[4], status ? 0xFD : 2);

    cpu->gpr[3] = status;
}
REGISTER_NATIVE_FUNCTION(0x804EF040, MH3_WPADProbe);

extern "C" void MH3_WPADSetDataFormat(CpuContext* cpu) {
    const auto channel = cpu->gpr[3], format = cpu->gpr[4];
    auto status = ControllerStatus(channel, ReadGameController());
    const auto block = ControlBlock(channel);

    if (!status && !block)
        status = -2;

    if (!status && !(format <= 2 || (format >= 6 && format <= 8)))
        status = -4;

    if (!status) {
        Memory::Write32(block + 0x8B8, format);

        if (std::getenv("MH3_TRACE_ERRORS"))
            std::cerr << "MH3 WPAD: channel " << channel << " format " << format << '\n';
    }

    cpu->gpr[3] = status;
}
REGISTER_NATIVE_FUNCTION(0x804EF2A0, MH3_WPADSetDataFormat);

extern "C" void MH3_WPADRead(CpuContext* cpu) {
    const auto channel = cpu->gpr[3];
    const auto sample = ReadGameController();
    const auto status = ControllerStatus(channel, sample);
    const auto block = ControlBlock(channel);
    const auto size = status || !block ? 0x2Au : ReportSize(Memory::Read32(block + 0x8B8));
    CopyReport(cpu->gpr[4], Report(sample, status), size);
}
REGISTER_NATIVE_FUNCTION(0x804EF9D0, MH3_WPADRead);

extern "C" void MH3_WPADGetInfoAsync(CpuContext* cpu) {
    const auto channel = cpu->gpr[3], output = cpu->gpr[4], callback = cpu->gpr[5];
    auto status = ControllerStatus(channel, ReadGameController());

    if (!status && !output)
        status = -4;

    if (!status) {
        for (uint32_t offset = 0; offset < 0x18; offset += 4)
            Memory::Write32(output + offset, 0);

        Memory::Write32(output + 8, 1);   // Classic extension attached
        Memory::Write8(output + 0x14, 4); // host input has no Wii Remote battery
        Memory::Write8(output + 0x15, 1);
    }

    CompleteController(callback, channel, status);
    cpu->gpr[3] = status;
}
REGISTER_NATIVE_FUNCTION(0x804EF3C0, MH3_WPADGetInfoAsync);

// Classic input has no IR camera or remote speaker. Disable requests succeed;
// enabling hardware that the host bridge does not supply returns invalid.
extern "C" void MH3_WPADControlDpd(CpuContext* cpu) {
    const auto channel = cpu->gpr[3];
    auto status = ControllerStatus(channel, ReadGameController());

    if (!status && cpu->gpr[4])
        status = -4;

    CompleteController(cpu->gpr[5], channel, status);
    cpu->gpr[3] = status;
}
REGISTER_NATIVE_FUNCTION(0x804F1B20, MH3_WPADControlDpd);
extern "C" void MH3_WPADControlSpeaker(CpuContext* cpu) {
    MH3_WPADControlDpd(cpu);
}
REGISTER_NATIVE_FUNCTION(0x804EFF80, MH3_WPADControlSpeaker);

void ServiceHostController(CpuContext* cpu, bool force) {
    using Clock = std::chrono::steady_clock;
    static auto next = Clock::time_point::min();
    static bool wasConnected = false;

    if (!Memory::Contains(cpu->gpr[13] - 0x3700, 4) || !Memory::Read32(cpu->gpr[13] - 0x3700))
        return;

    const auto now = Clock::now();

    if (!force && now < next)
        return;

    next = now + std::chrono::milliseconds(5);
    const auto block = ControlBlock(0);

    if (!block)
        return;

    const auto sample = ReadGameController();

    if (sample.connected != wasConnected) {
        CompleteController(Memory::Read32(block + 0x8A4), 0, sample.connected ? 0 : -1);

        if (sample.connected)
            CompleteController(Memory::Read32(block + 0x8A0), 0, 2);

        wasConnected = sample.connected;
    }

    const auto report = Report(sample, sample.connected ? 0 : -1);
    const auto size = ReportSize(Memory::Read32(block + 0x8B8));
    const auto buffer = Memory::Read32(block + 0x8AC), count = Memory::Read32(block + 0x8B4);

    if (buffer && count) {
        if (count > 0x10000 || !Memory::Contains(buffer, uint64_t(count) * size))
            throw std::runtime_error("Invalid WPAD auto-sampling buffer");

        const auto previous = Memory::Read32(block + 0x8B0);

        if (previous != 0xFFFFFFFF && previous >= count)
            throw std::runtime_error("Invalid WPAD sampling index");

        const auto index = previous == 0xFFFFFFFF || previous + 1 == count ? 0 : previous + 1;
        CopyReport(buffer + index * size, report, size);
        Memory::Write32(block + 0x8B0, index);
    }

    if (sample.connected)
        CompleteController(Memory::Read32(block + 0x8A8), 0, 0);
}
