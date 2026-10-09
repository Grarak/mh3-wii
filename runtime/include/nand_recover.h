#pragma once

// Vita: a NAND file replace (ISFS Rename over an existing file) moves the old
// file to NAME.replaced before the new one takes its name, since the Vita's
// rename refuses to overwrite. Interrupted between the two (power loss, a
// killed app), the save has no file under its name and its only copy is the
// aside: put it back; one left beside a present file is dropped.
// C linkage, so runtime/vita/nand.inc (inside ios.cpp's anonymous namespace)
// can declare it there too.
extern "C" void MH3_RecoverNandReplaces(const char* nandRoot);

// The same for the user's NAND (MH3_USER_DIR/nand), before anything reads a
// save (the launcher's list, a quest launch). Other hosts rename in place.
void RecoverNandSaves();
