#pragma once
#include "controller_host.h"
#include <span>

// One-shot unattended entry through the game's ordinary save/menu path.
void StartVillageBoot();
void FinishVillageBoot();
ClassicControllerSample ReadGameController();
void RunVillageBootSelfTests();
