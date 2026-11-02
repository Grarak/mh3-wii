// SPDX-License-Identifier: AGPL-3.0-or-later
// GameSpy's NAT negotiation server, as much of it as the embedded server needs
// (local_server.h; the game's side is gamespy_natneg.cpp). Before two hunters
// link, each sends INIT from its game socket (port type 0) and its negotiation
// socket (1 to 3, once for each of three servers, which are all this one),
// with a cookie they agreed on. Each INIT gets an INITACK; with both ends in,
// each gets a CONNECT saying where the other's game socket is, resent until
// it answers. The two then find each other and report the result.
#include "local_server.h"

#include <array>
#include <chrono>
#include <cstring>
#include <iterator>
#include <map>
#include <optional>

namespace LocalServer {
namespace {
using Clock = std::chrono::steady_clock;

constexpr uint8_t kMagic[]{0xFD, 0xFC, 0x1E, 0x66, 0x6A, 0xB2};

enum Type : uint8_t { kInit = 0, kInitAck = 1, kConnect = 5, kConnectAck = 6, kReport = 13, kReportAck = 14 };

// A packet: magic, version, type, the cookie, then by type.
enum Offset : size_t { kOffsetType = 7, kOffsetCookie = 8, kOffsetPortType = 12, kOffsetClient = 13, kOffsetUseGamePort = 14, kOffsetRemoteIp = 12, kOffsetRemotePort = 16, kOffsetGotData = 18, kOffsetFinished = 19 };

constexpr size_t kHeaderBytes = 12, kInitBytes = 21, kConnectBytes = 20, kPortTypes = 4, kClients = 2;
constexpr uint8_t kGotData = 0x42;
constexpr auto kResend = std::chrono::milliseconds(500);
constexpr auto kForget = std::chrono::seconds(30);

struct Negotiator {
    std::array<std::optional<Address>, kPortTypes> sockets;
    bool useGamePort = false, connectAcked = false;

    bool Ready() const { return sockets[1] && (!useGamePort || sockets[0]); }

    const Address& GameSocket() const { return useGamePort ? *sockets[0] : *sockets[1]; }
};

struct Negotiation {
    std::array<uint8_t, kHeaderBytes> header{};
    std::array<Negotiator, kClients> clients;
    Clock::time_point connected{}, started = Clock::now();
};

std::map<uint32_t, Negotiation> negotiations;

void SendConnect(Datagrams& socket, const Negotiation& negotiation, size_t client) {
    const Negotiator& to = negotiation.clients[client];

    if (to.connectAcked)
        return;

    const Address& partner = negotiation.clients[client ^ 1].GameSocket();
    uint8_t packet[kConnectBytes]{};
    std::memcpy(packet, negotiation.header.data(), kHeaderBytes);
    packet[kOffsetType] = kConnect;
    std::memcpy(packet + kOffsetRemoteIp, &partner.ip, 4);
    std::memcpy(packet + kOffsetRemotePort, &partner.port, 2);
    packet[kOffsetGotData] = kGotData;
    packet[kOffsetFinished] = 0;
    socket.Send(packet, sizeof(packet), *to.sockets[1]);
}

void Init(Datagrams& socket, const uint8_t* data, const Address& from) {
    uint8_t ack[kInitBytes]{};
    std::memcpy(ack, data, kOffsetUseGamePort);
    ack[kOffsetType] = kInitAck;
    socket.Send(ack, sizeof(ack), from);
    const uint8_t type = data[kOffsetPortType], client = data[kOffsetClient];

    if (type >= kPortTypes || client >= kClients)
        return;

    uint32_t cookie;
    std::memcpy(&cookie, data + kOffsetCookie, 4);
    Negotiation& negotiation = negotiations[cookie];
    std::memcpy(negotiation.header.data(), data, kHeaderBytes);
    Negotiator& negotiator = negotiation.clients[client];
    negotiator.sockets[type] = from;
    negotiator.useGamePort = data[kOffsetUseGamePort];
}

void Receive(Datagrams& socket, const uint8_t* data, size_t size, const Address& from) {
    if (size < kHeaderBytes || std::memcmp(data, kMagic, sizeof(kMagic)))
        return;

    const uint8_t type = data[kOffsetType];

    if (type == kInit && size >= kInitBytes) {
        Init(socket, data, from);
    } else if (type == kConnectAck && size > kOffsetClient) {
        uint32_t cookie;
        std::memcpy(&cookie, data + kOffsetCookie, 4);

        if (const auto found = negotiations.find(cookie); found != negotiations.end() && data[kOffsetClient] < kClients)
            found->second.clients[data[kOffsetClient]].connectAcked = true;
    } else if (type == kReport && size >= kInitBytes) {
        uint8_t ack[kInitBytes];
        std::memcpy(ack, data, sizeof(ack));
        ack[kOffsetType] = kReportAck;
        ack[kOffsetUseGamePort] = 0;
        socket.Send(ack, sizeof(ack), from);
    }
}
} // namespace

void PollNatNegotiation(Datagrams& socket) {
    uint8_t data[128];
    Address from;

    while (const int count = socket.Receive(data, sizeof(data), from))
        Receive(socket, data, size_t(count), from);

    const auto now = Clock::now();

    for (auto entry = negotiations.begin(); entry != negotiations.end();) {
        Negotiation& negotiation = entry->second;
        auto& clients = negotiation.clients;

        if (clients[0].Ready() && clients[1].Ready() && now - negotiation.connected >= kResend) {
            SendConnect(socket, negotiation, 0);
            SendConnect(socket, negotiation, 1);
            negotiation.connected = now;
        }

        const bool done = clients[0].connectAcked && clients[1].connectAcked;
        entry = done || now - negotiation.started >= kForget ? negotiations.erase(entry) : std::next(entry);
    }
}
} // namespace LocalServer
