// NetHost (net_host.h) for platforms without a port of it yet (the Vita, the
// web build): no network, so the game's connection attempt fails as on a Wii
// without one. The Vita's needs sceNet sockets and resolver and sceSsl (or
// OpenSSL over sceNet) behind this interface (docs/online.md).
#include "net_host.h"

namespace NetHost {
bool Startup() {
    return false;
}

int32_t OpenSocket(bool) {
    return -kSoENetDown;
}

int32_t CloseSocket(int) {
    return -kSoEBadF;
}

int32_t Bind(int, const Address&) {
    return -kSoENetDown;
}

int32_t Connect(int, const Address&) {
    return -kSoENetDown;
}

int32_t ConnectResult(int) {
    return -kSoENetDown;
}

int32_t Listen(int, int) {
    return -kSoENetDown;
}

int32_t Accept(int, Address*) {
    return -kSoENetDown;
}

int32_t Shutdown(int, int) {
    return -kSoENetDown;
}

int32_t Send(int, const uint8_t*, size_t, bool, const Address*) {
    return -kSoENetDown;
}

int32_t Receive(int, uint8_t*, size_t, bool, bool, Address*) {
    return -kSoENetDown;
}

int32_t LocalAddress(int, Address*) {
    return -kSoENetDown;
}

int32_t PeerAddress(int, Address*) {
    return -kSoENetDown;
}

int32_t SetOption(int, uint32_t, uint32_t, const uint8_t*, uint32_t) {
    return -kSoENetDown;
}

int32_t GetOption(int, uint32_t, uint32_t, uint8_t*, uint32_t*) {
    return -kSoENetDown;
}

int32_t Poll(std::vector<PollEntry>& entries) {
    for (auto& entry : entries)
        entry.revents = 0x80;

    return int32_t(entries.size());
}

bool Readable(int) {
    return true;
}

bool Writable(int) {
    return true;
}

Interface DefaultInterface() {
    // 10.0.1.30/24, gateway 10.0.1.1, DNS 8.8.8.8 (network byte order).
    return {0x1E01000A, 0x00FFFFFF, 0xFF01000A, 0x0101000A, 0x08080808};
}

bool ParseIpv4(const std::string&, uint32_t*) {
    return false;
}

struct Lookup::State {
    std::string name;
    std::vector<uint32_t> addresses;
};

Lookup::Lookup(std::string name) : state_(std::make_shared<State>()) {
    state_->name = std::move(name);
}

Lookup::~Lookup() = default;

bool Lookup::Done() const {
    return true;
}

const std::vector<uint32_t>& Lookup::Addresses() const {
    return state_->addresses;
}

const std::string& Lookup::Name() const {
    return state_->name;
}

std::unique_ptr<Tls> Tls::Create(const std::string&, bool) {
    return nullptr;
}

bool HasTls() {
    return false;
}
} // namespace NetHost
