// SPDX-License-Identifier: AGPL-3.0-or-later
// The embedded server's sockets, BSD sockets (local_server.h).
#include "local_server.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#ifdef __vita__
// sceNet has no SIGPIPE.
#define MSG_NOSIGNAL 0
#endif

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

class UdpSocket final : public Datagrams {
  public:
    explicit UdpSocket(int socket) : handle(socket) {}

    ~UdpSocket() override { ::close(handle); }

    int Receive(uint8_t* data, size_t size, Address& from) override {
        sockaddr_in source{};
        socklen_t length = sizeof(source);
        const auto count = ::recvfrom(handle, data, size, 0, reinterpret_cast<sockaddr*>(&source), &length);

        if (count <= 0)
            return 0;

        from = {source.sin_addr.s_addr, source.sin_port};
        return int(count);
    }

    void Send(const uint8_t* data, size_t size, const Address& to) override {
        sockaddr_in target{};
        target.sin_family = AF_INET;
        target.sin_port = to.port;
        target.sin_addr.s_addr = to.ip;
        ::sendto(handle, data, size, 0, reinterpret_cast<const sockaddr*>(&target), sizeof(target));
    }

    void Broadcast(const uint8_t* data, size_t size, uint16_t port) override { Send(data, size, {htonl(INADDR_BROADCAST), htons(port)}); }

  private:
    int handle;
};
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

std::unique_ptr<Datagrams> Bind(uint16_t port) {
    const int handle = ::socket(AF_INET, SOCK_DGRAM, 0);

    if (handle < 0)
        return nullptr;

    const int on = 1;
    ::setsockopt(handle, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
    const sockaddr_in address = Any(port);

    if (::bind(handle, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(handle);
        return nullptr;
    }

    NonBlocking(handle);
    return std::make_unique<UdpSocket>(handle);
}
} // namespace LocalServer
