// Online play against the community MH3SP servers (docs/online.md).
#pragma once

// Writes the MH3SP patch's data changes into guest memory: its CA for the
// Capcom server's pinned certificate and its server host names
// (online_patches.cpp). After the DOL's data sections are initialized.
void ApplyOnlinePatches();
