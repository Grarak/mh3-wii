// GameSpy Transport 2 (GT2) in RMHP08, 0x8050CFD0..0x80512B10, ported from
// the translated C++ and its Ghidra decompilation (gamespy.h). The game's
// peer layer (NetworkGameSpyInterface, 0x8041B..0x8041E) runs its quest
// peers on it: one UDP socket, reliable and unreliable messages per
// connection, a challenge/response handshake.
//
// The timers read current_time (OSGetTime in milliseconds) once per think:
// a keep-alive after 30 s without a send, a reliable message resent after
// 1 s without its ack, a pending ack sent after 100 ms, the connect timeout
// the caller gives (the game's 10 s) and 60 s for an incoming connection
// that is not accepted. They count real time, whatever the frame rate.
//
// MH3_PORT_CHECK names the ports that send nothing: the challenge and
// response, the buffers, the address helpers and the connection lookups.
#include "gamespy.h"

#include "port_check.h"

#include <cstring>
#include <tuple>

namespace GameSpy {

namespace {

// struct GTI2Socket.
enum : uint32_t {
    kSocket = 0x00,
    kSocketIp = 0x04,
    kSocketPort = 0x08,
    kConnections = 0x0C,
    kClosedConnections = 0x10,
    kSocketClose = 0x14,
    kSocketError = 0x18,
    kSocketCallbackLevel = 0x1C,
    kConnectAttemptCallback = 0x20,
    kSocketErrorCallback = 0x24,
    kSendDumpCallback = 0x28,
    kReceiveDumpCallback = 0x2C,
    kUnrecognizedMessageCallback = 0x30,
    kOutgoingBufferSize = 0x38,
    kIncomingBufferSize = 0x3C,
    kProtocolType = 0x40,
    kProtocolOffset = 0x44,
    kSocketBytes = 0x4C,
};

// struct GTI2Connection.
enum : uint32_t {
    kIp = 0x00,
    kPort = 0x04,
    kConnSocket = 0x08,
    kState = 0x0C,
    kInitiated = 0x10,
    kFreeAtAcceptReject = 0x14,
    kConnectionResult = 0x18,
    kStartTime = 0x1C,
    kTimeout = 0x20,
    kCallbackLevel = 0x24,
    kConnectedCallback = 0x28,
    kReceivedCallback = 0x2C,
    kClosedCallback = 0x30,
    kPingCallback = 0x34,
    kInitialMessage = 0x38,
    kInitialMessageLength = 0x3C,
    kIncomingBuffer = 0x44,
    kOutgoingBuffer = 0x50,
    kIncomingMessages = 0x5C,
    kOutgoingMessages = 0x60,
    kSerialNumber = 0x64,
    kExpectedSerialNumber = 0x66,
    kResponse = 0x68,
    kLastSend = 0x88,
    kChallengeTime = 0x8C,
    kPendingAck = 0x90,
    kPendingAckTime = 0x94,
    kSendFilters = 0x98,
    kReceiveFilters = 0x9C,
    kConnectionBytes = 0xA0,
};

// GTI2Buffer: data, size, length.
enum : uint32_t {
    kBufferData = 0x0,
    kBufferSize = 0x4,
    kBufferLength = 0x8,
};

// A buffered message: its start and length in the buffer, then (outgoing)
// the serial number and the last send, or (incoming) the type and the
// serial number.
enum : uint32_t {
    kMessageStart = 0x0,
    kMessageLength = 0x4,
    kMessageSerial = 0x8,
    kMessageLastSend = 0xC,
    kIncomingType = 0x8,
    kIncomingSerial = 0xC,
    kBufferedMessageBytes = 0x10,
};

// GTI2ConnectionState.
enum : int32_t {
    kAwaitingServerChallenge = 0,
    kAwaitingAcceptance = 1,
    kAwaitingClientChallenge = 2,
    kAwaitingClientResponse = 3,
    kAwaitingAcceptReject = 4,
    kConnected = 5,
    kClosing = 6,
    kClosed = 7,
};

// GT2Result.
enum : uint32_t {
    kSuccess = 0,
    kOutOfMemory = 1,
    kRejected = 2,
    kAddressError = 4,
    kDuplicateAddress = 5,
    kTimedOut = 6,
    kNegotiationError = 7,
    kInvalidConnection = 8,
    kInvalidMessage = 9,
    kSendFailed = 10,
};

// GT2CloseReason.
enum : uint32_t {
    kLocalClose = 0,
    kRemoteClose = 1,
    kCommunicationError = 2,
    kNotEnoughMemory = 4,
};

// GTI2MessageType.
enum : uint32_t {
    kMsgAppReliable = 0,
    kMsgClientChallenge = 1,
    kMsgServerChallenge = 2,
    kMsgClientResponse = 3,
    kMsgAccept = 4,
    kMsgReject = 5,
    kMsgClose = 6,
    kMsgKeepAlive = 7,
    kNumReliableMessages = 8,
    kMsgAck = 100,
    kMsgNack = 101,
    kMsgPing = 102,
    kMsgPong = 103,
    kMsgClosed = 104,
};

// The VDP protocol (2) puts a 16-bit length before each message.
constexpr uint32_t kVdpProtocol = 2;

// FE FE, then "time" for pings.
constexpr uint32_t kMagic = 0x80796810;
constexpr uint32_t kTimeTag = 0x80796814;
constexpr uint32_t kMagicBytes = 2;

constexpr uint32_t kChallengeBytes = 32;
constexpr uint32_t kResponseKey = 0x80631140;

constexpr uint32_t kKeepAliveTime = 30000;
constexpr uint32_t kResendTime = 1000;
constexpr uint32_t kPendingAckDelay = 100;
constexpr uint32_t kServerTimeout = 60000;

// The receive buffer, 4 KiB.
constexpr uint32_t kReceiveBuffer = 0x80763A80;
constexpr uint32_t kReceiveBufferBytes = 0x1000;

constexpr uint32_t Srand = 0x8045E640;

int32_t State(uint32_t connection) {
    return int32_t(Memory::Read32(connection + kState));
}

uint32_t SocketOf(uint32_t connection) {
    return Memory::Read32(connection + kConnSocket);
}

uint32_t ProtocolOffset(uint32_t socket) {
    return Memory::Read32(socket + kProtocolOffset);
}

uint32_t Random93(CpuContext* cpu) {
    return uint32_t(int32_t(GuestCall{cpu}.Call<Rand>()) % 93 + 33);
}

// The challenge's check bit for byte i: from its first byte, byte i - 1 and i.
uint32_t CheckBit(uint32_t check, uint32_t first, uint32_t previous, uint32_t i) {
    return (first & 1) ^ check ^ ((i ^ previous) & 1) ^ ((first - 0x4F) >> 31) ^ ((previous - first) >> 31);
}

bool CheckBitMismatch(uint32_t check, uint8_t byte) {
    return (check != 0 && (byte & 1) == 0) || (check == 0 && (byte & 1) == 1);
}

} // namespace

// Declared here, defined further down.
bool ConnectedCallback(CpuContext* cpu, uint32_t connection, uint32_t result, uint32_t message, int32_t length);
bool ClosedCallback(CpuContext* cpu, uint32_t connection, uint32_t reason);
void ConnectionClosed(CpuContext* cpu, uint32_t connection);
bool RawSend(CpuContext* cpu, uint32_t connection, uint32_t message, int32_t length);
bool SocketSend(CpuContext* cpu, uint32_t socket, uint32_t ip, uint32_t port, uint32_t message, int32_t length);
bool BeginReliableMessage(CpuContext* cpu, uint32_t connection, uint32_t type, int32_t length, uint32_t full);
bool SendClientChallenge(CpuContext* cpu, uint32_t connection, uint32_t challenge);
bool SendAccept(CpuContext* cpu, uint32_t connection);
bool SendReject(CpuContext* cpu, uint32_t connection, uint32_t message, int32_t length);
bool SendClose(CpuContext* cpu, uint32_t connection);
bool SendClosed(CpuContext* cpu, uint32_t connection);
bool Send(CpuContext* cpu, uint32_t connection, uint32_t message, int32_t length, bool reliable);
uint32_t NewSocketConnection(CpuContext* cpu, uint32_t socket, uint32_t out, uint32_t ip, uint32_t port);
void FreeSocketConnection(CpuContext* cpu, uint32_t connection);
bool ReceiveMessages(CpuContext* cpu, uint32_t socket);
bool SocketConnectionsThink(CpuContext* cpu, uint32_t socket);
void FreeClosedConnections(CpuContext* cpu, uint32_t socket);
bool StringToAddress(CpuContext* cpu, uint32_t string, uint32_t ip, uint32_t port);
void MessageCheck(CpuContext* cpu, uint32_t message, uint32_t length);
uint32_t CreateSocket(CpuContext* cpu, uint32_t out, uint32_t address, uint32_t outgoingSize, uint32_t incomingSize, uint32_t errorCallback, uint32_t type);

// 0x8050CFD0: 32 printable bytes (33..125) from rand, seeded with
// current_time; each byte after the first is nudged to its check bit.
void GetChallenge(CpuContext* cpu, uint32_t buffer) {
    Guest(cpu, Srand, {CurrentTime(cpu)});
    Memory::Write8(buffer, uint8_t(Random93(cpu)));
    uint32_t check = 0;

    for (uint32_t i = 1; i < kChallengeBytes; ++i) {
        check = CheckBit(check, Memory::Read8(buffer), Memory::Read8(buffer + i - 1), i);
        Memory::Write8(buffer + i, uint8_t(Random93(cpu)));

        if (CheckBitMismatch(check, Memory::Read8(buffer + i)))
            Memory::Write8(buffer + i, uint8_t(Memory::Read8(buffer + i) + 1));
    }
}

// 0x8050D0E0: the response to a challenge, keyed with the game's secret.
// Bytes 0 and 13 (and all of them for a challenge whose check bits are
// wrong) are random.
void GetResponse(CpuContext* cpu, uint32_t response, uint32_t challenge) {
    const int32_t keyLength = int32_t(StringLength(kResponseKey));
    bool valid = true;
    uint32_t check = 0;

    for (uint32_t i = 1; i < kChallengeBytes; ++i) {
        check = CheckBit(check, Memory::Read8(challenge), Memory::Read8(challenge + i - 1), i);

        if (CheckBitMismatch(check, Memory::Read8(challenge + i))) {
            valid = false;
            break;
        }
    }

    for (int32_t i = 0, step = 0; i < int32_t(kChallengeBytes); ++i, step += 0x4647) {
        if (!valid || i == 0 || i == 13) {
            Memory::Write8(response + uint32_t(i), uint8_t(Random93(cpu)));
            continue;
        }

        const int32_t source = int32_t(int8_t(Memory::Read8(challenge + uint32_t(i == 1 || i == 14 ? i : i - 1))));
        const int32_t byte = Memory::Read8(challenge + uint32_t(i));
        const int32_t key1 = int8_t(Memory::Read8(kResponseKey + uint32_t((i + byte) % keyLength)));
        const int32_t key2 = int8_t(Memory::Read8(kResponseKey + uint32_t((source * step) % keyLength)));
        const int32_t index = (key1 + i * byte) % 32;
        int32_t value = int32_t(Memory::Read8(challenge + uint32_t(index))) ^ key2;
        value = value < 0 ? -value : value;
        Memory::Write8(response + uint32_t(i), uint8_t(value % 93 + 33));
    }
}

// 0x8050D2D0: responses match but for bytes 0 and 13.
bool CheckResponse(uint32_t a, uint32_t b) {
    for (uint32_t i = 0; i < kChallengeBytes; ++i)
        if (i != 0 && i != 13 && Memory::Read8(a + i) != Memory::Read8(b + i))
            return false;

    return true;
}

// 0x8050D3A0.
bool AllocateBuffer(CpuContext* cpu, uint32_t buffer, uint32_t size) {
    const uint32_t data = Malloc(cpu, size);
    Memory::Write32(buffer + kBufferData, data);

    if (data)
        Memory::Write32(buffer + kBufferSize, size);

    return data != 0;
}

// 0x8050D400.
int32_t BufferFreeSpace(uint32_t buffer) {
    return int32_t(Memory::Read32(buffer + kBufferSize) - Memory::Read32(buffer + kBufferLength));
}

// 0x8050D410.
void BufferWriteByte(uint32_t buffer, uint32_t value) {
    const uint32_t length = Memory::Read32(buffer + kBufferLength);
    Memory::Write8(Memory::Read32(buffer + kBufferData) + length, uint8_t(value));
    Memory::Write32(buffer + kBufferLength, length + 1);
}

// 0x8050D430: big-endian.
void BufferWriteUShort(uint32_t buffer, uint32_t value) {
    BufferWriteByte(buffer, value >> 8);
    BufferWriteByte(buffer, value);
}

// 0x8050D460: a length of -1 writes the string.
void BufferWriteData(CpuContext* cpu, uint32_t buffer, uint32_t data, int32_t length) {
    if (!data || !length)
        return;

    if (length == -1)
        length = int32_t(StringLength(data));

    Memcpy4(cpu, Memory::Read32(buffer + kBufferData) + Memory::Read32(buffer + kBufferLength), data, uint32_t(length));
    Memory::Write32(buffer + kBufferLength, Memory::Read32(buffer + kBufferLength) + uint32_t(length));
}

// 0x8050D4F0: a start of -1 cuts from the end.
void BufferShorten(CpuContext* cpu, uint32_t buffer, int32_t start, int32_t by) {
    const int32_t length = int32_t(Memory::Read32(buffer + kBufferLength));

    if (start == -1)
        start = length - by;

    const uint32_t data = Memory::Read32(buffer + kBufferData);
    Memmove4(cpu, data + uint32_t(start), data + uint32_t(start + by), uint32_t(int32_t(Memory::Read32(buffer + kBufferLength)) - start - by));
    Memory::Write32(buffer + kBufferLength, Memory::Read32(buffer + kBufferLength) - uint32_t(by));
}

namespace {

void Adjust(uint32_t address, int32_t by) {
    Memory::Write32(address, Memory::Read32(address) + uint32_t(by));
}

// After a callback: a socket closed in one is freed once no callback runs.
bool SocketStillOpen(CpuContext* cpu, uint32_t socket);

// The callbacks run one level deeper on the connection and its socket.
void EnterConnection(uint32_t connection) {
    Adjust(connection + kCallbackLevel, 1);
    Adjust(SocketOf(connection) + kSocketCallbackLevel, 1);
}

void LeaveConnection(uint32_t connection) {
    Adjust(connection + kCallbackLevel, -1);
    Adjust(SocketOf(connection) + kSocketCallbackLevel, -1);
}

} // namespace

void CloseGt2Socket(CpuContext* cpu, uint32_t socket);
void SocketError(CpuContext* cpu, uint32_t socket);

namespace {

bool SocketStillOpen(CpuContext* cpu, uint32_t socket) {
    if (!Memory::Read32(socket + kSocketClose) || Memory::Read32(socket + kSocketCallbackLevel))
        return true;

    CloseGt2Socket(cpu, socket);
    return false;
}

} // namespace

// 0x8050D560.
bool SocketErrorCallback(CpuContext* cpu, uint32_t socket) {
    if (!socket)
        return true;

    const uint32_t callback = Memory::Read32(socket + kSocketErrorCallback);

    if (!callback)
        return true;

    Adjust(socket + kSocketCallbackLevel, 1);
    Guest(cpu, callback, {socket});
    Adjust(socket + kSocketCallbackLevel, -1);
    return SocketStillOpen(cpu, socket);
}

// 0x8050D600.
bool ConnectAttemptCallback(CpuContext* cpu, uint32_t socket, uint32_t connection, uint32_t ip, uint32_t port, uint32_t latency, uint32_t message,
                            int32_t length) {
    if (!socket || !connection)
        return true;

    const uint32_t callback = Memory::Read32(socket + kConnectAttemptCallback);

    if (!callback)
        return true;

    Adjust(socket + kSocketCallbackLevel, 1);
    Adjust(connection + kCallbackLevel, 1);
    Guest(cpu, callback, {socket, connection, ip, port, latency, message, uint32_t(length)});
    Adjust(socket + kSocketCallbackLevel, -1);
    Adjust(connection + kCallbackLevel, -1);
    return SocketStillOpen(cpu, socket);
}

// 0x8050D6F0: keeps the result first.
bool ConnectedCallback(CpuContext* cpu, uint32_t connection, uint32_t result, uint32_t message, int32_t length) {
    if (!connection)
        return true;

    Memory::Write32(connection + kConnectionResult, result);
    const uint32_t callback = Memory::Read32(connection + kConnectedCallback);

    if (!callback)
        return true;

    if (!length || !message) {
        message = 0;
        length = 0;
    }

    EnterConnection(connection);
    Guest(cpu, callback, {connection, result, message, uint32_t(length)});
    LeaveConnection(connection);
    return SocketStillOpen(cpu, SocketOf(connection));
}

// 0x8050D7D0.
bool ReceivedCallback(CpuContext* cpu, uint32_t connection, uint32_t message, int32_t length, uint32_t reliable) {
    if (!connection)
        return true;

    const uint32_t callback = Memory::Read32(connection + kReceivedCallback);

    if (!callback)
        return true;

    if (!length || !message) {
        message = 0;
        length = 0;
    }

    EnterConnection(connection);
    Guest(cpu, callback, {connection, message, uint32_t(length), reliable});
    LeaveConnection(connection);
    return SocketStillOpen(cpu, SocketOf(connection));
}

// 0x8050D8B0.
bool ClosedCallback(CpuContext* cpu, uint32_t connection, uint32_t reason) {
    if (!connection)
        return true;

    const uint32_t callback = Memory::Read32(connection + kClosedCallback);

    if (!callback)
        return true;

    EnterConnection(connection);
    Guest(cpu, callback, {connection, reason});
    LeaveConnection(connection);
    return SocketStillOpen(cpu, SocketOf(connection));
}

// 0x8050D970.
bool PingCallback(CpuContext* cpu, uint32_t connection, uint32_t latency) {
    if (!connection)
        return true;

    const uint32_t callback = Memory::Read32(connection + kPingCallback);

    if (!callback)
        return true;

    EnterConnection(connection);
    Guest(cpu, callback, {connection, latency});
    LeaveConnection(connection);
    return SocketStillOpen(cpu, SocketOf(connection));
}

namespace {

// 0x8050DA30, 0x8050DB40: filter `id` of the connection's send or receive
// filters.
bool FilterCallback(CpuContext* cpu, uint32_t connection, uint32_t filters, uint32_t id, uint32_t message, int32_t length, uint32_t reliable) {
    if (!connection)
        return true;

    const uint32_t filter = ArrayNth(Memory::Read32(connection + filters), int32_t(id));

    if (!filter)
        return true;

    if (!length || !message) {
        message = 0;
        length = 0;
    }

    EnterConnection(connection);
    Guest(cpu, Memory::Read32(filter), {connection, id, message, uint32_t(length), reliable});
    LeaveConnection(connection);
    return SocketStillOpen(cpu, SocketOf(connection));
}

} // namespace

bool SendFilterCallback(CpuContext* cpu, uint32_t connection, uint32_t id, uint32_t message, int32_t length, uint32_t reliable) {
    return FilterCallback(cpu, connection, kSendFilters, id, message, length, reliable);
}

bool ReceiveFilterCallback(CpuContext* cpu, uint32_t connection, uint32_t id, uint32_t message, int32_t length, uint32_t reliable) {
    return FilterCallback(cpu, connection, kReceiveFilters, id, message, length, reliable);
}

// 0x8050DC50: the send or the receive dump.
bool DumpCallback(CpuContext* cpu, uint32_t socket, uint32_t connection, uint32_t ip, uint32_t port, uint32_t reset, uint32_t message, int32_t length,
                  bool send) {
    if (!socket)
        return true;

    const uint32_t callback = Memory::Read32(socket + (send ? kSendDumpCallback : kReceiveDumpCallback));

    if (!callback)
        return true;

    if (!length || !message) {
        message = 0;
        length = 0;
    }

    Adjust(socket + kSocketCallbackLevel, 1);

    if (connection)
        Adjust(connection + kCallbackLevel, 1);

    Guest(cpu, callback, {socket, connection, ip, port, reset, message, uint32_t(length)});
    Adjust(socket + kSocketCallbackLevel, -1);

    if (connection)
        Adjust(connection + kCallbackLevel, -1);

    return SocketStillOpen(cpu, socket);
}

// 0x8050DD50: whether the game took a message no connection knows.
bool UnrecognizedMessageCallback(CpuContext* cpu, uint32_t socket, uint32_t ip, uint32_t port, uint32_t message, int32_t length, uint32_t handled) {
    Memory::Write32(handled, 0);

    if (!socket)
        return true;

    const uint32_t callback = Memory::Read32(socket + kUnrecognizedMessageCallback);

    if (!callback)
        return true;

    if (!length || !message) {
        message = 0;
        length = 0;
    }

    Adjust(socket + kSocketCallbackLevel, 1);
    Memory::Write32(handled, Guest(cpu, callback, {socket, ip, port, message, uint32_t(length)}));
    Adjust(socket + kSocketCallbackLevel, -1);
    return SocketStillOpen(cpu, socket);
}

// 0x8050DE20, 0x8050DE80.
uint32_t NewOutgoingConnection(CpuContext* cpu, uint32_t socket, uint32_t out, uint32_t ip, uint32_t port) {
    const uint32_t result = NewSocketConnection(cpu, socket, out, ip, port);

    if (result)
        return result;

    Memory::Write32(Memory::Read32(out) + kState, kAwaitingServerChallenge);
    Memory::Write32(Memory::Read32(out) + kInitiated, 1);
    return 0;
}

uint32_t NewIncomingConnection(CpuContext* cpu, uint32_t socket, uint32_t out, uint32_t ip, uint32_t port) {
    const uint32_t result = NewSocketConnection(cpu, socket, out, ip, port);

    if (result)
        return result;

    Memory::Write32(Memory::Read32(out) + kState, kAwaitingClientChallenge);
    Memory::Write32(Memory::Read32(out) + kInitiated, 0);
    return 0;
}

namespace {

void CopyCallbacks(uint32_t connection, uint32_t callbacks) {
    for (uint32_t i = 0; i < 16; i += 4)
        Memory::Write32(connection + kConnectedCallback + i, Memory::Read32(callbacks + i));
}

} // namespace

// 0x8050DEE0: keeps the initial message and the callbacks, then sends the
// client challenge.
uint32_t StartConnectionAttempt(CpuContext* cpu, uint32_t connection, uint32_t message, int32_t length, uint32_t callbacks) {
    GuestFrame frame(cpu, 0x40);
    const uint32_t messageSlot = frame[8], lengthSlot = frame[0xC], challenge = frame[0x10];
    Memory::Write32(messageSlot, message);
    Memory::Write32(lengthSlot, uint32_t(length));
    MessageCheck(cpu, messageSlot, lengthSlot);

    if (int32_t(Memory::Read32(lengthSlot)) > 0) {
        const uint32_t copy = Malloc(cpu, Memory::Read32(lengthSlot));
        Memory::Write32(connection + kInitialMessage, copy);

        if (!copy)
            return kOutOfMemory;

        Memcpy4(cpu, copy, Memory::Read32(messageSlot), Memory::Read32(lengthSlot));
        Memory::Write32(connection + kInitialMessageLength, Memory::Read32(lengthSlot));
    }

    if (callbacks)
        CopyCallbacks(connection, callbacks);

    GetChallenge(cpu, challenge);
    GetResponse(cpu, connection + kResponse, challenge);
    SendClientChallenge(cpu, connection, challenge);
    Memory::Write32(connection + kState, kAwaitingServerChallenge);
    return 0;
}

// 0x8050DFC0 (gt2Accept 0x8050E600): false when the game closed the socket
// meanwhile or the connection is past waiting.
bool AcceptConnection(CpuContext* cpu, uint32_t connection, uint32_t callbacks) {
    if (Memory::Read32(connection + kFreeAtAcceptReject)) {
        Memory::Write32(connection + kFreeAtAcceptReject, 0);
        return false;
    }

    Memory::Write32(connection + kFreeAtAcceptReject, 0);

    if (State(connection) != kAwaitingAcceptReject)
        return false;

    SendAccept(cpu, connection);
    Memory::Write32(connection + kState, kConnected);

    if (callbacks)
        CopyCallbacks(connection, callbacks);

    return true;
}

// 0x8050E070 (gt2Reject 0x8050E610).
void RejectConnection(CpuContext* cpu, uint32_t connection, uint32_t message, int32_t length) {
    GuestFrame frame(cpu, 0x20);
    const uint32_t messageSlot = frame[8], lengthSlot = frame[0xC];
    Memory::Write32(messageSlot, message);
    Memory::Write32(lengthSlot, uint32_t(length));
    Memory::Write32(connection + kFreeAtAcceptReject, 0);

    if (State(connection) != kAwaitingAcceptReject)
        return;

    MessageCheck(cpu, messageSlot, lengthSlot);
    SendReject(cpu, connection, Memory::Read32(messageSlot), int32_t(Memory::Read32(lengthSlot)));
    Memory::Write32(connection + kState, kClosing);
}

// 0x8050E0E0: a send that keeps its time.
bool RawSend(CpuContext* cpu, uint32_t connection, uint32_t message, int32_t length) {
    if (!SocketSend(cpu, SocketOf(connection), Memory::Read32(connection + kIp), Memory::Read16(connection + kPort), message, length))
        return false;

    Memory::Write32(connection + kLastSend, CurrentTime(cpu));
    return true;
}

// 0x8050E430 gti2ConnectionClosed, also inlined: out of the socket's table
// into its closed list.
void ConnectionClosed(CpuContext* cpu, uint32_t connection) {
    if (State(connection) == kClosed)
        return;

    GuestFrame frame(cpu, 0x10);
    const uint32_t slot = frame[8];
    Memory::Write32(connection + kState, kClosed);
    Memory::Write32(slot, connection);
    TableRemove(cpu, Memory::Read32(SocketOf(Memory::Read32(slot)) + kConnections), slot);
    ArrayAppend(cpu, Memory::Read32(SocketOf(Memory::Read32(slot)) + kClosedConnections), slot);
}

// gti2ConnectionError, inlined where GT2 fails a connection: one still
// connecting reports `result` to its initiator (an incoming one waiting for
// the game's accept is freed then); a connected one is closed with `reason`.
bool ConnectionError(CpuContext* cpu, uint32_t connection, uint32_t result, uint32_t reason) {
    const int32_t state = State(connection);

    if (state < kConnected) {
        if (Memory::Read32(connection + kInitiated)) {
            ConnectionClosed(cpu, connection);
            return ConnectedCallback(cpu, connection, result, 0, 0);
        }

        if (state == kAwaitingAcceptReject)
            Memory::Write32(connection + kFreeAtAcceptReject, 1);

        ConnectionClosed(cpu, connection);
        return true;
    }

    if (state == kClosed)
        return true;

    ConnectionClosed(cpu, connection);
    return ClosedCallback(cpu, connection, reason);
}

// The reason a Close or Closed gives: our own close if we were closing.
uint32_t CloseReason(uint32_t connection) {
    return State(connection) == kClosing ? kLocalClose : kRemoteClose;
}

// 0x8050E140: an initiated connection's own timeout, or 60 s for an
// incoming one that never got to the accept.
bool CheckTimeout(CpuContext* cpu, uint32_t connection, uint32_t now) {
    const int32_t state = State(connection);

    if (state >= kConnected)
        return true;

    bool timedOut = false;

    if (Memory::Read32(connection + kInitiated)) {
        const uint32_t timeout = Memory::Read32(connection + kTimeout);
        timedOut = timeout && now - Memory::Read32(connection + kStartTime) > timeout;
    } else {
        timedOut = state < kAwaitingAcceptReject && now - Memory::Read32(connection + kStartTime) > kServerTimeout;
    }

    if (!timedOut)
        return true;

    SendClosed(cpu, connection);
    ConnectionClosed(cpu, connection);
    return ConnectedCallback(cpu, connection, kTimedOut, 0, 0);
}

bool ResendMessage(CpuContext* cpu, uint32_t connection, uint32_t message);
bool SendKeepAlive(CpuContext* cpu, uint32_t connection);
bool SendAck(CpuContext* cpu, uint32_t connection);

// 0x8050E240: the timeout, then a keep-alive after 30 s without a send, the
// reliable messages unacked for 1 s again, a pending ack after 100 ms.
bool ConnectionThink(CpuContext* cpu, uint32_t connection, uint32_t now) {
    if (!CheckTimeout(cpu, connection, now))
        return false;

    if (now - Memory::Read32(connection + kLastSend) > kKeepAliveTime && !SendKeepAlive(cpu, connection))
        return false;

    const uint32_t messages = Memory::Read32(connection + kOutgoingMessages);
    const int32_t count = ArrayLength(messages);

    for (int32_t i = 0; i < count; ++i) {
        const uint32_t message = ArrayNth(Memory::Read32(connection + kOutgoingMessages), i);

        if (now - Memory::Read32(message + kMessageLastSend) > kResendTime && !ResendMessage(cpu, connection, message))
            return false;
    }

    if (!Memory::Read32(connection + kPendingAck))
        return true;

    if (now - Memory::Read32(connection + kPendingAckTime) > kPendingAckDelay && !SendAck(cpu, connection))
        return false;

    return true;
}

// 0x8050E390 gt2CloseConnection: hard closes at once (Closed to the peer,
// the closed callback, freed); soft sends Close and waits for Closed.
void CloseConnection(CpuContext* cpu, uint32_t connection, bool hard) {
    if (!hard) {
        Memory::Write32(connection + kState, kClosing);
        SendClose(cpu, connection);
        return;
    }

    if (State(connection) >= kClosed)
        return;

    ConnectionClosed(cpu, connection);
    SendClosed(cpu, connection);
    ClosedCallback(cpu, connection, kLocalClose);
    FreeSocketConnection(cpu, connection);
}

// 0x8050E490.
void FreeConnection(CpuContext* cpu, uint32_t connection) {
    if (Memory::Read32(connection + kInitialMessage))
        Free(cpu, Memory::Read32(connection + kInitialMessage));

    if (Memory::Read32(connection + kIncomingBuffer + kBufferData))
        Free(cpu, Memory::Read32(connection + kIncomingBuffer + kBufferData));

    if (Memory::Read32(connection + kOutgoingBuffer + kBufferData))
        Free(cpu, Memory::Read32(connection + kOutgoingBuffer + kBufferData));

    for (const uint32_t array : {kIncomingMessages, kOutgoingMessages, kSendFilters, kReceiveFilters})
        if (Memory::Read32(connection + array))
            ArrayFree(cpu, Memory::Read32(connection + array));

    Free(cpu, connection);
}

// The table map function that closes each connection hard (0x8050E8E0).
constexpr uint32_t kCloseConnectionHardMap = 0x8050E8E0;

// 0x8050E550 gt2CloseSocket.
void CloseSocketPublic(CpuContext* cpu, uint32_t socket) {
    TableMap(cpu, Memory::Read32(socket + kConnections), kCloseConnectionHardMap, 0);
    CloseGt2Socket(cpu, socket);
}

// 0x8050E5A0 gt2Think: receive, then the connections' timers, then free
// the closed ones.
void Think(CpuContext* cpu, uint32_t socket) {
    if (ReceiveMessages(cpu, socket) && SocketConnectionsThink(cpu, socket))
        FreeClosedConnections(cpu, socket);
}

// 0x8050E620 gt2Connect: to "host:port"; a blocking connect thinks (and
// sleeps 1 ms) until it is connected or failed.
uint32_t Connect(CpuContext* cpu, uint32_t socket, uint32_t out, uint32_t address, uint32_t message, int32_t length, uint32_t timeout, uint32_t callbacks,
                 bool blocking) {
    GuestFrame frame(cpu, 0x40);
    const uint32_t port = frame[8], ip = frame[0xC], connectionSlot = frame[0x10];

    if (!StringToAddress(cpu, address, ip, port) || !Memory::Read32(ip) || !Memory::Read16(port))
        return kAddressError;

    if ((Memory::Read32(ip) & 0xE0000000) == 0xE0000000)
        return kAddressError;

    const uint32_t result = NewOutgoingConnection(cpu, socket, connectionSlot, Memory::Read32(ip), Memory::Read16(port));

    if (result)
        return result;

    Memory::Write32(Memory::Read32(connectionSlot) + kTimeout, timeout);
    const uint32_t started = StartConnectionAttempt(cpu, Memory::Read32(connectionSlot), message, length, callbacks);

    if (started) {
        FreeSocketConnection(cpu, Memory::Read32(connectionSlot));
        return started;
    }

    if (!blocking) {
        if (out)
            Memory::Write32(out, Memory::Read32(connectionSlot));

        return 0;
    }

    Adjust(Memory::Read32(connectionSlot) + kCallbackLevel, 1);

    for (;;) {
        if (ReceiveMessages(cpu, socket) && SocketConnectionsThink(cpu, socket))
            FreeClosedConnections(cpu, socket);

        if (uint32_t(State(Memory::Read32(connectionSlot))) >= uint32_t(kConnected))
            break;

        Guest(cpu, 0x8050CBC0, {1});
    }

    const uint32_t connection = Memory::Read32(connectionSlot);
    Adjust(connection + kCallbackLevel, -1);

    if (State(connection) == kConnected)
        Memory::Write32(out, connection);

    return Memory::Read32(connection + kConnectionResult);
}

// 0x8050E7D0 gt2Send: a VDP reliable message must carry its own length;
// with send filters the first filter gets it instead.
uint32_t SendPublic(CpuContext* cpu, uint32_t connection, uint32_t message, int32_t length, bool reliable) {
    GuestFrame frame(cpu, 0x20);
    const uint32_t messageSlot = frame[8], lengthSlot = frame[0xC], prefix = frame[0x10];
    Memory::Write32(messageSlot, message);
    Memory::Write32(lengthSlot, uint32_t(length));

    if (State(connection) != kConnected)
        return kInvalidConnection;

    MessageCheck(cpu, messageSlot, lengthSlot);

    if (reliable && Memory::Read32(SocketOf(connection) + kProtocolType) == kVdpProtocol) {
        Memcpy4(cpu, prefix, Memory::Read32(messageSlot), 2);

        if (Memory::Read32(lengthSlot) != Memory::Read16(prefix) + Memory::Read32(SocketOf(connection) + kProtocolOffset))
            return kInvalidMessage;
    }

    if (ArrayLength(Memory::Read32(connection + kSendFilters))) {
        SendFilterCallback(cpu, connection, 0, Memory::Read32(messageSlot), int32_t(Memory::Read32(lengthSlot)), reliable);
        return 0;
    }

    return Send(cpu, connection, Memory::Read32(messageSlot), int32_t(Memory::Read32(lengthSlot)), reliable) ? 0 : kSendFailed;
}

// 0x8050E930: drops the outgoing messages before the acked serial number
// and moves the rest to the buffer's start.
bool ProcessAck(CpuContext* cpu, uint32_t connection, uint32_t serial) {
    const uint32_t messages = Memory::Read32(connection + kOutgoingMessages);
    const int32_t count = ArrayLength(messages);

    if (!count)
        return true;

    int32_t acked = 0;

    while (acked < count && int16_t(Memory::Read16(ArrayNth(Memory::Read32(connection + kOutgoingMessages), acked) + kMessageSerial) - serial) < 0)
        ++acked;

    if (!acked)
        return true;

    while (acked--)
        ArrayDeleteAt(cpu, Memory::Read32(connection + kOutgoingMessages), acked);

    const int32_t left = ArrayLength(Memory::Read32(connection + kOutgoingMessages));

    if (!left) {
        Memory::Write32(connection + kOutgoingBuffer + kBufferLength, 0);
        return true;
    }

    const uint32_t shift = Memory::Read32(ArrayNth(Memory::Read32(connection + kOutgoingMessages), 0) + kMessageStart);

    for (int32_t i = 0; i < left; ++i) {
        const uint32_t message = ArrayNth(Memory::Read32(connection + kOutgoingMessages), i);
        Memory::Write32(message + kMessageStart, Memory::Read32(message + kMessageStart) - shift);
    }

    BufferShorten(cpu, connection + kOutgoingBuffer, 0, int32_t(shift));
    return true;
}

namespace {

// A Closed (FE FE 68, after a VDP length of 3) to an address.
bool SendClosedTo(CpuContext* cpu, uint32_t socket, uint32_t ip, uint32_t port) {
    GuestFrame frame(cpu, 0x20);
    const uint32_t prefix = frame[8], message = frame[0xC];
    uint32_t length = 0;

    if (Memory::Read32(socket + kProtocolType) == kVdpProtocol) {
        Memory::Write16(prefix, 3);
        Memcpy4(cpu, message, prefix, 2);
        length = 2;
    }

    Memcpy4(cpu, message + length, kMagic, kMagicBytes);
    length += kMagicBytes;
    Memory::Write8(message + length, kMsgClosed);
    return SocketSend(cpu, socket, ip, port, message, int32_t(length + 1));
}

bool SendClosedToPeer(CpuContext* cpu, uint32_t connection) {
    return SendClosedTo(cpu, SocketOf(connection), Memory::Read32(connection + kIp), Memory::Read16(connection + kPort));
}

// The message BeginReliableMessage just queued, sent.
bool SendLastReliable(CpuContext* cpu, uint32_t connection) {
    const uint32_t messages = Memory::Read32(connection + kOutgoingMessages);
    const uint32_t message = ArrayNth(messages, ArrayLength(messages) - 1);

    if (!RawSend(cpu, connection, Memory::Read32(connection + kOutgoingBuffer + kBufferData) + Memory::Read32(message + kMessageStart),
                 int32_t(Memory::Read32(message + kMessageLength))))
        return false;

    Memory::Write32(connection + kPendingAck, 0);
    return true;
}

// A reliable message of `type` with `length` bytes after its header, sent.
bool SendReliable(CpuContext* cpu, uint32_t connection, uint32_t type, uint32_t data, int32_t length, int32_t extra) {
    GuestFrame frame(cpu, 0x20);
    const uint32_t full = frame[8];

    if (!BeginReliableMessage(cpu, connection, type, int32_t(ProtocolOffset(SocketOf(connection))) + extra, full))
        return false;

    if (Memory::Read32(full))
        return true;

    if (data)
        BufferWriteData(cpu, connection + kOutgoingBuffer, data, length);

    return SendLastReliable(cpu, connection);
}

} // namespace

// 0x8050EA60: the client gets the server's challenge (and its response to
// ours): answer with ours and the initial message, then await acceptance.
bool ReceivedServerChallenge(CpuContext* cpu, uint32_t connection, uint32_t message, int32_t length) {
    if (State(connection) != kAwaitingServerChallenge || length < 0x40 || !CheckResponse(message, connection + kResponse))
        return ConnectionError(cpu, connection, kNegotiationError, kCommunicationError);

    GuestFrame frame(cpu, 0x40);
    const uint32_t full = frame[8], response = frame[0x10];
    GetResponse(cpu, response, message + kChallengeBytes);
    const int32_t initialLength = int32_t(Memory::Read32(connection + kInitialMessageLength));
    const uint32_t initial = Memory::Read32(connection + kInitialMessage);

    if (!BeginReliableMessage(cpu, connection, kMsgClientResponse, initialLength + int32_t(ProtocolOffset(SocketOf(connection))) + 0x27, full))
        return false;

    if (!Memory::Read32(full)) {
        BufferWriteData(cpu, connection + kOutgoingBuffer, response, kChallengeBytes);
        BufferWriteData(cpu, connection + kOutgoingBuffer, initial, initialLength);

        if (!SendLastReliable(cpu, connection))
            return false;
    }

    if (Memory::Read32(connection + kInitialMessage)) {
        Free(cpu, Memory::Read32(connection + kInitialMessage));
        Memory::Write32(connection + kInitialMessage, 0);
    }

    Memory::Write32(connection + kState, kAwaitingAcceptance);
    return true;
}

// 0x8050EDA0: the server gets the client's response: with no connect
// attempt callback it closes the connection, else the game decides.
bool ReceivedClientResponse(CpuContext* cpu, uint32_t connection, uint32_t message, int32_t length) {
    if (State(connection) != kAwaitingClientResponse || length < 0x20 || !CheckResponse(message, connection + kResponse))
        return ConnectionError(cpu, connection, kNegotiationError, kCommunicationError);

    const uint32_t socket = SocketOf(connection);

    if (!Memory::Read32(socket + kConnectAttemptCallback)) {
        if (!SendClosedTo(cpu, socket, Memory::Read32(connection + kIp), Memory::Read16(connection + kPort)))
            return false;

        ConnectionClosed(cpu, connection);
        return true;
    }

    Memory::Write32(connection + kState, kAwaitingAcceptReject);
    const uint32_t latency = CurrentTime(cpu) - Memory::Read32(connection + kChallengeTime);
    return ConnectAttemptCallback(cpu, SocketOf(connection), connection, Memory::Read32(connection + kIp), Memory::Read16(connection + kPort), latency,
                                  message + kChallengeBytes, length - int32_t(kChallengeBytes));
}

// 0x8050F0B0: a reliable message in order; counts the expected serial
// number on first.
bool HandleReliableMessage(CpuContext* cpu, uint32_t connection, uint32_t type, uint32_t message, int32_t length) {
    Memory::Write16(connection + kExpectedSerialNumber, uint16_t(Memory::Read16(connection + kExpectedSerialNumber) + 1));

    switch (type) {
    case kMsgAppReliable: {
        const int32_t state = State(connection);

        if (state != kConnected && state != kClosing)
            return ConnectionError(cpu, connection, kNegotiationError, kCommunicationError);

        if (!ArrayLength(Memory::Read32(connection + kReceiveFilters)))
            return ReceivedCallback(cpu, connection, message, length, 1);

        return ReceiveFilterCallback(cpu, connection, 0, message, length, 1);
    }

    case kMsgClientChallenge: {
        if (State(connection) != kAwaitingClientChallenge || length < 0x20)
            return ConnectionError(cpu, connection, kNegotiationError, kCommunicationError);

        GuestFrame frame(cpu, 0x80);
        const uint32_t full = frame[0xC], response = frame[0x18], challenge = frame[0x38];
        GetResponse(cpu, response, message);
        GetChallenge(cpu, challenge);
        GetResponse(cpu, connection + kResponse, challenge);

        if (!BeginReliableMessage(cpu, connection, kMsgServerChallenge, int32_t(ProtocolOffset(SocketOf(connection))) + 0x47, full))
            return false;

        if (!Memory::Read32(full)) {
            BufferWriteData(cpu, connection + kOutgoingBuffer, response, kChallengeBytes);
            BufferWriteData(cpu, connection + kOutgoingBuffer, challenge, kChallengeBytes);

            if (!SendLastReliable(cpu, connection))
                return false;

            Memory::Write32(connection + kChallengeTime, Memory::Read32(connection + kLastSend));
        }

        Memory::Write32(connection + kState, kAwaitingClientResponse);
        return true;
    }

    case kMsgServerChallenge:
        return ReceivedServerChallenge(cpu, connection, message, length);

    case kMsgClientResponse:
        return ReceivedClientResponse(cpu, connection, message, length);

    case kMsgAccept:
        if (State(connection) != kAwaitingAcceptance)
            return ConnectionError(cpu, connection, kNegotiationError, kCommunicationError);

        Memory::Write32(connection + kState, kConnected);
        return ConnectedCallback(cpu, connection, kSuccess, 0, 0);

    case kMsgReject:
        if (State(connection) != kAwaitingAcceptance)
            return ConnectionError(cpu, connection, kNegotiationError, kCommunicationError);

        ConnectionClosed(cpu, connection);

        if (!SendClosedToPeer(cpu, connection))
            return false;

        return ConnectedCallback(cpu, connection, kRejected, message, length);

    case kMsgClose:
        if (!SendClosedToPeer(cpu, connection))
            return false;

        return ConnectionError(cpu, connection, kRejected, CloseReason(connection));
    }

    return true;
}

// 0x8050F870: incoming messages by serial number.
int32_t CompareIncoming(uint32_t a, uint32_t b) {
    return int16_t(Memory::Read16(a + kIncomingSerial) - Memory::Read16(b + kIncomingSerial));
}

bool SendNack(CpuContext* cpu, uint32_t connection, uint32_t first, uint32_t last);

// The incoming messages' compare (0x8050F870), for the sorted insert.
constexpr uint32_t kCompareIncoming = 0x8050F870;

// 0x8050F890: a reliable message ahead of the expected one, kept in order
// (a duplicate is dropped); the gap before it is nacked. `full` when it
// does not fit.
bool BufferIncoming(CpuContext* cpu, uint32_t connection, uint32_t type, uint32_t serial, uint32_t data, int32_t length, uint32_t full) {
    const int32_t count = ArrayLength(Memory::Read32(connection + kIncomingMessages));

    for (int32_t i = 0; i < count; ++i) {
        const uint32_t buffered = Memory::Read16(ArrayNth(Memory::Read32(connection + kIncomingMessages), i) + kIncomingSerial);

        if (buffered == serial) {
            Memory::Write32(full, 0);
            return true;
        }

        if (int16_t(buffered - serial) > 0)
            break;
    }

    if (BufferFreeSpace(connection + kIncomingBuffer) < length) {
        Memory::Write32(full, 1);
        return true;
    }

    GuestFrame frame(cpu, 0x40);
    const uint32_t elem = frame[8];
    Memory::Write32(elem + kMessageStart, Memory::Read32(connection + kIncomingBuffer + kBufferLength));
    Memory::Write32(elem + kMessageLength, uint32_t(length));
    Memory::Write32(elem + kIncomingType, type);
    Memory::Write16(elem + kIncomingSerial, uint16_t(serial));
    ArrayInsertSorted(cpu, Memory::Read32(connection + kIncomingMessages), elem, kCompareIncoming);

    if (ArrayLength(Memory::Read32(connection + kIncomingMessages)) != count + 1) {
        Memory::Write32(full, 1);
        return true;
    }

    BufferWriteData(cpu, connection + kIncomingBuffer, data, length);

    if (!count) {
        if (!SendNack(cpu, connection, Memory::Read16(connection + kExpectedSerialNumber), (serial - 1) & 0xFFFF))
            return false;
    } else if (Memory::Read16(ArrayNth(Memory::Read32(connection + kIncomingMessages), count) + kIncomingSerial) == serial) {
        const uint32_t previous = Memory::Read16(ArrayNth(Memory::Read32(connection + kIncomingMessages), count - 1) + kIncomingSerial);

        if (((serial - previous) & 0xFFFF) > 1 && !SendNack(cpu, connection, (previous + 1) & 0xFFFF, (serial - 1) & 0xFFFF))
            return false;
    }

    Memory::Write32(full, 0);
    return true;
}

namespace {

// An ack is pending from now, if none was.
void PendAck(CpuContext* cpu, uint32_t connection) {
    if (Memory::Read32(connection + kPendingAck))
        return;

    Memory::Write32(connection + kPendingAck, 1);
    Memory::Write32(connection + kPendingAckTime, CurrentTime(cpu));
}

} // namespace

// 0x8050FA40: a reliable message (type < 8): its ack first; the expected
// one is handled with the buffered ones that follow it, an old one only
// acked again, a later one buffered.
bool HandleReliable(CpuContext* cpu, uint32_t connection, uint32_t type, uint32_t message, int32_t length) {
    const uint32_t socket = SocketOf(connection);
    const int32_t offset = int32_t(ProtocolOffset(socket));
    const int32_t header = offset + 7;

    if (length < header)
        return ConnectionError(cpu, connection, kNegotiationError, kCommunicationError);

    const uint32_t start = message + uint32_t(offset);
    const uint32_t serial = (uint32_t(Memory::Read8(start + 3)) << 8) | Memory::Read8(start + 4);
    const uint32_t ack = (uint32_t(Memory::Read8(start + 5)) << 8) | Memory::Read8(start + 6);
    uint32_t data;
    int32_t dataLength;

    // A VDP application message keeps its length: moved in front of its data.
    if (Memory::Read32(socket + kProtocolType) == kVdpProtocol && type == kMsgAppReliable) {
        Memory::Write8(start + 5, Memory::Read8(message));
        Memory::Write8(message + ProtocolOffset(SocketOf(connection)) + 6, Memory::Read8(message + 1));
        const int32_t skip = header - int32_t(ProtocolOffset(SocketOf(connection)));
        data = message + uint32_t(skip);
        dataLength = length - skip;
    } else {
        data = message + uint32_t(header);
        dataLength = length - header;
    }

    if (!ProcessAck(cpu, connection, ack))
        return false;

    const uint32_t expected = Memory::Read16(connection + kExpectedSerialNumber);

    if (serial == expected) {
        PendAck(cpu, connection);

        if (!HandleReliableMessage(cpu, connection, type, data, dataLength))
            return false;

        for (;;) {
            int32_t i = ArrayLength(Memory::Read32(connection + kIncomingMessages)) - 1;
            uint32_t buffered = 0;

            for (; i >= 0; --i) {
                buffered = ArrayNth(Memory::Read32(connection + kIncomingMessages), i);

                if (Memory::Read16(buffered + kIncomingSerial) == Memory::Read16(connection + kExpectedSerialNumber))
                    break;
            }

            if (i < 0)
                return true;

            if (!HandleReliableMessage(cpu, connection, Memory::Read32(buffered + kIncomingType),
                                       Memory::Read32(connection + kIncomingBuffer + kBufferData) + Memory::Read32(buffered + kMessageStart),
                                       int32_t(Memory::Read32(buffered + kMessageLength))))
                return false;

            const int32_t removedStart = int32_t(Memory::Read32(buffered + kMessageStart));
            const int32_t removedLength = int32_t(Memory::Read32(buffered + kMessageLength));
            ArrayDeleteAt(cpu, Memory::Read32(connection + kIncomingMessages), i);
            const int32_t left = ArrayLength(Memory::Read32(connection + kIncomingMessages));

            for (int32_t j = 0; j < left; ++j) {
                const uint32_t other = ArrayNth(Memory::Read32(connection + kIncomingMessages), j);

                if (int32_t(Memory::Read32(other + kMessageStart)) > removedStart)
                    Memory::Write32(other + kMessageStart, Memory::Read32(other + kMessageStart) - uint32_t(removedLength));
            }

            BufferShorten(cpu, connection + kIncomingBuffer, removedStart, removedLength);
        }
    }

    if (int16_t(serial - expected) < 0) {
        PendAck(cpu, connection);
        return true;
    }

    GuestFrame frame(cpu, 0x30);
    const uint32_t full = frame[0xC];

    if (!BufferIncoming(cpu, connection, type, serial, data, dataLength, full))
        return false;

    if (!Memory::Read32(full))
        return true;

    if (!SendClosedToPeer(cpu, connection))
        return false;

    return ConnectionError(cpu, connection, kOutOfMemory, kNotEnoughMemory);
}


// 0x8050FE90: resends the outgoing messages a Nack names (one serial
// number, or a range).
bool HandleNack(CpuContext* cpu, uint32_t connection, uint32_t data, int32_t length) {
    const uint32_t first = Memory::Read16(data);
    uint32_t last = first;

    if (length != 2) {
        if (length != 4)
            return ConnectionError(cpu, connection, kNegotiationError, kCommunicationError);

        last = Memory::Read16(data + 2);
    }

    const int32_t count = ArrayLength(Memory::Read32(connection + kOutgoingMessages));

    for (int32_t i = 0; i < count; ++i) {
        const uint32_t message = ArrayNth(Memory::Read32(connection + kOutgoingMessages), i);
        const uint32_t serial = Memory::Read16(message + kMessageSerial);

        if (int16_t(serial - first) >= 0 && int16_t(serial - last) <= 0 && !ResendMessage(cpu, connection, message))
            return false;
    }

    return true;
}

bool PingCallback(CpuContext* cpu, uint32_t connection, uint32_t latency);

// 0x80510080: Ack, Nack, Ping (answered with a Pong in place), Pong (the
// ping callback gets the latency from the time it carries) and Closed.
bool HandleUnreliable(CpuContext* cpu, uint32_t connection, uint32_t type, uint32_t message, int32_t length) {
    const int32_t header = int32_t(ProtocolOffset(SocketOf(connection))) + 3;
    const uint32_t data = message + uint32_t(header);
    const int32_t dataLength = length - header;

    switch (type) {
    case kMsgAck:
        if (dataLength == 2)
            return ProcessAck(cpu, connection, Memory::Read16(data));

        return ConnectionError(cpu, connection, kNegotiationError, kCommunicationError);

    case kMsgNack:
        return HandleNack(cpu, connection, data, dataLength);

    case kMsgPing:
        Memory::Write8(message + 2, kMsgPong);
        return RawSend(cpu, connection, message, length);

    case kMsgPong: {
        if (!Memory::Read32(connection + kPingCallback))
            return true;

        if (dataLength != 8 || !BytesEqual(data, kTimeTag, 4))
            return true;

        uint32_t sent = 0;

        for (uint32_t i = 0; i < 4; ++i)
            sent = (sent << 8) | Memory::Read8(data + 4 + i);

        return PingCallback(cpu, connection, CurrentTime(cpu) - sent);
    }

    case kMsgClosed:
        if (State(connection) == kClosed)
            return true;

        return ConnectionError(cpu, connection, kRejected, CloseReason(connection));
    }

    return true;
}

uint32_t FindConnection(CpuContext* cpu, uint32_t socket, uint32_t ip, uint32_t port);

// 0x80510350: one datagram. Without a connection: the game's unrecognized
// callback, a client challenge (a new incoming connection when the game
// takes connect attempts), else Closed back. With one: its message, or a
// Closed answer once it is closed. Application data that starts with the
// magic was escaped with a second magic.
bool HandleMessage(CpuContext* cpu, uint32_t socket, uint32_t message, int32_t length, uint32_t ip, uint32_t port) {
    GuestFrame frame(cpu, 0x50);
    const uint32_t handled = frame[0x10], connectionSlot = frame[0x14];
    const int32_t offset = int32_t(ProtocolOffset(socket));
    uint32_t data = message + uint32_t(offset);
    int32_t dataLength = length - offset;
    Memory::Write32(connectionSlot, FindConnection(cpu, socket, ip, port));

    if (Memory::Read32(socket + kReceiveDumpCallback) && !DumpCallback(cpu, socket, Memory::Read32(connectionSlot), ip, port, 0, message, length, false))
        return false;

    bool magic = dataLength > 2 && BytesEqual(data, kMagic, kMagicBytes);

    if (!Memory::Read32(connectionSlot)) {
        if (!UnrecognizedMessageCallback(cpu, socket, ip, port, message, length, handled))
            return false;

        if (Memory::Read32(handled))
            return true;

        if (magic && Memory::Read8(data + 2) == kMsgClientChallenge) {
            if (!Memory::Read32(socket + kConnectAttemptCallback))
                return true;

            const uint32_t result = NewIncomingConnection(cpu, socket, connectionSlot, ip, port);

            if (result == kDuplicateAddress)
                return true;

            if (result)
                return SendClosedTo(cpu, socket, ip, port);
        } else {
            if (magic && Memory::Read8(data + 2) == kMsgClosed)
                return true;

            return SendClosedTo(cpu, socket, ip, port);
        }
    }

    const uint32_t connection = Memory::Read32(connectionSlot);

    if (State(connection) == kClosed) {
        if (magic && Memory::Read8(data + 2) == kMsgClosed)
            return true;

        return SendClosedToPeer(cpu, connection);
    }

    if (magic && dataLength >= 4 && BytesEqual(data + 2, kMagic, kMagicBytes)) {
        magic = false;
        data += 2;
        Memory::Write8(message + 3, Memory::Read8(message + 1));
        Memory::Write8(message + 2, Memory::Read8(message));
        message += 2;
        length -= 2;
    }

    if (magic) {
        const uint32_t type = Memory::Read8(data + 2);

        if (type < kNumReliableMessages)
            return HandleReliable(cpu, connection, type, message, length);

        return HandleUnreliable(cpu, connection, type, message, length);
    }

    const int32_t state = State(connection);

    if (state < kConnected)
        return UnrecognizedMessageCallback(cpu, socket, ip, port, message, length, handled);

    if (state != kConnected && state != kClosing)
        return true;

    if (ArrayLength(Memory::Read32(connection + kReceiveFilters)))
        return ReceiveFilterCallback(cpu, connection, 0, message, length, 0);

    return ReceivedCallback(cpu, connection, message, length, 0);
}

// 0x805107E0: the peer's port refused a datagram. A connect attempt still
// waiting for the server's challenge only fails at its timeout.
bool HandleConnectionReset(CpuContext* cpu, uint32_t socket, uint32_t ip, uint32_t port) {
    const uint32_t connection = FindConnection(cpu, socket, ip, port);

    if (Memory::Read32(socket + kReceiveDumpCallback) && !DumpCallback(cpu, socket, connection, ip, port, 1, 0, 0, false))
        return false;

    if (!connection)
        return true;

    if (State(connection) == kAwaitingServerChallenge) {
        const uint32_t timeout = Memory::Read32(connection + kTimeout);

        if (!timeout || CurrentTime(cpu) - Memory::Read32(connection + kStartTime) < timeout)
            return true;

        return ConnectionError(cpu, connection, kTimedOut, kRemoteClose);
    }

    return ConnectionError(cpu, connection, kRejected, kRemoteClose);
}

// 0x80510A00: the peer is unreachable.
bool HandleHostUnreachable(CpuContext* cpu, uint32_t socket, uint32_t ip, uint32_t port, bool send) {
    const uint32_t connection = FindConnection(cpu, socket, ip, port);

    if (Memory::Read32(socket + kReceiveDumpCallback) && !DumpCallback(cpu, socket, connection, ip, port, 1, 0, 0, send))
        return false;

    if (!connection)
        return true;

    return ConnectionError(cpu, connection, kTimedOut, kRemoteClose);
}


// 0x80510B30: every datagram waiting on the socket (into the 4 KiB receive
// buffer). A refused or unreachable peer fails its connection; EMSGSIZE is
// skipped; another error fails the socket.
bool ReceiveMessages(CpuContext* cpu, uint32_t socket) {
    GuestFrame frame(cpu, 0x40);
    const uint32_t fromLength = frame[8], from = frame[0x10];

    while (CanReceiveOnSocket(cpu, Memory::Read32(socket + kSocket))) {
        Memory::Write32(fromLength, kAddrBytes);
        const int32_t received = RecvFrom(cpu, Memory::Read32(socket + kSocket), kReceiveBuffer, kReceiveBufferBytes, 0, from, fromLength);

        if (received != -1) {
            if (!HandleMessage(cpu, socket, kReceiveBuffer, received, Memory::Read32(from + kAddrIp), Short(Memory::Read16(from + kAddrPort))))
                return false;

            continue;
        }

        const int32_t error = LastSocketError();

        if (error == -0xF) {
            if (!HandleConnectionReset(cpu, socket, Memory::Read32(from + kAddrIp), Short(Memory::Read16(from + kAddrPort))))
                return false;
        } else if (error == -0x17) {
            if (!HandleHostUnreachable(cpu, socket, Memory::Read32(from + kAddrIp), Short(Memory::Read16(from + kAddrPort)), false))
                return false;
        } else if (error != -0x23) {
            SocketError(cpu, socket);
            return false;
        }
    }

    return true;
}

// 0x80510F30: room for a reliable message of `length` bytes and its header
// (VDP length, magic, type, serial number, expected serial number) in the
// outgoing buffer. Out of room, the connection fails (`full` set).
bool BeginReliableMessage(CpuContext* cpu, uint32_t connection, uint32_t type, int32_t length, uint32_t full) {
    GuestFrame frame(cpu, 0x50);
    const uint32_t prefix = frame[0xC], elem = frame[0x20];
    Memory::Write16(prefix, uint16_t(length - int32_t(ProtocolOffset(SocketOf(connection)))));
    bool queued = false;

    if (BufferFreeSpace(connection + kOutgoingBuffer) >= length) {
        const uint32_t serial = Memory::Read16(connection + kSerialNumber);
        Memset4(cpu, elem, 0, kBufferedMessageBytes);
        Memory::Write32(elem + kMessageStart, Memory::Read32(connection + kOutgoingBuffer + kBufferLength));
        Memory::Write32(elem + kMessageLength, uint32_t(length));
        Memory::Write16(elem + kMessageSerial, uint16_t(serial));
        Memory::Write32(elem + kMessageLastSend, CurrentTime(cpu));
        const int32_t count = ArrayLength(Memory::Read32(connection + kOutgoingMessages));
        ArrayAppend(cpu, Memory::Read32(connection + kOutgoingMessages), elem);
        queued = ArrayLength(Memory::Read32(connection + kOutgoingMessages)) == count + 1;
    }

    if (!queued) {
        if (!SendClosedToPeer(cpu, connection) || !ConnectionError(cpu, connection, kOutOfMemory, kNotEnoughMemory))
            return false;

        Memory::Write32(full, 1);
        return true;
    }

    const uint32_t outgoing = connection + kOutgoingBuffer;

    if (Memory::Read32(SocketOf(connection) + kProtocolType) == kVdpProtocol)
        BufferWriteData(cpu, outgoing, prefix, int32_t(ProtocolOffset(SocketOf(connection))));

    BufferWriteData(cpu, outgoing, kMagic, kMagicBytes);
    BufferWriteByte(outgoing, type & 0xFF);
    const uint32_t serial = Memory::Read16(connection + kSerialNumber);
    Memory::Write16(connection + kSerialNumber, uint16_t(serial + 1));
    BufferWriteUShort(outgoing, serial);
    BufferWriteUShort(outgoing, Memory::Read16(connection + kExpectedSerialNumber));
    Memory::Write32(full, 0);
    return true;
}

// 0x805112D0, 0x805113C0, 0x80511490, 0x80511590, 0x80511660.
bool SendClientChallenge(CpuContext* cpu, uint32_t connection, uint32_t challenge) {
    return SendReliable(cpu, connection, kMsgClientChallenge, challenge, kChallengeBytes, 0x27);
}

bool SendAccept(CpuContext* cpu, uint32_t connection) {
    return SendReliable(cpu, connection, kMsgAccept, 0, 0, 7);
}

bool SendReject(CpuContext* cpu, uint32_t connection, uint32_t message, int32_t length) {
    return SendReliable(cpu, connection, kMsgReject, message, length, length + 7);
}

bool SendClose(CpuContext* cpu, uint32_t connection) {
    return SendReliable(cpu, connection, kMsgClose, 0, 0, 7);
}

bool SendKeepAlive(CpuContext* cpu, uint32_t connection) {
    return SendReliable(cpu, connection, kMsgKeepAlive, 0, 0, 7);
}

// 0x80511730: an unreliable message as it is, but one that starts with the
// magic gets a second magic (dropped when it does not fit the outgoing
// buffer, where it is built).
bool SendUnreliable(CpuContext* cpu, uint32_t connection, uint32_t message, int32_t length) {
    const uint32_t offset = ProtocolOffset(SocketOf(connection));

    if (length < 2 || !BytesEqual(message + offset, kMagic, kMagicBytes))
        return RawSend(cpu, connection, message, length);

    const int32_t escaped = length + 2;
    const uint32_t outgoing = connection + kOutgoingBuffer;

    if (BufferFreeSpace(outgoing) < escaped)
        return true;

    const uint32_t start = Memory::Read32(outgoing + kBufferData) + Memory::Read32(outgoing + kBufferLength);

    if (Memory::Read32(SocketOf(connection) + kProtocolType) == kVdpProtocol)
        BufferWriteData(cpu, outgoing, message, 2);

    BufferWriteData(cpu, outgoing, kMagic, kMagicBytes);
    const uint32_t skip = ProtocolOffset(SocketOf(connection));
    BufferWriteData(cpu, outgoing, message + skip, length - int32_t(skip));

    if (!RawSend(cpu, connection, start, escaped))
        return false;

    BufferShorten(cpu, outgoing, -1, escaped);
    return true;
}

namespace {

// The header of an unreliable GT2 message: the VDP length, magic, type.
uint32_t UnreliableHeader(CpuContext* cpu, uint32_t connection, uint32_t buffer, uint32_t vdpLength, uint32_t type, uint32_t prefix) {
    uint32_t length = 0;

    if (Memory::Read32(SocketOf(connection) + kProtocolType) == kVdpProtocol) {
        Memory::Write16(prefix, uint16_t(vdpLength));
        Memcpy4(cpu, buffer, prefix, 2);
        length = 2;
    }

    Memcpy4(cpu, buffer + length, kMagic, kMagicBytes);
    length += kMagicBytes;
    Memory::Write8(buffer + length, uint8_t(type));
    return length + 1;
}

} // namespace

// 0x80511860: acks up to the expected serial number.
bool SendAck(CpuContext* cpu, uint32_t connection) {
    GuestFrame frame(cpu, 0x20);
    const uint32_t prefix = frame[8], buffer = frame[0xC];
    uint32_t length = UnreliableHeader(cpu, connection, buffer, 5, kMsgAck, prefix);
    const uint32_t expected = Memory::Read16(connection + kExpectedSerialNumber);
    Memory::Write8(buffer + length, uint8_t(expected >> 8));
    Memory::Write8(buffer + length + 1, uint8_t(expected));
    length += 2;

    if (!RawSend(cpu, connection, buffer, int32_t(length)))
        return false;

    Memory::Write32(connection + kPendingAck, 0);
    return true;
}

// 0x80511930: a Nack of one serial number or a range.
bool SendNack(CpuContext* cpu, uint32_t connection, uint32_t first, uint32_t last) {
    GuestFrame frame(cpu, 0x30);
    const uint32_t prefix = frame[8], buffer = frame[0xC];
    uint32_t length = UnreliableHeader(cpu, connection, buffer, 7, kMsgNack, prefix);
    Memory::Write8(buffer + length, uint8_t(first >> 8));
    Memory::Write8(buffer + length + 1, uint8_t(first));
    length += 2;

    if (first != last) {
        Memory::Write8(buffer + length, uint8_t(last >> 8));
        Memory::Write8(buffer + length + 1, uint8_t(last));
        length += 2;
    }

    return RawSend(cpu, connection, buffer, int32_t(length));
}

// 0x80511A30.
bool SendClosed(CpuContext* cpu, uint32_t connection) {
    return SendClosedToPeer(cpu, connection);
}

// 0x80511AF0: with the current expected serial number; a server challenge
// restarts the latency clock.
bool ResendMessage(CpuContext* cpu, uint32_t connection, uint32_t message) {
    const uint32_t offset = ProtocolOffset(SocketOf(connection));
    const uint32_t header = Memory::Read32(connection + kOutgoingBuffer + kBufferData) + Memory::Read32(message + kMessageStart) + offset;
    const uint32_t expected = Memory::Read16(connection + kExpectedSerialNumber);
    Memory::Write8(header + 5, uint8_t(expected >> 8));
    Memory::Write8(header + 6, uint8_t(expected));

    if (!RawSend(cpu, connection, Memory::Read32(connection + kOutgoingBuffer + kBufferData) + Memory::Read32(message + kMessageStart),
                 int32_t(Memory::Read32(message + kMessageLength))))
        return false;

    Memory::Write32(message + kMessageLastSend, Memory::Read32(connection + kLastSend));
    const uint32_t start = Memory::Read32(message + kMessageStart) + ProtocolOffset(SocketOf(connection)) + Memory::Read32(connection + kOutgoingBuffer + kBufferData);

    if (Memory::Read8(start + 2) == kMsgServerChallenge)
        Memory::Write32(connection + kChallengeTime, Memory::Read32(connection + kLastSend));

    return true;
}

// 0x80511BB0.
bool Send(CpuContext* cpu, uint32_t connection, uint32_t message, int32_t length, bool reliable) {
    if (!reliable)
        return SendUnreliable(cpu, connection, message, length);

    GuestFrame frame(cpu, 0x20);
    const uint32_t full = frame[8];

    if (!BeginReliableMessage(cpu, connection, kMsgAppReliable, length + 7, full))
        return false;

    if (Memory::Read32(full))
        return true;

    BufferWriteData(cpu, connection + kOutgoingBuffer, message, length);
    return SendLastReliable(cpu, connection);
}

// 0x80511CA0, 0x80511CC0: the connections table's hash and order, by
// address and port.
uint32_t ConnectionHash(uint32_t elem, uint32_t buckets) {
    const uint32_t connection = Memory::Read32(elem);
    return (Memory::Read32(connection + kIp) * Memory::Read16(connection + kPort)) % buckets;
}

int32_t ConnectionCompare(uint32_t a, uint32_t b) {
    const uint32_t x = Memory::Read32(a), y = Memory::Read32(b);
    const uint32_t ipx = Memory::Read32(x + kIp), ipy = Memory::Read32(y + kIp);

    if (ipx != ipy)
        return int32_t(ipx - ipy);

    return int16_t(Memory::Read16(x + kPort) - Memory::Read16(y + kPort));
}

// 0x80511D10.
uint32_t FindConnection(CpuContext* cpu, uint32_t socket, uint32_t ip, uint32_t port) {
    GuestFrame frame(cpu, 0xB0);
    const uint32_t keyPointer = frame[8], key = frame[0x10];
    Memory::Write32(key + kIp, ip);
    Memory::Write16(key + kPort, uint16_t(port));
    Memory::Write32(keyPointer, key);
    const uint32_t found = TableLookup(cpu, Memory::Read32(socket + kConnections), keyPointer);
    return found ? Memory::Read32(found) : 0;
}

namespace {

constexpr uint32_t kConnectionHashFn = 0x80511CA0;
constexpr uint32_t kConnectionCompareFn = 0x80511CC0;
constexpr uint32_t kClosedConnectionFree = 0x80511D00;
constexpr uint32_t kConnectionThinkMap = 0x80512590;
constexpr uint32_t kDefaultBufferSize = 0x10000;
constexpr uint32_t kAdHocProtocol = 3;

void FreeSocket(CpuContext* cpu, uint32_t socket) {
    TableFree(cpu, Memory::Read32(socket + kConnections));
    ArrayFree(cpu, Memory::Read32(socket + kClosedConnections));
    Free(cpu, socket);
}

} // namespace

// 0x80511D60 (gt2CreateSocket 0x8050E540 with type 0): a UDP socket bound to
// the local address (not for type 3), its connection table and closed list.
uint32_t CreateSocket(CpuContext* cpu, uint32_t out, uint32_t address, uint32_t outgoingSize, uint32_t incomingSize, uint32_t errorCallback, uint32_t type) {
    GuestFrame frame(cpu, 0x40);
    const uint32_t port = frame[8], length = frame[0xC], ip = frame[0x10], addr = frame[0x18];

    if (!incomingSize)
        incomingSize = kDefaultBufferSize;

    if (!outgoingSize)
        outgoingSize = kDefaultBufferSize;

    if (!StringToAddress(cpu, address, ip, port))
        return kAddressError;

    const uint32_t socket = Malloc(cpu, kSocketBytes);

    if (!socket)
        return kOutOfMemory;

    Memset4(cpu, socket, 0, kSocketBytes);
    Memory::Write32(socket + kSocket, 0xFFFFFFFF);
    Memory::Write32(socket + kIncomingBufferSize, incomingSize);
    Memory::Write32(socket + kOutgoingBufferSize, outgoingSize);
    Memory::Write32(socket + kSocketErrorCallback, errorCallback);
    Memory::Write32(socket + kConnections, TableNew2(cpu, 4, 0x20, 2, kConnectionHashFn, kConnectionCompareFn, 0));

    if (!Memory::Read32(socket + kConnections)) {
        Free(cpu, socket);
        return kOutOfMemory;
    }

    Memory::Write32(socket + kClosedConnections, ArrayNew(cpu, 4, 4, kClosedConnectionFree));

    if (!Memory::Read32(socket + kClosedConnections)) {
        TableFree(cpu, Memory::Read32(socket + kConnections));
        Free(cpu, socket);
        return kOutOfMemory;
    }

    Memory::Write32(socket + kSocket, uint32_t(Socket(cpu, 2, 2, 17)));
    Memory::Write32(socket + kProtocolType, type);
    Memory::Write32(socket + kProtocolOffset, type == kAdHocProtocol ? 0 : type);

    if (int32_t(Memory::Read32(socket + kSocket)) == -1) {
        FreeSocket(cpu, socket);
        return 3;
    }

    Memset4(cpu, addr, 0, kAddrBytes);
    Memory::Write8(addr + kAddrFamily, 2);
    Memory::Write32(addr + kAddrIp, Memory::Read32(ip));
    Memory::Write16(addr + kAddrPort, uint16_t(Short(Memory::Read16(port))));

    if (type != kAdHocProtocol && Bind(cpu, Memory::Read32(socket + kSocket), addr, kAddrBytes) == -1) {
        CloseSocket(cpu, Memory::Read32(socket + kSocket));
        FreeSocket(cpu, socket);
        return 3;
    }

    Memory::Write32(length, kAddrBytes);
    GetSockName(cpu, Memory::Read32(socket + kSocket), addr, length);
    Memory::Write32(socket + kSocketIp, Memory::Read32(addr + kAddrIp));
    Memory::Write16(socket + kSocketPort, uint16_t(Short(Memory::Read16(addr + kAddrPort))));
    Memory::Write32(out, socket);
    return 0;
}

// 0x80511FA0: freed once no callback runs on it, else marked to close.
void CloseGt2Socket(CpuContext* cpu, uint32_t socket) {
    if (Memory::Read32(socket + kSocketCallbackLevel)) {
        Memory::Write32(socket + kSocketClose, 1);
        return;
    }

    GameSpy::CloseSocket(cpu, Memory::Read32(socket + kSocket));
    FreeSocket(cpu, socket);
}

// 0x80512020: a connection to ip:port with its buffers and message lists,
// entered in the socket's table (5: one exists, 1: out of memory).
uint32_t NewSocketConnection(CpuContext* cpu, uint32_t socket, uint32_t out, uint32_t ip, uint32_t port) {
    GuestFrame frame(cpu, 0x170);
    const uint32_t keyPointer2 = frame[8], keyPointer = frame[0xC], slot = frame[0x10], key2 = frame[0x18], key = frame[0xB8];
    Memory::Write32(slot, 0);
    Memory::Write32(key + kIp, ip);
    Memory::Write16(key + kPort, uint16_t(port));
    Memory::Write32(keyPointer, key);
    uint32_t found = TableLookup(cpu, Memory::Read32(socket + kConnections), keyPointer);

    if (found && Memory::Read32(found))
        return kDuplicateAddress;

    const uint32_t connection = Malloc(cpu, kConnectionBytes);
    Memory::Write32(slot, connection);

    if (connection) {
        Memset4(cpu, connection, 0, kConnectionBytes);
        Memory::Write32(connection + kIp, ip);
        Memory::Write16(connection + kPort, uint16_t(port));
        Memory::Write32(connection + kConnSocket, socket);
        Memory::Write32(connection + kStartTime, CurrentTime(cpu));
        Memory::Write32(connection + kLastSend, Memory::Read32(connection + kStartTime));
        Memory::Write16(connection + kSerialNumber, 0);
        Memory::Write16(connection + kExpectedSerialNumber, 0);

        if (AllocateBuffer(cpu, connection + kIncomingBuffer, Memory::Read32(socket + kIncomingBufferSize)) &&
            AllocateBuffer(cpu, connection + kOutgoingBuffer, Memory::Read32(socket + kOutgoingBufferSize))) {
            bool lists = true;

            for (const auto& [array, size, count] : {std::tuple{kIncomingMessages, 0x10u, 0x40}, {kOutgoingMessages, 0x10u, 0x40}, {kSendFilters, 4u, 2},
                                                     {kReceiveFilters, 4u, 2}}) {
                Memory::Write32(connection + array, ArrayNew(cpu, size, count, 0));

                if (!Memory::Read32(connection + array)) {
                    lists = false;
                    break;
                }
            }

            if (lists) {
                TableEnter(cpu, Memory::Read32(socket + kConnections), slot);
                Memory::Write32(key2 + kIp, ip);
                Memory::Write16(key2 + kPort, uint16_t(port));
                Memory::Write32(keyPointer2, key2);
                found = TableLookup(cpu, Memory::Read32(socket + kConnections), keyPointer2);
                const uint32_t entered = found ? Memory::Read32(found) : 0;
                Memory::Write32(out, entered);

                if (entered)
                    return 0;
            }
        }
    }

    if (const uint32_t failed = Memory::Read32(slot)) {
        Free(cpu, Memory::Read32(failed + kIncomingBuffer + kBufferData));
        Free(cpu, Memory::Read32(failed + kOutgoingBuffer + kBufferData));

        for (const uint32_t array : {kIncomingMessages, kOutgoingMessages, kSendFilters, kReceiveFilters})
            if (Memory::Read32(Memory::Read32(slot) + array))
                ArrayFree(cpu, Memory::Read32(Memory::Read32(slot) + array));

        Free(cpu, Memory::Read32(slot));
    }

    return kOutOfMemory;
}

// 0x805122A0: a closed connection leaves the closed list (which frees it), a
// live one the table; not while a callback runs on it or the game has yet to
// accept or reject it.
void FreeSocketConnection(CpuContext* cpu, uint32_t connection) {
    if (Memory::Read32(connection + kFreeAtAcceptReject) || Memory::Read32(connection + kCallbackLevel))
        return;

    GuestFrame frame(cpu, 0x20);
    const uint32_t slot = frame[8];
    Memory::Write32(slot, connection);

    if (State(connection) != kClosed) {
        TableRemove(cpu, Memory::Read32(SocketOf(connection) + kConnections), slot);
        return;
    }

    const int32_t count = ArrayLength(Memory::Read32(SocketOf(connection) + kClosedConnections));

    for (int32_t i = 0; i < count; ++i) {
        const uint32_t self = Memory::Read32(slot);

        if (self == Memory::Read32(ArrayNth(Memory::Read32(SocketOf(self) + kClosedConnections), i))) {
            ArrayDeleteAt(cpu, Memory::Read32(SocketOf(self) + kClosedConnections), i);
            return;
        }
    }
}

// 0x80512370: a datagram to ip:port (only when the socket can send, but for
// type 3). Refused or unreachable peers fail their connection; a full
// network buffer (-6, -42) or EMSGSIZE drops it; another error fails the
// socket.
bool SocketSend(CpuContext* cpu, uint32_t socket, uint32_t ip, uint32_t port, uint32_t message, int32_t length) {
    GuestFrame frame(cpu, 0xD0);
    const uint32_t messageSlot = frame[8], lengthSlot = frame[0xC], dumpKeyPointer = frame[0x10], addr = frame[0x18], dumpKey = frame[0x20];
    Memory::Write32(messageSlot, message);
    Memory::Write32(lengthSlot, uint32_t(length));
    MessageCheck(cpu, messageSlot, lengthSlot);

    if (Memory::Read32(socket + kProtocolType) != kAdHocProtocol && !CanSendOnSocket(cpu, Memory::Read32(socket + kSocket)))
        return true;

    Memset4(cpu, addr, 0, kAddrBytes);
    Memory::Write8(addr + kAddrFamily, 2);
    Memory::Write32(addr + kAddrIp, ip);
    Memory::Write16(addr + kAddrPort, uint16_t(Short(port)));

    if (SendTo(cpu, Memory::Read32(socket + kSocket), Memory::Read32(messageSlot), Memory::Read32(lengthSlot), 0, addr, kAddrBytes) == -1) {
        const int32_t error = LastSocketError();

        if (error == -0xF)
            return HandleConnectionReset(cpu, socket, ip, port);

        if (error == -0x17)
            return HandleHostUnreachable(cpu, socket, ip, port, true);

        if (error == -0x2A || error == -6 || error == -0x23)
            return true;

        SocketError(cpu, socket);
        return false;
    }

    if (!Memory::Read32(socket + kSendDumpCallback))
        return true;

    Memory::Write32(dumpKey + kIp, ip);
    Memory::Write16(dumpKey + kPort, uint16_t(port));
    Memory::Write32(dumpKeyPointer, dumpKey);
    const uint32_t found = TableLookup(cpu, Memory::Read32(socket + kConnections), dumpKeyPointer);
    return DumpCallback(cpu, socket, found ? Memory::Read32(found) : 0, ip, port, 0, Memory::Read32(messageSlot), int32_t(Memory::Read32(lengthSlot)), true);
}

// 0x80512590: TableMap2's step over the connections; 0 stops at a
// connection whose think failed. A closed one is freed.
uint32_t ConnectionThinkMap(CpuContext* cpu, uint32_t elem, uint32_t now) {
    const uint32_t connection = Memory::Read32(elem);

    if (State(connection) != kClosed && !ConnectionThink(cpu, connection, Memory::Read32(now)))
        return 0;

    if (State(connection) == kClosed && !Memory::Read32(connection + kFreeAtAcceptReject) && !Memory::Read32(connection + kCallbackLevel))
        FreeSocketConnection(cpu, connection);

    return 1;
}

// 0x805126B0.
bool SocketConnectionsThink(CpuContext* cpu, uint32_t socket) {
    GuestFrame frame(cpu, 0x20);
    const uint32_t now = frame[8];
    Memory::Write32(now, CurrentTime(cpu));
    return TableMap2(cpu, Memory::Read32(socket + kConnections), kConnectionThinkMap, now) == 0;
}

// 0x80512700: frees the closed connections no callback holds.
void FreeClosedConnections(CpuContext* cpu, uint32_t socket) {
    for (int32_t i = ArrayLength(Memory::Read32(socket + kClosedConnections)) - 1; i >= 0; --i) {
        const uint32_t connection = Memory::Read32(ArrayNth(Memory::Read32(socket + kClosedConnections), i));

        if (!Memory::Read32(connection + kFreeAtAcceptReject) && !Memory::Read32(connection + kCallbackLevel))
            FreeSocketConnection(cpu, connection);
    }
}

// 0x805127F0: once per socket: every connection closed hard, then the
// socket error callback (and the close it may have asked for).
void CloseAllConnectionsHard(CpuContext* cpu, uint32_t socket);

void SocketError(CpuContext* cpu, uint32_t socket) {
    if (Memory::Read32(socket + kSocketError))
        return;

    Memory::Write32(socket + kSocketError, 1);
    CloseAllConnectionsHard(cpu, socket);

    if (SocketErrorCallback(cpu, socket))
        CloseGt2Socket(cpu, socket);
}

// 0x8050E8F0 gt2CloseAllConnectionsHard.
void CloseAllConnectionsHard(CpuContext* cpu, uint32_t socket) {
    TableMap(cpu, Memory::Read32(socket + kConnections), kCloseConnectionHardMap, 0);
}

namespace {

// The address strings' two alternating buffers (22 bytes) and formats.
constexpr uint32_t kAddressToggle = 0x80797CE0;
constexpr uint32_t kAddressBuffers = 0x80764A80;
constexpr uint32_t kFormatAddressPort = 0x80796820;
constexpr uint32_t kFormatAddress = 0x80796828;
constexpr uint32_t kFormatPort = 0x8079682C;

// MSL's ctype table and atoi.
constexpr uint32_t kCtypeTable = 0x8060F6A8;
constexpr uint32_t Atoi = 0x80461D34;

} // namespace

// 0x80512890 gt2AddressToString: "a.b.c.d:port", into one of two static
// buffers when none is given.
uint32_t AddressToString(CpuContext* cpu, uint32_t ip, uint32_t port, uint32_t string) {
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

// 0x80512980 gt2StringToAddress: "host[:port]"; the host is an address or
// a name (looked up), the port all digits up to 65535.
bool StringToAddress(CpuContext* cpu, uint32_t string, uint32_t ip, uint32_t port) {
    GuestFrame frame(cpu, 0x130);
    const uint32_t hostCopy = frame[8];
    uint32_t address = 0, number = 0, host = string;

    if (string && Memory::Read8(string)) {
        const char* text = reinterpret_cast<const char*>(Memory::GetPointer(string));
        const char* colon = std::strchr(text, ':');

        if (colon) {
            const uint32_t at = string + uint32_t(colon - text);

            if (at == string) {
                host = 0;
            } else {
                const uint32_t length = at - string;
                Memcpy4(cpu, hostCopy, string, length);
                Memory::Write8(hostCopy + length, 0);
                host = hostCopy;
            }

            const uint32_t ctype = Memory::Read32(Memory::Read32(kCtypeTable) + 8);

            for (uint32_t c = at + 1; Memory::Read8(c); ++c) {
                const int32_t ch = int8_t(Memory::Read8(c));

                if (uint32_t(ch) > 0xFF || !(Memory::Read16(ctype + uint32_t(ch) * 2) & 8))
                    return false;
            }

            const uint32_t value = Guest(cpu, Atoi, {at + 1});

            if (value > 0xFFFF)
                return false;

            number = value & 0xFFFF;
        }

        if (host) {
            address = InetAddr(cpu, host);

            if (address == 0xFFFFFFFF) {
                const uint32_t entry = GetHostByName(cpu, host);

                if (!entry)
                    return false;

                address = Memory::Read32(Memory::Read32(Memory::Read32(entry + 0xC)));
            }
        }
    }

    if (ip)
        Memory::Write32(ip, address);

    if (port)
        Memory::Write16(port, uint16_t(number));

    return true;
}

// 0x80512B10 gti2MessageCheck: no message is "", length 0; a length of -1
// is the string's with its terminator.
void MessageCheck(CpuContext*, uint32_t message, uint32_t length) {
    if (!Memory::Read32(message)) {
        Memory::Write32(message, 0x80796830);
        Memory::Write32(length, 0);
    } else if (int32_t(Memory::Read32(length)) == -1) {
        Memory::Write32(length, StringLength(Memory::Read32(message)) + 1);
    }
}

namespace {

// The ports' register wrappers.
void GetChallengePort(CpuContext* cpu) {
    GetChallenge(cpu, cpu->gpr[3]);
}

void GetResponsePort(CpuContext* cpu) {
    GetResponse(cpu, cpu->gpr[3], cpu->gpr[4]);
}

void CheckResponsePort(CpuContext* cpu) {
    cpu->gpr[3] = CheckResponse(cpu->gpr[3], cpu->gpr[4]) ? 1 : 0;
}

void AllocateBufferPort(CpuContext* cpu) {
    cpu->gpr[3] = AllocateBuffer(cpu, cpu->gpr[3], cpu->gpr[4]) ? 1 : 0;
}

void BufferFreeSpacePort(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(BufferFreeSpace(cpu->gpr[3]));
}

void BufferWriteBytePort(CpuContext* cpu) {
    BufferWriteByte(cpu->gpr[3], cpu->gpr[4]);
}

void BufferWriteUShortPort(CpuContext* cpu) {
    BufferWriteUShort(cpu->gpr[3], cpu->gpr[4]);
}

void BufferWriteDataPort(CpuContext* cpu) {
    BufferWriteData(cpu, cpu->gpr[3], cpu->gpr[4], int32_t(cpu->gpr[5]));
}

void BufferShortenPort(CpuContext* cpu) {
    BufferShorten(cpu, cpu->gpr[3], int32_t(cpu->gpr[4]), int32_t(cpu->gpr[5]));
}

uint32_t Bool(bool value) {
    return value ? 1 : 0;
}

void SocketErrorCallbackPort(CpuContext* cpu) {
    cpu->gpr[3] = Bool(SocketErrorCallback(cpu, cpu->gpr[3]));
}

void ConnectAttemptCallbackPort(CpuContext* cpu) {
    cpu->gpr[3] = Bool(ConnectAttemptCallback(cpu, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7], cpu->gpr[8], int32_t(cpu->gpr[9])));
}

void ConnectedCallbackPort(CpuContext* cpu) {
    cpu->gpr[3] = Bool(ConnectedCallback(cpu, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], int32_t(cpu->gpr[6])));
}

void ReceivedCallbackPort(CpuContext* cpu) {
    cpu->gpr[3] = Bool(ReceivedCallback(cpu, cpu->gpr[3], cpu->gpr[4], int32_t(cpu->gpr[5]), cpu->gpr[6]));
}

void ClosedCallbackPort(CpuContext* cpu) {
    cpu->gpr[3] = Bool(ClosedCallback(cpu, cpu->gpr[3], cpu->gpr[4]));
}

void PingCallbackPort(CpuContext* cpu) {
    cpu->gpr[3] = Bool(PingCallback(cpu, cpu->gpr[3], cpu->gpr[4]));
}

void SendFilterCallbackPort(CpuContext* cpu) {
    cpu->gpr[3] = Bool(SendFilterCallback(cpu, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], int32_t(cpu->gpr[6]), cpu->gpr[7]));
}

void ReceiveFilterCallbackPort(CpuContext* cpu) {
    cpu->gpr[3] = Bool(ReceiveFilterCallback(cpu, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], int32_t(cpu->gpr[6]), cpu->gpr[7]));
}

void DumpCallbackPort(CpuContext* cpu) {
    cpu->gpr[3] = Bool(DumpCallback(cpu, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7], cpu->gpr[8], int32_t(cpu->gpr[9]), cpu->gpr[10] != 0));
}

void UnrecognizedMessageCallbackPort(CpuContext* cpu) {
    cpu->gpr[3] = Bool(UnrecognizedMessageCallback(cpu, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], int32_t(cpu->gpr[7]), cpu->gpr[8]));
}

void NewOutgoingConnectionPort(CpuContext* cpu) {
    cpu->gpr[3] = NewOutgoingConnection(cpu, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6]);
}

void NewIncomingConnectionPort(CpuContext* cpu) {
    cpu->gpr[3] = NewIncomingConnection(cpu, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6]);
}

void StartConnectionAttemptPort(CpuContext* cpu) {
    cpu->gpr[3] = StartConnectionAttempt(cpu, cpu->gpr[3], cpu->gpr[4], int32_t(cpu->gpr[5]), cpu->gpr[6]);
}

void AcceptConnectionPort(CpuContext* cpu) {
    cpu->gpr[3] = Bool(AcceptConnection(cpu, cpu->gpr[3], cpu->gpr[4]));
}

void RejectConnectionPort(CpuContext* cpu) {
    RejectConnection(cpu, cpu->gpr[3], cpu->gpr[4], int32_t(cpu->gpr[5]));
}

void RawSendPort(CpuContext* cpu) {
    cpu->gpr[3] = Bool(RawSend(cpu, cpu->gpr[3], cpu->gpr[4], int32_t(cpu->gpr[5])));
}

void CheckTimeoutPort(CpuContext* cpu) {
    cpu->gpr[3] = Bool(CheckTimeout(cpu, cpu->gpr[3], cpu->gpr[4]));
}

void ConnectionThinkPort(CpuContext* cpu) {
    cpu->gpr[3] = Bool(ConnectionThink(cpu, cpu->gpr[3], cpu->gpr[4]));
}

void CloseConnectionPort(CpuContext* cpu) {
    CloseConnection(cpu, cpu->gpr[3], cpu->gpr[4] != 0);
}

void ConnectionClosedPort(CpuContext* cpu) {
    ConnectionClosed(cpu, cpu->gpr[3]);
}

void FreeConnectionPort(CpuContext* cpu) {
    FreeConnection(cpu, cpu->gpr[3]);
}

void CreateSocketPublicPort(CpuContext* cpu) {
    cpu->gpr[3] = CreateSocket(cpu, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7], 0);
}

void CloseSocketPublicPort(CpuContext* cpu) {
    CloseSocketPublic(cpu, cpu->gpr[3]);
}

void ThinkPort(CpuContext* cpu) {
    Think(cpu, cpu->gpr[3]);
}

void ListenPort(CpuContext* cpu) {
    Memory::Write32(cpu->gpr[3] + kConnectAttemptCallback, cpu->gpr[4]);
}

void ConnectPort(CpuContext* cpu) {
    cpu->gpr[3] = Connect(cpu, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], int32_t(cpu->gpr[7]), cpu->gpr[8], cpu->gpr[9], cpu->gpr[10] != 0);
}

void SendPublicPort(CpuContext* cpu) {
    cpu->gpr[3] = SendPublic(cpu, cpu->gpr[3], cpu->gpr[4], int32_t(cpu->gpr[5]), cpu->gpr[6] != 0);
}

void CloseConnectionHardPort(CpuContext* cpu) {
    CloseConnection(cpu, cpu->gpr[3], true);
}

void CloseConnectionHardMapPort(CpuContext* cpu) {
    CloseConnection(cpu, Memory::Read32(cpu->gpr[3]), true);
}

void CloseAllConnectionsHardPort(CpuContext* cpu) {
    CloseAllConnectionsHard(cpu, cpu->gpr[3]);
}

void GetSocketSocketPort(CpuContext* cpu) {
    cpu->gpr[3] = Memory::Read32(cpu->gpr[3] + kSocket);
}

void SetUnrecognizedMessageCallbackPort(CpuContext* cpu) {
    Memory::Write32(cpu->gpr[3] + kUnrecognizedMessageCallback, cpu->gpr[4]);
}

void ProcessAckPort(CpuContext* cpu) {
    cpu->gpr[3] = Bool(ProcessAck(cpu, cpu->gpr[3], cpu->gpr[4]));
}

void ReceivedServerChallengePort(CpuContext* cpu) {
    cpu->gpr[3] = Bool(ReceivedServerChallenge(cpu, cpu->gpr[3], cpu->gpr[4], int32_t(cpu->gpr[5])));
}

void ReceivedClientResponsePort(CpuContext* cpu) {
    cpu->gpr[3] = Bool(ReceivedClientResponse(cpu, cpu->gpr[3], cpu->gpr[4], int32_t(cpu->gpr[5])));
}

void HandleReliableMessagePort(CpuContext* cpu) {
    cpu->gpr[3] = Bool(HandleReliableMessage(cpu, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], int32_t(cpu->gpr[6])));
}

void CompareIncomingPort(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(CompareIncoming(cpu->gpr[3], cpu->gpr[4]));
}

void BufferIncomingPort(CpuContext* cpu) {
    cpu->gpr[3] = Bool(BufferIncoming(cpu, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5] & 0xFFFF, cpu->gpr[6], int32_t(cpu->gpr[7]), cpu->gpr[8]));
}

void HandleReliablePort(CpuContext* cpu) {
    cpu->gpr[3] = Bool(HandleReliable(cpu, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], int32_t(cpu->gpr[6])));
}

void HandleNackPort(CpuContext* cpu) {
    cpu->gpr[3] = Bool(HandleNack(cpu, cpu->gpr[3], cpu->gpr[4], int32_t(cpu->gpr[5])));
}

void HandleUnreliablePort(CpuContext* cpu) {
    cpu->gpr[3] = Bool(HandleUnreliable(cpu, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], int32_t(cpu->gpr[6])));
}

void HandleMessagePort(CpuContext* cpu) {
    cpu->gpr[3] = Bool(HandleMessage(cpu, cpu->gpr[3], cpu->gpr[4], int32_t(cpu->gpr[5]), cpu->gpr[6], cpu->gpr[7] & 0xFFFF));
}

void HandleConnectionResetPort(CpuContext* cpu) {
    cpu->gpr[3] = Bool(HandleConnectionReset(cpu, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5] & 0xFFFF));
}

void HandleHostUnreachablePort(CpuContext* cpu) {
    cpu->gpr[3] = Bool(HandleHostUnreachable(cpu, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5] & 0xFFFF, cpu->gpr[6] != 0));
}

void ReceiveMessagesPort(CpuContext* cpu) {
    cpu->gpr[3] = Bool(ReceiveMessages(cpu, cpu->gpr[3]));
}

void BeginReliableMessagePort(CpuContext* cpu) {
    cpu->gpr[3] = Bool(BeginReliableMessage(cpu, cpu->gpr[3], cpu->gpr[4], int32_t(cpu->gpr[5]), cpu->gpr[6]));
}

void SendClientChallengePort(CpuContext* cpu) {
    cpu->gpr[3] = Bool(SendClientChallenge(cpu, cpu->gpr[3], cpu->gpr[4]));
}

void SendAcceptPort(CpuContext* cpu) {
    cpu->gpr[3] = Bool(SendAccept(cpu, cpu->gpr[3]));
}

void SendRejectPort(CpuContext* cpu) {
    cpu->gpr[3] = Bool(SendReject(cpu, cpu->gpr[3], cpu->gpr[4], int32_t(cpu->gpr[5])));
}

void SendClosePort(CpuContext* cpu) {
    cpu->gpr[3] = Bool(SendClose(cpu, cpu->gpr[3]));
}

void SendKeepAlivePort(CpuContext* cpu) {
    cpu->gpr[3] = Bool(SendKeepAlive(cpu, cpu->gpr[3]));
}

void SendUnreliablePort(CpuContext* cpu) {
    cpu->gpr[3] = Bool(SendUnreliable(cpu, cpu->gpr[3], cpu->gpr[4], int32_t(cpu->gpr[5])));
}

void SendAckPort(CpuContext* cpu) {
    cpu->gpr[3] = Bool(SendAck(cpu, cpu->gpr[3]));
}

void SendNackPort(CpuContext* cpu) {
    cpu->gpr[3] = Bool(SendNack(cpu, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5]));
}

void SendClosedPort(CpuContext* cpu) {
    cpu->gpr[3] = Bool(SendClosed(cpu, cpu->gpr[3]));
}

void ResendMessagePort(CpuContext* cpu) {
    cpu->gpr[3] = Bool(ResendMessage(cpu, cpu->gpr[3], cpu->gpr[4]));
}

void SendPort(CpuContext* cpu) {
    cpu->gpr[3] = Bool(Send(cpu, cpu->gpr[3], cpu->gpr[4], int32_t(cpu->gpr[5]), cpu->gpr[6] != 0));
}

void ConnectionHashPort(CpuContext* cpu) {
    cpu->gpr[3] = ConnectionHash(cpu->gpr[3], cpu->gpr[4]);
}

void ConnectionComparePort(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(ConnectionCompare(cpu->gpr[3], cpu->gpr[4]));
}

void ClosedConnectionFreePort(CpuContext* cpu) {
    FreeConnection(cpu, Memory::Read32(cpu->gpr[3]));
}

void FindConnectionPort(CpuContext* cpu) {
    cpu->gpr[3] = FindConnection(cpu, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5] & 0xFFFF);
}

void CreateSocketPort(CpuContext* cpu) {
    cpu->gpr[3] = CreateSocket(cpu, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7], cpu->gpr[8]);
}

void CloseSocketPort(CpuContext* cpu) {
    CloseGt2Socket(cpu, cpu->gpr[3]);
}

void NewSocketConnectionPort(CpuContext* cpu) {
    cpu->gpr[3] = NewSocketConnection(cpu, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6] & 0xFFFF);
}

void FreeSocketConnectionPort(CpuContext* cpu) {
    FreeSocketConnection(cpu, cpu->gpr[3]);
}

void SocketSendPort(CpuContext* cpu) {
    cpu->gpr[3] = Bool(SocketSend(cpu, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5] & 0xFFFF, cpu->gpr[6], int32_t(cpu->gpr[7])));
}

void ConnectionThinkMapPort(CpuContext* cpu) {
    cpu->gpr[3] = ConnectionThinkMap(cpu, cpu->gpr[3], cpu->gpr[4]);
}

void SocketConnectionsThinkPort(CpuContext* cpu) {
    cpu->gpr[3] = Bool(SocketConnectionsThink(cpu, cpu->gpr[3]));
}

void FreeClosedConnectionsPort(CpuContext* cpu) {
    FreeClosedConnections(cpu, cpu->gpr[3]);
}

void SocketErrorPort(CpuContext* cpu) {
    SocketError(cpu, cpu->gpr[3]);
}

void NetworkToHostShortPort(CpuContext* cpu) {
    cpu->gpr[3] = Short(cpu->gpr[3]);
}

void AddressToStringPort(CpuContext* cpu) {
    cpu->gpr[3] = AddressToString(cpu, cpu->gpr[3], cpu->gpr[4] & 0xFFFF, cpu->gpr[5]);
}

void StringToAddressPort(CpuContext* cpu) {
    cpu->gpr[3] = Bool(StringToAddress(cpu, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5]));
}

void MessageCheckPort(CpuContext* cpu) {
    MessageCheck(cpu, cpu->gpr[3], cpu->gpr[4]);
}

} // namespace

} // namespace GameSpy

using namespace GameSpy;

extern "C" void func_8050CFD0(CpuContext*);
extern "C" void func_8050D0E0(CpuContext*);
extern "C" void func_8050D2D0(CpuContext*);
extern "C" void func_8050D3A0(CpuContext*);
extern "C" void func_8050D400(CpuContext*);
extern "C" void func_8050D410(CpuContext*);
extern "C" void func_8050D430(CpuContext*);
extern "C" void func_8050D460(CpuContext*);
extern "C" void func_8050D4F0(CpuContext*);
extern "C" void func_8050DE20(CpuContext*);
extern "C" void func_8050DE80(CpuContext*);
extern "C" void func_8050E430(CpuContext*);
extern "C" void func_8050E490(CpuContext*);
extern "C" void func_8050E5F0(CpuContext*);
extern "C" void func_8050E910(CpuContext*);
extern "C" void func_8050E920(CpuContext*);
extern "C" void func_8050E930(CpuContext*);
extern "C" void func_8050F870(CpuContext*);
extern "C" void func_80511CA0(CpuContext*);
extern "C" void func_80511CC0(CpuContext*);
extern "C" void func_80511D00(CpuContext*);
extern "C" void func_80511D10(CpuContext*);
extern "C" void func_80512020(CpuContext*);
extern "C" void func_805122A0(CpuContext*);
extern "C" void func_80512700(CpuContext*);
extern "C" void func_80512880(CpuContext*);
extern "C" void func_80512890(CpuContext*);
extern "C" void func_80512B10(CpuContext*);

extern "C" void MH3_Gti2GetChallenge(CpuContext* cpu) {
    PortCheck::Call(cpu, "gti2GetChallenge", func_8050CFD0, GetChallengePort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050CFD0, MH3_Gti2GetChallenge, "gti2GetChallenge");

extern "C" void MH3_Gti2GetResponse(CpuContext* cpu) {
    PortCheck::Call(cpu, "gti2GetResponse", func_8050D0E0, GetResponsePort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050D0E0, MH3_Gti2GetResponse, "gti2GetResponse");

extern "C" void MH3_Gti2CheckResponse(CpuContext* cpu) {
    PortCheck::Call(cpu, "gti2CheckResponse", func_8050D2D0, CheckResponsePort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050D2D0, MH3_Gti2CheckResponse, "gti2CheckResponse");

extern "C" void MH3_Gti2AllocateBuffer(CpuContext* cpu) {
    PortCheck::Call(cpu, "gti2AllocateBuffer", func_8050D3A0, AllocateBufferPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050D3A0, MH3_Gti2AllocateBuffer, "gti2AllocateBuffer");

extern "C" void MH3_Gti2GetBufferFreeSpace(CpuContext* cpu) {
    PortCheck::Call(cpu, "gti2GetBufferFreeSpace", func_8050D400, BufferFreeSpacePort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050D400, MH3_Gti2GetBufferFreeSpace, "gti2GetBufferFreeSpace");

extern "C" void MH3_Gti2BufferWriteByte(CpuContext* cpu) {
    PortCheck::Call(cpu, "gti2BufferWriteByte", func_8050D410, BufferWriteBytePort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050D410, MH3_Gti2BufferWriteByte, "gti2BufferWriteByte");

extern "C" void MH3_Gti2BufferWriteUShort(CpuContext* cpu) {
    PortCheck::Call(cpu, "gti2BufferWriteUShort", func_8050D430, BufferWriteUShortPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050D430, MH3_Gti2BufferWriteUShort, "gti2BufferWriteUShort");

extern "C" void MH3_Gti2BufferWriteData(CpuContext* cpu) {
    PortCheck::Call(cpu, "gti2BufferWriteData", func_8050D460, BufferWriteDataPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050D460, MH3_Gti2BufferWriteData, "gti2BufferWriteData");

extern "C" void MH3_Gti2BufferShorten(CpuContext* cpu) {
    PortCheck::Call(cpu, "gti2BufferShorten", func_8050D4F0, BufferShortenPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050D4F0, MH3_Gti2BufferShorten, "gti2BufferShorten");

extern "C" void MH3_Gti2SocketErrorCallback(CpuContext* cpu) {
    SocketErrorCallbackPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050D560, MH3_Gti2SocketErrorCallback, "gti2SocketErrorCallback");

extern "C" void MH3_Gti2ConnectAttemptCallback(CpuContext* cpu) {
    ConnectAttemptCallbackPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050D600, MH3_Gti2ConnectAttemptCallback, "gti2ConnectAttemptCallback");

extern "C" void MH3_Gti2ConnectedCallback(CpuContext* cpu) {
    ConnectedCallbackPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050D6F0, MH3_Gti2ConnectedCallback, "gti2ConnectedCallback");

extern "C" void MH3_Gti2ReceivedCallback(CpuContext* cpu) {
    ReceivedCallbackPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050D7D0, MH3_Gti2ReceivedCallback, "gti2ReceivedCallback");

extern "C" void MH3_Gti2ClosedCallback(CpuContext* cpu) {
    ClosedCallbackPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050D8B0, MH3_Gti2ClosedCallback, "gti2ClosedCallback");

extern "C" void MH3_Gti2PingCallback(CpuContext* cpu) {
    PingCallbackPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050D970, MH3_Gti2PingCallback, "gti2PingCallback");

extern "C" void MH3_Gti2SendFilterCallback(CpuContext* cpu) {
    SendFilterCallbackPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050DA30, MH3_Gti2SendFilterCallback, "gti2SendFilterCallback");

extern "C" void MH3_Gti2ReceiveFilterCallback(CpuContext* cpu) {
    ReceiveFilterCallbackPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050DB40, MH3_Gti2ReceiveFilterCallback, "gti2ReceiveFilterCallback");

extern "C" void MH3_Gti2DumpCallback(CpuContext* cpu) {
    DumpCallbackPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050DC50, MH3_Gti2DumpCallback, "gti2DumpCallback");

extern "C" void MH3_Gti2UnrecognizedMessageCallback(CpuContext* cpu) {
    UnrecognizedMessageCallbackPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050DD50, MH3_Gti2UnrecognizedMessageCallback, "gti2UnrecognizedMessageCallback");

extern "C" void MH3_Gti2NewOutgoingConnection(CpuContext* cpu) {
    PortCheck::Call(cpu, "gti2NewOutgoingConnection", func_8050DE20, NewOutgoingConnectionPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050DE20, MH3_Gti2NewOutgoingConnection, "gti2NewOutgoingConnection");

extern "C" void MH3_Gti2NewIncomingConnection(CpuContext* cpu) {
    PortCheck::Call(cpu, "gti2NewIncomingConnection", func_8050DE80, NewIncomingConnectionPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050DE80, MH3_Gti2NewIncomingConnection, "gti2NewIncomingConnection");

extern "C" void MH3_Gti2StartConnectionAttempt(CpuContext* cpu) {
    StartConnectionAttemptPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050DEE0, MH3_Gti2StartConnectionAttempt, "gti2StartConnectionAttempt");

extern "C" void MH3_Gti2AcceptConnection(CpuContext* cpu) {
    AcceptConnectionPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050DFC0, MH3_Gti2AcceptConnection, "gti2AcceptConnection");

extern "C" void MH3_Gti2RejectConnection(CpuContext* cpu) {
    RejectConnectionPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050E070, MH3_Gti2RejectConnection, "gti2RejectConnection");

extern "C" void MH3_Gti2ConnectionSendData(CpuContext* cpu) {
    RawSendPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050E0E0, MH3_Gti2ConnectionSendData, "gti2ConnectionSendData");

extern "C" void MH3_Gti2CheckTimeout(CpuContext* cpu) {
    CheckTimeoutPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050E140, MH3_Gti2CheckTimeout, "gti2CheckTimeout");

extern "C" void MH3_Gti2ConnectionThink(CpuContext* cpu) {
    ConnectionThinkPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050E240, MH3_Gti2ConnectionThink, "gti2ConnectionThink");

extern "C" void MH3_Gt2CloseConnection(CpuContext* cpu) {
    CloseConnectionPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050E390, MH3_Gt2CloseConnection, "gt2CloseConnection");

extern "C" void MH3_Gti2ConnectionClosed(CpuContext* cpu) {
    PortCheck::Call(cpu, "gti2ConnectionClosed", func_8050E430, ConnectionClosedPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050E430, MH3_Gti2ConnectionClosed, "gti2ConnectionClosed");

extern "C" void MH3_Gti2FreeConnection(CpuContext* cpu) {
    PortCheck::Call(cpu, "gti2FreeConnection", func_8050E490, FreeConnectionPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050E490, MH3_Gti2FreeConnection, "gti2FreeConnection");

extern "C" void MH3_Gt2CreateSocket(CpuContext* cpu) {
    CreateSocketPublicPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050E540, MH3_Gt2CreateSocket, "gt2CreateSocket");

extern "C" void MH3_Gt2CloseSocket(CpuContext* cpu) {
    CloseSocketPublicPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050E550, MH3_Gt2CloseSocket, "gt2CloseSocket");

extern "C" void MH3_Gt2Think(CpuContext* cpu) {
    ThinkPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050E5A0, MH3_Gt2Think, "gt2Think");

extern "C" void MH3_Gt2Listen(CpuContext* cpu) {
    PortCheck::Call(cpu, "gt2Listen", func_8050E5F0, ListenPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050E5F0, MH3_Gt2Listen, "gt2Listen");

extern "C" void MH3_Gt2Accept(CpuContext* cpu) {
    AcceptConnectionPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050E600, MH3_Gt2Accept, "gt2Accept");

extern "C" void MH3_Gt2Reject(CpuContext* cpu) {
    RejectConnectionPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050E610, MH3_Gt2Reject, "gt2Reject");

extern "C" void MH3_Gt2Connect(CpuContext* cpu) {
    ConnectPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050E620, MH3_Gt2Connect, "gt2Connect");

extern "C" void MH3_Gt2Send(CpuContext* cpu) {
    SendPublicPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050E7D0, MH3_Gt2Send, "gt2Send");

extern "C" void MH3_Gt2CloseConnectionHard(CpuContext* cpu) {
    CloseConnectionHardPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050E8D0, MH3_Gt2CloseConnectionHard, "gt2CloseConnectionHard");

extern "C" void MH3_Gti2CloseConnectionHardMap(CpuContext* cpu) {
    CloseConnectionHardMapPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050E8E0, MH3_Gti2CloseConnectionHardMap, "gti2CloseConnectionHardMap");

extern "C" void MH3_Gt2CloseAllConnectionsHard(CpuContext* cpu) {
    CloseAllConnectionsHardPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050E8F0, MH3_Gt2CloseAllConnectionsHard, "gt2CloseAllConnectionsHard");

extern "C" void MH3_Gt2GetSocketSOCKET(CpuContext* cpu) {
    PortCheck::Call(cpu, "gt2GetSocketSOCKET", func_8050E910, GetSocketSocketPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050E910, MH3_Gt2GetSocketSOCKET, "gt2GetSocketSOCKET");

extern "C" void MH3_Gt2SetUnrecognizedMessageCallback(CpuContext* cpu) {
    PortCheck::Call(cpu, "gt2SetUnrecognizedMessageCallback", func_8050E920, SetUnrecognizedMessageCallbackPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050E920, MH3_Gt2SetUnrecognizedMessageCallback, "gt2SetUnrecognizedMessageCallback");

extern "C" void MH3_Gti2ProcessAck(CpuContext* cpu) {
    PortCheck::Call(cpu, "gti2ProcessAck", func_8050E930, ProcessAckPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050E930, MH3_Gti2ProcessAck, "gti2ProcessAck");

extern "C" void MH3_Gti2ReceivedServerChallenge(CpuContext* cpu) {
    ReceivedServerChallengePort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050EA60, MH3_Gti2ReceivedServerChallenge, "gti2ReceivedServerChallenge");

extern "C" void MH3_Gti2ReceivedClientResponse(CpuContext* cpu) {
    ReceivedClientResponsePort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050EDA0, MH3_Gti2ReceivedClientResponse, "gti2ReceivedClientResponse");

extern "C" void MH3_Gti2HandleReliableMessage(CpuContext* cpu) {
    HandleReliableMessagePort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050F0B0, MH3_Gti2HandleReliableMessage, "gti2HandleReliableMessage");

extern "C" void MH3_Gti2IncomingBufferMessageCompare(CpuContext* cpu) {
    PortCheck::Call(cpu, "gti2IncomingBufferMessageCompare", func_8050F870, CompareIncomingPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050F870, MH3_Gti2IncomingBufferMessageCompare, "gti2IncomingBufferMessageCompare");

extern "C" void MH3_Gti2BufferIncomingMessage(CpuContext* cpu) {
    BufferIncomingPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050F890, MH3_Gti2BufferIncomingMessage, "gti2BufferIncomingMessage");

extern "C" void MH3_Gti2HandleReliable(CpuContext* cpu) {
    HandleReliablePort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050FA40, MH3_Gti2HandleReliable, "gti2HandleReliable");

extern "C" void MH3_Gti2HandleNack(CpuContext* cpu) {
    HandleNackPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050FE90, MH3_Gti2HandleNack, "gti2HandleNack");

extern "C" void MH3_Gti2HandleUnreliable(CpuContext* cpu) {
    HandleUnreliablePort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x80510080, MH3_Gti2HandleUnreliable, "gti2HandleUnreliable");

extern "C" void MH3_Gti2HandleMessage(CpuContext* cpu) {
    HandleMessagePort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x80510350, MH3_Gti2HandleMessage, "gti2HandleMessage");

extern "C" void MH3_Gti2HandleConnectionReset(CpuContext* cpu) {
    HandleConnectionResetPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x805107E0, MH3_Gti2HandleConnectionReset, "gti2HandleConnectionReset");

extern "C" void MH3_Gti2HandleHostUnreachable(CpuContext* cpu) {
    HandleHostUnreachablePort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x80510A00, MH3_Gti2HandleHostUnreachable, "gti2HandleHostUnreachable");

extern "C" void MH3_Gti2ReceiveMessages(CpuContext* cpu) {
    ReceiveMessagesPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x80510B30, MH3_Gti2ReceiveMessages, "gti2ReceiveMessages");

extern "C" void MH3_Gti2BeginReliableMessage(CpuContext* cpu) {
    BeginReliableMessagePort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x80510F30, MH3_Gti2BeginReliableMessage, "gti2BeginReliableMessage");

extern "C" void MH3_Gti2SendClientChallenge(CpuContext* cpu) {
    SendClientChallengePort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x805112D0, MH3_Gti2SendClientChallenge, "gti2SendClientChallenge");

extern "C" void MH3_Gti2SendAccept(CpuContext* cpu) {
    SendAcceptPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x805113C0, MH3_Gti2SendAccept, "gti2SendAccept");

extern "C" void MH3_Gti2SendReject(CpuContext* cpu) {
    SendRejectPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x80511490, MH3_Gti2SendReject, "gti2SendReject");

extern "C" void MH3_Gti2SendClose(CpuContext* cpu) {
    SendClosePort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x80511590, MH3_Gti2SendClose, "gti2SendClose");

extern "C" void MH3_Gti2SendKeepAlive(CpuContext* cpu) {
    SendKeepAlivePort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x80511660, MH3_Gti2SendKeepAlive, "gti2SendKeepAlive");

extern "C" void MH3_Gti2SendUnreliable(CpuContext* cpu) {
    SendUnreliablePort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x80511730, MH3_Gti2SendUnreliable, "gti2SendUnreliable");

extern "C" void MH3_Gti2SendAck(CpuContext* cpu) {
    SendAckPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x80511860, MH3_Gti2SendAck, "gti2SendAck");

extern "C" void MH3_Gti2SendNack(CpuContext* cpu) {
    SendNackPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x80511930, MH3_Gti2SendNack, "gti2SendNack");

extern "C" void MH3_Gti2SendClosed(CpuContext* cpu) {
    SendClosedPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x80511A30, MH3_Gti2SendClosed, "gti2SendClosed");

extern "C" void MH3_Gti2ResendMessage(CpuContext* cpu) {
    ResendMessagePort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x80511AF0, MH3_Gti2ResendMessage, "gti2ResendMessage");

extern "C" void MH3_Gti2Send(CpuContext* cpu) {
    SendPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x80511BB0, MH3_Gti2Send, "gti2Send");

extern "C" void MH3_Gti2ConnectionHash(CpuContext* cpu) {
    PortCheck::Call(cpu, "gti2ConnectionHash", func_80511CA0, ConnectionHashPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80511CA0, MH3_Gti2ConnectionHash, "gti2ConnectionHash");

extern "C" void MH3_Gti2ConnectionCompare(CpuContext* cpu) {
    PortCheck::Call(cpu, "gti2ConnectionCompare", func_80511CC0, ConnectionComparePort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80511CC0, MH3_Gti2ConnectionCompare, "gti2ConnectionCompare");

extern "C" void MH3_Gti2ClosedConnectionFree(CpuContext* cpu) {
    PortCheck::Call(cpu, "gti2ClosedConnectionFree", func_80511D00, ClosedConnectionFreePort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80511D00, MH3_Gti2ClosedConnectionFree, "gti2ClosedConnectionFree");

extern "C" void MH3_Gti2SocketFindConnection(CpuContext* cpu) {
    PortCheck::Call(cpu, "gti2SocketFindConnection", func_80511D10, FindConnectionPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80511D10, MH3_Gti2SocketFindConnection, "gti2SocketFindConnection");

extern "C" void MH3_Gti2CreateSocket(CpuContext* cpu) {
    CreateSocketPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x80511D60, MH3_Gti2CreateSocket, "gti2CreateSocket");

extern "C" void MH3_Gti2CloseSocket(CpuContext* cpu) {
    CloseSocketPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x80511FA0, MH3_Gti2CloseSocket, "gti2CloseSocket");

extern "C" void MH3_Gti2NewSocketConnection(CpuContext* cpu) {
    PortCheck::Call(cpu, "gti2NewSocketConnection", func_80512020, NewSocketConnectionPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80512020, MH3_Gti2NewSocketConnection, "gti2NewSocketConnection");

extern "C" void MH3_Gti2FreeSocketConnection(CpuContext* cpu) {
    PortCheck::Call(cpu, "gti2FreeSocketConnection", func_805122A0, FreeSocketConnectionPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x805122A0, MH3_Gti2FreeSocketConnection, "gti2FreeSocketConnection");

extern "C" void MH3_Gti2SocketSend(CpuContext* cpu) {
    SocketSendPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x80512370, MH3_Gti2SocketSend, "gti2SocketSend");

extern "C" void MH3_Gti2ConnectionThinkMap(CpuContext* cpu) {
    ConnectionThinkMapPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x80512590, MH3_Gti2ConnectionThinkMap, "gti2ConnectionThinkMap");

extern "C" void MH3_Gti2SocketConnectionsThink(CpuContext* cpu) {
    SocketConnectionsThinkPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x805126B0, MH3_Gti2SocketConnectionsThink, "gti2SocketConnectionsThink");

extern "C" void MH3_Gti2FreeClosedConnections(CpuContext* cpu) {
    PortCheck::Call(cpu, "gti2FreeClosedConnections", func_80512700, FreeClosedConnectionsPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80512700, MH3_Gti2FreeClosedConnections, "gti2FreeClosedConnections");

extern "C" void MH3_Gti2SocketError(CpuContext* cpu) {
    SocketErrorPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x805127F0, MH3_Gti2SocketError, "gti2SocketError");

extern "C" void MH3_Gt2NetworkToHostShort(CpuContext* cpu) {
    PortCheck::Call(cpu, "gt2NetworkToHostShort", func_80512880, NetworkToHostShortPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80512880, MH3_Gt2NetworkToHostShort, "gt2NetworkToHostShort");

extern "C" void MH3_Gt2AddressToString(CpuContext* cpu) {
    PortCheck::Call(cpu, "gt2AddressToString", func_80512890, AddressToStringPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80512890, MH3_Gt2AddressToString, "gt2AddressToString");

extern "C" void MH3_Gt2StringToAddress(CpuContext* cpu) {
    StringToAddressPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x80512980, MH3_Gt2StringToAddress, "gt2StringToAddress");

extern "C" void MH3_Gti2MessageCheck(CpuContext* cpu) {
    PortCheck::Call(cpu, "gti2MessageCheck", func_80512B10, MessageCheckPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80512B10, MH3_Gti2MessageCheck, "gti2MessageCheck");
