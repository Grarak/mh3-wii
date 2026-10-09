// Leaving the village for a quest as soon as it is up (--quest options).
#pragma once

#include "quest_catalog.h"

#include <cstdint>

// quest 0: the season's free-roam quest (the Moga Woods exit). area < 0: the
// quest's start area.
void RequestQuestJump(uint32_t quest, QuestCatalog::Set set, int area);
