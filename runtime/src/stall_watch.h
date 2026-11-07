// MH3_TRACE=stall: reports the hunter and the large monsters standing frozen
// while the game runs on (stall_watch.cpp).
#pragma once

#ifdef MH3_DEVELOPMENT
// Once a frame, after its logic step.
void StallWatchFrame();
#endif
