// The MH3SP patch (MH3SP.xml, the community server's Riivolution patch for
// RMHP08/RMHE08), applied to the static recompilation: its data patches are
// written into guest memory after the DOL's data sections are set up, and its
// instruction patch is part of the native port of the function it changes.
//
// - The SSL root certificate the game pins for its Capcom server (passed to
//   SSLSetRootCA) becomes the MH3SP CA.
// - The Capcom server (mmh-t1-opn04.mmh-service.capcom.co.jp) becomes
//   opn.mh3sp.com; the Nintendo account server (naswii.nintendowifi.net,
//   HTTPS) becomes http://nas.mh3sp.com; GameSpy's availability check and NAT
//   negotiation servers become available/natneg1-3.mh3sp.com.
// - NNBeginNegotiationWithSocket uses those NAT negotiation host names as
//   they are instead of prefixing the game name (the sprintf patch).
#include "online.h"

#include "abi_bridge.h"
#include "fatal.h"
#include "guest_call.h"
#include "memory.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
// mh3sp/ca.der from MH3SP-Riivolution.zip: the MH3SP CA, which signs the
// community server's certificate (same subject and size as Nintendo's).
constexpr uint8_t kMh3spCa[] = {
#include "mh3sp_ca.inc"
};

static_assert(sizeof(kMh3spCa) == 924);

// The pinned CA in the DOL: Nintendo Class 2 CA, 924 bytes of DER.
constexpr uint32_t kPinnedCa = 0x80572840;
constexpr std::string_view kPinnedCaPrefix{"\x30\x82\x03\x98\x30\x82\x03\x01\xA0\x03\x02\x01\x02\x02\x01", 15};

struct StringPatch {
    uint32_t address;
    std::string_view original; // what must be there, without the terminator
    const char* replacement;
    uint32_t length; // bytes the patch writes (the original's slot)
};

// RMHP08 addresses of the strings MH3SP.xml searches for.
constexpr StringPatch kStrings[]{
    {0x80601200, "mmh-t1-opn04.mmh-service.capcom.co.jp", "opn.mh3sp.com", 38},
    {0x80630920, "https://naswii.nintendowifi.net/ac", "http://nas.mh3sp.com/ac", 34},
    {0x806309A0, "https://naswii.nintendowifi.net/pr", "http://nas.mh3sp.com/pr", 34},
    {0x80631118, "%s.available.gs.nintendowifi.net", "available.mh3sp.com", 32},
    {0x806311A8, "natneg1.gs.nintendowifi.net", "natneg1.mh3sp.com", 28},
    {0x806311C4, "natneg2.gs.nintendowifi.net", "natneg2.mh3sp.com", 28},
    {0x806311E0, "natneg3.gs.nintendowifi.net", "natneg3.mh3sp.com", 28},
};

bool Matches(uint32_t address, std::string_view bytes) {
    return std::memcmp(Memory::GetPointer(address, uint32_t(bytes.size())), bytes.data(), bytes.size()) == 0;
}

// The CA to install: MH3_ONLINE_CA (a DER file, for a local test server) or
// the MH3SP CA.
std::vector<uint8_t> RootCa() {
    if (const char* path = std::getenv("MH3_ONLINE_CA"); path && *path) {
        std::ifstream file(path, std::ios::binary);
        std::vector<uint8_t> der((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

        if (der.empty() || der.size() > sizeof(kMh3spCa))
            MH3_THROW(std::runtime_error(std::string("MH3_ONLINE_CA: no DER certificate of at most 924 bytes in ") + path));

        return der;
    }

    return {std::begin(kMh3spCa), std::end(kMh3spCa)};
}
} // namespace

void ApplyOnlinePatches() {
    if (!Matches(kPinnedCa, kPinnedCaPrefix))
        MH3_THROW(std::runtime_error("Online patch: the pinned CA is not where RMHP08 has it"));

    // The certificate's slot keeps its size; a shorter one is padded with zeros.
    const auto ca = RootCa();
    std::memset(Memory::GetPointer(kPinnedCa, sizeof(kMh3spCa)), 0, sizeof(kMh3spCa));
    std::memcpy(Memory::GetPointer(kPinnedCa, uint32_t(ca.size())), ca.data(), ca.size());

    for (const auto& patch : kStrings) {
        if (!Matches(patch.address, patch.original))
            MH3_THROW(std::runtime_error("Online patch: unexpected string at " + std::to_string(patch.address)));

        auto* slot = Memory::GetPointer(patch.address, patch.length);
        std::memset(slot, 0, patch.length);
        std::memcpy(slot, patch.replacement, std::strlen(patch.replacement));
    }
}

namespace {
// GameSpy NAT negotiation (natneg.c) globals, r13-relative.
enum : int32_t {
    kAvailability = -13832, // __GSIACResult: 1 when the backend reported available
    kNegotiators = -13784,  // DArray negotiateList
    kMatchup1Ip = -13780,   // resolved server addresses, 0 until resolved
    kMatchup2Ip = -13776,
    kMatchup3Ip = -13772,
    kMatchup3Hostname = -13752, // override host names (char*), null for the defaults
    kMatchup2Hostname = -13748,
    kMatchup1Hostname = -13744,
};

// The default host names, 28 bytes apart: natneg1, natneg2, natneg3.
constexpr uint32_t kDefaultHostnames = 0x806311A8;

// struct _NATNegotiator (0x54 bytes).
enum : uint32_t {
    kNegotiateSocket = 0x00,
    kGameSocket = 0x04,
    kCookie = 0x08,
    kClientIndex = 0x0C,
    kRetryCount = 0x24,
    kMaxRetryCount = 0x28,
    kGuessedIp = 0x30,
    kGuessedPort = 0x34,       // u16
    kGotRemoteData = 0x36,     // u8
    kSendGotRemoteData = 0x37, // u8
    kProgressCallback = 0x38,
    kCompletedCallback = 0x3C,
    kUserData = 0x40,
    kResult = 0x44,
    kNegotiatorBytes = 0x54,
};

enum : uint32_t {
    Memset = 0x80004350,
    ArrayNew = 0x8050B170,
    ArrayLength = 0x8050B2B0,
    ArrayNth = 0x8050B2C0,
    ArrayAppend = 0x8050B2F0,
    ArrayDeleteAt = 0x8050B5A0,
    Socket = 0x8050BF00,
    InetAddr = 0x8050C140,
    GetHostByName = 0x8050C2C0,
    SendInitPackets = 0x80513510,
};

enum NegotiateError : uint32_t {
    kNoError = 0,
    kAllocError = 1,
    kSocketError = 2,
    kDnsError = 3
};

constexpr uint32_t kNoResult = 5; // nr_noresult
} // namespace

// NNBeginNegotiationWithSocket(gamesocket, cookie, clientindex,
// progresscallback, completedcallback, userdata), 0x80513940, decompiled with
// Ghidra. ResolveServers is inlined: each server not resolved yet is looked up
// by its override host name or, with the MH3SP patch, its default host name
// as it is (the original used "<gamename>.<default>").
namespace {
uint32_t BeginNegotiation(CpuContext* cpu) {
    GuestFrame frame(cpu, 0x140);
    GuestCall call{cpu};
    const uint32_t sda = frame.Arg(13);
    auto global = [sda](int32_t offset) {
        return sda + uint32_t(offset);
    };

    if (Memory::Read32(global(kAvailability)) != 1)
        return kSocketError;

    const int32_t ips[3]{kMatchup1Ip, kMatchup2Ip, kMatchup3Ip};
    const int32_t overrides[3]{kMatchup1Hostname, kMatchup2Hostname, kMatchup3Hostname};

    for (int server = 0; server < 3; ++server) {
        if (Memory::Read32(global(ips[server])))
            continue;

        uint32_t name = Memory::Read32(global(overrides[server]));

        if (!name)
            name = kDefaultHostnames + server * 28;

        // NameToIp: a dotted address, else a DNS lookup's first address.
        uint32_t ip = call.Call<InetAddr>({name});

        if (ip == 0xFFFFFFFF) {
            const uint32_t host = call.Call<GetHostByName>({name});
            ip = host ? Memory::Read32(Memory::Read32(Memory::Read32(host + 12))) : 0;
        }

        Memory::Write32(global(ips[server]), ip);
    }

    if (!Memory::Read32(global(kMatchup1Ip)) || !Memory::Read32(global(kMatchup2Ip)) || !Memory::Read32(global(kMatchup3Ip)))
        return kDnsError;

    // AddNegotiator: a zeroed negotiator appended to the list.
    const uint32_t blank = frame[200];
    call.Call<Memset>({blank, 0, kNegotiatorBytes});

    if (!Memory::Read32(global(kNegotiators)))
        Memory::Write32(global(kNegotiators), call.Call<ArrayNew>({kNegotiatorBytes, 4, 0}));

    const uint32_t list = Memory::Read32(global(kNegotiators));
    call.Call<ArrayAppend>({list, blank});
    const uint32_t negotiator = call.Call<ArrayNth>({list, call.Call<ArrayLength>({list}) - 1});

    if (!negotiator)
        return kAllocError;

    Memory::Write32(negotiator + kGameSocket, frame.Arg(3));
    Memory::Write32(negotiator + kCookie, frame.Arg(4));
    Memory::Write32(negotiator + kClientIndex, frame.Arg(5));
    Memory::Write32(negotiator + kProgressCallback, frame.Arg(6));
    Memory::Write32(negotiator + kCompletedCallback, frame.Arg(7));
    Memory::Write32(negotiator + kUserData, frame.Arg(8));
    const uint32_t socket = call.Call<Socket>({2, 2, 17}); // AF_INET, SOCK_DGRAM, IPPROTO_UDP
    Memory::Write32(negotiator + kNegotiateSocket, socket);
    Memory::Write32(negotiator + kRetryCount, 0);
    Memory::Write8(negotiator + kGotRemoteData, 0);
    Memory::Write8(negotiator + kSendGotRemoteData, 0);
    Memory::Write32(negotiator + kGuessedIp, 0);
    Memory::Write16(negotiator + kGuessedPort, 0);
    Memory::Write32(negotiator + kMaxRetryCount, 0);
    Memory::Write32(negotiator + kResult, kNoResult);

    if (socket == 0xFFFFFFFF) {
        // RemoveNegotiator.
        for (int32_t i = 0; i < int32_t(call.Call<ArrayLength>({list})); ++i)
            if (call.Call<ArrayNth>({list, uint32_t(i)}) == negotiator) {
                call.Call<ArrayDeleteAt>({list, uint32_t(i)});
                break;
            }

        return kSocketError;
    }

    call.Call<SendInitPackets>({negotiator});
    return kNoError;
}
} // namespace

extern "C" void MH3_NNBeginNegotiationWithSocket(CpuContext* cpu) {
    const uint32_t result = BeginNegotiation(cpu);
    cpu->gpr[3] = result;
}
REGISTER_NATIVE_FUNCTION_AS(0x80513940, MH3_NNBeginNegotiationWithSocket, "NNBeginNegotiationWithSocket");
