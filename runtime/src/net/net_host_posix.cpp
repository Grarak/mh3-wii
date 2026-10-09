// NetHost (net_host.h) on BSD sockets, getaddrinfo and OpenSSL: Linux, and
// the Vita, whose newlib implements them on sceNet (its first socket starts
// the network stack: _vita_net_init loads SceNet and runs sceNetInit and
// sceNetCtlInit).
#include "net_host.h"

#include "guest_threads.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <mutex>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <string_view>
#include <sys/socket.h>
#include <pthread.h>
#include <unistd.h>
#include <vector>

#ifdef __vita__
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>
#else
#include <ifaddrs.h>
#include <resolv.h>
#endif

#ifdef MH3_HAS_OPENSSL
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#endif

#ifdef __vita__
// sceNet has no SIGPIPE.
#define MSG_NOSIGNAL 0

extern "C" int _vita_net_init();
#endif
namespace NetHost {
namespace {
// A host errno as a negated Wii SO error (Dolphin's TranslateErrorCode).
int32_t Error(int error) {
    switch (error) {
    case EBADF:
    case ENOTSOCK:
        return -kSoEBadF;
    case EADDRINUSE:
        return -kSoEAddrInUse;
    case ECONNRESET:
        return -kSoEConnReset;
    case EISCONN:
        return -kSoEIsConn;
    case ENOTCONN:
        return -kSoENotConn;
    case EINPROGRESS:
        return -kSoEInProgress;
    case EALREADY:
        return -kSoEAlready;
    case ECONNREFUSED:
        return -kSoEConnRefused;
    case ENETUNREACH:
        return -kSoENetUnreach;
    case EHOSTUNREACH:
        return -kSoEHostUnreach;
    case ENOMEM:
    case ENOBUFS:
        return -kSoENoMem;
    case ETIMEDOUT:
        return -kSoETimedOut;
    case EAGAIN:
        return -kSoEAgain;
    case EINVAL:
        return -kSoEInval;
    default:
        return -1;
    }
}

int32_t Result(int result) {
    return result < 0 ? Error(errno) : result;
}

// MH3_ONLINE_PORTS (from=to[,from=to...]): destination ports to change, so a
// local test server can stand in for one on a privileged port (the account
// server's 80).
std::mutex overrideLock;
std::vector<std::pair<uint16_t, uint16_t>> portOverrides;

uint16_t MapPort(uint16_t port) {
    const unsigned wanted = ntohs(port);

    {
        std::lock_guard guard(overrideLock);

        for (const auto& [from, to] : portOverrides)
            if (from == wanted)
                return htons(to);
    }

    const char* ports = std::getenv("MH3_ONLINE_PORTS");

    if (!ports)
        return port;

    std::string_view rest(ports);

    while (!rest.empty()) {
        const auto comma = rest.find(',');
        const std::string entry(rest.substr(0, comma));
        rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
        unsigned from = 0, to = 0;

        if (std::sscanf(entry.c_str(), "%u=%u", &from, &to) == 2 && from == wanted)
            return htons(uint16_t(to));
    }

    return port;
}

sockaddr_in Native(const Address& address) {
    sockaddr_in native{};
    native.sin_family = AF_INET;
    native.sin_addr.s_addr = address.ip;
    native.sin_port = MapPort(address.port);
    return native;
}

Address FromNative(const sockaddr_in& native) {
    return {native.sin_addr.s_addr, native.sin_port};
}

// A Wii socket option as the host's; false for one this layer drops.
bool NativeOption(uint32_t level, uint32_t name, int* nativeLevel, int* nativeName) {
    if (level == 6) { // IPPROTO_TCP
        *nativeLevel = IPPROTO_TCP;

        if (name == 0x2001) {
            *nativeName = TCP_NODELAY;
            return true;
        }

        return false; // 0x2005 and others: IOS-only timeouts
    }

    if (level != 0xFFFF)
        return false;

    *nativeLevel = SOL_SOCKET;

    switch (name) {
    case 0x0004:
        *nativeName = SO_REUSEADDR;
        return true;
    case 0x0008:
        *nativeName = SO_KEEPALIVE;
        return true;
    case 0x0020:
        *nativeName = SO_BROADCAST;
        return true;
    case 0x0080:
        *nativeName = SO_LINGER;
        return true;
    case 0x0100:
        *nativeName = SO_OOBINLINE;
        return true;
    case 0x1001:
        *nativeName = SO_SNDBUF;
        return true;
    case 0x1002:
        *nativeName = SO_RCVBUF;
        return true;
    case 0x1003:
        *nativeName = SO_SNDLOWAT;
        return true;
    case 0x1004:
        *nativeName = SO_RCVLOWAT;
        return true;
    case 0x1008:
        *nativeName = SO_TYPE;
        return true;
    case 0x1009:
        *nativeName = SO_ERROR;
        return true;
    default:
        return false;
    }
}

uint32_t Swap32(uint32_t value) {
    return __builtin_bswap32(value);
}

struct HostOverride {
    std::string pattern;
    uint32_t ip;
};

std::vector<HostOverride> hostOverrides;

bool MatchesHost(std::string_view pattern, std::string_view name) {
    return pattern == name || (pattern.starts_with("*.") && name.ends_with(pattern.substr(1)));
}

// AddHostOverride's entries, then MH3_ONLINE_HOSTS: name=address entries,
// "*.domain" for every host in it.
bool Override(const std::string& name, uint32_t* ip) {
    {
        std::lock_guard guard(overrideLock);

        for (const auto& entry : hostOverrides)
            if (MatchesHost(entry.pattern, name)) {
                *ip = entry.ip;
                return true;
            }
    }

    const char* hosts = std::getenv("MH3_ONLINE_HOSTS");

    if (!hosts)
        return false;

    std::string_view rest(hosts);

    while (!rest.empty()) {
        const auto comma = rest.find(',');
        const std::string_view entry = rest.substr(0, comma);
        rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
        const auto equals = entry.find('=');

        if (equals == std::string_view::npos)
            continue;

        const std::string_view pattern = entry.substr(0, equals);
        const std::string address(entry.substr(equals + 1));

        if (MatchesHost(pattern, name) && inet_pton(AF_INET, address.c_str(), ip) == 1)
            return true;
    }

    return false;
}
} // namespace

void AddHostOverride(std::string pattern, uint32_t ip) {
    std::lock_guard guard(overrideLock);
    hostOverrides.push_back({std::move(pattern), ip});
}

void AddPortOverride(uint16_t from, uint16_t to) {
    std::lock_guard guard(overrideLock);
    portOverrides.emplace_back(from, to);
}

bool Startup() {
#ifdef __vita__
    // newlib's own sceNetInit pool (about 140 KiB) runs out with the game's
    // sockets: the stack then stops taking (and acknowledging) segments for
    // minutes. Initialized first, newlib keeps this one.
    static const bool started = [] {
        constexpr int kPoolBytes = 2 << 20;

        if (sceSysmoduleIsLoaded(SCE_SYSMODULE_NET) != SCE_SYSMODULE_LOADED && sceSysmoduleLoadModule(SCE_SYSMODULE_NET) < 0)
            return false;

        SceNetInitParam parameters{};
        parameters.memory = std::malloc(kPoolBytes);
        parameters.size = kPoolBytes;
        const int initialized = sceNetInit(&parameters);

        if (initialized < 0)
            std::fprintf(stderr, "MH3 net: sceNetInit 0x%08X\n", unsigned(initialized));

        return _vita_net_init() >= 0;
    }();
    return started;
#else
    return true;
#endif
}

int32_t OpenSocket(bool stream) {
    const int handle = ::socket(AF_INET, stream ? SOCK_STREAM : SOCK_DGRAM, 0);

    if (handle < 0)
        return Error(errno);

    const int flags = ::fcntl(handle, F_GETFL, 0);
    ::fcntl(handle, F_SETFL, (flags < 0 ? 0 : flags) | O_NONBLOCK);

    // Wii UDP sockets may send to the broadcast address by default.
    if (!stream) {
        const int on = 1;
        ::setsockopt(handle, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
    }

    return handle;
}

int32_t CloseSocket(int handle) {
    return Result(::close(handle));
}

int32_t Bind(int handle, const Address& address) {
    const sockaddr_in native = Native(address);
    return Result(::bind(handle, reinterpret_cast<const sockaddr*>(&native), sizeof(native)));
}

int32_t Connect(int handle, const Address& address) {
    const sockaddr_in native = Native(address);
    return Result(::connect(handle, reinterpret_cast<const sockaddr*>(&native), sizeof(native)));
}

int32_t ConnectResult(int handle) {
    pollfd entry{handle, POLLOUT, 0};

    if (::poll(&entry, 1, 0) <= 0)
        return -kSoEInProgress;

    int error = 0;
    socklen_t size = sizeof(error);

    if (::getsockopt(handle, SOL_SOCKET, SO_ERROR, &error, &size) != 0)
        return Error(errno);

    return error ? Error(error) : 0;
}

int32_t Listen(int handle, int backlog) {
    return Result(::listen(handle, backlog));
}

int32_t Accept(int handle, Address* peer) {
    sockaddr_in native{};
    socklen_t size = sizeof(native);
    const int accepted = ::accept(handle, reinterpret_cast<sockaddr*>(&native), &size);

    if (accepted < 0)
        return Error(errno);

    const int flags = ::fcntl(accepted, F_GETFL, 0);
    ::fcntl(accepted, F_SETFL, (flags < 0 ? 0 : flags) | O_NONBLOCK);

    if (peer)
        *peer = FromNative(native);

    return accepted;
}

int32_t Shutdown(int handle, int how) {
    return Result(::shutdown(handle, how));
}

int32_t Send(int handle, const uint8_t* data, size_t size, bool outOfBand, const Address* to) {
    const int flags = MSG_NOSIGNAL | (outOfBand ? MSG_OOB : 0);

    if (!to)
        return Result(int(::send(handle, data, size, flags)));

    const sockaddr_in native = Native(*to);
    return Result(int(::sendto(handle, data, size, flags, reinterpret_cast<const sockaddr*>(&native), sizeof(native))));
}

int32_t Receive(int handle, uint8_t* data, size_t size, bool peek, bool outOfBand, Address* from) {
    const int flags = (peek ? MSG_PEEK : 0) | (outOfBand ? MSG_OOB : 0);
    sockaddr_in native{};
    socklen_t length = sizeof(native);
    const auto received = ::recvfrom(handle, data, size, flags, from ? reinterpret_cast<sockaddr*>(&native) : nullptr, from ? &length : nullptr);

    if (received < 0)
        return Error(errno);

    if (from)
        *from = FromNative(native);

    return int32_t(received);
}

int32_t LocalAddress(int handle, Address* address) {
    sockaddr_in native{};
    socklen_t size = sizeof(native);

    if (::getsockname(handle, reinterpret_cast<sockaddr*>(&native), &size) != 0)
        return Error(errno);

    *address = FromNative(native);
    return 0;
}

int32_t PeerAddress(int handle, Address* address) {
    sockaddr_in native{};
    socklen_t size = sizeof(native);

    if (::getpeername(handle, reinterpret_cast<sockaddr*>(&native), &size) != 0)
        return Error(errno);

    *address = FromNative(native);
    return 0;
}

int32_t SetOption(int handle, uint32_t level, uint32_t name, const uint8_t* value, uint32_t size) {
    int nativeLevel = 0, nativeName = 0;

    if (!NativeOption(level, name, &nativeLevel, &nativeName))
        return 0;

    // Options are big-endian ints (linger: two of them).
    uint32_t words[2]{};
    const uint32_t count = std::min<uint32_t>(size / 4, 2);

    for (uint32_t i = 0; i < count; ++i) {
        uint32_t word;
        std::memcpy(&word, value + i * 4, 4);
        words[i] = Swap32(word);
    }

    return Result(::setsockopt(handle, nativeLevel, nativeName, words, count * 4));
}

int32_t GetOption(int handle, uint32_t level, uint32_t name, uint8_t* value, uint32_t* size) {
    int nativeLevel = 0, nativeName = 0;
    uint32_t words[2]{};

    if (NativeOption(level, name, &nativeLevel, &nativeName)) {
        socklen_t length = sizeof(words);

        if (::getsockopt(handle, nativeLevel, nativeName, words, &length) != 0)
            return Error(errno);

        if (nativeName == SO_ERROR && nativeLevel == SOL_SOCKET)
            words[0] = words[0] ? uint32_t(-Error(int(words[0]))) : 0;

        *size = std::min<uint32_t>(length, 8);
    } else
        *size = 4;

    for (uint32_t i = 0; i < *size / 4; ++i) {
        const uint32_t word = Swap32(words[i]);
        std::memcpy(value + i * 4, &word, 4);
    }

    return 0;
}

int32_t Poll(std::vector<PollEntry>& entries) {
    constexpr struct {
        short native;
        uint32_t wii;
    } kEvents[]{{POLLRDNORM, 0x01}, {POLLRDBAND, 0x02}, {POLLPRI, 0x04}, {POLLWRNORM, 0x08}, {POLLWRBAND, 0x10}, {POLLERR, 0x20}, {POLLHUP, 0x40}, {POLLNVAL, 0x80}};
    std::vector<pollfd> natives;

    for (const auto& entry : entries) {
        short events = 0;

        for (const auto& event : kEvents)
            if (entry.events & event.wii)
                events |= event.native;

        natives.push_back({entry.handle, short(events & ~(POLLERR | POLLHUP | POLLNVAL)), 0});
    }

    const int ready = ::poll(natives.data(), nfds_t(natives.size()), 0);

    if (ready < 0)
        return Error(errno);

    int32_t count = 0;

    for (size_t i = 0; i < entries.size(); ++i) {
        uint32_t revents = 0;

        if (entries[i].handle < 0)
            revents = 0x80;

        for (const auto& event : kEvents)
            if (natives[i].revents & event.native)
                revents |= event.wii;

        // Only the events asked for, and the conditions always reported (the
        // Vita's poll.h makes POLLPRI and POLLRDBAND POLLIN).
        entries[i].revents = revents & (entries[i].events | 0xE0);
        count += entries[i].revents != 0;
    }

    return count;
}

bool Readable(int handle) {
    pollfd entry{handle, POLLIN, 0};
    return ::poll(&entry, 1, 0) > 0;
}

bool Writable(int handle) {
    pollfd entry{handle, POLLOUT, 0};
    return ::poll(&entry, 1, 0) > 0;
}

#ifdef __vita__
Interface DefaultInterface() {
    Interface result{};
    inet_pton(AF_INET, "10.0.1.30", &result.ip);
    inet_pton(AF_INET, "255.255.255.0", &result.netmask);
    inet_pton(AF_INET, "10.0.1.1", &result.gateway);
    inet_pton(AF_INET, "8.8.8.8", &result.dns);
    Startup();

    const auto read = [](int code, uint32_t* address) {
        SceNetCtlInfo info{};

        if (sceNetCtlInetGetInfo(code, &info) == 0)
            inet_pton(AF_INET, info.ip_address, address);
    };

    read(SCE_NETCTL_INFO_GET_IP_ADDRESS, &result.ip);
    read(SCE_NETCTL_INFO_GET_NETMASK, &result.netmask);
    read(SCE_NETCTL_INFO_GET_DEFAULT_ROUTE, &result.gateway);
    read(SCE_NETCTL_INFO_GET_PRIMARY_DNS, &result.dns);
    result.broadcast = (result.ip & result.netmask) | ~result.netmask;
    return result;
}
#else
Interface DefaultInterface() {
    Interface result{};
    inet_pton(AF_INET, "10.0.1.30", &result.ip);
    inet_pton(AF_INET, "255.255.255.0", &result.netmask);
    inet_pton(AF_INET, "10.0.1.255", &result.broadcast);
    inet_pton(AF_INET, "10.0.1.1", &result.gateway);
    inet_pton(AF_INET, "8.8.8.8", &result.dns);

    if (res_init() == 0)
        for (int i = 0; i < _res.nscount; ++i)
            if (_res.nsaddr_list[i].sin_family == AF_INET) {
                result.dns = _res.nsaddr_list[i].sin_addr.s_addr;
                break;
            }

    // The address the host would use to reach the Internet; no packet is sent.
    const int probe = ::socket(AF_INET, SOCK_DGRAM, 0);

    if (probe < 0)
        return result;

    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_port = htons(53);
    inet_pton(AF_INET, "8.8.8.8", &target.sin_addr);
    sockaddr_in local{};
    socklen_t size = sizeof(local);
    const bool found = ::connect(probe, reinterpret_cast<sockaddr*>(&target), sizeof(target)) == 0 && ::getsockname(probe, reinterpret_cast<sockaddr*>(&local), &size) == 0;
    ::close(probe);

    if (!found)
        return result;

    ifaddrs* interfaces = nullptr;

    if (getifaddrs(&interfaces) != 0)
        return result;

    for (const ifaddrs* entry = interfaces; entry; entry = entry->ifa_next) {
        if (!entry->ifa_addr || entry->ifa_addr->sa_family != AF_INET)
            continue;

        if (reinterpret_cast<const sockaddr_in*>(entry->ifa_addr)->sin_addr.s_addr != local.sin_addr.s_addr)
            continue;

        result.ip = local.sin_addr.s_addr;

        if (entry->ifa_netmask)
            result.netmask = reinterpret_cast<const sockaddr_in*>(entry->ifa_netmask)->sin_addr.s_addr;

        result.broadcast = (result.ip & result.netmask) | ~result.netmask;
        result.gateway = (result.ip & result.netmask) | htonl(1);
        break;
    }

    freeifaddrs(interfaces);
    return result;
}
#endif

bool ParseIpv4(const std::string& text, uint32_t* ip) {
    return inet_pton(AF_INET, text.c_str(), ip) == 1;
}

namespace {
// One DNS A query over UDP to SERVER (network order), for when the system's
// resolver fails: the Vita devkit's answered every name with ECONNREFUSED
// (getaddrinfo -5, oct07-tls2) while its network worked.
bool QueryDns(const std::string& name, uint32_t server, std::vector<uint32_t>& addresses) {
    uint8_t packet[512] = {0x4d, 0x33, 0x01, 0x00, 0x00, 0x01}; // id, recursion desired, one question
    size_t size = 12;

    for (size_t start = 0; start <= name.size();) {
        const size_t dot = std::min(name.find('.', start), name.size());
        const size_t length = dot - start;

        if (length == 0 || length > 63 || size + length + 6 > sizeof(packet))
            return false;

        packet[size++] = uint8_t(length);
        std::memcpy(packet + size, name.data() + start, length);
        size += length;
        start = dot + 1;
    }

    const uint8_t question[] = {0, 0, 1, 0, 1}; // end of name, type A, class IN
    std::memcpy(packet + size, question, sizeof(question));
    size += sizeof(question);
    const int handle = ::socket(AF_INET, SOCK_DGRAM, 0);

    if (handle < 0)
        return false;

    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(53);
    to.sin_addr.s_addr = server;
    pollfd entry{handle, POLLIN, 0};
    uint8_t reply[512];
    ssize_t got = -1;

    if (::sendto(handle, packet, size, 0, reinterpret_cast<const sockaddr*>(&to), sizeof(to)) == ssize_t(size) && ::poll(&entry, 1, 2000) > 0)
        got = ::recv(handle, reply, sizeof(reply), 0);

    ::close(handle);

    // The same id, a response, no error, at least one answer.
    if (got < 12 || reply[0] != packet[0] || reply[1] != packet[1] || !(reply[2] & 0x80) || (reply[3] & 0x0f) || !(reply[6] | reply[7]))
        return false;

    const auto skipName = [&](size_t at) -> size_t {
        while (at < size_t(got)) {
            if ((reply[at] & 0xc0) == 0xc0)
                return at + 2;

            if (!reply[at])
                return at + 1;

            at += reply[at] + 1u;
        }

        return size_t(got);
    };

    size_t at = skipName(12) + 4;

    for (unsigned answers = reply[6] << 8 | reply[7]; answers && at + 10 <= size_t(got); --answers) {
        at = skipName(at);

        if (at + 10 > size_t(got))
            break;

        const unsigned type = reply[at] << 8 | reply[at + 1], length = reply[at + 8] << 8 | reply[at + 9];
        at += 10;

        if (type == 1 && length == 4 && at + 4 <= size_t(got)) {
            uint32_t address;
            std::memcpy(&address, reply + at, 4);
            addresses.push_back(address);
        }

        at += length;
    }

    return !addresses.empty();
}
} // namespace

struct Lookup::State {
    std::mutex lock;
    bool done = false;
    std::string name;
    std::vector<uint32_t> addresses;
};

Lookup::Lookup(std::string name) : state_(std::make_shared<State>()) {
    state_->name = std::move(name);
    uint32_t ip = 0;

    if (Override(state_->name, &ip) || inet_pton(AF_INET, state_->name.c_str(), &ip) == 1) {
        state_->addresses.push_back(ip);
        state_->done = true;
        return;
    }

    // getaddrinfo blocks; the guest thread waits in the scheduler meanwhile.
    // A detached pthread, not std::thread: without exceptions libc++ aborts
    // the process when a thread call fails, which on the Vita stopped the
    // lookup thread (and its TTY) for good.
    const auto resolve = [](const std::shared_ptr<State>& state) {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* found = nullptr;
        std::vector<uint32_t> addresses;

        if (const int failed = getaddrinfo(state->name.c_str(), nullptr, &hints, &found); failed == 0) {
            for (const addrinfo* entry = found; entry; entry = entry->ai_next)
                if (entry->ai_family == AF_INET)
                    addresses.push_back(reinterpret_cast<const sockaddr_in*>(entry->ai_addr)->sin_addr.s_addr);

            freeaddrinfo(found);
        } else {
            // The game shows only that the host was not found (-305).
            const int error = errno;
            uint32_t servers[] = {DefaultInterface().dns, 0, 0};
            inet_pton(AF_INET, "1.1.1.1", &servers[1]);
            inet_pton(AF_INET, "8.8.8.8", &servers[2]);
            char text[INET_ADDRSTRLEN] = "";

            for (const uint32_t server : servers)
                if (server && QueryDns(state->name, server, addresses)) {
                    inet_ntop(AF_INET, &server, text, sizeof(text));
                    break;
                }

            std::fprintf(stderr, "MH3 net: lookup of %s failed: %d (errno %d)%s%s\n", state->name.c_str(), failed, error,
                         addresses.empty() ? "; no DNS server answered either" : "; answered by DNS server ", text);
        }

        {
            std::lock_guard guard(state->lock);
            state->addresses = std::move(addresses);
            state->done = true;
        }

        GuestThreads::Notify();
    };

    struct Job {
        std::shared_ptr<State> state;
        void (*resolve)(const std::shared_ptr<State>&);
    };

    auto* job = new Job{state_, resolve};
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, 256u << 10);
    pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    pthread_t thread;
    const int failed = pthread_create(
        &thread, &attributes,
        [](void* argument) -> void* {
            std::unique_ptr<Job> job(static_cast<Job*>(argument));
            job->resolve(job->state);
            return nullptr;
        },
        job);
    pthread_attr_destroy(&attributes);

    // No thread: resolve here, blocking the guest thread instead.
    if (failed) {
        std::unique_ptr<Job> owned(job);
        resolve(state_);
    }
}

Lookup::~Lookup() = default;

bool Lookup::Done() const {
    std::lock_guard guard(state_->lock);
    return state_->done;
}

const std::vector<uint32_t>& Lookup::Addresses() const {
    return state_->addresses;
}

const std::string& Lookup::Name() const {
    return state_->name;
}

#ifdef MH3_HAS_OPENSSL
namespace {
std::string TimeText(ASN1_TIME* time) {
    BIO* text = BIO_new(BIO_s_mem());
    char* data = nullptr;

    if (!text)
        return "?";

    ASN1_TIME_print(text, time);
    const long length = BIO_get_mem_data(text, &data);
    std::string result(data && length > 0 ? data : "", data && length > 0 ? size_t(length) : 0);
    BIO_free(text);
    return result;
}

// A certificate the verification refused, with the clock it was checked
// against: the Wii reports only the kind (verify date, root CA, ...).
int LogRefusedCertificate(int ok, X509_STORE_CTX* store) {
    if (ok)
        return ok;

    X509* certificate = X509_STORE_CTX_get_current_cert(store);
    char subject[256] = "?";
    std::string from = "?", to = "?";

    if (certificate) {
        X509_NAME_oneline(X509_get_subject_name(certificate), subject, sizeof(subject));
        from = TimeText(X509_get_notBefore(certificate));
        to = TimeText(X509_get_notAfter(certificate));
    }

    const time_t now = std::time(nullptr);
    tm utc{};
    char clock[32] = "?";

    if (gmtime_r(&now, &utc))
        std::strftime(clock, sizeof(clock), "%Y-%m-%d %H:%M:%S", &utc);

    std::fprintf(stderr, "MH3 net: TLS certificate %s at depth %d refused: %s (valid %s to %s; now %s UTC)\n", subject, X509_STORE_CTX_get_error_depth(store),
                 X509_verify_cert_error_string(X509_STORE_CTX_get_error(store)), from.c_str(), to.c_str(), clock);
    return ok;
}

class OpenSslTls final : public Tls {
  public:
    OpenSslTls(const std::string& hostname, bool verify) : hostname_(hostname), verify_(verify) {
#if OPENSSL_VERSION_NUMBER < 0x10100000L
        // 1.1 initializes itself (the Vita has 1.0.2).
        static std::once_flag initialized;
        std::call_once(initialized, [] {
            SSL_library_init();
            SSL_load_error_strings();
        });
        context_ = SSL_CTX_new(SSLv23_client_method());
#else
        context_ = SSL_CTX_new(TLS_client_method());

        // The Wii's servers use SHA-1 and 1024-bit RSA certificates.
        SSL_CTX_set_security_level(context_, 0);
#endif
        SSL_CTX_set_verify(context_, verify ? SSL_VERIFY_PEER : SSL_VERIFY_NONE, verify ? LogRefusedCertificate : nullptr);
        session_ = SSL_new(context_);

        // The Wii checks the server's common name against the name it was
        // given, and sends no server name indication.
        if (verify && !hostname.empty())
            X509_VERIFY_PARAM_set1_host(SSL_get0_param(session_), hostname.c_str(), 0);
    }

    ~OpenSslTls() override {
        SSL_free(session_);
        SSL_CTX_free(context_);
    }

    bool AddRootCa(const uint8_t* der, size_t size) override {
        const unsigned char* cursor = der;
        X509* certificate = d2i_X509(nullptr, &cursor, long(size));

        if (!certificate)
            return false;

        const bool added = X509_STORE_add_cert(SSL_CTX_get_cert_store(context_), certificate) == 1;
        X509_free(certificate);
        return added;
    }

    void Attach(int handle) override { SSL_set_fd(session_, handle); }

    int32_t Handshake() override {
        const int result = SSL_connect(session_);

        if (result == 1)
            return kOk;

        const int32_t error = Failure(result);

        if (error != kFailed || !verify_)
            return error;

        // A failed verification, by what the Wii would report.
        switch (SSL_get_verify_result(session_)) {
        case X509_V_OK:
            return kFailed;
        case X509_V_ERR_HOSTNAME_MISMATCH:
            return kVerifyCommonName;
        case X509_V_ERR_CERT_HAS_EXPIRED:
        case X509_V_ERR_CERT_NOT_YET_VALID:
            return kVerifyDate;
        case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY:
        case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT:
        case X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT:
        case X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN:
            return kVerifyRootCa;
        default:
            return kVerifyChain;
        }
    }

    int32_t Write(const uint8_t* data, size_t size) override {
        const int result = SSL_write(session_, data, int(size));
        return result > 0 ? result : Failure(result);
    }

    int32_t Read(uint8_t* data, size_t size) override {
        const int result = SSL_read(session_, data, int(size));
        return result > 0 ? result : Failure(result);
    }

    void Close() override { SSL_shutdown(session_); }

  private:
    int32_t Failure(int result) {
        const int error = SSL_get_error(session_, result);
        ERR_clear_error();

        switch (error) {
        case SSL_ERROR_WANT_READ:
            return kWantRead;
        case SSL_ERROR_WANT_WRITE:
            return kWantWrite;
        case SSL_ERROR_ZERO_RETURN:
            return kZero;
        case SSL_ERROR_SYSCALL:
            return result == 0 ? kZero : kSyscall;
        default:
            return kFailed;
        }
    }

    SSL_CTX* context_ = nullptr;
    SSL* session_ = nullptr;
    std::string hostname_;
    bool verify_;
};
} // namespace

std::unique_ptr<Tls> Tls::Create(const std::string& hostname, bool verify) {
    return std::make_unique<OpenSslTls>(hostname, verify);
}

bool HasTls() {
    return true;
}
#else
std::unique_ptr<Tls> Tls::Create(const std::string&, bool) {
    return nullptr;
}

bool HasTls() {
    return false;
}
#endif
} // namespace NetHost
