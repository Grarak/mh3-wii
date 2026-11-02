// SPDX-License-Identifier: AGPL-3.0-or-later
// The embedded multiplayer server: a C++ port of the MH3SP server project's
// OPN, LMP and FMP servers (https://github.com/sepalani/MH3SP, AGPL-3.0-or-later;
// the local_server files keep that license), with the account check the game
// makes first. One player hosts it; the others connect to that player.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace LocalServer {
// A stream to a client, non-blocking both ways.
class Connection {
  public:
    virtual ~Connection() = default;

    // Bytes moved, 0 when none can move now, < 0 when the connection ended.
    virtual int Send(const uint8_t* data, size_t size) = 0;
    virtual int Receive(uint8_t* data, size_t size) = 0;
};

class Listener {
  public:
    virtual ~Listener() = default;

    // A new connection, or null.
    virtual std::unique_ptr<Connection> Accept() = 0;
};

enum Port : uint16_t { kOpnPort = 8200, kLmpPort = 8201, kFmpPort = 8202, kNasPort = 18080, kAnnouncePort = 12419, kAvailabilityPort = 27900, kNatNegotiationPort = 27901 };

// Listening sockets. BSD sockets: the Vita's ad hoc network carries them too.
std::unique_ptr<Listener> Listen(uint16_t port);

// A datagram's other end, in network byte order.
struct Address {
    uint32_t ip = 0;
    uint16_t port = 0;
};

// A non-blocking datagram socket on `port` (0: any), or null.
class Datagrams {
  public:
    virtual ~Datagrams() = default;

    // Bytes read, 0 when none came.
    virtual int Receive(uint8_t* data, size_t size, Address& from) = 0;
    virtual void Send(const uint8_t* data, size_t size, const Address& to) = 0;
    virtual void Broadcast(const uint8_t* data, size_t size, uint16_t port) = 0;
};

std::unique_ptr<Datagrams> Bind(uint16_t port);

// GameSpy's NAT negotiation: pairs the two ends of a link by their cookie and
// tells each where the other is. Called on the server's thread.
void PollNatNegotiation(Datagrams& socket);

// The first host announcing itself on the network within `seconds`: its
// address in network byte order, or 0.
uint32_t FindHost(int seconds);

// Starts the server on its own thread. `address` is what clients connect to:
// the host's address as they see it.
bool Start(const std::string& address);
void Stop();
bool Running();
} // namespace LocalServer
