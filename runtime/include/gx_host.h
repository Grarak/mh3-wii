#pragma once
#include <cstddef>
#include <cstdint>

void ResetGxRegisters();
bool ReadGxRegister(uint32_t address, unsigned width, uint32_t& value);
bool WriteGxRegister(uint32_t address, unsigned width, uint32_t value);
void SubmitGuestGxBytes(const uint8_t* data, size_t size);
