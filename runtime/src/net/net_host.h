// The host side of the Wii network devices (ios_net.cpp): sockets, name
// lookups and TLS sessions on the platform's network stack: BSD sockets and
// OpenSSL on Linux and the Vita (net_host_posix.cpp), none on the web.
//
// Every socket is non-blocking on the host. A guest call that would block
// waits in the guest thread scheduler (GuestThreads::BlockUntil) and retries.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace NetHost {
// Results: >= 0 success, or a negated Wii SO error (SO_E*).
enum SoError : int32_t {
    kSoEAddrInUse = 3,
    kSoEAgain = 6,
    kSoEAlready = 7,
    kSoEBadF = 8,
    kSoEConnRefused = 14,
    kSoEConnReset = 15,
    kSoEHostUnreach = 23,
    kSoEInProgress = 26,
    kSoEInval = 28,
    kSoEIsConn = 30,
    kSoEMFile = 33,
    kSoEMsgSize = 35,
    kSoENetDown = 38,
    kSoENetUnreach = 40,
    kSoENoMem = 49,
    kSoENotConn = 56,
    kSoEProtoNoSupport = 68,
    kSoEProtoType = 69,
    kSoETimedOut = 76,
};

// An IPv4 address and port, both in network byte order.
struct Address {
    uint32_t ip = 0;
    uint16_t port = 0;
};

// Starts the platform's network stack once; false when there is none.
bool Startup();

// socket(AF_INET, SOCK_STREAM or SOCK_DGRAM): a host handle >= 0 or an error.
int32_t OpenSocket(bool stream);

int32_t CloseSocket(int handle);
int32_t Bind(int handle, const Address& address);

// Starts a connection; -kSoEInProgress while it is under way.
int32_t Connect(int handle, const Address& address);

// The pending error of a connection under way: 0 once connected,
// -kSoEInProgress while it still is, or its error.
int32_t ConnectResult(int handle);

int32_t Listen(int handle, int backlog);
int32_t Accept(int handle, Address* peer);
int32_t Shutdown(int handle, int how);
int32_t Send(int handle, const uint8_t* data, size_t size, bool outOfBand, const Address* to);
int32_t Receive(int handle, uint8_t* data, size_t size, bool peek, bool outOfBand, Address* from);
int32_t LocalAddress(int handle, Address* address);
int32_t PeerAddress(int handle, Address* address);

// Socket options, Wii level 0xFFFF (SOL_SOCKET) or 6 (TCP).
int32_t SetOption(int handle, uint32_t level, uint32_t name, const uint8_t* value, uint32_t size);
int32_t GetOption(int handle, uint32_t level, uint32_t name, uint8_t* value, uint32_t* size);

// Wii poll events (POLLRDNORM 1, POLLRDBAND 2, POLLPRI 4, POLLWRNORM 8,
// POLLWRBAND 16, POLLERR 32, POLLHUP 64, POLLNVAL 128) of each handle, now.
struct PollEntry {
    int handle; // < 0: not a socket (POLLNVAL)
    uint32_t events;
    uint32_t revents;
};

int32_t Poll(std::vector<PollEntry>& entries);

// Whether `handle` is readable / writable now (for blocking waits).
bool Readable(int handle);
bool Writable(int handle);

// The host's IPv4 address, netmask and broadcast address on its default
// route (network byte order), or a private fallback.
struct Interface {
    uint32_t ip, netmask, broadcast, gateway, dns;
};

Interface DefaultInterface();

// A dotted IPv4 address (network byte order).
bool ParseIpv4(const std::string& text, uint32_t* ip);

// The launcher's server address: host names matching `pattern` ("*.domain"
// for every host in it) resolve to `ip` (network byte order) before
// MH3_ONLINE_HOSTS and DNS, and connections to port `from` go to `to`
// instead (MH3_ONLINE_PORTS answers after them).
void AddHostOverride(std::string pattern, uint32_t ip);
void AddPortOverride(uint16_t from, uint16_t to);

// A name lookup started on a helper thread; Done() turns true once it
// finished. MH3_ONLINE_HOSTS (name=address[,name=address...], "*.domain"
// matching any host in the domain) answers first, without a lookup.
class Lookup {
  public:
    explicit Lookup(std::string name);
    ~Lookup();
    bool Done() const;
    const std::vector<uint32_t>& Addresses() const; // network byte order; empty: not found
    const std::string& Name() const;

  private:
    struct State;
    std::shared_ptr<State> state_;
};

// A TLS client session over a connected socket.
class Tls {
  public:
    enum Result : int32_t {
        kOk = 0,
        kFailed = -1,
        kWantRead = -2,
        kWantWrite = -3,
        kSyscall = -5,
        kZero = -6,
        kVerifyCommonName = -9,
        kVerifyRootCa = -10,
        kVerifyChain = -11,
        kVerifyDate = -12,
    };

    // `verify`: check the server's chain against the root CAs and its name
    // against `hostname` (sent as no SNI, as the Wii's SSL does not).
    static std::unique_ptr<Tls> Create(const std::string& hostname, bool verify);

    virtual ~Tls() = default;
    virtual bool AddRootCa(const uint8_t* der, size_t size) = 0;
    virtual void Attach(int handle) = 0;

    // kOk, kWantRead/kWantWrite (retry when the socket is ready), or an error.
    virtual int32_t Handshake() = 0;

    // Bytes written/read (> 0), kZero, kWantRead/kWantWrite or an error.
    virtual int32_t Write(const uint8_t* data, size_t size) = 0;
    virtual int32_t Read(uint8_t* data, size_t size) = 0;
    virtual void Close() = 0;

    // After the handshake: whether a certificate the server sent is signed
    // by the CA `der` (its signature only: no names, dates or chain).
    virtual bool PeerSignedBy(const uint8_t* der, size_t size) = 0;
};

// Whether TLS sessions are available in this build.
bool HasTls();
} // namespace NetHost
