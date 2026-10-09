#pragma once
#include <cstdint>

// Register state for the streaming AI clock. DSP DMA/mixing is a separate service.
class AudioInterfaceState {
  public:
    void Reset(uint64_t now);
    uint32_t Read(uint32_t offset, uint64_t now);
    void Write(uint32_t offset, uint32_t value, uint64_t now);

    // AICR bit 6: the DSP DMA rate, which time does not change.
    uint32_t DmaRate() const { return (control & 0x40) ? 32000 : 48000; }

  private:
    void Advance(uint64_t now);
    uint32_t control = 0x42, volume = 0, counter = 0, interruptTiming = 0;
    uint64_t lastTime = 0, phase = 0;
};

void ResetAudioInterface();
bool ReadAudioRegister32(uint32_t address, uint32_t& value);
bool WriteAudioRegister32(uint32_t address, uint32_t value);

// The DSP DMA sample rate the game set in AICR, for the native audio DMA
// service: read from the model, not through an MMIO access.
uint32_t AudioInterfaceDmaRate();
