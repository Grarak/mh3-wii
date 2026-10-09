#pragma once
#include <cstdint>

bool DecodeEfbColorAddress(uint32_t address, uint16_t& x, uint16_t& y);
uint32_t ConvertEfbArgb(uint32_t argb, unsigned pixelFormat, unsigned alphaMode);
bool ReadEfbColor32(uint32_t address, uint32_t& value);
uint32_t ReadHostEfbColor(uint16_t x, uint16_t y);

bool WriteEfbColor32(uint32_t address, uint32_t value);
void WriteHostEfbColor(uint16_t x, uint16_t y, uint32_t value);
