// SPDX-License-Identifier: AGPL-3.0-or-later
// The embedded server's sockets on BSD sockets (local_server.h).
#include "local_server.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace LocalServer {
namespace {
bool Blocked() {
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
}

void NonBlocking(int handle) {
    fcntl(handle, F_SETFL, fcntl(handle, F_GETFL, 0) | O_NONBLOCK);
}

class Stream final : public Connection {
  public:
    explicit Stream(int socket) : handle(socket) {}

    ~Stream() override { ::close(handle); }

    int Send(const uint8_t* data, size_t size) override {
        const auto sent = ::send(handle, data, size, MSG_NOSIGNAL);
        return sent >= 0 ? int(sent) : Blocked() ? 0 : -1;
    }

    int Receive(uint8_t* data, size_t size) override {
        const auto count = ::recv(handle, data, size, 0);
        return count > 0 ? int(count) : count == 0 ? -1 : Blocked() ? 0 : -1;
    }

  private:
    int handle;
};

class TcpListener final : public Listener {
  public:
    explicit TcpListener(int socket) : handle(socket) {}

    ~TcpListener() override { ::close(handle); }

    std::unique_ptr<Connection> Accept() override {
        const int client = ::accept(handle, nullptr, nullptr);

        if (client < 0)
            return nullptr;

        NonBlocking(client);
        return std::make_unique<Stream>(client);
    }

  private:
    int handle;
};

sockaddr_in Any(uint16_t port) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    return address;
}

int availability = -1;
} // namespace

std::unique_ptr<Listener> Listen(uint16_t port) {
    const int handle = ::socket(AF_INET, SOCK_STREAM, 0);

    if (handle < 0)
        return nullptr;

    const int on = 1;
    ::setsockopt(handle, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    const sockaddr_in address = Any(port);

    if (::bind(handle, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 || ::listen(handle, 8) != 0) {
        ::close(handle);
        return nullptr;
    }

    NonBlocking(handle);
    return std::make_unique<TcpListener>(handle);
}

void PollAvailability() {
    if (availability < 0) {
        availability = ::socket(AF_INET, SOCK_DGRAM, 0);
        const sockaddr_in address = Any(kAvailabilityPort);

        if (availability < 0 || ::bind(availability, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0)
            return;

        NonBlocking(availability);
    }

    uint8_t data[256];
    sockaddr_in from{};
    socklen_t size = sizeof(from);
    const auto count = ::recvfrom(availability, data, sizeof(data), 0, reinterpret_cast<sockaddr*>(&from), &size);

    // 0x09, four zero bytes, the game's name: the backend is available.
    if (count > 0 && data[0] == 0x09) {
        static constexpr uint8_t kAvailable[]{0xFE, 0xFD, 0x09, 0, 0, 0, 0};
        ::sendto(availability, kAvailable, sizeof(kAvailable), 0, reinterpret_cast<const sockaddr*>(&from), size);
    }
}
} // namespace LocalServer
