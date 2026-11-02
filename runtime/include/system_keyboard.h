// The Vita's system keyboard in place of the game's own, a launcher option
// (MH3_SYSTEM_KEYBOARD=1 for runs without the launcher).
#pragma once

namespace SystemKeyboard {

bool Setting();
void SetSetting(bool on);

// The dialog is on the screen: the game's controls stay neutral.
bool Showing();

} // namespace SystemKeyboard
