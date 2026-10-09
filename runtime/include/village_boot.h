#pragma once
#include "abi_bridge.h"
#include "controller_host.h"
#include <span>

#if defined(MH3_DEVELOPMENT) || defined(__vita__)
// --village: into the village with File 1's first character, past the
// title's movies and menus.
void StartVillageBoot(unsigned file = 0, unsigned character = 0);
// Each logic step, before the tasks run.
void StepVillageBoot(CpuContext* ctx);
#endif
void FinishVillageBoot();
ClassicControllerSample ReadGameController();
void RunVillageBootSelfTests();
