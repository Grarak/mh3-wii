// Wii network IOS devices (ios_net.h). Request layouts follow Dolphin's
// Core/IOS/Network (IP/Top.cpp, Socket.cpp, SSL.cpp, NCD/Manage.cpp).
#include "ios_net.h"

#include "guest_threads.h"
#include "ios_host.h"
#include "memory.h"
#include "net_host.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {
using NetHost::Address;

// IOS errors.
constexpr int32_t kIpcEInval = -4;
constexpr int32_t kIpcENoEnt = -6;

// /dev/net/ip/top requests.
enum : uint32_t {
    kSoAccept = 1,
    kSoBind = 2,
    kSoClose = 3,
    kSoConnect = 4,
    kSoFcntl = 5,
    kSoGetPeerName = 6,
    kSoGetSockName = 7,
    kSoGetSockOpt = 8,
    kSoSetSockOpt = 9,
    kSoListen = 10,
    kSoPoll = 11,
    kSoRecvFrom = 12, // ioctlv
    kSoSendTo = 13,   // ioctlv
    kSoShutdown = 14,
    kSoSocket = 15,
    kSoGetHostId = 16,
    kSoGetHostByName = 17,
    kSoInetAtoN = 21,
    kSoInetPtoN = 22,
    kSoInetNtoP = 23,
    kSoGetAddrInfo = 24, // ioctlv
    kSoStartup = 26,
    kSoCleanup = 27,
    kSoGetInterfaceOpt = 28, // ioctlv
    kSoSetInterfaceOpt = 29,
    kSoSetInterface = 30,
    kSoInitInterface = 31,
};

// /dev/net/ssl requests (ioctlv).
enum : uint32_t {
    kSslNew = 1,
    kSslConnect = 2,
    kSslHandshake = 3,
    kSslRead = 4,
    kSslWrite = 5,
    kSslShutdown = 6,
    kSslSetClientCert = 7,
    kSslSetClientCertDefault = 8,
    kSslRemoveClientCert = 9,
    kSslSetRootCa = 10,
    kSslSetRootCaDefault = 11,
    kSslHandshakeEx = 12,
    kSslSetBuiltinRootCa = 13,
    kSslSetBuiltinClientCert = 14,
    kSslDisableVerify = 15,
};

// /dev/net/ncd/manage requests (ioctlv).
enum : uint32_t {
    kNcdLockWirelessDriver = 1,
    kNcdUnlockWirelessDriver = 2,
    kNcdGetConfig = 3,
    kNcdSetConfig = 4,
    kNcdReadConfig = 5,
    kNcdWriteConfig = 6,
    kNcdGetLinkStatus = 7,
    kNcdGetWirelessMacAddress = 8,
};

constexpr int kMaxSockets = 24;    // WII_SOCKET_FD_MAX
constexpr int kMaxTlsSessions = 4; // NET_SSL_MAXINSTANCES
constexpr uint32_t kSslErrId = uint32_t(-8);

// SO receive/send flags.
constexpr uint32_t kMsgOutOfBand = 1, kMsgPeek = 2, kMsgNonBlocking = 4;

// The console's (fixed) wireless MAC address.
constexpr uint8_t kMac[6]{0x00, 0x17, 0xAB, 0x4D, 0x48, 0x33};

struct Socket {
    int handle = -1;
    bool stream = false;
    bool nonBlocking = false;
    bool connecting = false; // a connect under way
};

std::array<std::optional<Socket>, kMaxSockets> sockets;

struct TlsSession {
    std::unique_ptr<NetHost::Tls> tls;
    int32_t socket = -1; // the Wii socket it runs over
};

std::array<std::optional<TlsSession>, kMaxTlsSessions> sessions;

// The NCD configuration (/shared2/sys/net/02/config.dat): a wired connection
// with DHCP that passed the connection test.
constexpr uint32_t kConnectionBytes = 0x91C, kNetConfigBytes = 8 + 3 * kConnectionBytes;
std::array<uint8_t, kNetConfigBytes> netConfig = [] {
    std::array<uint8_t, kNetConfigBytes> config{};
    config[4] = 2;    // connType: wired
    config[8] = 0xA7; // connection 0: wired, DNS and IP by DHCP, test passed, selected
    return config;
}();

uint32_t ncdLock = 0;

// /dev/net/kd/request (WiiConnect24, KD) requests.
enum : uint32_t {
    kKdSuspendScheduler = 0x01,
    kKdTrySuspendScheduler = 0x02,
    kKdResumeScheduler = 0x03,
    kKdGetTimeTriggers = 0x04,
    kKdSetScheduleSpan = 0x05,
    kKdStartupSocket = 0x06,
    kKdCleanupSocket = 0x07,
    kKdLockSocket = 0x08,
    kKdUnlockSocket = 0x09,
    kKdGetSchedulerStat = 0x1E,
    kKdSetScriptMode = 0x22,
    kKdRequestShutdown = 0x28,
};

// Whether the online library started (it opened the network configuration
// or the socket device): only then does KD answer. Offline, the game keeps
// finding no WiiConnect24 daemon, as it always did here.
bool onlineStarted = false;

// A request that may have to wait: its attempt returns the result, or
// nothing while it would block. Attempts repeat until one returns.
using Attempt = std::function<std::optional<int32_t>()>;

struct Pending {
    Attempt attempt;
    uint32_t callback, argument;
};

std::vector<Pending> pending;
bool traceNet = std::getenv("MH3_NET_TRACE") != nullptr;

// A request that is done at once.
Attempt Done(int32_t result) {
    return [result] {
        return std::optional<int32_t>(result);
    };
}

int32_t RunSync(CpuContext* cpu, Attempt attempt) {
    std::optional<int32_t> result = attempt();

    if (!result)
        GuestThreads::BlockUntil(cpu, [&] {
            result = attempt();
            return result.has_value();
        });

    return *result;
}

void RunAsync(Attempt attempt, uint32_t callback, uint32_t argument) {
    if (const auto result = attempt()) {
        QueueIosCompletion({callback, *result, argument});
        return;
    }

    pending.push_back({std::move(attempt), callback, argument});
}

Socket* Find(int32_t fd) {
    return fd >= 0 && fd < kMaxSockets && sockets[fd] ? &*sockets[fd] : nullptr;
}

// A Wii sockaddr_in: len, family, port and address (network byte order).
Address ReadAddress(uint32_t address) {
    Address result;
    std::memcpy(&result.port, Memory::GetPointer(address + 2, 2), 2);
    std::memcpy(&result.ip, Memory::GetPointer(address + 4, 4), 4);
    return result;
}

void WriteAddress(uint32_t address, uint32_t size, const Address& value) {
    if (size >= 1)
        Memory::Write8(address, uint8_t(std::min<uint32_t>(size, 8)));

    if (size >= 2)
        Memory::Write8(address + 1, 2); // AF_INET

    if (size >= 4)
        std::memcpy(Memory::GetPointer(address + 2, 2), &value.port, 2);

    if (size >= 8)
        std::memcpy(Memory::GetPointer(address + 4, 4), &value.ip, 4);
}

std::string ReadString(uint32_t address, uint32_t limit) {
    std::string text;

    for (uint32_t i = 0; i < limit; ++i) {
        const char c = char(Memory::Read8(address + i));

        if (!c)
            break;

        text += c;
    }

    return text;
}

// Byte order: the hosts are little-endian, the Wii's network order is big.
uint16_t Swap16(uint16_t value) {
    return __builtin_bswap16(value);
}

uint32_t Swap32(uint32_t value) {
    return __builtin_bswap32(value);
}

std::string Dotted(uint32_t ip) {
    const auto* bytes = reinterpret_cast<const uint8_t*>(&ip);
    char text[16];
    std::snprintf(text, sizeof(text), "%u.%u.%u.%u", bytes[0], bytes[1], bytes[2], bytes[3]);
    return text;
}

// A guest socket call that waits on a blocking socket: `once` runs the call,
// and while it reports -SO_EAGAIN the attempt waits (blocking sockets) or
// returns it (non-blocking sockets and SO_MSG_NONBLOCK).
Attempt Retrying(int32_t fd, bool nonBlocking, std::function<int32_t(Socket&)> once) {
    return [fd, nonBlocking, once = std::move(once)]() -> std::optional<int32_t> {
        Socket* socket = Find(fd);

        if (!socket)
            return -NetHost::kSoEBadF;

        const int32_t result = once(*socket);

        if (result == -NetHost::kSoEAgain && !nonBlocking && !socket->nonBlocking)
            return std::nullopt;

        return result;
    };
}

Attempt ConnectAttempt(int32_t fd, Address address) {
    auto started = std::make_shared<bool>(false);
    return [fd, address, started]() -> std::optional<int32_t> {
        Socket* socket = Find(fd);

        if (!socket)
            return -NetHost::kSoEBadF;

        if (!*started) {
            *started = true;

            if (traceNet)
                std::fprintf(stderr, "MH3 net: socket %d connects to %s:%u\n", fd, Dotted(address.ip).c_str(), Swap16(address.port));

            if (socket->connecting) {
                // Another connect while one is under way.
                const int32_t result = NetHost::ConnectResult(socket->handle);

                if (result == -NetHost::kSoEInProgress)
                    return socket->nonBlocking ? std::optional<int32_t>(-NetHost::kSoEAlready) : std::nullopt;

                socket->connecting = false;
                return result == 0 && socket->nonBlocking ? -NetHost::kSoEIsConn : result;
            }

            const int32_t result = NetHost::Connect(socket->handle, address);

            if (result == -NetHost::kSoEIsConn && !socket->nonBlocking)
                return 0;

            if (result != -NetHost::kSoEInProgress && result != -NetHost::kSoEAgain)
                return result;

            socket->connecting = true;

            if (socket->nonBlocking)
                return -NetHost::kSoEInProgress;
        }

        // A blocking connect waits for the result.
        const int32_t result = NetHost::ConnectResult(socket->handle);

        if (result == -NetHost::kSoEInProgress)
            return std::nullopt;

        socket->connecting = false;
        return result;
    };
}

// IOCTL_SO_POLL: waits up to the timeout (milliseconds; negative: no limit).
Attempt Poll(uint32_t in, uint32_t out, uint32_t outSize) {
    const int64_t timeout = int64_t(Memory::Read64(in));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout < 0 ? 0 : timeout);
    const uint32_t count = outSize / 12;
    return [=]() -> std::optional<int32_t> {
        if (count == 0 || count > kMaxSockets)
            return -NetHost::kSoEInval;

        std::vector<NetHost::PollEntry> entries;

        for (uint32_t i = 0; i < count; ++i) {
            const Socket* socket = Find(int32_t(Memory::Read32(out + i * 12)));
            entries.push_back({socket ? socket->handle : -1, Memory::Read32(out + i * 12 + 4), 0});
        }

        const int32_t ready = NetHost::Poll(entries);

        if (ready == 0 && (timeout < 0 || std::chrono::steady_clock::now() < deadline))
            return std::nullopt;

        for (uint32_t i = 0; i < count; ++i)
            Memory::Write32(out + i * 12 + 8, entries[i].revents);

        return ready;
    };
}

// IOCTL_SO_GETHOSTBYNAME: a hostent in the 0x460-byte output, laid out as
// IOS does (the SO library converts it with fixed offsets).
Attempt GetHostByName(uint32_t in, uint32_t out, uint32_t outSize) {
    auto lookup = std::make_shared<NetHost::Lookup>(ReadString(in, 256));
    return [=]() -> std::optional<int32_t> {
        if (!lookup->Done())
            return std::nullopt;

        const auto& addresses = lookup->Addresses();

        if (traceNet)
            std::fprintf(stderr, "MH3 net: gethostbyname %s: %s\n", lookup->Name().c_str(), addresses.empty() ? "not found" : Dotted(addresses[0]).c_str());

        if (outSize != 0x460 || addresses.empty())
            return -1;

        constexpr uint32_t kNameOffset = 0x10, kAddressOffset = 0x110, kPointerOffset = 0x340;
        const std::string& name = lookup->Name();

        if (name.size() + 1 > kAddressOffset - kNameOffset)
            return -1;

        std::memset(Memory::GetPointer(out, outSize), 0, outSize);
        std::memcpy(Memory::GetPointer(out + kNameOffset, uint32_t(name.size() + 1)), name.c_str(), name.size() + 1);
        const uint32_t count = uint32_t(std::min<size_t>(addresses.size(), 71));

        for (uint32_t i = 0; i < count; ++i) {
            std::memcpy(Memory::GetPointer(out + kAddressOffset + i * 4, 4), &addresses[i], 4);
            Memory::Write32(out + kPointerOffset + i * 4, out + kAddressOffset + i * 4);
        }

        Memory::Write32(out + kPointerOffset + count * 4, 0);
        Memory::Write32(out + 0x0, out + kNameOffset);                // h_name
        Memory::Write32(out + 0x4, out + kPointerOffset + count * 4); // h_aliases: none
        Memory::Write16(out + 0x8, 2);                                // h_addrtype: AF_INET
        Memory::Write16(out + 0xA, 4);                                // h_length
        Memory::Write32(out + 0xC, out + kPointerOffset);             // h_addr_list
        return 0;
    };
}

std::optional<Attempt> IpTopIoctl(uint32_t command, uint32_t in, uint32_t inSize, uint32_t out, uint32_t outSize) {

    switch (command) {
    case kSoStartup:
    case kSoCleanup:
    case kSoInitInterface:
    case kSoSetInterface:
    case kSoSetInterfaceOpt:
        return Done(0);

    case kSoSocket: {
        const uint32_t family = Memory::Read32(in), type = Memory::Read32(in + 4), protocol = Memory::Read32(in + 8);

        if (family != 2)
            return Done(-5); // SO_EAFNOSUPPORT

        if (protocol != 0)
            return Done(-NetHost::kSoEProtoNoSupport);

        if (type != 1 && type != 2)
            return Done(-NetHost::kSoEProtoType);

        int32_t fd = 0;

        while (fd < kMaxSockets && sockets[fd])
            ++fd;

        if (fd == kMaxSockets)
            return Done(-NetHost::kSoEMFile);

        const int32_t handle = NetHost::OpenSocket(type == 1);

        if (handle < 0)
            return Done(handle);

        sockets[fd] = Socket{handle, type == 1};

        if (traceNet)
            std::fprintf(stderr, "MH3 net: socket %d (%s)\n", fd, type == 1 ? "TCP" : "UDP");

        return Done(fd);
    }

    case kSoClose: {
        const int32_t fd = int32_t(Memory::Read32(in));
        Socket* socket = Find(fd);

        if (!socket)
            return Done(-NetHost::kSoEBadF);

        const int32_t result = NetHost::CloseSocket(socket->handle);
        sockets[fd].reset();

        for (auto& session : sessions)
            if (session && session->socket == fd)
                session->socket = -1;

        return Done(result);
    }

    case kSoBind: {
        Socket* socket = Find(int32_t(Memory::Read32(in)));
        return Done(socket ? NetHost::Bind(socket->handle, ReadAddress(in + 8)) : -NetHost::kSoEBadF);
    }

    case kSoConnect:
        return ConnectAttempt(int32_t(Memory::Read32(in)), ReadAddress(in + 8));

    case kSoAccept: {
        const int32_t fd = int32_t(Memory::Read32(in));
        return Retrying(fd, false, [=](Socket& socket) -> int32_t {
            Address peer;
            const int32_t handle = NetHost::Accept(socket.handle, &peer);

            if (handle < 0)
                return handle;

            int32_t accepted = 0;

            while (accepted < kMaxSockets && sockets[accepted])
                ++accepted;

            if (accepted == kMaxSockets) {
                NetHost::CloseSocket(handle);
                return -NetHost::kSoEMFile;
            }

            sockets[accepted] = Socket{handle, true};

            if (outSize)
                WriteAddress(out, outSize, peer);

            return accepted;
        });
    }

    case kSoFcntl: {
        Socket* socket = Find(int32_t(Memory::Read32(in)));

        if (!socket)
            return Done(-NetHost::kSoEBadF);

        const uint32_t cmd = Memory::Read32(in + 4), argument = Memory::Read32(in + 8);

        if (cmd == 3) // F_GETFL
            return Done(socket->nonBlocking ? 4 : 0);

        if (cmd == 4) // F_SETFL: O_NONBLOCK is 4
            socket->nonBlocking = (argument & 4) != 0;

        return Done(0);
    }

    case kSoShutdown: {
        Socket* socket = Find(int32_t(Memory::Read32(in)));
        const uint32_t how = Memory::Read32(in + 4);

        if (!socket)
            return Done(-NetHost::kSoEBadF);

        if (how > 2)
            return Done(-NetHost::kSoEInval);

        // IOS does nothing for UDP.
        return Done(socket->stream ? NetHost::Shutdown(socket->handle, int(how)) : 0);
    }

    case kSoListen: {
        Socket* socket = Find(int32_t(Memory::Read32(in)));
        return Done(socket ? NetHost::Listen(socket->handle, int(Memory::Read32(in + 4))) : -NetHost::kSoEBadF);
    }

    case kSoGetSockOpt: {
        // The parameters come in the output buffer: fd, level, name, length, value.
        Socket* socket = Find(int32_t(Memory::Read32(out)));

        if (!socket)
            return Done(-NetHost::kSoEBadF);

        uint8_t value[8]{};
        uint32_t size = 0;
        const int32_t result = NetHost::GetOption(socket->handle, Memory::Read32(out + 4), Memory::Read32(out + 8), value, &size);
        Memory::Write32(out + 0xC, size);

        if (outSize >= 0x10 + size)
            std::memcpy(Memory::GetPointer(out + 0x10, size), value, size);

        return Done(result);
    }

    case kSoSetSockOpt: {
        Socket* socket = Find(int32_t(Memory::Read32(in)));

        if (!socket)
            return Done(-NetHost::kSoEBadF);

        const uint32_t size = std::min<uint32_t>(Memory::Read32(in + 0xC), 20);
        return Done(NetHost::SetOption(socket->handle, Memory::Read32(in + 4), Memory::Read32(in + 8), Memory::GetPointer(in + 0x10, size), size));
    }

    case kSoGetSockName:
    case kSoGetPeerName: {
        Socket* socket = Find(int32_t(Memory::Read32(in)));

        if (!socket)
            return Done(-NetHost::kSoEBadF);

        Address address;
        const int32_t result = command == kSoGetSockName ? NetHost::LocalAddress(socket->handle, &address) : NetHost::PeerAddress(socket->handle, &address);

        if (result == 0)
            WriteAddress(out, outSize, address);

        return Done(result);
    }

    case kSoGetHostId:
        return Done(int32_t(Swap32(NetHost::DefaultInterface().ip)));

    case kSoGetHostByName:
        return GetHostByName(in, out, outSize);

    case kSoInetAtoN: {
        auto lookup = std::make_shared<NetHost::Lookup>(ReadString(in, inSize ? inSize : 256));
        return Attempt([=]() -> std::optional<int32_t> {
            if (!lookup->Done())
                return std::nullopt;

            if (lookup->Addresses().empty())
                return 0;

            const uint32_t ip = lookup->Addresses()[0];
            std::memcpy(Memory::GetPointer(out, 4), &ip, 4);
            return 1;
        });
    }

    case kSoInetPtoN: {
        uint32_t ip = 0;

        if (!NetHost::ParseIpv4(ReadString(in, inSize ? inSize : 16), &ip))
            return Done(0);

        std::memcpy(Memory::GetPointer(out + 4, 4), &ip, 4);
        return Done(1);
    }

    case kSoInetNtoP: {
        uint32_t ip;
        std::memcpy(&ip, Memory::GetPointer(in + 8, 4), 4);
        const std::string text = Dotted(ip);
        std::memcpy(Memory::GetPointer(out, uint32_t(text.size())), text.data(), text.size());
        return Done(0);
    }

    case kSoPoll:
        if (!in || !out)
            return Done(-NetHost::kSoEInval);

        return Poll(in, out, outSize);

    default:
        return std::nullopt;
    }
}

struct Vector {
    uint32_t address, size;
};

std::vector<Vector> Vectors(uint32_t count, uint32_t table) {
    std::vector<Vector> vectors;

    for (uint32_t i = 0; i < count; ++i)
        vectors.push_back({Memory::Read32(table + i * 8), Memory::Read32(table + i * 8 + 4)});

    return vectors;
}

// IOCTLV_SO_GETINTERFACEOPT: in[0] = {0xFFFE, option}, out[0] = value,
// out[1] = its size.
int32_t GetInterfaceOption(const std::vector<Vector>& in, const std::vector<Vector>& out) {
    if (in.empty() || out.size() < 2)
        return kIpcEInval;

    if (Memory::Read32(in[0].address) != 0xFFFE)
        return -51;

    const uint32_t option = Memory::Read32(in[0].address + 4), value = out[0].address;
    const auto net = NetHost::DefaultInterface();

    switch (option) {
    case 0xB003: // DNS servers
        Memory::Write32(value, Swap32(net.dns));
        Memory::Write32(value + 4, 0x08080404);
        break;
    case 0x1003: // error
        Memory::Write32(value, 0);
        break;
    case 0x1004: // MAC address
        std::memcpy(Memory::GetPointer(value, 6), kMac, 6);
        break;
    case 0x1005: // link state
        Memory::Write32(value, 1);
        break;
    case 0x3001:
        Memory::Write32(value, 0x10);
        break;
    case 0x4002: // number of IP addresses
        Memory::Write32(value, 1);
        break;
    case 0x4003: // IP address table
        Memory::Write32(out[1].address, 0xC);
        Memory::Write32(value, Swap32(net.ip));
        Memory::Write32(value + 4, Swap32(net.netmask));
        Memory::Write32(value + 8, Swap32(net.broadcast));
        break;
    case 0x4005: // routing table size
        Memory::Write32(value, 0x20);
        break;
    case 0x4006: { // routing table: the default route
        const uint32_t offset = out[0].size >= 8 ? Memory::Read32(value + 4) : 0;
        Memory::Write32(value + offset, 0);
        Memory::Write32(value + offset + 4, 0);
        Memory::Write32(value + offset + 8, Swap32(net.gateway));
        Memory::Write32(value + offset + 12, 1);
        Memory::Write64(value + offset + 16, 0);
        Memory::Write32(out[1].address, offset + 24);
        break;
    }
    case 0x6003:
    case 0x600A:
    case 0x600C:
        Memory::Write32(value, 0x80);
        break;
    case 0xB002:
        Memory::Write32(value, 2);
        break;
    default:
        std::fprintf(stderr, "MH3 net: unknown interface option 0x%X\n", option);
        break;
    }

    return 0;
}

std::optional<Attempt> IpTopIoctlv(uint32_t command, const std::vector<Vector>& in, const std::vector<Vector>& out) {

    switch (command) {
    case kSoStartup:
    case kSoCleanup:
        return Done(0);

    case kSoGetInterfaceOpt:
        return Done(GetInterfaceOption(in, out));

    case kSoSendTo: {
        // in[0]: data; in[1]: fd, flags, has destination, sockaddr.
        if (in.size() < 2)
            return Done(kIpcEInval);

        const uint32_t parameters = in[1].address, flags = Memory::Read32(parameters + 4);
        const bool hasDestination = Memory::Read32(parameters + 8) != 0;
        const Address destination = hasDestination ? ReadAddress(parameters + 0xC) : Address{};
        const Vector data = in[0];
        return Retrying(int32_t(Memory::Read32(parameters)), (flags & kMsgNonBlocking) != 0, [=](Socket& socket) {
            // A connection under way takes no data yet.
            if (socket.stream && socket.connecting) {
                if (NetHost::ConnectResult(socket.handle) == -NetHost::kSoEInProgress)
                    return int32_t(-NetHost::kSoEAgain);

                socket.connecting = false;
            }

            return NetHost::Send(socket.handle, Memory::GetPointer(data.address, data.size), data.size, flags & kMsgOutOfBand, hasDestination ? &destination : nullptr);
        });
    }

    case kSoRecvFrom: {
        // in[0]: fd, flags; out[0]: data; out[1]: sockaddr (optional).
        if (in.empty() || out.empty())
            return Done(kIpcEInval);

        const uint32_t flags = Memory::Read32(in[0].address + 4);
        const Vector data = out[0], from = out.size() > 1 ? out[1] : Vector{0, 0};
        return Retrying(int32_t(Memory::Read32(in[0].address)), (flags & kMsgNonBlocking) != 0, [=](Socket& socket) {
            if (socket.stream && socket.connecting) {
                if (NetHost::ConnectResult(socket.handle) == -NetHost::kSoEInProgress)
                    return int32_t(-NetHost::kSoEAgain);

                socket.connecting = false;
            }

            Address sender;
            const int32_t result = NetHost::Receive(socket.handle, Memory::GetPointer(data.address, data.size), data.size, flags & kMsgPeek, flags & kMsgOutOfBand, from.size ? &sender : nullptr);

            if (result >= 0 && from.size)
                WriteAddress(from.address, from.size, sender);

            return result;
        });
    }

    case kSoGetAddrInfo: {
        // in[0]: node, in[1]: service, in[2]: hints; out[0]: addrinfo list.
        if (in.empty() || out.empty() || !in[0].size)
            return Done(-305);

        auto lookup = std::make_shared<NetHost::Lookup>(ReadString(in[0].address, in[0].size));
        const uint16_t port = in.size() > 1 && in[1].size ? Swap16(uint16_t(std::atoi(ReadString(in[1].address, in[1].size).c_str()))) : 0;
        const uint32_t list = out[0].address;
        return Attempt([=]() -> std::optional<int32_t> {
            if (!lookup->Done())
                return std::nullopt;

            const auto& addresses = lookup->Addresses();

            if (addresses.empty())
                return -305; // SO_ERROR_HOST_NOT_FOUND

            uint32_t entry = list, address = list + 0x460;
            const size_t count = std::min<size_t>(addresses.size(), 8);

            for (size_t i = 0; i < count; ++i, entry += 0x20, address += 0x1C) {
                Memory::Write32(entry, 0);         // flags
                Memory::Write32(entry + 0x4, 2);   // AF_INET
                Memory::Write32(entry + 0x8, 1);   // SOCK_STREAM
                Memory::Write32(entry + 0xC, 0);   // protocol
                Memory::Write32(entry + 0x10, 16); // address length
                Memory::Write32(entry + 0x14, 0);  // canonical name
                Memory::Write32(entry + 0x18, address);
                Memory::Write32(entry + 0x1C, i + 1 < count ? entry + 0x20 : 0);
                std::memset(Memory::GetPointer(address, 0x1C), 0, 0x1C);
                WriteAddress(address, 8, {addresses[i], port});
            }

            return 0;
        });
    }

    default:
        return std::nullopt;
    }
}

// SSL requests write their result into in[0]; out[0] holds the session id
// (one-based) for every request but NEW.
std::optional<Attempt> SslIoctlv(uint32_t command, const std::vector<Vector>& in, const std::vector<Vector>& out) {

    if (in.empty() || out.empty())
        return Done(kIpcEInval);

    const uint32_t reply = in[0].address;
    auto answer = [reply](int32_t value) -> Attempt {
        return [reply, value] {
            Memory::Write32(reply, uint32_t(value));
            return std::optional<int32_t>(0);
        };
    };

    if (command == kSslNew) {
        // out[0]: verify option; out[1]: the server's host name.
        const bool verify = Memory::Read32(out[0].address) != 0;
        const std::string hostname = out.size() > 1 ? ReadString(out[1].address, out[1].size) : std::string{};
        int slot = 0;

        while (slot < kMaxTlsSessions && sessions[slot])
            ++slot;

        auto tls = slot < kMaxTlsSessions ? NetHost::Tls::Create(hostname, verify) : nullptr;

        if (!tls)
            return answer(NetHost::Tls::kFailed);

        sessions[slot] = TlsSession{std::move(tls), -1};

        if (traceNet)
            std::fprintf(stderr, "MH3 net: TLS session %d for %s (verify %d)\n", slot + 1, hostname.c_str(), verify);

        return answer(slot + 1);
    }

    const int32_t id = int32_t(Memory::Read32(out[0].address)) - 1;
    TlsSession* session = id >= 0 && id < kMaxTlsSessions && sessions[id] ? &*sessions[id] : nullptr;

    if (!session)
        return answer(int32_t(kSslErrId));

    switch (command) {
    case kSslShutdown:
        session->tls->Close();
        sessions[id].reset();
        return answer(NetHost::Tls::kOk);

    case kSslSetRootCa:
        if (out.size() < 2)
            return Done(kIpcEInval);

        return answer(session->tls->AddRootCa(Memory::GetPointer(out[1].address, out[1].size), out[1].size) ? NetHost::Tls::kOk : NetHost::Tls::kFailed);

    case kSslSetBuiltinClientCert:
    case kSslSetClientCert:
    case kSslSetClientCertDefault:
    case kSslRemoveClientCert:
    case kSslSetRootCaDefault:
    case kSslSetBuiltinRootCa:
    case kSslDisableVerify:
        // No Nintendo client certificate or built-in root: the MH3SP servers
        // need neither.
        return answer(NetHost::Tls::kOk);

    case kSslConnect: {
        if (out.size() < 2)
            return Done(kIpcEInval);

        const int32_t fd = int32_t(Memory::Read32(out[1].address));
        const Socket* socket = Find(fd);

        if (!socket)
            return answer(NetHost::Tls::kFailed);

        session->socket = fd;
        session->tls->Attach(socket->handle);
        return answer(NetHost::Tls::kOk);
    }

    case kSslHandshake:
    case kSslHandshakeEx:
    case kSslRead:
    case kSslWrite: {
        const Vector data = command == kSslRead ? (in.size() > 1 ? in[1] : Vector{}) : (out.size() > 1 ? out[1] : Vector{});
        return Attempt([=]() -> std::optional<int32_t> {
            TlsSession* current = sessions[id] ? &*sessions[id] : nullptr;
            const Socket* socket = current ? Find(current->socket) : nullptr;

            if (!socket) {
                Memory::Write32(reply, uint32_t(int32_t(NetHost::Tls::kSyscall)));
                return 0;
            }

            // A handshake waits for the socket's connection first.
            if (socket->connecting && NetHost::ConnectResult(socket->handle) == -NetHost::kSoEInProgress) {
                if (socket->nonBlocking) {
                    Memory::Write32(reply, uint32_t(int32_t(NetHost::Tls::kWantRead)));
                    return 0;
                }

                return std::nullopt;
            }

            int32_t result;

            if (command == kSslRead)
                result = current->tls->Read(Memory::GetPointer(data.address, data.size), data.size);
            else if (command == kSslWrite)
                result = current->tls->Write(Memory::GetPointer(data.address, data.size), data.size);
            else {
                result = current->tls->Handshake();

                if (traceNet && result != NetHost::Tls::kWantRead && result != NetHost::Tls::kWantWrite)
                    std::fprintf(stderr, "MH3 net: TLS session %d handshake: %d\n", id + 1, result);
            }

            if ((result == NetHost::Tls::kWantRead || result == NetHost::Tls::kWantWrite) && !socket->nonBlocking)
                return std::nullopt;

            Memory::Write32(reply, uint32_t(result));
            return 0;
        });
    }

    default:
        return std::nullopt;
    }
}

std::optional<Attempt> NcdIoctlv(uint32_t command, const std::vector<Vector>& in, const std::vector<Vector>& out) {
    uint32_t common = 0, reply = 0;

    switch (command) {
    case kNcdLockWirelessDriver:
        if (out.empty() || out[0].size < 8)
            return Done(kIpcEInval);

        if (ncdLock)
            common = uint32_t(kIpcEInval);
        else
            Memory::Write32(out[0].address + 4, ncdLock = 1);

        break;
    case kNcdUnlockWirelessDriver:
        if (in.empty() || out.empty())
            return Done(kIpcEInval);

        common = Memory::Read32(in[0].address) == ncdLock ? (ncdLock = 0) : uint32_t(-3);
        break;
    case kNcdGetConfig:
    case kNcdReadConfig:
        if (out.size() < (command == kNcdGetConfig ? 2u : 1u) || out[0].size < kNetConfigBytes)
            return Done(kIpcEInval);

        std::memcpy(Memory::GetPointer(out[0].address, kNetConfigBytes), netConfig.data(), kNetConfigBytes);
        reply = command == kNcdGetConfig ? 1 : 0;
        break;
    case kNcdSetConfig:
    case kNcdWriteConfig:
        if (in.empty() || in[0].size < kNetConfigBytes)
            return Done(kIpcEInval);

        std::memcpy(netConfig.data(), Memory::GetPointer(in[0].address, kNetConfigBytes), kNetConfigBytes);
        break;
    case kNcdGetLinkStatus:
        if (out.empty())
            return Done(kIpcEInval);

        Memory::Write32(out[0].address + 4, 3); // LINK_WIRED
        break;
    case kNcdGetWirelessMacAddress:
        if (out.size() < 2)
            return Done(kIpcEInval);

        std::memcpy(Memory::GetPointer(out[1].address, 6), kMac, 6);
        break;
    default:
        return std::nullopt;
    }

    if (out.size() <= reply)
        return Done(kIpcEInval);

    Memory::Write32(out[reply].address, common);

    if (reply == 1)
        Memory::Write32(out[reply].address + 4, common);

    return Done(0);
}

void Unknown(std::string_view path, const char* kind, uint32_t command) {
    std::fprintf(stderr, "MH3 net: %.*s %s 0x%X has no host implementation\n", int(path.size()), path.data(), kind, command);
}

// KD keeps no mail or download schedule here; SOStartup needs its socket
// startup (KD owns the network interface on the Wii).
std::optional<Attempt> KdIoctl(uint32_t command, uint32_t out, uint32_t outSize) {
    auto reply = [&](int32_t value) {
        if (outSize >= 4)
            Memory::Write32(out, uint32_t(value));
    };

    switch (command) {
    case kKdSuspendScheduler:
    case kKdResumeScheduler:
    case kKdSetScheduleSpan:
    case kKdSetScriptMode:
        reply(0);
        return Done(0);
    case kKdStartupSocket:
        reply(0);

        if (outSize >= 8)
            Memory::Write32(out + 4, 0);

        return Done(0);
    case kKdGetTimeTriggers:
        if (outSize < 12)
            return Done(kIpcEInval);

        reply(0);
        Memory::Write32(out + 4, 0);
        Memory::Write32(out + 8, 0);
        return Done(0);
    case kKdGetSchedulerStat:
        if (outSize < 16)
            return Done(kIpcEInval);

        std::memset(Memory::GetPointer(out, std::min<uint32_t>(outSize, 256)), 0, std::min<uint32_t>(outSize, 256));
        return Done(0);
    case kKdTrySuspendScheduler:
    case kKdCleanupSocket:
    case kKdLockSocket:
    case kKdUnlockSocket:
    case kKdRequestShutdown:
        return Done(0);
    default:
        return std::nullopt;
    }
}

Attempt IoctlAttempt(std::string_view path, uint32_t command, uint32_t in, uint32_t inSize, uint32_t out, uint32_t outSize) {
    if (path == "/dev/net/ip/top")
        if (auto attempt = IpTopIoctl(command, in, inSize, out, outSize))
            return *attempt;

    if (path == "/dev/net/kd/request")
        if (auto attempt = KdIoctl(command, out, outSize))
            return *attempt;

    Unknown(path, "ioctl", command);
    return [] {
        return std::optional<int32_t>(0);
    };
}

Attempt IoctlvAttempt(std::string_view path, uint32_t command, uint32_t inputs, uint32_t outputs, uint32_t table) {
    const auto all = Vectors(inputs + outputs, table);
    const std::vector<Vector> in(all.begin(), all.begin() + inputs), out(all.begin() + inputs, all.end());
    std::optional<Attempt> attempt;

    if (path == "/dev/net/ip/top")
        attempt = IpTopIoctlv(command, in, out);
    else if (path == "/dev/net/ssl")
        attempt = SslIoctlv(command, in, out);
    else if (path == "/dev/net/ncd/manage")
        attempt = NcdIoctlv(command, in, out);

    if (attempt)
        return *attempt;

    Unknown(path, "ioctlv", command);
    return [] {
        return std::optional<int32_t>(0);
    };
}
} // namespace

namespace IosNet {
bool IsDevice(std::string_view path) {
    return path == "/dev/net/ip/top" || path == "/dev/net/ssl" || path == "/dev/net/ncd/manage" || path == "/dev/net/kd/request";
}

int32_t Open(std::string_view path) {
    if (!IsDevice(path))
        return kIpcENoEnt;

    if (path == "/dev/net/ip/top" && !NetHost::Startup())
        return kIpcENoEnt;

    if (path == "/dev/net/kd/request" && !onlineStarted)
        return kIpcENoEnt;

    if (path == "/dev/net/ncd/manage" || path == "/dev/net/ip/top")
        onlineStarted = true;

    if (traceNet)
        std::fprintf(stderr, "MH3 net: open %.*s\n", int(path.size()), path.data());

    return 0;
}

void Close(std::string_view path) {
    // IOS keeps sockets and sessions to the SO library's SOCleanup.
    (void)path;
}

int32_t Ioctl(CpuContext* cpu, std::string_view path, uint32_t command, uint32_t in, uint32_t inSize, uint32_t out, uint32_t outSize) {
    const int32_t result = RunSync(cpu, IoctlAttempt(path, command, in, inSize, out, outSize));

    if (traceNet)
        std::fprintf(stderr, "MH3 net: %.*s ioctl %u = %d\n", int(path.size()), path.data(), command, result);

    return result;
}

int32_t Ioctlv(CpuContext* cpu, std::string_view path, uint32_t command, uint32_t inputs, uint32_t outputs, uint32_t vectors) {
    const int32_t result = RunSync(cpu, IoctlvAttempt(path, command, inputs, outputs, vectors));

    // The interface options are asked every frame online.
    if (traceNet && !(path == "/dev/net/ip/top" && command == kSoGetInterfaceOpt))
        std::fprintf(stderr, "MH3 net: %.*s ioctlv %u = %d\n", int(path.size()), path.data(), command, result);

    return result;
}

void IoctlAsync(std::string_view path, uint32_t command, uint32_t in, uint32_t inSize, uint32_t out, uint32_t outSize, uint32_t callback, uint32_t argument) {
    if (traceNet)
        std::fprintf(stderr, "MH3 net: %.*s ioctl %u (asynchronous)\n", int(path.size()), path.data(), command);

    RunAsync(IoctlAttempt(path, command, in, inSize, out, outSize), callback, argument);
}

void IoctlvAsync(std::string_view path, uint32_t command, uint32_t inputs, uint32_t outputs, uint32_t vectors, uint32_t callback, uint32_t argument) {
    if (traceNet)
        std::fprintf(stderr, "MH3 net: %.*s ioctlv %u (asynchronous)\n", int(path.size()), path.data(), command);

    RunAsync(IoctlvAttempt(path, command, inputs, outputs, vectors), callback, argument);
}

void Service() {
    for (auto it = pending.begin(); it != pending.end();) {
        if (const auto result = it->attempt()) {
            QueueIosCompletion({it->callback, *result, it->argument});
            it = pending.erase(it);
        } else
            ++it;
    }
}
} // namespace IosNet
