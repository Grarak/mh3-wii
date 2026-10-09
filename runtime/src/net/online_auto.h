// The online autopilot (development builds, online_auto.cpp).
#pragma once

#include "abi_bridge.h"

#include <sstream>
#include <string>

namespace OnlineAuto {

// The debug port's `online` command: its words after `online`; the answer.
std::string Command(CpuContext* cpu, std::istringstream& in);

} // namespace OnlineAuto
