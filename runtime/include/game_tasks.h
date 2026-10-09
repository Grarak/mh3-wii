// Game tasks, RMHP08: the task slots RunTasks runs.
#pragma once

#include <cstdint>

namespace GameTasks {
void StartTask(uint32_t function, int16_t slot); // 0x80041930
void StartGameModeTask();                        // 0x800419DC
// Before each task pass: the boot goes past the Wii strap screen.
void SkipStrapScreen();
} // namespace GameTasks
