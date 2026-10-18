#include "abi_bridge.h"
#include "fatal.h"
#include "controller_host.h"
#include "village_boot.h"
#include "ios_host.h"
#include <array>
#include <chrono>
#include <cstring>
#include <cmath>
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

// The Wii Remote itself, under the Classic Controller: the touch screen (the
// mouse) is its pointer, with B and A shaped for the game's pointing in the
// village and the field (log_pointer.cpp). The original holds B to show the
// cursor, then presses A over a monster (a hunter online) and drags it with A
// held onto the log on the HUD. Here a finger down holds B, and A goes down
// once the game reports the cursor over a target (state 2) and stays down
// until the finger lifts: touch a monster, drag it to the log, let go. B
// outlasts the finger by a moment, as the drop is only seen while B is held.
constexpr uint16_t kRemoteB = 0x0400, kRemoteA = 0x0800;
constexpr uint32_t kPointingState = 0x806BFE89; // u8: 2 over a target, 4 dragging one
constexpr auto kDropHold = std::chrono::milliseconds(150);

struct RemotePointer {
    uint16_t buttons = 0;
    bool aiming = false; // the dots are in view
    float x = 0, y = 0;  // KPAD's pos, -1..1
};

RemotePointer Pointer(const ClassicControllerSample& sample) {
    using Clock = std::chrono::steady_clock;
    static bool touching = false, grabbing = false;
    static Clock::time_point lifted{};
    static float x = 0, y = 0;
    const auto now = Clock::now();

    if (sample.pointing) {
        touching = true;
        x = sample.pointX * 2.0f - 1.0f;
        y = sample.pointY * 2.0f - 1.0f;
    } else if (touching) {
        touching = false;
        lifted = now;
    }

    grabbing = touching && (grabbing || Memory::Read8(kPointingState) == 2);
    RemotePointer pointer;
    pointer.aiming = touching || (lifted != Clock::time_point{} && now - lifted < kDropHold);
    pointer.buttons = (pointer.aiming ? kRemoteB : 0) | (grabbing ? kRemoteA : 0);
    pointer.x = x;
    pointer.y = y;
    return pointer;
}

// The IR camera's view of a sensor bar for KPAD's pos (x, y), through the
// game's copy of KPAD at 0x8065D310 (pad_pointer.cpp), inverted: two dots
// level, as far apart as a remote half way into the distances it accepts.
bool SensorBarDots(float x, float y, int16_t dots[2][2]) {
    constexpr uint32_t kp = 0x8065D310;
    const auto f = [](uint32_t a) {
        return Memory::ReadFloat32(a);
    };

    const float rx = f(kp + 0xD4), ry = f(kp + 0xD8); // screen rotation
    const float sx = f(kp + 0xDC), sy = f(kp + 0xE0); // sensor rotation
    const float scale = f(kp + 0xEC);
    const float distance = (f(kp + 0x1A4) + f(0x80793378)) * 0.5f; // between the nearest and farthest

    if (!(scale > 0.0f) || !(distance > 0.0f))
        return false; // before KpadInit

    const float bx = -ry * x - rx * y, by = rx * x - ry * y;
    const float u = f(kp + 0xE4) - bx / scale, v = f(kp + 0xE8) - by / scale;
    const float mx = sx * u + sy * v, my = -sy * u + sx * v;

    // Camera units: x 0..1023, y 0..767; KPAD's -1..1 is 512 of them.
    const int32_t apart = int32_t(std::lround(f(kp + 0x1A0) / distance * 512.0f));
    const int32_t left = int32_t(std::lround((mx + 1023.0f / 1024.0f) * 512.0f - apart * 0.5f));
    const int32_t row = int32_t(std::lround((my + 767.0f / 1024.0f) * 512.0f));
    dots[0][0] = int16_t(left);
    dots[1][0] = int16_t(left + apart);
    dots[0][1] = dots[1][1] = int16_t(row);
    return true;
}

std::array<uint8_t, 0x36> Report(const ClassicControllerSample& sample, int32_t status, bool irOn) {
    std::array<uint8_t, 0x36> bytes{};
    const auto word = [&](size_t offset, uint16_t value) {
        bytes[offset] = uint8_t(value >> 8);
        bytes[offset + 1] = uint8_t(value);
    };

    bytes[0x28] = status ? 0xFD : 2;
    bytes[0x29] = uint8_t(status);

    if (status)
        return bytes;

    const RemotePointer pointer = Pointer(sample);
    word(0, pointer.buttons);
    word(2, 512);
    word(4, 512);
    word(6, 612); // stationary remote, one g on Z
    for (unsigned i = 0; i < 4; ++i) {
        word(8 + i * 8, 0x3FF);
        word(10 + i * 8, 0x3FF);
    }

    if (int16_t dots[2][2]; irOn && pointer.aiming && SensorBarDots(pointer.x, pointer.y, dots)) {
        for (unsigned i = 0; i < 2; ++i) {
            word(8 + i * 8, uint16_t(dots[i][0]));
            word(10 + i * 8, uint16_t(dots[i][1]));
            word(12 + i * 8, 2); // the dot's size: seen
        }
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

    MH3_THROW(std::runtime_error("Host Classic Controller encountered unsupported WPAD format"));
}

void CopyReport(uint32_t address, const std::array<uint8_t, 0x36>& report, uint32_t size) {
    auto* destination = Memory::GetPointer(address, size);
    RecompMod::CheckExecutableWrite(address, size, 0);
    std::memcpy(destination, report.data(), size);
}

// WPADIsDpdEnabled's flag (the remote's status report sets it).
bool IrOn(uint32_t block) {
    return Memory::Read32(block + 0x838) != 0;
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
    CopyReport(cpu->gpr[4], Report(sample, status, block && IrOn(block)), size);
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

// The IR camera (the pointer, from the touch screen or mouse) switches on and
// off. There is no remote speaker: disabling it succeeds, enabling returns
// invalid.
extern "C" void MH3_WPADControlDpd(CpuContext* cpu) {
    const auto channel = cpu->gpr[3];
    const auto status = ControllerStatus(channel, ReadGameController());

    if (!status)
        if (const auto block = ControlBlock(channel))
            Memory::Write32(block + 0x838, cpu->gpr[4] != 0);

    CompleteController(cpu->gpr[5], channel, status);
    cpu->gpr[3] = status;
}
REGISTER_NATIVE_FUNCTION(0x804F1B20, MH3_WPADControlDpd);

extern "C" void MH3_WPADControlSpeaker(CpuContext* cpu) {
    const auto channel = cpu->gpr[3];
    auto status = ControllerStatus(channel, ReadGameController());

    if (!status && cpu->gpr[4])
        status = -4;

    CompleteController(cpu->gpr[5], channel, status);
    cpu->gpr[3] = status;
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

    const auto report = Report(sample, sample.connected ? 0 : -1, IrOn(block));
    const auto size = ReportSize(Memory::Read32(block + 0x8B8));
    const auto buffer = Memory::Read32(block + 0x8AC), count = Memory::Read32(block + 0x8B4);

    if (buffer && count) {
        if (count > 0x10000 || !Memory::Contains(buffer, uint64_t(count) * size))
            MH3_THROW(std::runtime_error("Invalid WPAD auto-sampling buffer"));

        const auto previous = Memory::Read32(block + 0x8B0);

        if (previous != 0xFFFFFFFF && previous >= count)
            MH3_THROW(std::runtime_error("Invalid WPAD sampling index"));

        const auto index = previous == 0xFFFFFFFF || previous + 1 == count ? 0 : previous + 1;
        CopyReport(buffer + index * size, report, size);
        Memory::Write32(block + 0x8B0, index);
    }

    if (sample.connected)
        CompleteController(Memory::Read32(block + 0x8A8), 0, 0);
}
