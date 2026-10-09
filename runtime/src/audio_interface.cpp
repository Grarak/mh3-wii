#include "audio_interface.h"
#include <chrono>
#include <stdexcept>

void AudioInterfaceState::Reset(uint64_t now) {
    control = 0x42; // stream 48 kHz; DSP DMA 32 kHz; stopped
    volume = counter = interruptTiming = 0;
    lastTime = now;
    phase = 0;
}
void AudioInterfaceState::Advance(uint64_t now) {
    if (now < lastTime) throw std::runtime_error("AI clock moved backwards");
    const auto elapsed = now - lastTime;
    lastTime = now;
    if (!(control & 1)) return;
    const uint64_t rate = (control & 2) ? 48000 : 32000;
    const uint64_t scaled = (elapsed % 1000000000) * rate + phase;
    const uint64_t samples = (elapsed / 1000000000) * rate + scaled / 1000000000;
    phase = scaled % 1000000000;
    uint64_t distance = uint32_t(interruptTiming - counter);
    if (!distance) distance = 0x100000000ULL;
    if (samples >= distance) control |= 8;
    counter += static_cast<uint32_t>(samples);
}
uint32_t AudioInterfaceState::Read(uint32_t offset, uint64_t now) {
    Advance(now);
    switch (offset) {
    case 0: return control;
    case 4: return volume;
    case 8: return counter;
    case 12: return interruptTiming;
    default: throw std::runtime_error("Unknown AI register");
    }
}
void AudioInterfaceState::Write(uint32_t offset, uint32_t value, uint64_t now) {
    Advance(now);
    switch (offset) {
    case 0:
        control = (control & 8) | (value & 0x57);
        if (value & 8) control &= ~8u; // interrupt status is write-one-to-clear
        if (value & 0x20) { counter = 0; phase = 0; }
        break;
    case 4: volume = value; break;
    case 8: counter = value; phase = 0; break;
    case 12: interruptTiming = value; break;
    default: throw std::runtime_error("Unknown AI register");
    }
}
namespace {
AudioInterfaceState state;
uint64_t Now() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
bool Offset(uint32_t address, uint32_t& offset) {
    if ((address >> 30) == 1) return false;
    const auto physical = address & 0x3FFFFFFF;
    if ((physical & ~0x01000000u) < 0x0C006C00 ||
        (physical & ~0x01000000u) > 0x0C006C0C || (address & 3)) return false;
    offset = physical & 0xF;
    return true;
}
}
void ResetAudioInterface() { state.Reset(Now()); }
bool ReadAudioRegister32(uint32_t address, uint32_t& value) {
    uint32_t offset;
    if (!Offset(address, offset)) return false;
    value = state.Read(offset, Now());
    return true;
}
bool WriteAudioRegister32(uint32_t address, uint32_t value) {
    uint32_t offset;
    if (!Offset(address, offset)) return false;
    state.Write(offset, value, Now());
    return true;
}
