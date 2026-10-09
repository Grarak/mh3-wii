// Online play against the community MH3SP servers (docs/online.md).
#pragma once

#include <span>
#include <string>

namespace NetHost {
class Tls;
}

// Writes the MH3SP patch's data changes into guest memory: its CA for the
// Capcom server's pinned certificate and its server host names
// (online_patches.cpp). After the DOL's data sections are initialized.
void ApplyOnlinePatches();

// The launcher's online choices for this launch; MH3_ONLINE_SERVER and
// MH3_ONLINE_SKIP_TLS (1) give their defaults.
//
// The server: a domain whose opn., nas., available. and natneg1-3. hosts are
// the servers (mh3sp.com, the community's, by default), or an IPv4 address
// that serves them all (a local MH3SP). ":port" after either moves the
// account server (NAS, HTTP on port 80) to that port.
inline constexpr const char* kDefaultOnlineServer = "mh3sp.com";
std::string OnlineServerSetting();
void SetOnlineServerSetting(std::string server);

// Why `server` cannot be used, or empty.
std::string CheckOnlineServer(const std::string& server);

// The game's TLS sessions carry their bytes unencrypted, with no handshake:
// for a server whose OPN runs without TLS (MH3SP's UseSSL = OFF).
bool SkipTlsSetting();
void SetSkipTlsSetting(bool skip);

// No online play while any cheat is on: no name lookup, connection or
// datagram leaves (ios_net.cpp), so the game's first server contact fails
// and it shows its connection error, before any server is reached. Whether
// they may; HUDDraw (native_system_ui.cpp) shows why over that error.
bool AdmitOnline();

// Community servers: a server whose TLS certificate the MH3SP CA signed
// (mh3sp.com, or any server on its patch). The City's loading screens show
// the rules after such a server's handshake. After each TLS handshake.
void NoteTlsServer(NetHost::Tls& tls);

// The notice's lines (empty: none), from HUDDraw once a frame; `loading`
// while the loading screen is up.
std::span<const char* const> OnlineNotice(bool loading);
