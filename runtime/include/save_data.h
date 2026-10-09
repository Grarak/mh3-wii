// Save data, RMHP08: the save manager's buffer and the character load.
#pragma once

#include "abi_bridge.h"

#include <cstdint>

namespace SaveData {
constexpr uint32_t kFileSize = 0x14000;     // one save file (data00..05)
constexpr uint32_t kCharacters = 0x48;      // the character records in a save file
constexpr uint32_t kCharacterSize = 0x6000; // one character record

// 0x80442088: makes character `slot` of the save manager's buffer the working
// character. 1 on success, 0 when the file is unused or its sum is wrong.
uint32_t LoadCharacter(CpuContext* ctx, uint8_t slot);
} // namespace SaveData
