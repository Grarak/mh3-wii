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

The guest thread scheduler, the IOS devices and the patches are portable
(they compile with the Vita build's flags); what the Vita needs is a
`NetHost` (`runtime/src/net/net_host.h`) of its own instead of
`net_host_none.cpp`:

- `sceNetInit` with a memory pool, `sceNetCtlInit`, and the BSD calls
  (`sceNetSocket`, `sceNetConnect`, `sceNetRecvfrom`, ... with
  `SCE_NET_SO_NBIO` for non-blocking sockets; or newlib's socket wrappers)
  and `sceNetEpoll`/`sceNetPoll` for `Poll`/`Readable`/`Writable`;
  `sceNetResolverStartNtoa` on a helper thread for `Lookup` (it blocks);
  `sceNetCtlInetGetInfo` for `DefaultInterface`.
- TLS: VitaSDK's OpenSSL 1.0.2 works with `net_host_posix.cpp`'s
  `OpenSslTls` as is (it has 1.0.2 paths; SHA-1 and 1024-bit RSA pass there
  without lowering a security level). VitaSDK's sceSsl stubs document no TLS
  session over a socket of one's own; sceHttp only does whole HTTP requests.
- Host thread stacks: each guest thread that runs gets a 1 MiB host thread;
  online play starts five or six. The waits use timed re-checks, which the
  Vita's condition variables need (lost wakeups).
- The devkit's network: the community servers resolve through the console's
  DNS; a local test server needs `MH3_ONLINE_HOSTS` pointing at the host.

## Running

Against the community servers, nothing is needed: the host build resolves
`opn.mh3sp.com` and friends through the system resolver.

Against a local MH3SP server (recommended for testing):

```sh
git clone https://github.com/sepalani/MH3SP && cd MH3SP
# config.ini: IP = 127.0.0.1 for every server, LegacySSL = ON, LogToWindow = OFF
python3 master_server.py -s &
python3 scripts/dev/online-local-nas.py &      # NAS and the availability check
MH3_ONLINE_CA=MH3SP/cert/ca.der MH3_ONLINE_HOSTS='*.mh3sp.com=127.0.0.1' \
  MH3_ONLINE_PORTS=80=18080 MH3_NET_TRACE=1 build/linux-arm64-dev/MH3
```

| Variable | Meaning |
| --- | --- |
| `MH3_ONLINE_CA=PATH` | install this DER CA instead of the MH3SP one (the local server's `cert/ca.der`) |
| `MH3_ONLINE_HOSTS=name=ip,...` | answer these host names (`*.domain` for a whole domain) without DNS |
| `MH3_ONLINE_PORTS=from=to,...` | connect to `to` instead of port `from` (the NAS's port 80) |
| `MH3_NET_TRACE=1` | log sockets, connects, lookups and TLS sessions |
