# Online play (MH3SP)

Capcom's and Nintendo's Monster Hunter Tri servers are gone. The community
[MH3SP](https://github.com/sepalani/MH3SP) project runs replacements; Dolphin
reaches them with the MH3SP Riivolution patch (`MH3SP.xml`). This build
applies the same patch and emulates the Wii network devices the game uses,
so the City mode connects to MH3SP from the host build.

**Testing rules (from the user).** Never interact with other players. Online,
only open a private city and start quests; no public cities or gates beyond
what the connection forces, no chat, no joins, no invites. Scripted input
must go straight to a private city. If a flow cannot guarantee that, stop at
the step before. A local MH3SP server (below) has no other players at all and
is the default for testing.

## What the game needs

Choosing **City** in the Mode Menu loads `net_data.rso` (the connection and
terms menus) at the demo module's base `0x8097D080` and starts the online
library (`sNetworkLibraryWii`, DWC, NHTTP, GameSpy). In order:

1. **Guest threads.** The online library runs on its own OS threads
   (`sNetworkLibraryWii::init/start/stop` create them; DWC's NHTTP and
   GameSpy threads follow). The runtime had no thread switching: a thread
   switch (`OSLoadContext`'s `rfi`) returned to the caller. See
   [Guest threads](#guest-threads).
2. **IOS devices.** `/dev/net/ip/top` (the SO socket library),
   `/dev/net/ncd/manage` (network configuration and link state),
   `/dev/net/kd/request` (`SOStartup` asks WiiConnect24's KD to start the
   socket interface, and polls until it has), `/dev/net/ssl` (the Capcom
   server is TLS with a pinned CA), and the USB host controller
   `/dev/usb/oh0`, which the Wii Speak library (PMIC) probes.
3. **GX in `net_data.rso`.** Its menus draw sprites with GX calls of their
   own (`0x8097F9A0`, `0x8097FB58`); the renderer takes only native
   submissions.
4. **The MH3SP patch.** Its data patches (CA, host names) and its NAT
   negotiation instruction patch (below).
5. **Servers.** `nas.mh3sp.com` (HTTP, the Nintendo account server DWC logs
   in to), `available.mh3sp.com` (UDP 27900, GameSpy's availability check),
   `opn.mh3sp.com:8200` (TLS, the Capcom "OPN" server: terms, CAPCOM ID,
   server and gate lists), then the LMP/FMP/RFP servers the OPN server names
   (MH3SP: ports 8201-8203), and `natneg1-3.mh3sp.com` for GameSpy NAT
   negotiation between players.

## What is implemented

| Piece | Where |
| --- | --- |
| `net_data.rso` overlay profile at `0x8097D080`; `map_town.rso` (Loc Lac) at `0x809A6EE0`, right after it | `scripts/translate_rso_overlays.py`, `rso_services.cpp` |
| Native ports of `net_data`'s sprite drawing, entered through the overlay's dispatch records (`NATIVE_PORTS`) | `runtime/src/native_ui.cpp` |
| Guest thread scheduler: native `SelectThread` (0x804D4360) and `OSExitThread` (0x804D4860), `OSCreateThread` bookkeeping | `runtime/src/guest_threads.cpp` |
| Scheduler-aware `OSSleepTicks`, device pump and frame pacer donation | `os_sleep.cpp`, `frame_pacer.cpp` |
| MH3SP data patches and `NNBeginNegotiationWithSocket` (0x80513940) port | `runtime/src/net/online_patches.cpp` |
| `/dev/net/ip/top`, `/dev/net/ssl`, `/dev/net/ncd/manage`, `/dev/net/kd/request` (once online play started; offline the game still finds no WiiConnect24) | `runtime/src/net/ios_net.cpp` |
| Host sockets, name lookups, TLS (OpenSSL) behind `NetHost` | `runtime/src/net/net_host.h`, `net_host_posix.cpp` |
| USB library's IOS open/close with timeout (0x804BC630, 0x804BC8C0); `/dev/usb/oh*` not found | `runtime/src/ios.cpp` |

### The MH3SP patch

Riivolution searches memory for byte strings. In `RMHP08`'s DOL they are:

| Address | Original | Patched |
| --- | --- | --- |
| `0x80572840` | Nintendo Class 2 CA (924 bytes DER, pinned for the Capcom server) | MH3SP CA (`mh3sp/ca.der`, embedded as `net/mh3sp_ca.inc`) |
| `0x80601200` | `mmh-t1-opn04.mmh-service.capcom.co.jp` | `opn.mh3sp.com` |
| `0x80630920`, `0x806309A0` | `https://naswii.nintendowifi.net/ac`, `/pr` | `http://nas.mh3sp.com/ac`, `/pr` |
| `0x80631118` | `%s.available.gs.nintendowifi.net` | `available.mh3sp.com` |
| `0x806311A8`, `C4`, `E0` | `natneg1-3.gs.nintendowifi.net` | `natneg1-3.mh3sp.com` |
| `0x805139BC`, `A34`, `AAC` | `bl sprintf; addi r24,r1,N` | `mr r24,r7; mr r24,r24` |

`ApplyOnlinePatches` (after `InitializeDataSections`) checks each original
and writes the replacement. The three instruction patches are in
`ResolveServers`, inlined into GameSpy's `NNBeginNegotiationWithSocket`: the
original resolved `"<gamename>.<default host>"`, the patch the default host
name itself. That function is a native port with the patch applied; the
translated code is not changed.

### No cheats on community servers

A server whose TLS certificate the MH3SP CA signed is a community server:
`mh3sp.com`'s, or any other running on the MH3SP patch (its certificate is
`mh.capcom.co.jp`, issued under the CA's Nintendo Class 2 subject). After
each TLS handshake `AdmitTlsServer` (`online_patches.cpp`) checks the
certificates the server sent against the embedded CA's key, whatever the
game's verify option, `MH3_ONLINE_CA` or the server setting. While any cheat
is on (the launcher's, `--cheat`, `--no-damage`), the handshake fails: the
game shows its connection error (11602) and goes back to the mode menu.

`HUDDraw` (`native_system_ui.cpp`) tells the player why:

- after a refusal, "Online play refused: cheats are on.", "Cheats are not
  allowed online." and "Do not harass other players." over the online
  screens' message window (the error) and on the next loading screen;
- in the City (`GetOnlineMode` 2) after a community server's handshake, the
  last two lines on every loading screen. The OPN session is the only TLS
  one and ends after the login, so the last handshake's server decides.

The lines are drawn in the game's debug font (`flfntPrintf`), as the loading
screen's own text. A local MH3SP (Skip TLS, or its own CA) is no community
server; `MH3_ONLINE_COMMUNITY_TEST=1` (development builds) makes every server
one, to try both on it.

## Guest threads

One guest thread runs at a time, as on the Wii's single core. Each guest
thread that runs gets a host thread; `SelectThread` keeps the SDK's run
queues, states and switch callback, and replaces `OSSaveContext`/
`OSLoadContext` with a baton: the thread switched out waits on its host thread
until it is selected again, with its registers intact.

- The game thread runs the boot thread and does all device work. When no
  thread can run, the game thread runs the idle loop: device pumps (IOS
  replies, alarms, VI retraces, sleeps and blocking requests that are over)
  until one can.
- `PumpDevices` ends with an interrupt boundary: sleeps and waits that are
  over end, and a woken thread of higher priority runs at once.
- The game steps without retrace waits, which on the Wii were where its
  lower-priority threads ran. The frame pacer lets other runnable threads run
  up to 2 ms a step (`GuestThreads::Donate`).
- `OSSleepTicks` sleeps in the scheduler once another thread exists; offline
  nothing changes (no second thread ever runs).
- A blocking IOS request (connect, receive, poll, TLS handshake, name lookup)
  waits in the scheduler; the game thread's pumps retry it.

## How far it gets

Host build (arm64 VM, llvmpipe), against a local MH3SP server (October 5):
Mode Menu → City → the network screen (SO, NCD and KD start) → the NAS login
(DWC's `acctcreate`, `login` and `svcloc` over NHTTP) → the Capcom OPN
server over TLS, verified against the installed CA (terms, announcements,
vulgarity lists, authentication, LMP address) → the two user agreements, the
announcement and the mic-chat notice → LMP (ticket, user list, server time)
→ CAPCOM ID selection (a new ID in slot 1) → server type (Open) → server →
gate (FMP) → the Loc Lac introduction movie → **the gate in Loc Lac**, drawn
and running at the VM's usual 30 fps → "Select a City" (walk up the street
from the spawn) → a new city ("LayerStartEmpty", recruitment text, OK) →
**inside the city**, walking around the plaza, Lodge Street and the upper
deck → the Tavern (the archway left of the spawn, then up the street): the
Guildmaster's hunter registration, then the Quest Receptionist's Quest
Counter → "Harvest 'Shroom" (Deserted Island), quest settings, OK
(`CircleCreate` on the FMP server) → the Departure Gate at the top of the
Tavern's stairs ("Depart on Quest", Yes) → **the quest on the Deserted
Island**, the hunter running from the base camp. The field loads the common,
quest, enemy and map01 modules at the offline field bases, so their existing
profiles serve.

Creating the city ran into a callback only reachable through a code constant
(`0x803CF830`, a register shuffle that tail-calls, passed as `lis`/`addi`):
`scripts/generate_symbols.py` now seeds text addresses the code builds with
`lis` + `addi`/`ori` that start after a `blr`/`bctr` (127 more functions).

Against the community server (`opn.mh3sp.com`, October 5) the same flow
works with no overrides: the NAS login at `nas.mh3sp.com`, the OPN server's
TLS verified against the embedded MH3SP CA, LMP and FMP, a new CAPCOM ID,
the server list (one Open server, "Valor US") and its gate list ("City Gate
1-4"). A gate is a shared area other players walk in, and a city is only
reached through one, so testing there stops at the gate list (see the rules
above).

## The Vita

Devkit, October 5, against a local MH3SP on the LAN (OPN without TLS,
`MH3_ONLINE_SERVER=<ip>:18080`, Skip TLS handshake): the launcher's City
(online) → the network screen → NAS login → OPN notices → CAPCOM ID → server
type, server, gate (LMP, FMP) → the Loc Lac movie → **the gate area**,
running at about 47 fps. Exchanges sometimes stall for up to a minute: the
Vita's Wi-Fi answers pings in 2-200 ms with losses (power saving), and TCP's
retransmission backoff stretches each loss.

Fixed on the way: newlib closed the process's stderr when an exited thread's
reent was reclaimed (`runtime/vita/newlib_reent.c`), and texture packs and
extracted streams held more than the 50 descriptors a Vita process has.

`net_host_posix.cpp` serves the Vita too: VitaSDK's newlib implements the
BSD socket calls, `poll` (on `sceNetEpoll`), `fcntl(O_NONBLOCK)` (sceNet's
NBIO option) and `getaddrinfo` (sceNet's resolver) on SceNet, and its first
socket starts the network stack (`_vita_net_init`: the NET module,
`sceNetInit`, `sceNetCtlInit`). The differences are behind `__vita__`:
`DefaultInterface` asks `sceNetCtlInetGetInfo` (no `getifaddrs` or
`res_init`), and sends have no `MSG_NOSIGNAL`. TLS is VitaSDK's OpenSSL
1.0.2 (`SSL_library_init` once; it seeds itself from
`sceKernelGetRandomNumber`). The Vita's `poll.h` makes `POLLPRI` and
`POLLRDBAND` `POLLIN`, so `Poll` reports only the events asked for (and
error, hang-up, invalid).

Each guest thread that runs gets a 1 MiB host thread; online play starts
five or six. The waits use timed re-checks, which the Vita's condition
variables need (lost wakeups).

## Running

Against the community servers, nothing is needed: the game resolves
`opn.mh3sp.com` and friends through the system's DNS.

### The launcher's Online page

The Vita launcher's main menu has **City (online)** after Start game: with a
hunter chosen, it skips the title and goes to the network screen. Its last
row, **Online settings**, holds:

- **Server** (X: keyboard, Triangle: back to `mh3sp.com`). A domain whose
  `opn.`, `nas.`, `available.` and `natneg1-3.` hosts are the servers (at
  most 19 characters: the game's string slots), or the IPv4 address of a
  local MH3SP, which then answers every `*.mh3sp.com` name. `:port` after
  either moves the account server (NAS, HTTP on port 80) to that port.
- **Skip TLS handshake**. The game's TLS sessions carry their bytes
  unencrypted, with no handshake and no certificate, for a server whose OPN
  runs without TLS (MH3SP's `[OPN] UseSSL = OFF`). The community server needs
  TLS, so leave this off for it.

Without the launcher (the host build, a devkit run with arguments),
`MH3_ONLINE_SERVER` and `MH3_ONLINE_SKIP_TLS=1` set the same.

Against a local MH3SP server (recommended for testing):

```sh
git clone https://github.com/sepalani/MH3SP && cd MH3SP
# config.ini: IP = 127.0.0.1 for every server, LegacySSL = ON, LogToWindow = OFF
python3 master_server.py -s &
python3 scripts/dev/online-local-nas.py &      # NAS and the availability check
MH3_ONLINE_CA=MH3SP/cert/ca.der MH3_ONLINE_HOSTS='*.mh3sp.com=127.0.0.1' \
  MH3_ONLINE_PORTS=80=18080 MH3_TRACE=net build/linux-arm64-dev/MH3
```

| Variable | Meaning |
| --- | --- |
| `MH3_ONLINE_SERVER=DOMAIN[:PORT]` or `IP[:PORT]` | the server (default `mh3sp.com`); see the Online page above |
| `MH3_ONLINE_SKIP_TLS=1` | TLS sessions pass their bytes through unencrypted (OPN `UseSSL = OFF`) |
| `MH3_ONLINE_CA=PATH` | install this DER CA instead of the MH3SP one (the local server's `cert/ca.der`) |
| `MH3_ONLINE_HOSTS=name=ip,...` | answer these host names (`*.domain` for a whole domain) without DNS |
| `MH3_ONLINE_PORTS=from=to,...` | connect to `to` instead of port `from` (the NAS's port 80) |
| `MH3_ONLINE_ADDRESSES=from=to,...` | reach IPv4 address `from` at `to`, and read packets from `to` as from `from` (a server that names its ZeroTier address, reached from a Vita on the LAN) |
| `MH3_ONLINE_COMMUNITY_TEST=1` | development builds: every server counts as a community server (the loading-screen lines above) |
| `MH3_ONLINE_AUTO=id=,server=,gate=,city=,post=QUEST` or `...,join=N` | development builds: the autopilot takes a `--city` launch through the online screens into a quest (`runtime/src/net/online_auto.cpp`); `city=-1` stops in the gate (Loc Lac's streets) |
| `MH3_TRACE=net` | development builds: log sockets, connects, lookups and TLS sessions |
