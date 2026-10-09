#pragma once
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <functional>

void ResetGxRegisters();
bool ReadGxRegister(uint32_t address, unsigned width, uint32_t& value);
bool WriteGxRegister(uint32_t address, unsigned width, uint32_t value);
// Runs native graphics work on the game thread without waiting for the GPU.
void SubmitHostGraphicsTask(std::function<void()> task);
void SubmitGuestGxBytes(const uint8_t* data, size_t size);
#ifdef MH3_DEVELOPMENT
#include <vector>
// Port checks: while `into` is set, the guest's FIFO bytes are appended to it
// instead of going to the GPU (the FIFO window is closed meanwhile).
void CaptureGuestGx(std::vector<uint8_t>* into);
#endif
// SubmitGuestGxBytes for the 1-, 2- and 4-byte FIFO writes.
template <size_t N>
void SubmitGuestGxFixed(const uint8_t* data);

// The window was closed (or SIGINT, which SDL turns into a quit): thrown from
// ServiceHostGraphicsEvents, where the game can stop; main exits on it.
struct HostQuit : std::runtime_error {
    HostQuit() : std::runtime_error("MH3 window closed") {}
};
