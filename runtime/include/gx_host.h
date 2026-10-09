#pragma once
#include <cstddef>
#include <cstdint>
#include <stdexcept>

void ResetGxRegisters();
bool ReadGxRegister(uint32_t address, unsigned width, uint32_t& value);
bool WriteGxRegister(uint32_t address, unsigned width, uint32_t value);
void SubmitGuestGxBytes(const uint8_t* data, size_t size);
// SubmitGuestGxBytes for the 1-, 2- and 4-byte FIFO writes.
template <size_t N>
void SubmitGuestGxFixed(const uint8_t* data);

// The window was closed (or SIGINT, which SDL turns into a quit): thrown from
// ServiceHostGraphicsEvents, where the game can stop; main exits on it.
struct HostQuit : std::runtime_error {
    HostQuit() : std::runtime_error("MH3 window closed") {}
};
