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

enum Port : uint16_t { kOpnPort = 8200, kLmpPort = 8201, kFmpPort = 8202, kNasPort = 18080, kAvailabilityPort = 27900 };

// Listening sockets, by platform: BSD sockets or the Vita's ad hoc ones.
std::unique_ptr<Listener> Listen(uint16_t port);

// Answers GameSpy's availability check, if a datagram came.
void PollAvailability();

// Starts the server on its own thread. `address` is what clients connect to:
// the host's address as they see it.
bool Start(const std::string& address);
void Stop();
bool Running();
} // namespace LocalServer
