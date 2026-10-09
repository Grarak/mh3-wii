#pragma once
#include <optional>

// The console's 16:9 setting (SYSCONF IPL.AR) as the game reads it through
// SCGetAspectRatio. MH3_WIDESCREEN=1 forces 16:9 and MH3_WIDESCREEN=0 forces
// 4:3. Unset, the Vita uses 16:9 and other hosts read the NAND's SYSCONF (no
// SYSCONF reads as 4:3). The game reads the setting once, at RenderInit, so a
// launcher sets it before the game starts: SetWidescreenSetting overrides the
// variable.
std::optional<bool> WidescreenSetting();
void SetWidescreenSetting(bool widescreen);
