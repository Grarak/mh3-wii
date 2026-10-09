#pragma once
#include "abi_bridge.h"
#include "controller_host.h"
#include <span>

#ifdef MH3_DEVELOPMENT
// --village: into the village with File 1's first character, past the
// title's movies and menus.
void StartVillageBoot();
// Each logic step, before the tasks run.
void StepVillageBoot(CpuContext* ctx);
#endif
void FinishVillageBoot();
ClassicControllerSample ReadGameController();
void RunVillageBootSelfTests();
