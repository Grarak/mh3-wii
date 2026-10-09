// The step counter (system_w+0xC, step_counter.cpp) from native ports.
#pragma once

#include <cstdint>

namespace StepCounter {
// The game's periodic test `counter % n == k` ("every n steps"), in ticks of
// game time: true on the step whose ticks include one that is k mod n, so it
// passes once every n ticks whatever the step length. At the stock 1/30 s
// step (one tick) this is the original test.
bool Every(uint32_t n, uint32_t k = 0);
} // namespace StepCounter
