// GameSpy NAT negotiation (natneg.c, natify.c) in RMHP08, 0x80512B80..
// 0x80514A80, ported from the translated C++ and its Ghidra decompilation
// (gamespy.h). NNBeginNegotiationWithSocket (0x80513940) is in
// online_patches.cpp with the MH3SP host patch.
//
// A negotiation (struct _NATNegotiator, 0x54 bytes) sends INIT packets to
// the three natneg servers, waits for their INITACKs and the server's
// CONNECT with the partner's address, pings the partner until both have
// seen each other, then reports the result. Its retries run on current_time:
// INIT every 500 ms (10 tries), pings every 700 ms (7), the report every
// 1 s (5), 5 s for the partner's last ping, 60 s for the CONNECT.
//
// MH3_PORT_CHECK names the ports that send nothing (the NAT type and the
// address string).
#include "gamespy.h"

#include "port_check.h"

#include <cstring>

namespace GameSpy {

namespace {

// struct _NATNegotiator.
enum : uint32_t {
    kNegotiateSocket = 0x00,
    kGameSocket = 0x04,
    kCookie = 0x08,
    kClientIndex = 0x0C,
    kState = 0x10,
    kInitAckReceived = 0x14,
    kRetryCount = 0x24,
    kMaxRetryCount = 0x28,
    kRetryTime = 0x2C,
    kGuessedIp = 0x30,
    kGuessedPort = 0x34,
    kGotRemoteData = 0x36,
    kSendGotRemoteData = 0x37,
    kProgressCallback = 0x38,
    kCompletedCallback = 0x3C,
    kUserData = 0x40,
    kResult = 0x44,
    kConnectedSocket = 0x48,
    kRemoteAddr = 0x4C,
};

// NegotiateState.
enum : uint32_t {
    kInitSent = 0,
    kInitAcked = 1,
    kConnectPing = 2,
    kFinished = 3,
    kCanceled = 4,
    kReportSent = 5,
    kReportAcked = 6,
};

// NegotiateResult.
enum : uint32_t {
    kSuccess = 0,
    kDeadbeatPartner = 1,
    kInitTimeout = 2,
    kPingTimeout = 3,
    kUnknownError = 4,
};

// Packet types.
enum : uint32_t {
    kPacketInit = 0,
    kPacketInitAck = 1,
    kPacketErtTest = 2,
    kPacketErtAck = 3,
    kPacketConnect = 5,
    kPacketConnectAck = 6,
    kPacketConnectPing = 7,
    kPacketAddressReply = 11,
    kPacketReport = 13,
    kPacketReportAck = 14,
};

// A packet: magic (6), version, type, cookie, then by type.
enum : uint32_t {
    kPacketVersion = 6,
    kPacketType = 7,
    kPacketCookie = 8,
    kPacketPortType = 0xC,
    kPacketClientIndex = 0xD,
    kInitUseGamePort = 0xE,
    kInitLocalIp = 0xF,
    kInitLocalPort = 0x13,
    kInitGameName = 0x15,
    kConnectRemoteIp = 0xC,
    kConnectRemotePort = 0x10,
    kConnectGotYourData = 0x12,
    kConnectFinished = 0x13,
    kReportResult = 0xE,
    kReportNatType = 0xF,
    kReportMapping = 0x13,
    kReportGameName = 0x17,
    kInitPacketBytes = 0x15,
    kConnectPacketBytes = 0x14,
    kReportPacketBytes = 0x49,
};

constexpr uint32_t kVersion = 3;
constexpr uint32_t kNatnegPort = 27901;
constexpr uint32_t kMagic = 0x80796848;
constexpr uint32_t kMagicBytes = 6;

// natneg.c's globals.
constexpr uint32_t kNegotiators = 0x80797D08;
constexpr uint32_t kMatchupIps = 0x80797D0C;
constexpr uint32_t kNatifyActive = 0x80797D18;
constexpr uint32_t kMappingScheme = 0x80797D1C;
constexpr uint32_t kNatType = 0x80796858;
constexpr uint32_t kGameName = 0x80763998;
constexpr uint32_t kGameNameBytes = 0x32;
constexpr uint32_t kReceiveBuffer = 0x80764EE0;
constexpr uint32_t kReceiveBytes = 0x200;

// natify.c: its two sockets, start time, callback, reply flags and the NAT
// it measures (0xD8 bytes).
constexpr uint32_t kNatifySockets = 0x80796850;
constexpr uint32_t kNatifyStart = 0x80797D20;
constexpr uint32_t kNatifyCallback = 0x80797D24;
constexpr uint32_t kNatifyReceiveBuffer = 0x80764BC0;
constexpr uint32_t kNat = 0x80764E00;
constexpr uint32_t kNatBytes = 0xD8;
constexpr uint32_t kNatifyTimeout = 10000;

// The reply flags: an ERT reply of each kind, then an address reply for
// each mapping 0..3.
constexpr uint32_t kErtReplied1 = 0x80797D04;
constexpr uint32_t kErtReplied2 = 0x80797D00;
constexpr uint32_t kErtReplied3 = 0x80797CFC;
constexpr uint32_t kMappingReplied0 = 0x80797CF8;
constexpr uint32_t kMappingReplied1 = 0x80797CF0;
constexpr uint32_t kMappingReplied2 = 0x80797CEC;
constexpr uint32_t kMappingReplied3 = 0x80797CF4;

// struct NAT.
enum : uint32_t {
    kIpRestricted = 0x80,
    kPortRestricted = 0x84,
    kPromiscuity = 0x88,
    kNatTypeField = 0x8C,
    kMappingSchemeField = 0x90,
    kMappings = 0x94,
    kQr2Compatible = 0xD4,
};

// A mapping: private address and port, public address and port.
enum : uint32_t {
    kPrivateIp = 0x0,
    kPrivatePort = 0x4,
    kPublicIp = 0x8,
    kPublicPort = 0xC,
    kMappingBytes = 0x10,
};

// The address strings' two alternating buffers and formats.
constexpr uint32_t kAddressToggle = 0x80797CE8;
constexpr uint32_t kAddressBuffers = 0x80764DC0;
constexpr uint32_t kFormatAddressPort = 0x80796838;
constexpr uint32_t kFormatAddress = 0x80796840;
constexpr uint32_t kFormatPort = 0x80796844;

uint32_t Field(uint32_t negotiator, uint32_t field) {
    return Memory::Read32(negotiator + field);
}

void Set(uint32_t negotiator, uint32_t field, uint32_t value) {
    Memory::Write32(negotiator + field, value);
}

void WriteWord(uint32_t at, uint32_t value) {
    for (uint32_t i = 0; i < 4; ++i)
        Memory::Write8(at + i, uint8_t(value >> (24 - 8 * i)));
}

uint32_t ReadWord(uint32_t at) {
    uint32_t value = 0;

    for (uint32_t i = 0; i < 4; ++i)
        value = (value << 8) | Memory::Read8(at + i);

    return value;
}

void WriteShort(uint32_t at, uint32_t value) {
    Memory::Write8(at, uint8_t(value >> 8));
    Memory::Write8(at + 1, uint8_t(value));
}

uint32_t ReadShort(uint32_t at) {
    return (uint32_t(Memory::Read8(at)) << 8) | Memory::Read8(at + 1);
}

// inet_ntoa of an address, as the SDK's debug output does (its static
// buffer is guest memory).
void DebugAddress(CpuContext* cpu, uint32_t ip) {
    GuestFrame frame(cpu, 0x10);
    Memory::Write32(frame[8], ip);
    Guest(cpu, SOInetNtoA, {frame[8]});
}

void Header(CpuContext* cpu, uint32_t packet, uint32_t type, uint32_t cookie) {
    Memcpy4(cpu, packet, kMagic, kMagicBytes);
    Memory::Write8(packet + kPacketVersion, kVersion);
    Memory::Write8(packet + kPacketType, uint8_t(type));
    WriteWord(packet + kPacketCookie, cookie);
}

// sendto ip:port (host order port).
void SendPacket(CpuContext* cpu, uint32_t s, uint32_t packet, uint32_t length, uint32_t ip, uint32_t port) {
    GuestFrame frame(cpu, 0x20);
    const uint32_t addr = frame[8];
    Memory::Write8(addr + kAddrFamily, 2);
    Memory::Write16(addr + kAddrPort, uint16_t(Short(port)));
    Memory::Write32(addr + kAddrIp, ip);
    SendTo(cpu, s, packet, length, 0, addr, kAddrBytes);
}

uint32_t FindNegotiator(uint32_t cookie) {
    const uint32_t list = Memory::Read32(kNegotiators);

    if (!list)
        return 0;

    for (int32_t i = 0; i < ArrayLength(list); ++i) {
        const uint32_t negotiator = ArrayNth(Memory::Read32(kNegotiators), i);

        if (Field(negotiator, kCookie) == cookie)
            return negotiator;
    }

    return 0;
}

// The partner's game socket, else the negotiation socket.
uint32_t PingSocket(uint32_t negotiator) {
    return int32_t(Field(negotiator, kGameSocket)) == -1 ? Field(negotiator, kNegotiateSocket) : Field(negotiator, kGameSocket);
}

// A CONNECT_PING to the partner's guessed address (finished once past the
// pinging state), retried every 700 ms.
void SendPing(CpuContext* cpu, uint32_t negotiator) {
    GuestFrame frame(cpu, 0x60);
    const uint32_t packet = frame[0x10];
    Header(cpu, packet, kPacketConnectPing, Field(negotiator, kCookie));
    Memory::Write32(packet + kConnectRemoteIp, Field(negotiator, kGuessedIp));
    Memory::Write16(packet + kConnectRemotePort, uint16_t(Short(Memory::Read16(negotiator + kGuessedPort))));
    Memory::Write8(packet + kConnectGotYourData, Memory::Read8(negotiator + kGotRemoteData));
    Memory::Write8(packet + kConnectFinished, Field(negotiator, kState) != kConnectPing ? 1 : 0);
    DebugAddress(cpu, Field(negotiator, kGuessedIp));
    SendPacket(cpu, PingSocket(negotiator), packet, kConnectPacketBytes, Field(negotiator, kGuessedIp), Memory::Read16(negotiator + kGuessedPort));
    Set(negotiator, kRetryTime, CurrentTime(cpu) + 700);
    Set(negotiator, kMaxRetryCount, 7);

    if (Memory::Read8(negotiator + kGotRemoteData))
        Memory::Write8(negotiator + kSendGotRemoteData, 1);
}

// The REPORT of the result to natneg1 (its port type byte left unset, as
// the original does).
void SendReport(CpuContext* cpu, uint32_t negotiator) {
    GuestFrame frame(cpu, 0x80);
    const uint32_t packet = frame[0x18];
    Header(cpu, packet, kPacketReport, Field(negotiator, kCookie));
    Memory::Write8(packet + kPacketClientIndex, uint8_t(Field(negotiator, kClientIndex)));
    Memory::Write8(packet + kReportResult, Field(negotiator, kResult) == kSuccess ? 1 : 0);
    WriteWord(packet + kReportNatType, Memory::Read32(kNatType));
    WriteWord(packet + kReportMapping, Memory::Read32(kMappingScheme));

    if (StringLength(kGameName))
        Memcpy4(cpu, packet + kReportGameName, kGameName, kGameNameBytes);

    DebugAddress(cpu, Memory::Read32(kMatchupIps));
    SendPacket(cpu, Field(negotiator, kNegotiateSocket), packet, kReportPacketBytes, Memory::Read32(kMatchupIps), kNatnegPort);
}

void CompletedCallback(CpuContext* cpu, uint32_t negotiator) {
    Guest(cpu, Field(negotiator, kCompletedCallback),
          {Field(negotiator, kResult), Field(negotiator, kConnectedSocket), negotiator + kRemoteAddr, Field(negotiator, kUserData)});
}

void SetupLocalIp(CpuContext* cpu, uint32_t& ip);

} // namespace

// 0x80513C60 NNCancel: closes the negotiation socket; the next think drops
// the negotiation.
void NNCancel(CpuContext* cpu, uint32_t cookie) {
    const uint32_t negotiator = FindNegotiator(cookie);

    if (!negotiator)
        return;

    if (int32_t(Field(negotiator, kNegotiateSocket)) != -1)
        CloseSocket(cpu, Field(negotiator, kNegotiateSocket));

    Set(negotiator, kNegotiateSocket, 0xFFFFFFFF);
    Set(negotiator, kState, kCanceled);
}

// 0x80512B80: natneg's "a.b.c.d:port" (two static buffers).
uint32_t NNAddressToString(CpuContext* cpu, uint32_t ip, uint32_t port, uint32_t string) {
    if (!string) {
        const uint32_t toggle = Memory::Read32(kAddressToggle) ^ 1;
        Memory::Write32(kAddressToggle, toggle);
        string = kAddressBuffers + toggle * 0x16;
    }

    GuestFrame frame(cpu, 0x20);
    const uint32_t addr = frame[8];
    cpu->cr &= ~kCrFloatArgs;

    if (ip && port) {
        Memory::Write32(addr + 4, ip);
        Guest(cpu, Sprintf, {string, kFormatAddressPort, Guest(cpu, SOInetNtoA, {addr + 4}), port});
    } else if (ip) {
        Memory::Write32(addr, ip);
        Guest(cpu, Sprintf, {string, kFormatAddress, Guest(cpu, SOInetNtoA, {addr})});
    } else if (port) {
        Guest(cpu, Sprintf, {string, kFormatPort, port});
    } else {
        Memory::Write8(string, 0);
    }

    return string;
}

namespace {

// The first private non-loopback address of the local host, else the last
// non-loopback one, else 0.
void SetupLocalIp(CpuContext* cpu, uint32_t& ip) {
    ip = 0;
    const uint32_t host = Guest(cpu, 0x8050C8F0);

    if (!host)
        return;

    for (uint32_t i = 0;; ++i) {
        const uint32_t address = Memory::Read32(Memory::Read32(host + 0xC) + 4 * i);

        if (!address)
            return;

        const uint32_t value = Memory::Read32(address);

        if (value == 0x7F000001)
            continue;

        ip = value;

        if (Guest(cpu, 0x8050CAD0, {address}))
            return;
    }
}

} // namespace

// 0x80512C70 natify: reads the replies to NAT detection on a socket into the
// NAT (ERT replies: the address and port restrictions; address replies: one
// mapping each) until every reply came (0) or the socket is empty (1).
uint32_t NatifyThink(CpuContext* cpu, uint32_t s, uint32_t nat) {
    if (Memory::Read32(kErtReplied1) && Memory::Read32(kErtReplied2) && Memory::Read32(kErtReplied3) && Memory::Read32(kMappingReplied0) &&
        Memory::Read32(kMappingReplied3) && Memory::Read32(kMappingReplied1) && Memory::Read32(kMappingReplied2))
        return 0;

    GuestFrame frame(cpu, 0xB0);
    const uint32_t nameLength = frame[8], fromLength = frame[0xC], name = frame[0x10], from = frame[0x18], packet = frame[0x20];
    Memory::Write32(fromLength, kAddrBytes);

    while (int32_t(s) != -1 && CanReceiveOnSocket(cpu, s)) {
        const int32_t received = RecvFrom(cpu, s, kNatifyReceiveBuffer, kReceiveBytes, 0, from, fromLength);

        if (received == -1) {
            LastSocketError();
            return 1;
        }

        if (!BytesEqual(kNatifyReceiveBuffer, kMagic, kMagicBytes) || received < int32_t(kInitPacketBytes))
            return 1;

        const uint32_t type = Memory::Read8(kNatifyReceiveBuffer + kPacketType);
        const uint32_t fromIp = Memory::Read32(from + kAddrIp), fromPort = Short(Memory::Read16(from + kAddrPort));

        if (type == kPacketErtTest) {
            Memcpy4(cpu, packet, kNatifyReceiveBuffer, kInitPacketBytes);
            const uint32_t kind = Memory::Read8(packet + kPacketPortType);

            if (kind == 1) {
                Memory::Write32(kErtReplied1, 1);
                NNAddressToString(cpu, fromIp, fromPort, 0);
            } else if (kind == 2) {
                Memory::Write32(nat + kIpRestricted, 0);
                Memory::Write32(kErtReplied2, 1);
                NNAddressToString(cpu, fromIp, fromPort, 0);
            } else if (kind == 3) {
                Memory::Write32(nat + kPortRestricted, 0);
                Memory::Write32(kErtReplied3, 1);
                NNAddressToString(cpu, fromIp, fromPort, 0);
            }
        } else if (type == kPacketAddressReply) {
            Memcpy4(cpu, packet, kNatifyReceiveBuffer, kInitPacketBytes);
            const int32_t mapping = int32_t(Memory::Read32(packet + kPacketCookie));

            if (mapping == 0)
                Memory::Write32(kMappingReplied0, 1);
            else if (mapping == 1)
                Memory::Write32(kMappingReplied1, 1);
            else if (mapping == 2)
                Memory::Write32(kMappingReplied2, 1);
            else if (mapping == 3)
                Memory::Write32(kMappingReplied3, 1);

            uint32_t localIp;
            SetupLocalIp(cpu, localIp);
            const uint32_t entry = nat + kMappings + uint32_t(mapping) * kMappingBytes;
            Memory::Write32(entry + kPrivateIp, localIp);
            Memory::Write32(nameLength, kAddrBytes);
            const uint32_t port = GetSockName(cpu, s, name, nameLength) == -1 ? 0 : Memory::Read16(name + kAddrPort);
            Memory::Write16(nat + kMappings + uint32_t(int32_t(Memory::Read32(packet + kPacketCookie))) * kMappingBytes + kPrivatePort, uint16_t(Short(port)));
            const uint32_t index = uint32_t(int32_t(Memory::Read32(packet + kPacketCookie)));
            Memory::Write32(nat + kMappings + index * kMappingBytes + kPublicIp, ReadWord(packet + kInitLocalIp));
            Memory::Write16(nat + kMappings + index * kMappingBytes + kPublicPort, uint16_t(Short(ReadShort(packet + kInitLocalPort))));
            NNAddressToString(cpu, fromIp, fromPort, 0);
            const uint32_t filled = nat + kMappings + index * kMappingBytes;
            NNAddressToString(cpu, Memory::Read32(filled + kPrivateIp), Memory::Read16(filled + kPrivatePort), 0);
            NNAddressToString(cpu, Memory::Read32(filled + kPublicIp), Memory::Read16(filled + kPublicPort), 0);
        }
    }

    return 1;
}

// 0x80513010 natify: the NAT type and port mapping scheme from what came
// back; 0 without the three public addresses.
uint32_t DetermineNatType(uint32_t nat) {
    const uint32_t public0 = Memory::Read32(nat + kMappings + kPublicIp);
    Memory::Write32(nat + kNatTypeField, 6);
    Memory::Write32(nat + kPromiscuity, 4);
    Memory::Write32(nat + kQr2Compatible, 1);

    if (!public0 || !Memory::Read32(nat + kMappings + kMappingBytes + kPublicIp) || !Memory::Read32(nat + kMappings + 2 * kMappingBytes + kPublicIp))
        return 0;

    const bool ip = Memory::Read32(nat + kIpRestricted) != 0, port = Memory::Read32(nat + kPortRestricted) != 0;
    const uint32_t private0 = Memory::Read32(nat + kMappings + kPrivateIp);
    const uint32_t public1Port = Memory::Read16(nat + kMappings + kMappingBytes + kPublicPort);
    const uint32_t public2Port = Memory::Read16(nat + kMappings + 2 * kMappingBytes + kPublicPort);
    int32_t step = int32_t(public2Port) - int32_t(public1Port);
    const bool moved = (step < 0 ? -step : step) >= 1;

    auto symmetric = [&](uint32_t promiscuity) {
        Memory::Write32(nat + kNatTypeField, 5);
        Memory::Write32(nat + kPromiscuity, promiscuity);
    };

    if (!port && !ip && public0 == private0)
        Memory::Write32(nat + kNatTypeField, 0);
    else if (public0 == private0)
        Memory::Write32(nat + kNatTypeField, 1);
    else if (!ip && !port && moved)
        symmetric(0);
    else if (ip && !port && moved)
        symmetric(2);
    else if (!ip && port && moved)
        symmetric(3);
    else if (ip && port && moved)
        symmetric(1);
    else if (port)
        Memory::Write32(nat + kNatTypeField, 4);
    else if (ip)
        Memory::Write32(nat + kNatTypeField, 3);
    else
        Memory::Write32(nat + kNatTypeField, 2);

    const uint32_t private0Port = Memory::Read16(nat + kMappings + kPrivatePort);
    const uint32_t public0Port = Memory::Read16(nat + kMappings + kPublicPort);
    const uint32_t private1Port = Memory::Read16(nat + kMappings + kMappingBytes + kPrivatePort);
    const uint32_t private2Port = Memory::Read16(nat + kMappings + 2 * kMappingBytes + kPrivatePort);
    step = int32_t(public2Port) - int32_t(public1Port);
    uint32_t scheme = 0;

    if (public0Port == private0Port && public1Port == private1Port && public2Port == private2Port)
        scheme = 1;
    else if (public0Port == public1Port && public1Port == public2Port)
        scheme = 2;
    else if (public0Port == private0Port && step == 1)
        scheme = 4;
    else if (step == 1)
        scheme = 3;

    Memory::Write32(nat + kMappingSchemeField, scheme);
    const uint32_t public3Port = Memory::Read16(nat + kMappings + 3 * kMappingBytes + kPublicPort);

    if (public3Port && public0Port != public3Port)
        Memory::Write32(nat + kQr2Compatible, 0);

    return 1;
}

// 0x805132D0.
void NNFreeNegotiateList(CpuContext* cpu) {
    if (!Memory::Read32(kNegotiators))
        return;

    ArrayFree(cpu, Memory::Read32(kNegotiators));
    Memory::Write32(kNegotiators, 0);
}

// 0x80513310: the result. A dead partner or no INITACK ends it at once
// (completed callback, canceled); otherwise the result is reported first.
void NegotiationFinished(CpuContext* cpu, uint32_t negotiator, uint32_t result, uint32_t s, uint32_t remote) {
    Set(negotiator, kResult, result);
    Set(negotiator, kConnectedSocket, s);

    if (remote)
        Memcpy4(cpu, negotiator + kRemoteAddr, remote, kAddrBytes);

    if (result - 1 <= 1) {
        Set(negotiator, kState, kFinished);
        CompletedCallback(cpu, negotiator);
        NNCancel(cpu, Field(negotiator, kCookie));
        return;
    }

    SendReport(cpu, negotiator);
    Set(negotiator, kState, kReportSent);
    Set(negotiator, kRetryTime, CurrentTime(cpu) + 1000);
    Set(negotiator, kRetryCount, 0);
    Set(negotiator, kMaxRetryCount, 5);
}

// 0x80513510: INIT to natneg1 from the game socket (when given) and from the
// negotiation socket, then to natneg2 and 3, for each server not acked.
void SendInitPackets(CpuContext* cpu, uint32_t negotiator) {
    GuestFrame frame(cpu, 0xC0);
    const uint32_t nameLength = frame[8], name = frame[0x30], packet = frame[0x48];
    Header(cpu, packet, kPacketInit, Field(negotiator, kCookie));
    Memory::Write8(packet + kPacketClientIndex, uint8_t(Field(negotiator, kClientIndex)));
    const bool useGamePort = int32_t(Field(negotiator, kGameSocket)) != -1;
    Memory::Write8(packet + kInitUseGamePort, useGamePort ? 1 : 0);
    uint32_t localIp;
    SetupLocalIp(cpu, localIp);
    WriteWord(packet + kInitLocalIp, localIp);
    WriteShort(packet + kInitLocalPort, 0);
    std::strcpy(reinterpret_cast<char*>(Memory::GetPointer(packet + kInitGameName)), reinterpret_cast<const char*>(Memory::GetPointer(kGameName)));
    const uint32_t length = StringLength(kGameName) + 0x16;

    if (useGamePort && !Field(negotiator, kInitAckReceived)) {
        Memory::Write8(packet + kPacketPortType, 0);
        DebugAddress(cpu, Memory::Read32(kMatchupIps));
        SendPacket(cpu, Field(negotiator, kGameSocket), packet, length, Memory::Read32(kMatchupIps), kNatnegPort);
    }

    if (!Field(negotiator, kInitAckReceived + 4)) {
        Memory::Write8(packet + kPacketPortType, 1);
        DebugAddress(cpu, Memory::Read32(kMatchupIps));
        SendPacket(cpu, Field(negotiator, kNegotiateSocket), packet, length, Memory::Read32(kMatchupIps), kNatnegPort);
    }

    Memory::Write32(nameLength, kAddrBytes);
    const uint32_t s = useGamePort ? Field(negotiator, kGameSocket) : Field(negotiator, kNegotiateSocket);
    const uint32_t port = Short(GetSockName(cpu, s, name, nameLength) == -1 ? 0 : Memory::Read16(name + kAddrPort));
    WriteShort(packet + kInitLocalPort, port);

    for (uint32_t server = 1; server < 3; ++server) {
        if (Field(negotiator, kInitAckReceived + 4 * (server + 1)))
            continue;

        Memory::Write8(packet + kPacketPortType, uint8_t(server + 1));
        DebugAddress(cpu, Memory::Read32(kMatchupIps + 4 * server));
        SendPacket(cpu, Field(negotiator, kNegotiateSocket), packet, length, Memory::Read32(kMatchupIps + 4 * server), kNatnegPort);
    }

    Set(negotiator, kRetryTime, CurrentTime(cpu) + 500);
    Set(negotiator, kMaxRetryCount, 10);
}

// 0x80513830: NAT detection on one of its sockets: its replies for 10 s,
// then the NAT type to the game's natify callback (with a copy of the NAT)
// and both detection sockets closed. 0 once done.
uint32_t NatifyStatusThink(CpuContext* cpu, uint32_t s) {
    if (int32_t(s) == -1)
        return 1;

    uint32_t result = 0;

    if (CurrentTime(cpu) - Memory::Read32(kNatifyStart) < kNatifyTimeout)
        result = Guest(cpu, 0x80513000, {s, kNat});

    if (result)
        return result;

    GuestFrame frame(cpu, 0xF0);
    const uint32_t copy = frame[8];
    const uint32_t known = DetermineNatType(kNat);

    for (uint32_t i = 0; i < kNatBytes; i += 4)
        Memory::Write32(copy + i, Memory::Read32(kNat + i));

    Guest(cpu, Memory::Read32(kNatifyCallback), {known, copy});
    Memory::Write32(kNatType, Memory::Read32(kNat + kNatTypeField));
    Memory::Write32(kMappingScheme, Memory::Read32(kNat + kMappingSchemeField));

    for (uint32_t i = 0; i < 2; ++i) {
        if (int32_t(Memory::Read32(kNatifySockets + 4 * i)) != -1)
            CloseSocket(cpu, Memory::Read32(kNatifySockets + 4 * i));

        Memory::Write32(kNatifySockets + 4 * i, 0xFFFFFFFF);
    }

    return 0;
}

void NNProcessData(CpuContext* cpu, uint32_t data, int32_t length, uint32_t from);

// 0x80513D10: one negotiation's step: its socket's packets, then its
// retries and timeouts. A canceled one leaves the list (also the natify
// sockets' step, while NAT detection runs).
void NegotiateThink(CpuContext* cpu, uint32_t negotiator) {
    GuestFrame frame(cpu, 0xE0);
    const uint32_t fromLength = frame[0x10], remote = frame[0x28], from = frame[0x30];
    Memory::Write32(fromLength, kAddrBytes);

    if (Memory::Read32(kNatifyActive)) {
        Memory::Write32(kNatifyActive, NatifyStatusThink(cpu, Memory::Read32(kNatifySockets)));
        Memory::Write32(kNatifyActive, NatifyStatusThink(cpu, Memory::Read32(kNatifySockets + 4)));
    }

    if (!negotiator)
        return;

    if (Field(negotiator, kState) == kCanceled) {
        for (int32_t i = 0; i < ArrayLength(Memory::Read32(kNegotiators)); ++i)
            if (ArrayNth(Memory::Read32(kNegotiators), i) == negotiator) {
                ArrayRemoveAt(cpu, Memory::Read32(kNegotiators), i);
                return;
            }

        return;
    }

    if (int32_t(Field(negotiator, kNegotiateSocket)) != -1) {
        while (CanReceiveOnSocket(cpu, Field(negotiator, kNegotiateSocket))) {
            const int32_t received = RecvFrom(cpu, Field(negotiator, kNegotiateSocket), kReceiveBuffer, kReceiveBytes, 0, from, fromLength);

            if (received == -1) {
                LastSocketError();
                break;
            }

            NNProcessData(cpu, kReceiveBuffer, received, from);

            if (Field(negotiator, kState) == kCanceled || int32_t(Field(negotiator, kNegotiateSocket)) == -1)
                break;
        }
    }

    const uint32_t state = Field(negotiator, kState);

    if ((state == kInitSent || state == kConnectPing) && CurrentTime(cpu) > Field(negotiator, kRetryTime)) {
        if (int32_t(Field(negotiator, kRetryCount)) > int32_t(Field(negotiator, kMaxRetryCount))) {
            NegotiationFinished(cpu, negotiator, Field(negotiator, kState) == kInitSent ? kInitTimeout : kPingTimeout, 0xFFFFFFFF, 0);
        } else {
            Set(negotiator, kRetryCount, Field(negotiator, kRetryCount) + 1);

            if (Field(negotiator, kState) == kInitSent)
                SendInitPackets(cpu, negotiator);
            else
                SendPing(cpu, negotiator);
        }
    }

    if (Field(negotiator, kState) == kFinished && CurrentTime(cpu) > Field(negotiator, kRetryTime)) {
        Memory::Write8(remote + kAddrFamily, 2);
        Memory::Write16(remote + kAddrPort, uint16_t(Short(Memory::Read16(negotiator + kGuessedPort))));
        Memory::Write32(remote + kAddrIp, Field(negotiator, kGuessedIp));
        NegotiationFinished(cpu, negotiator, kSuccess, PingSocket(negotiator), remote);
    }

    if (Field(negotiator, kState) == kInitAcked && CurrentTime(cpu) > Field(negotiator, kRetryTime))
        NegotiationFinished(cpu, negotiator, kDeadbeatPartner, 0xFFFFFFFF, 0);

    if (Field(negotiator, kState) != kReportSent || CurrentTime(cpu) <= Field(negotiator, kRetryTime))
        return;

    if (int32_t(Field(negotiator, kRetryCount)) > int32_t(Field(negotiator, kMaxRetryCount))) {
        CompletedCallback(cpu, negotiator);

        if (int32_t(Field(negotiator, kGameSocket)) == -1)
            Set(negotiator, kNegotiateSocket, 0xFFFFFFFF);

        NNCancel(cpu, Field(negotiator, kCookie));
        return;
    }

    SendReport(cpu, negotiator);
    Set(negotiator, kRetryCount, Field(negotiator, kRetryCount) + 1);
    Set(negotiator, kRetryTime, CurrentTime(cpu) + 1000);
}

// 0x80514230 NNThink: every negotiation, last first (with none, the natify
// step alone).
void NNThink(CpuContext* cpu) {
    const uint32_t list = Memory::Read32(kNegotiators);

    if (!list || !ArrayLength(list)) {
        NegotiateThink(cpu, 0);
        return;
    }

    for (int32_t i = ArrayLength(Memory::Read32(kNegotiators)) - 1; i >= 0; --i)
        NegotiateThink(cpu, ArrayNth(Memory::Read32(kNegotiators), i));
}

// 0x805142B0: the server's CONNECT with the partner's address (acked
// unless it reports the end): start pinging the partner.
void ProcessConnectPacket(CpuContext* cpu, uint32_t negotiator, uint32_t packet, uint32_t from) {
    GuestFrame frame(cpu, 0xD0);
    const uint32_t ack = frame[0x6C];
    DebugAddress(cpu, Memory::Read32(packet + kConnectRemoteIp));
    const uint32_t finished = Memory::Read8(packet + kConnectFinished);

    if (!finished) {
        Header(cpu, ack, kPacketConnectAck, Field(negotiator, kCookie));
        Memory::Write8(ack + kPacketClientIndex, uint8_t(Field(negotiator, kClientIndex)));
        SendPacket(cpu, Field(negotiator, kNegotiateSocket), ack, kInitPacketBytes, Memory::Read32(from + kAddrIp), Short(Memory::Read16(from + kAddrPort)));
    }

    if (Field(negotiator, kState) >= kConnectPing)
        return;

    if (finished) {
        const uint32_t result = finished == 1 ? kDeadbeatPartner : finished == 2 ? kInitTimeout : kUnknownError;
        NegotiationFinished(cpu, negotiator, result, 0xFFFFFFFF, 0);
        return;
    }

    Set(negotiator, kGuessedIp, Memory::Read32(packet + kConnectRemoteIp));
    Memory::Write16(negotiator + kGuessedPort, uint16_t(Short(Memory::Read16(packet + kConnectRemotePort))));
    Set(negotiator, kRetryCount, 0);
    Set(negotiator, kState, kConnectPing);
    cpu->gpr[5] = 0;
    Guest(cpu, Field(negotiator, kProgressCallback), {kConnectPing, Field(negotiator, kUserData)});
    SendPing(cpu, negotiator);
}

// 0x80514500: the partner's CONNECT_PING: its real address. Answer until it
// has ours; once both have each other's, wait 5 s for its last ping.
void ProcessPingPacket(CpuContext* cpu, uint32_t negotiator, uint32_t packet, uint32_t from) {
    if (Field(negotiator, kState) < kConnectPing)
        return;

    DebugAddress(cpu, Memory::Read32(from + kAddrIp));
    Set(negotiator, kGuessedIp, Memory::Read32(from + kAddrIp));
    Memory::Write16(negotiator + kGuessedPort, uint16_t(Short(Memory::Read16(from + kAddrPort))));
    Memory::Write8(negotiator + kGotRemoteData, 1);

    if (!Memory::Read8(packet + kConnectGotYourData)) {
        SendPing(cpu, negotiator);
    } else if (Field(negotiator, kState) == kConnectPing) {
        if (!Memory::Read8(negotiator + kSendGotRemoteData))
            SendPing(cpu, negotiator);

        Set(negotiator, kState, kFinished);
        Set(negotiator, kRetryTime, CurrentTime(cpu) + 5000);
    } else if (!Memory::Read8(packet + kConnectFinished)) {
        SendPing(cpu, negotiator);
    }
}

// 0x80514880: INITACK (each server's; with all, wait 60 s for CONNECT),
// ERTTEST (echoed as ERTACK) and REPORT_ACK (the completed callback).
void ProcessInitAckPacket(CpuContext* cpu, uint32_t negotiator, uint32_t packet, uint32_t from) {
    const uint32_t type = Memory::Read8(packet + kPacketType);

    if (type == kPacketInitAck) {
        const uint32_t server = Memory::Read8(packet + kPacketPortType);

        if (server > 3)
            return;

        Set(negotiator, kInitAckReceived + 4 * server, 1);

        if (Field(negotiator, kState) != kInitSent || !Field(negotiator, kInitAckReceived + 4) || !Field(negotiator, kInitAckReceived + 8) ||
            !Field(negotiator, kInitAckReceived + 12))
            return;

        if (int32_t(Field(negotiator, kGameSocket)) != -1 && !Field(negotiator, kInitAckReceived))
            return;

        Set(negotiator, kState, kInitAcked);
        Set(negotiator, kRetryTime, CurrentTime(cpu) + 60000);
        Guest(cpu, Field(negotiator, kProgressCallback), {Field(negotiator, kState), Field(negotiator, kUserData)});
    } else if (type == kPacketErtTest) {
        Memory::Write8(packet + kPacketType, kPacketErtAck);
        SendPacket(cpu, Field(negotiator, kNegotiateSocket), packet, kInitPacketBytes, Memory::Read32(from + kAddrIp), Short(Memory::Read16(from + kAddrPort)));
    } else if (type == kPacketReportAck) {
        Set(negotiator, kState, kReportAcked);
        CompletedCallback(cpu, negotiator);

        if (int32_t(Field(negotiator, kGameSocket)) == -1)
            Set(negotiator, kNegotiateSocket, 0xFFFFFFFF);

        NNCancel(cpu, Field(negotiator, kCookie));
    }
}

// 0x80514A80 NNProcessData: a natneg packet to its negotiation (by cookie).
void NNProcessData(CpuContext* cpu, uint32_t data, int32_t length, uint32_t from) {
    if (!BytesEqual(data, kMagic, kMagicBytes))
        return;

    GuestFrame frame(cpu, 0x70);
    const uint32_t packet = frame[0xC];
    const uint32_t type = Memory::Read8(data + kPacketType);
    DebugAddress(cpu, Memory::Read32(from + kAddrIp));
    const bool connect = type == kPacketConnect || type == kPacketConnectPing;
    const uint32_t bytes = connect ? kConnectPacketBytes : kInitPacketBytes;

    if (length < int32_t(bytes))
        return;

    Memcpy4(cpu, packet, data, bytes);
    const uint32_t negotiator = FindNegotiator(Memory::Read32(packet + kPacketCookie));

    if (!negotiator)
        return;

    if (type == kPacketConnect)
        ProcessConnectPacket(cpu, negotiator, packet, from);
    else if (type == kPacketConnectPing)
        ProcessPingPacket(cpu, negotiator, packet, from);
    else
        ProcessInitAckPacket(cpu, negotiator, packet, from);
}

namespace {

void NNAddressToStringPort(CpuContext* cpu) {
    cpu->gpr[3] = NNAddressToString(cpu, cpu->gpr[3], cpu->gpr[4] & 0xFFFF, cpu->gpr[5]);
}

void NatifyThinkPort(CpuContext* cpu) {
    cpu->gpr[3] = NatifyThink(cpu, cpu->gpr[3], cpu->gpr[4]);
}

void DetermineNatTypePort(CpuContext* cpu) {
    cpu->gpr[3] = DetermineNatType(cpu->gpr[3]);
}

void NNFreeNegotiateListPort(CpuContext* cpu) {
    NNFreeNegotiateList(cpu);
}

void NegotiationFinishedPort(CpuContext* cpu) {
    NegotiationFinished(cpu, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6]);
}

void SendInitPacketsPort(CpuContext* cpu) {
    SendInitPackets(cpu, cpu->gpr[3]);
}

void NatifyStatusThinkPort(CpuContext* cpu) {
    cpu->gpr[3] = NatifyStatusThink(cpu, cpu->gpr[3]);
}

void NNCancelPort(CpuContext* cpu) {
    NNCancel(cpu, cpu->gpr[3]);
}

void NegotiateThinkPort(CpuContext* cpu) {
    NegotiateThink(cpu, cpu->gpr[3]);
}

void NNThinkPort(CpuContext* cpu) {
    NNThink(cpu);
}

void ProcessConnectPacketPort(CpuContext* cpu) {
    ProcessConnectPacket(cpu, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5]);
}

void ProcessPingPacketPort(CpuContext* cpu) {
    ProcessPingPacket(cpu, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5]);
}

void ProcessInitAckPacketPort(CpuContext* cpu) {
    ProcessInitAckPacket(cpu, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5]);
}

void NNProcessDataPort(CpuContext* cpu) {
    NNProcessData(cpu, cpu->gpr[3], int32_t(cpu->gpr[4]), cpu->gpr[5]);
}

} // namespace

} // namespace GameSpy

using namespace GameSpy;

extern "C" void func_80512B80(CpuContext*);
extern "C" void func_80513010(CpuContext*);

extern "C" void MH3_NNAddressToString(CpuContext* cpu) {
    PortCheck::Call(cpu, "NNAddressToString", func_80512B80, NNAddressToStringPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80512B80, MH3_NNAddressToString, "NNAddressToString");

extern "C" void MH3_NatifyThink(CpuContext* cpu) {
    NatifyThinkPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x80512C70, MH3_NatifyThink, "NatifyThink");

extern "C" void MH3_NatifyThinkEntry(CpuContext* cpu) {
    NatifyThinkPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x80513000, MH3_NatifyThinkEntry, "NatifyThinkEntry");

extern "C" void MH3_DetermineNatType(CpuContext* cpu) {
    PortCheck::Call(cpu, "DetermineNatType", func_80513010, DetermineNatTypePort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80513010, MH3_DetermineNatType, "DetermineNatType");

extern "C" void MH3_NNFreeNegotiateList(CpuContext* cpu) {
    NNFreeNegotiateListPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x805132D0, MH3_NNFreeNegotiateList, "NNFreeNegotiateList");

extern "C" void MH3_NegotiationFinished(CpuContext* cpu) {
    NegotiationFinishedPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x80513310, MH3_NegotiationFinished, "NegotiationFinished");

extern "C" void MH3_SendInitPackets(CpuContext* cpu) {
    SendInitPacketsPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x80513510, MH3_SendInitPackets, "SendInitPackets");

extern "C" void MH3_NatifyStatusThink(CpuContext* cpu) {
    NatifyStatusThinkPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x80513830, MH3_NatifyStatusThink, "NatifyStatusThink");

extern "C" void MH3_NNCancel(CpuContext* cpu) {
    NNCancelPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x80513C60, MH3_NNCancel, "NNCancel");

extern "C" void MH3_NegotiateThink(CpuContext* cpu) {
    NegotiateThinkPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x80513D10, MH3_NegotiateThink, "NegotiateThink");

extern "C" void MH3_NNThink(CpuContext* cpu) {
    NNThinkPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x80514230, MH3_NNThink, "NNThink");

extern "C" void MH3_ProcessConnectPacket(CpuContext* cpu) {
    ProcessConnectPacketPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x805142B0, MH3_ProcessConnectPacket, "ProcessConnectPacket");

extern "C" void MH3_ProcessPingPacket(CpuContext* cpu) {
    ProcessPingPacketPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x80514500, MH3_ProcessPingPacket, "ProcessPingPacket");

extern "C" void MH3_ProcessInitAckPacket(CpuContext* cpu) {
    ProcessInitAckPacketPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x80514880, MH3_ProcessInitAckPacket, "ProcessInitAckPacket");

extern "C" void MH3_NNProcessData(CpuContext* cpu) {
    NNProcessDataPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x80514A80, MH3_NNProcessData, "NNProcessData");
