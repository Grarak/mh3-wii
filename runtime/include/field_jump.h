// Leaving the village for a quest as soon as it is up (--quest options).
#pragma once

#include "quest_catalog.h"

#include <cstdint>
#include <vector>

// quest 0: the season's free-roam quest (the Moga Woods exit). area < 0: the
// quest's start area.
void RequestQuestJump(uint32_t quest, QuestCatalog::Set set, int area);

// --spawn: once the quest has placed the hunter, move them to (x, y, z).
void RequestQuestSpawn(float x, float y, float z);

// --spawn exit:n[,m...]: once placed, move the hunter into the area's exit
// toward area n, so the game changes area; there, into the exit toward m,
// and so on (game area indices).
void RequestQuestExit(std::vector<int> areas);

// --spawn exit:tour: after each arrival, the area's next exit in turn.
void RequestQuestTour();

// --cha-cha: Cha-Cha comes along on a village quest, wearing mask (0-6)
// and with the dances dance1 and dance2 (0-3), whatever the save has. Only
// the character in memory changes, as the quest counter would see it.
void RequestQuestChaCha(uint8_t mask, uint8_t dance1, uint8_t dance2);

// Each paced frame, with the game time in 30 Hz frames.
void FieldJumpFrame(uint64_t gameFrame);
