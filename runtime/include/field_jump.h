// Leaving the village for a quest as soon as it is up (--quest options).
#pragma once

#include "quest_catalog.h"

#include <cstdint>

// quest 0: the season's free-roam quest (the Moga Woods exit). area < 0: the
// quest's start area.
void RequestQuestJump(uint32_t quest, QuestCatalog::Set set, int area);

// --spawn: once the quest has placed the hunter, move them to (x, y, z).
void RequestQuestSpawn(float x, float y, float z);

// --spawn exit:n: once placed, move the hunter into the area's exit toward
// area n, so the game changes area.
void RequestQuestExit(int area);

// Each paced frame, with the game time in 30 Hz frames.
void FieldJumpFrame(uint64_t gameFrame);
