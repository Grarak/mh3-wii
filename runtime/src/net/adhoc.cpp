// Ad hoc play (adhoc.h). The Vita's netcheck dialog is what normally joins the
// ad hoc network; this does what its library does underneath (as azahar's
// Vita port does, GPLv2 or later): NP started with a communication ID, which
// the system wants and does not check, SceShell's events raised, and SceNetCtl
// asked to join the IBSS every Vita title shares. Ordinary sockets work on it,
// with a link-local address, so the game's network code stays as it is.
#include "adhoc.h"

#include "local_server.h"
#include "net_host.h"
#include "online.h"

#include <arpa/inet.h>
#include <string>

#ifdef __vita__
#include <cstdio>
#include <cstring>
#include <psp2/kernel/threadmgr.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/np/common.h>
#include <psp2/sysmodule.h>

extern "C" {
// SceNetCtl's unnamed ad hoc exports (runtime/vita/netctl_adhoc.yml).
int sceNetCtlAdhocConnectSsid(const char* ssid);
int sceNetCtlAdhocShellEventA(int which);
int sceNetCtlAdhocShellEventB(int which);

struct SceNpCommunicationConfig {
    const SceNpCommunicationId* id;
    const void* passphrase;
    const void* signature;
};

int sceNpInit(const SceNpCommunicationConfig* config, void* option);
}
#endif

namespace Adhoc {
namespace {
constexpr int kFindSeconds = 15;

#ifdef __vita__
constexpr char kSsid[] = "SCE_PSP2_IBSS", kCommunicationId[] = "MHTR00001";
constexpr int kConnectedState = 3, kConnectSeconds = 30;

std::string Hex(int value) {
    char text[16];
    std::snprintf(text, sizeof(text), "%08X", unsigned(value));
    return text;
}

int LinkState() {
    int state = 0;
    sceNetCtlCheckCallback();
    return sceNetCtlAdhocGetState(&state) < 0 ? -1 : state;
}

void IgnoreEvent(int, void*) {}

Status Connect(uint32_t& address) {
    if (LinkState() != kConnectedState) {
        static bool started = false;

        if (!started) {
            if (const int loaded = sceSysmoduleLoadModule(SCE_SYSMODULE_NP); loaded < 0)
                return Status::Error("The NP module did not load (" + Hex(loaded) + ").");

            SceNpCommunicationId id{};
            std::memcpy(id.data, kCommunicationId, sizeof(id.data));
            const SceNpCommunicationConfig config{&id, nullptr, nullptr};

            if (const int result = sceNpInit(&config, nullptr); result < 0)
                return Status::Error("NP did not start (" + Hex(result) + ").");

            int callback = -1;

            if (const int result = sceNetCtlAdhocRegisterCallback(IgnoreEvent, nullptr, &callback); result < 0)
                return Status::Error("The ad hoc network has no callback (" + Hex(result) + ").");

            started = true;
        }

        for (int which = 1; which <= 3; ++which) {
            sceNetCtlAdhocShellEventA(which);
            sceNetCtlAdhocShellEventB(which);
        }

        if (const int result = sceNetCtlAdhocConnectSsid(kSsid); result < 0)
            return Status::Error("The system refused the ad hoc network (" + Hex(result) + "). Is Wi-Fi on?");

        // SceNetCtl reports only through state it updates when asked.
        int tick = 0;

        for (; LinkState() != kConnectedState && tick < kConnectSeconds * 10; ++tick)
            sceKernelDelayThread(100 * 1000);

        if (tick >= kConnectSeconds * 10) {
            sceNetCtlAdhocDisconnect();
            return Status::Error("The ad hoc network could not be joined. Is Wi-Fi on?");
        }
    }

    SceNetInAddr own{};

    if (const int result = sceNetCtlAdhocGetInAddr(&own); result < 0)
        return Status::Error("The ad hoc network gave no address (" + Hex(result) + ").");

    address = own.s_addr;
    NetHost::UseLinkAddress(address);
    return {};
}
#else
Status Connect(uint32_t& address) {
    address = NetHost::DefaultInterface().ip;
    return {};
}
#endif

Status Link(const Progress& progress, uint32_t& address) {
    if (!NetHost::Startup())
        return Status::Error("The network did not start.");

    progress("Joining the ad hoc network");
    return Connect(address);
}

std::string Text(uint32_t address) {
    char text[INET_ADDRSTRLEN] = "";
    inet_ntop(AF_INET, &address, text, sizeof(text));
    return text;
}

// The game connects to `address` as to a local MH3SP.
void LeadTo(const std::string& address) {
    SetOnlineServerSetting(address + ":" + std::to_string(LocalServer::kNasPort));
    SetSkipTlsSetting(true);
}
} // namespace

Status Host(const Progress& progress) {
    uint32_t address = 0;
    MH3_RETURN_IF_ERROR(Link(progress, address));
    progress("Opening the game");
    const std::string text = Text(address);

    if (!LocalServer::Start(text))
        return Status::Error("The game could not be opened on " + text + ".");

    LeadTo(text);
    return {};
}

Status Join(const Progress& progress) {
    uint32_t address = 0;
    MH3_RETURN_IF_ERROR(Link(progress, address));
    progress("Looking for a host");
    const uint32_t host = LocalServer::FindHost(kFindSeconds);

    if (!host)
        return Status::Error("No host found nearby. The host starts first, and stays in range.");

    LeadTo(Text(host));
    return {};
}
} // namespace Adhoc
