// ran_suu (0x800CEF20): the game's random numbers (runtime/src/ran_suu.cpp).
#pragma once

#include <cstdint>

// Advances random stream `stream` (u16 seeds at 0x8065AA78) and returns its
// new seed: seed * 176 mod 65363, a zero seed taken as 1.
uint32_t RanSuu(uint32_t stream);
