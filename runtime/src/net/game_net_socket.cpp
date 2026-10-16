// The game's sockets and their helpers (Capcom's net library, RMHP08
// 0x803CE0C4..0x803CF830; decompiled with Ghidra, the call arguments checked
// in the translated C++), ported natively: NetworkSingleTcp (one stream to
// the server, its frames handed to up to four MCS peers), NetworkMultipleUdp
// (one datagram socket shared by up to four peers, each with its own receive
// buffer), the byte stream the session messages are built in (network byte
// order through the platform's hooks), NetworkResolverWii (a host name
// looked up by SOGetAddrInfo on its own thread) and the session defaults.
// The platform's sockets stay guest objects behind their virtuals.
#include "abi_bridge.h"
#include "guest_call.h"
#include "memory.h"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <initializer_list>

namespace {

enum : uint32_t {
    Memcpy = 0x80004000,
    Memset = 0x80004350,
    OperatorDelete = 0x80040564,
    CvtFloatToUnsigned = 0x80457360,
    Memmove = 0x8045BC18,
    Memcmp = 0x8045BD3C,
    Rand = 0x8045E620,
    Strlen = 0x80456C40,
    OSCreateThread = 0x804D45F0,
    OSResumeThread = 0x804D4C80,
    OSSleepTicks = 0x804D5320,
    OSUnlockMutex = 0x804D2680,
    OSLockMutex = 0x804D25A0,
    SOInetAtoN = 0x80520B78,
    SOGetAddrInfo = 0x8052138C,
    SOFreeAddrInfo = 0x80521670,
    PlatformSocket = 0x80418C84,
    PlatformSocketFree = 0x80418CF8,
    SessionForward = 0x803D1D44,
};

constexpr uint32_t kPlatformPtr = 0x80797180;
constexpr uint32_t kBusClock = 0x800000F8;
constexpr uint32_t kCrFloatArgs = 1u << (31 - 6);

// The platform object's hooks.
enum : uint32_t {
    vInfo = 0xC,
    vError = 0x10,
    vWarn = 0x14,
    vNtohs = 0x48,
    vHtons = 0x4C,
    vNtohl = 0x50,
    vHtonl = 0x54,
    vSeconds = 0x60,
};

// A platform socket's virtuals.
enum : uint32_t {
    sInit = 0xC,
    sConnect = 0x18,
    sPoll = 0x1C,
    sExport = 0x20,
    sImport = 0x24,
    sConnected = 0x28,
    sClose = 0x34,
    sSend = 0x38,
    sReceive = 0x3C,
};

// A socket's error: code and system value.
enum : uint32_t {
    soError = 0x4,
    soValue = 0x8,
};

// Strings for the log hooks.
enum : uint32_t {
    kMsgTcpBadPacket = 0x805F9DD0,
    kMsgTcpPutFailed = 0x805F9DFC,
    kMsgTcpAdd = 0x805F9E38,
    kMsgTcpAddFull = 0x805F9E54,
    kMsgTcpRemove = 0x805F9E80,
    kMsgTcpRemoveMissing = 0x805F9EA0,
    kMsgTcpSendNoSocket = 0x805F9ED0,
    kMsgUdpOverflow = 0x805F9EF8,
    kMsgUdpRemove = 0x805F9F5C,
    kMsgUdpResetBadPeer = 0x805F9F88,
    kMsgUdpResetNoSocket = 0x805F9FC0,
    kMsgUdpSendBadPeer = 0x805F9FEC,
    kMsgUdpSendNoSocket = 0x805FA020,
    kMsgUdpReceiveBadPeer = 0x805FA04C,
    kMsgUdpReceiveNoSocket = 0x805FA084,
    kMsgUdpReceiveBadLength = 0x805FA0B4,
    kMsgResolverName = 0x805FA118,
};

uint32_t Read(uint32_t address) {
    return Memory::Read32(address);
}

int32_t ReadInt(uint32_t address) {
    return int32_t(Memory::Read32(address));
}

void Write(uint32_t address, uint32_t value) {
    Memory::Write32(address, value);
}

uint32_t Platform() {
    return Read(kPlatformPtr);
}

uint32_t Arg(CpuContext* cpu, int n) {
    return cpu->gpr[n];
}

// The platform's byte order hooks: identities on the Wii, taken directly.
uint32_t Convert(CpuContext* cpu, uint32_t slot, uint32_t value) {
    const uint32_t platform = Platform();
    const uint32_t fn = Read(Read(platform) + slot);

    if (fn == 0x80418430 || fn == 0x80418438 || fn == 0x80418440 || fn == 0x80418448)
        return value;

    return GuestCall{cpu}(fn, {platform, value});
}

// The platform's log hooks (variadic): info (level, message, ...), error and
// warning (message, ...).
void Log(CpuContext* cpu, uint32_t slot, std::initializer_list<uint32_t> values) {
    cpu->cr &= ~kCrFloatArgs;
    GuestCall{cpu}.Virtual(Platform(), slot, values);
}

float Seconds(CpuContext* cpu) {
    GuestCall{cpu}.Virtual(Platform(), vSeconds, {});
    return float(cpu->fpr[1].d);
}

// NetworkSingleTcp: the platform socket, the server's address, four peers
// fed from it, and the stream received so far.
enum : uint32_t {
    tSocket = 0x4,
    tAddress = 0x8,
    tPeers = 0x10,
    tBuffer = 0x20,
    tLength = 0x2420,
};

constexpr uint32_t kTcpBuffer = 0x2400;
constexpr uint32_t kFrameMost = 0x400;
constexpr uint32_t kPeerPut = 0x18; // an MCS peer's virtual: bytes into its buffer

// 0x803CE3D0: closes and frees the socket.
void TcpClose(CpuContext* cpu, uint32_t tcp) {
    const uint32_t socket = Read(tcp + tSocket);

    if (!socket)
        return;

    GuestCall game{cpu};
    game.Virtual(socket, sClose, {});
    game.Call<PlatformSocketFree>({Platform(), Read(tcp + tSocket)});
    Write(tcp + tSocket, 0);
}

// 0x803CE0C8: what arrived on the stream: whole frames (u16 length, at most
// 0x400) go to every peer; a bad length closes the socket.
void TcpMove(CpuContext* cpu, uint32_t tcp) {
    GuestCall game{cpu};
    const uint32_t socket = Read(tcp + tSocket);

    if (!socket || !game.Virtual(socket, sConnected, {}))
        return;

    for (;;) {
        const uint32_t length = Read(tcp + tLength);
        const int32_t got = int32_t(game.Virtual(Read(tcp + tSocket), sReceive, {tcp + tBuffer + length, kTcpBuffer - length, 0}));

        if (got <= 0)
            return;

        Write(tcp + tLength, Read(tcp + tLength) + uint32_t(got));
        const int32_t total = ReadInt(tcp + tLength);
        int32_t whole = 0;

        for (int32_t at = 0; whole + 2 <= total; ) {
            const uint32_t size = Convert(cpu, vNtohs, Memory::Read16(tcp + tBuffer + uint32_t(whole))) & 0xFFFF;

            if (!size || size > kFrameMost) {
                Log(cpu, vError, {kMsgTcpBadPacket, size});
                TcpClose(cpu, tcp);
                return;
            }

            at = whole + int32_t(size) + 2;

            if (at > total)
                break;

            whole = at;
        }

        for (uint32_t i = 0; i < 4; ++i) {
            const uint32_t peer = Read(tcp + tPeers + i * 4);

            if (peer && int32_t(game.Virtual(peer, kPeerPut, {tcp + tBuffer, uint32_t(whole), 0, 0, 0})) < 0)
                Log(cpu, vWarn, {kMsgTcpPutFailed, i, Read(tcp + tLength), uint32_t(got), uint32_t(whole)});
        }

        const int32_t left = ReadInt(tcp + tLength) - whole;
        Write(tcp + tLength, uint32_t(left));

        if (left > 0)
            game.Call<Memmove>({tcp + tBuffer, tcp + tBuffer + uint32_t(whole), uint32_t(left)});
    }
}

// 0x803CE2AC: a new stream socket connecting to `address`: 0, -1 when one is
// open already, -2 without a free socket, -3 / -4 when it fails to start or
// connect.
int32_t TcpOpen(CpuContext* cpu, uint32_t tcp, uint32_t address) {
    if (Read(tcp + tSocket))
        return -1;

    GuestCall game{cpu};
    const uint32_t socket = game.Call<PlatformSocket>({Platform()});
    Write(tcp + tSocket, socket);

    if (!socket)
        return -2;

    if (int32_t(game.Virtual(socket, sInit, {1})) < 0) {
        TcpClose(cpu, tcp);
        return -3;
    }

    if (int32_t(game.Virtual(Read(tcp + tSocket), sConnect, {address})) < 0) {
        TcpClose(cpu, tcp);
        return -4;
    }

    game.Call<Memcpy>({tcp + tAddress, address, 6});
    return 0;
}

// 0x803CE428: a peer into the first free slot (its index, or -1).
int32_t TcpAdd(CpuContext* cpu, uint32_t tcp, uint32_t peer) {
    for (uint32_t i = 0; i < 4; ++i) {
        if (Read(tcp + tPeers + i * 4))
            continue;

        Write(tcp + tPeers + i * 4, peer);
        Log(cpu, vInfo, {3, kMsgTcpAdd, i});
        return int32_t(i);
    }

    Log(cpu, vError, {kMsgTcpAddFull});
    return -1;
}

// 0x803CE4D4.
void TcpRemove(CpuContext* cpu, uint32_t tcp, uint32_t peer) {
    for (uint32_t i = 0; i < 4; ++i) {
        if (Read(tcp + tPeers + i * 4) != peer)
            continue;

        Write(tcp + tPeers + i * 4, 0);
        Log(cpu, vInfo, {3, kMsgTcpRemove, i});
        return;
    }

    Log(cpu, vError, {kMsgTcpRemoveMissing});
}

// NetworkMultipleUdp: the platform socket, four peers' addresses, the
// datagram buffer, each peer's receive buffer and how much it holds.
enum : uint32_t {
    uSocket = 0x4,
    uAddresses = 0xE,  // 4 x 6 bytes
    uDatagram = 0x26,  // 0x5DC bytes
    uBuffers = 0x602,  // 4 x 6000 bytes
    uPending = 0x63C4, // 4 words
};

constexpr uint32_t kDatagram = 0x5DC;
constexpr uint32_t kPeerBuffer = 6000;

bool BadPeer(CpuContext* cpu, int32_t peer, uint32_t message) {
    if (peer >= 0 && peer <= 3)
        return false;

    Log(cpu, vError, {message, uint32_t(peer)});
    return true;
}

// 0x803CE758.
void UdpClose(CpuContext* cpu, uint32_t udp) {
    const uint32_t socket = Read(udp + uSocket);

    if (!socket)
        return;

    GuestCall game{cpu};
    game.Virtual(socket, sClose, {});
    game.Call<PlatformSocketFree>({Platform(), Read(udp + uSocket)});
    Write(udp + uSocket, 0);
}

// 0x803CE654: every datagram waiting goes to the peer it came from (dropped
// when that peer's buffer is full or the sender is no peer).
void UdpMove(CpuContext* cpu, uint32_t udp) {
    GuestCall game{cpu};
    GuestFrame frame(cpu, 0x30);
    const uint32_t from = frame[0x10];

    while (Read(udp + uSocket)) {
        const int32_t got = int32_t(game.Virtual(Read(udp + uSocket), sReceive, {udp + uDatagram, kDatagram, from}));

        if (got <= 0)
            return;

        const uint32_t size = uint32_t(got) & 0xFFFF;

        for (uint32_t i = 0; i < 4; ++i) {
            if (game.Call<Memcmp>({udp + uAddresses + i * 6, from, 6}))
                continue;

            const uint32_t pending = udp + uPending + i * 4;

            if (int32_t(size) + ReadInt(pending) <= int32_t(kPeerBuffer)) {
                game.Call<Memcpy>({udp + uBuffers + i * kPeerBuffer + Read(pending), udp + uDatagram, size});
                Write(pending, Read(pending) + size);
            } else {
                Log(cpu, vWarn, {kMsgUdpOverflow});
            }

            break;
        }
    }
}

// 0x803CE7B0: forgets the peer at `address`.
void UdpRemove(CpuContext* cpu, uint32_t udp, uint32_t address) {
    GuestCall game{cpu};

    for (uint32_t i = 0; i < 4; ++i) {
        if (game.Call<Memcmp>({udp + uAddresses + i * 6, address, 6}))
            continue;

        game.Call<Memset>({udp + uAddresses + i * 6, 0, 6});
        Write(udp + uPending + i * 4, 0);
        Log(cpu, vInfo, {3, kMsgUdpRemove, Memory::Read8(address), Memory::Read8(address + 1), Memory::Read8(address + 2),
            Memory::Read8(address + 3), Memory::Read16(address + 4)});
        return;
    }
}

// 0x803CE888: drops what a peer had waiting.
void UdpReset(CpuContext* cpu, uint32_t udp, int32_t peer) {
    if (BadPeer(cpu, peer, kMsgUdpResetBadPeer))
        return;

    if (!Read(udp + uSocket)) {
        Log(cpu, vError, {kMsgUdpResetNoSocket});
        return;
    }

    Write(udp + uPending + uint32_t(peer) * 4, 0);
}

// 0x803CE928: a datagram to a peer.
int32_t UdpSend(CpuContext* cpu, uint32_t udp, int32_t peer, uint32_t data, uint32_t length) {
    if (BadPeer(cpu, peer, kMsgUdpSendBadPeer))
        return -1;

    const uint32_t socket = Read(udp + uSocket);

    if (!socket) {
        Log(cpu, vError, {kMsgUdpSendNoSocket});
        return -1;
    }

    return int32_t(GuestCall{cpu}.Virtual(socket, sSend, {data, length, udp + uAddresses + uint32_t(peer) * 6}));
}

// 0x803CE9E8: a peer's next whole packet (u16 length and data, twice), if
// it fits `most`; a bad first length drops what the peer had.
int32_t UdpReceive(CpuContext* cpu, uint32_t udp, int32_t peer, uint32_t to, int32_t most) {
    if (BadPeer(cpu, peer, kMsgUdpReceiveBadPeer))
        return -1;

    if (!Read(udp + uSocket)) {
        Log(cpu, vError, {kMsgUdpReceiveNoSocket});
        return -1;
    }

    GuestCall game{cpu};
    const uint32_t pending = udp + uPending + uint32_t(peer) * 4;
    const int32_t have = ReadInt(pending);

    if (have < 2)
        return 0;

    const uint32_t buffer = udp + uBuffers + uint32_t(peer) * kPeerBuffer;
    const uint32_t first = Convert(cpu, vNtohs, Memory::Read16(buffer)) & 0xFFFF;

    if (!first || first > kFrameMost) {
        const uint32_t address = udp + uAddresses + uint32_t(peer) * 6;
        Log(cpu, vError, {kMsgUdpReceiveBadLength, first, Memory::Read8(address), Memory::Read8(address + 1), Memory::Read8(address + 2),
            Memory::Read8(address + 3), Memory::Read16(address + 4)});
        Write(pending, 0);
        return 0;
    }

    if (have < int32_t(first + 2) || have < int32_t(first + 4))
        return 0;

    const uint32_t second = Convert(cpu, vNtohs, Memory::Read16(buffer + first + 2)) & 0xFFFF;
    const int32_t total = int32_t(first + 4 + second);

    if (have < total || most < total)
        return 0;

    game.Call<Memcpy>({to, buffer, uint32_t(total)});
    Write(pending, uint32_t(have - total));

    if (have - total > 0)
        game.Call<Memmove>({buffer, buffer + uint32_t(total), uint32_t(have - total)});

    return total;
}

// A byte stream: data, capacity, length; network byte order.
enum : uint32_t {
    bData = 0x4,
    bCapacity = 0x8,
    bLength = 0xC,
};

constexpr uint32_t kAddressBytes = 0xE;

bool Fits(uint32_t stream, uint32_t bytes) {
    return Read(stream + bLength) + bytes <= Read(stream + bCapacity);
}

void Append(uint32_t stream, uint32_t bytes) {
    Write(stream + bLength, Read(stream + bLength) + bytes);
}

uint32_t End(uint32_t stream) {
    return Read(stream + bData) + Read(stream + bLength);
}

// Drops the first `bytes`.
void Consume(CpuContext* cpu, uint32_t stream, uint32_t bytes) {
    const uint32_t left = Read(stream + bLength) - bytes;
    Write(stream + bLength, left);

    if (left)
        GuestCall{cpu}.Call<Memmove>({Read(stream + bData), Read(stream + bData) + bytes, left});
}

// 0x803CEDD4 / 0x803CED4C / 0x803CEE5C.
void StreamWriteU16(CpuContext* cpu, uint32_t stream, uint32_t value) {
    if (!Fits(stream, 2))
        return;

    Memory::Write16(End(stream), uint16_t(Convert(cpu, vHtons, value & 0xFFFF)));
    Append(stream, 2);
}

void StreamWriteU32(CpuContext* cpu, uint32_t stream, uint32_t value) {
    if (!Fits(stream, 4))
        return;

    Memory::Write32(End(stream), Convert(cpu, vHtonl, value));
    Append(stream, 4);
}

void StreamWriteU8(uint32_t stream, uint32_t value) {
    if (!Fits(stream, 1))
        return;

    Memory::Write8(End(stream), uint8_t(value));
    Append(stream, 1);
}

// 0x803CEC50: an address object's 14 bytes, exported by the object.
void StreamWriteAddress(CpuContext* cpu, uint32_t stream, uint32_t object) {
    if (Fits(stream, kAddressBytes) && int32_t(GuestCall{cpu}.Virtual(object, sExport, {End(stream), kAddressBytes})) > 0)
        Append(stream, kAddressBytes);
}

// 0x803CECC0: a block {data, u16 length}: its length, then its bytes.
void StreamWriteBlock(CpuContext* cpu, uint32_t stream, uint32_t block) {
    const uint32_t length = Memory::Read16(block + 4);

    if (!Fits(stream, length + 2))
        return;

    StreamWriteU16(cpu, stream, length);

    if (Read(block) && length)
        GuestCall{cpu}.Call<Memcpy>({End(stream), Read(block), length});

    Append(stream, length);
}

// 0x803CF0D0 / 0x803CF044 / 0x803CEFB8.
void StreamReadU8(CpuContext* cpu, uint32_t stream, uint32_t to) {
    if (!Read(stream + bLength))
        return;

    Memory::Write8(to, Memory::Read8(Read(stream + bData)));
    Consume(cpu, stream, 1);
}

void StreamReadU16(CpuContext* cpu, uint32_t stream, uint32_t to) {
    if (Read(stream + bLength) < 2)
        return;

    Memory::Write16(to, uint16_t(Convert(cpu, vNtohs, Memory::Read16(Read(stream + bData)))));
    Consume(cpu, stream, 2);
}

void StreamReadU32(CpuContext* cpu, uint32_t stream, uint32_t to) {
    if (Read(stream + bLength) < 4)
        return;

    Write(to, Convert(cpu, vNtohl, Read(Read(stream + bData))));
    Consume(cpu, stream, 4);
}

// 0x803CEE88: an address object imported from the stream's 14 bytes.
void StreamReadAddress(CpuContext* cpu, uint32_t stream, uint32_t object) {
    if (Read(stream + bLength) <= kAddressBytes - 1)
        return;

    if (int32_t(GuestCall{cpu}.Virtual(object, sImport, {Read(stream + bData), kAddressBytes})) > 0)
        Consume(cpu, stream, kAddressBytes);
}

// 0x803CEF00: a block {data, u16 room}: the length read first; a block that
// does not fit gets length 0.
void StreamReadBlock(CpuContext* cpu, uint32_t stream, uint32_t block) {
    if (Read(stream + bLength) < 2)
        return;

    GuestFrame frame(cpu, 0x20);
    const uint32_t size = frame[0x10];
    StreamReadU16(cpu, stream, size);
    const uint32_t length = Memory::Read16(size);

    if (Read(stream + bLength) < length || Memory::Read16(block + 4) < length) {
        Memory::Write16(block + 4, 0);
        return;
    }

    Memory::Write16(block + 4, uint16_t(length));

    if (!length)
        return;

    if (Read(block))
        GuestCall{cpu}.Call<Memcpy>({Read(block), Read(stream + bData), length});

    Consume(cpu, stream, length);
}

// NetworkResolver (base) and NetworkResolverWii: the name, up to four
// IPv4 addresses found, the lookup's state and its thread.
enum : uint32_t {
    rName = 0x4,        // 0x200 bytes
    rAddresses = 0x204, // 4 x 4 bytes
    rCount = 0x214,
    rState = 0x218,     // u8: 0 to start, 10 looking up, 15 found, 20 done, 0x5A failed, 0xFF idle
    rThread = 0x220,
    rStackTop = 0x1538,
    rResult = 0x1538,   // the thread's SOGetAddrInfo result
    rNode = 0x153C,     // the name the thread looks up
    rHints = 0x1540,    // 0x20 bytes
    rInfo = 0x1560,     // the addrinfo list
};

enum : uint32_t {
    kResolverVtable = 0x805FA0F8,
    kResolverWiiVtable = 0x805FA140,
    kSessionBaseVtable = 0x805FA160,
};

enum : uint8_t {
    kStart = 0,
    kLooking = 10,
    kFound = 15,
    kDone = 20,
    kFailed = 0x5A,
    kIdle = 0xFF,
};

constexpr uint32_t kResolverThreadEntry = 0x803CF1B0;
constexpr uint32_t kErrorEmptyName = 0x80020002;

// 0x803CF10C.
void ResolverConstruct(CpuContext* cpu, uint32_t resolver) {
    GuestCall game{cpu};
    Write(resolver, kResolverVtable);
    game.Call<Memset>({resolver + rName, 0, 0x200});
    game.Call<Memset>({resolver + rAddresses, 0, 0x10});
    Write(resolver + rCount, 0);
}

// 0x803CF3DC: the lookup, on the resolver's thread.
void ResolverLookup(CpuContext* cpu, uint32_t resolver) {
    Write(resolver + rResult, GuestCall{cpu}.Call<SOGetAddrInfo>({Read(resolver + rNode), 0, resolver + rHints, resolver + rInfo}));
}

void FreeInfo(CpuContext* cpu, uint32_t resolver) {
    if (Read(resolver + rInfo)) {
        GuestCall{cpu}.Call<SOFreeAddrInfo>({Read(resolver + rInfo)});
        Write(resolver + rInfo, 0);
    }
}

// 0x803CF310: the name to look up next (-1 while a list is still held).
int32_t ResolverSetName(CpuContext* cpu, uint32_t resolver, uint32_t name) {
    GuestCall game{cpu};
    const uint32_t length = std::min<uint32_t>(game.Call<Strlen>({name}), 0x1FF);
    game.Call<Memcpy>({resolver + rName, name, length});
    Memory::Write8(resolver + rName + length, 0);
    game.Call<Memset>({resolver + rAddresses, 0, 0x10});
    Write(resolver + rCount, 0);
    Memory::Write8(resolver + rState, kStart);

    if (!Read(resolver + rInfo))
        return 0;

    Memory::Write8(resolver + rState, kFailed);
    return -1;
}

// OSIsThreadTerminated (inlined by the translator): moribund or exited.
bool ThreadDone(uint32_t thread) {
    const uint32_t state = Memory::Read16(thread + 0x2C8);
    return state == 8 || state == 0;
}

// 0x803CF41C: drives the lookup: a dotted address at once, a name on a
// thread; the number of addresses found, 0 while looking, negative on
// failure.
int32_t ResolverCheck(CpuContext* cpu, uint32_t resolver) {
    GuestCall game{cpu};
    const uint8_t state = Memory::Read8(resolver + rState);

    if (state == kStart) {
        Log(cpu, vInfo, {1, kMsgResolverName, resolver + rName});

        if (!Memory::Read8(resolver + rName)) {
            Memory::Write8(resolver + rState, kFailed);
            return int32_t(kErrorEmptyName);
        }

        GuestFrame frame(cpu, 0x20);
        const uint32_t address = frame[0x10];
        const int32_t parsed = int32_t(game.Call<SOInetAtoN>({resolver + rName, address}));

        if (parsed == 1) {
            Memory::Write8(resolver + rState, kDone);
            game.Call<Memcpy>({resolver + rAddresses, address, 4});
            Write(resolver + rCount, 1);
            return 1;
        }

        if (parsed < 0) {
            Memory::Write8(resolver + rState, kFailed);
            return parsed;
        }

        Write(resolver + rNode, resolver + rName);
        game.Call<Memset>({resolver + rHints, 0, 0x20});
        Write(resolver + rHints + 4, 2);

        if (!game.Call<OSCreateThread>({resolver + rThread, kResolverThreadEntry, resolver, resolver + rStackTop, 0x1000, 0xE, 1}))
            return -1;

        Write(resolver + rResult, 0);
        game.Call<OSResumeThread>({resolver + rThread});
        Memory::Write8(resolver + rState, kLooking);
        return 0;
    }

    if (state == kLooking) {
        if (!ThreadDone(resolver + rThread))
            return 0;

        const int32_t result = ReadInt(resolver + rResult);

        if (result < 0) {
            Memory::Write8(resolver + rState, kFailed);
            FreeInfo(cpu, resolver);
            return result;
        }

        Memory::Write8(resolver + rState, kFound);
        return 0;
    }

    if (state == kFound) {
        Memory::Write8(resolver + rState, kDone);
        Write(resolver + rCount, 0);
        GuestFrame frame(cpu, 0x20);
        const uint32_t sockaddr = frame[0x10];

        for (uint32_t info = Read(resolver + rInfo); info && ReadInt(resolver + rCount) < 4; info = Read(info + 0x1C)) {
            game.Call<Memcpy>({sockaddr, Read(info + 0x18), 8});
            game.Call<Memcpy>({resolver + rAddresses + Read(resolver + rCount) * 4, sockaddr + 4, 4});
            Write(resolver + rCount, Read(resolver + rCount) + 1);
        }

        FreeInfo(cpu, resolver);

        if (Read(resolver + rCount))
            return ReadInt(resolver + rCount);

        Memory::Write8(resolver + rState, kFailed);
        return -1;
    }

    if (state == kDone)
        return ReadInt(resolver + rCount);

    return -1;
}

// 0x803CF220: waits for a lookup in flight (17 ms at a time) before going.
void ResolverWiiDestroy(CpuContext* cpu, uint32_t resolver, int16_t flags) {
    if (!resolver)
        return;

    GuestCall game{cpu};
    Write(resolver, kResolverWiiVtable);

    if (!Memory::Read8(resolver + rState))
        Memory::Write8(resolver + rState, kIdle);

    const uint32_t ticksPerMs = (Read(kBusClock) / 4) / 1000;

    while (!ResolverCheck(cpu, resolver))
        game.Call<OSSleepTicks>({0, ticksPerMs * 17});

    FreeInfo(cpu, resolver);

    if (flags > 0)
        game.Call<OperatorDelete>({resolver});
}

// 0x803CF794: a session id from the clock in milliseconds and rand(),
// neither 0 nor -1.
uint32_t RandomSessionId(CpuContext* cpu) {
    GuestCall game{cpu};
    uint32_t id;

    do {
        const uint32_t ms = game.Call<CvtFloatToUnsigned>({}, {double(1000.0f * Seconds(cpu))});
        id = game.Call<Rand>() + ms;
    } while (id == 0 || id == 0xFFFFFFFF);

    return id;
}

// The session defaults (0x80795DD0..0x80795DE8).
enum : uint32_t {
    kSessionFlag = 0x80795DD0,
    kSessionA = 0x80795DD4,
    kSessionB = 0x80795DD8,
    kSessionC = 0x80795DDC,
    kSessionD = 0x80795DE0,
    kSessionRate = 0x80795DE4,
    kSessionQuota = 0x80795DE8,
};

} // namespace

extern "C" void MH3_NetTcpCloseEntry(CpuContext* cpu) {
    TcpClose(cpu, Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CE0C4, MH3_NetTcpCloseEntry, "NetTcpCloseEntry");

extern "C" void MH3_NetTcpMove(CpuContext* cpu) {
    TcpMove(cpu, Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CE0C8, MH3_NetTcpMove, "NetTcpMove");

extern "C" void MH3_NetTcpOpen(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(TcpOpen(cpu, Arg(cpu, 3), Arg(cpu, 4)));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CE2AC, MH3_NetTcpOpen, "NetTcpOpen");

extern "C" void MH3_NetTcpPoll(CpuContext* cpu) {
    const uint32_t socket = Read(Arg(cpu, 3) + tSocket);
    cpu->gpr[3] = socket ? GuestCall{cpu}.Virtual(socket, sPoll, {Arg(cpu, 4), Arg(cpu, 5)}) : 0xFFFFFFFF;
}
REGISTER_NATIVE_FUNCTION_AS(0x803CE380, MH3_NetTcpPoll, "NetTcpPoll");

extern "C" void MH3_NetTcpConnected(CpuContext* cpu) {
    const uint32_t socket = Read(Arg(cpu, 3) + tSocket);
    cpu->gpr[3] = socket ? GuestCall{cpu}.Virtual(socket, sConnected, {}) : 0;
}
REGISTER_NATIVE_FUNCTION_AS(0x803CE3A8, MH3_NetTcpConnected, "NetTcpConnected");

extern "C" void MH3_NetTcpClose(CpuContext* cpu) {
    TcpClose(cpu, Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CE3D0, MH3_NetTcpClose, "NetTcpClose");

extern "C" void MH3_NetTcpAdd(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(TcpAdd(cpu, Arg(cpu, 3), Arg(cpu, 4)));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CE428, MH3_NetTcpAdd, "NetTcpAdd");

extern "C" void MH3_NetTcpRemove(CpuContext* cpu) {
    TcpRemove(cpu, Arg(cpu, 3), Arg(cpu, 4));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CE4D4, MH3_NetTcpRemove, "NetTcpRemove");

extern "C" void MH3_NetTcpClear(CpuContext* cpu) {
    const uint32_t tcp = Arg(cpu, 3);
    GuestCall{cpu}.Call<Memset>({tcp + tBuffer, 0, kTcpBuffer});
    Write(tcp + tLength, 0);
}
REGISTER_NATIVE_FUNCTION_AS(0x803CE57C, MH3_NetTcpClear, "NetTcpClear");

extern "C" void MH3_NetTcpSend(CpuContext* cpu) {
    const uint32_t socket = Read(Arg(cpu, 3) + tSocket);
    const uint32_t data = Arg(cpu, 4), length = Arg(cpu, 5);

    if (!socket) {
        Log(cpu, vError, {kMsgTcpSendNoSocket});
        cpu->gpr[3] = 0xFFFFFFFF;
        return;
    }

    cpu->gpr[3] = GuestCall{cpu}.Virtual(socket, sSend, {data, length, 0});
}
REGISTER_NATIVE_FUNCTION_AS(0x803CE5BC, MH3_NetTcpSend, "NetTcpSend");

extern "C" void MH3_NetTcpErrorCode(CpuContext* cpu) {
    const uint32_t socket = Read(Arg(cpu, 3) + tSocket);
    cpu->gpr[3] = socket ? Read(socket + soError) : 0;
}
REGISTER_NATIVE_FUNCTION_AS(0x803CE620, MH3_NetTcpErrorCode, "NetTcpErrorCode");

extern "C" void MH3_NetTcpErrorValue(CpuContext* cpu) {
    const uint32_t socket = Read(Arg(cpu, 3) + tSocket);
    cpu->gpr[3] = socket ? Read(socket + soValue) : 0;
}
REGISTER_NATIVE_FUNCTION_AS(0x803CE638, MH3_NetTcpErrorValue, "NetTcpErrorValue");

extern "C" void MH3_NetUdpErrorCode(CpuContext* cpu) {
    const uint32_t socket = Read(Arg(cpu, 3) + uSocket);
    cpu->gpr[3] = socket ? Read(socket + soError) : 0;
}
REGISTER_NATIVE_FUNCTION_AS(0x803CEC10, MH3_NetUdpErrorCode, "NetUdpErrorCode");

extern "C" void MH3_NetUdpErrorValue(CpuContext* cpu) {
    const uint32_t socket = Read(Arg(cpu, 3) + uSocket);
    cpu->gpr[3] = socket ? Read(socket + soValue) : 0;
}
REGISTER_NATIVE_FUNCTION_AS(0x803CEC28, MH3_NetUdpErrorValue, "NetUdpErrorValue");

extern "C" void MH3_NetUdpCloseEntry(CpuContext* cpu) {
    UdpClose(cpu, Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CE650, MH3_NetUdpCloseEntry, "NetUdpCloseEntry");

extern "C" void MH3_NetUdpMove(CpuContext* cpu) {
    UdpMove(cpu, Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CE654, MH3_NetUdpMove, "NetUdpMove");

extern "C" void MH3_NetUdpClose(CpuContext* cpu) {
    UdpClose(cpu, Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CE758, MH3_NetUdpClose, "NetUdpClose");

extern "C" void MH3_NetUdpRemove(CpuContext* cpu) {
    UdpRemove(cpu, Arg(cpu, 3), Arg(cpu, 4));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CE7B0, MH3_NetUdpRemove, "NetUdpRemove");

extern "C" void MH3_NetUdpReset(CpuContext* cpu) {
    UdpReset(cpu, Arg(cpu, 3), int32_t(Arg(cpu, 4)));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CE888, MH3_NetUdpReset, "NetUdpReset");

extern "C" void MH3_NetUdpSend(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(UdpSend(cpu, Arg(cpu, 3), int32_t(Arg(cpu, 4)), Arg(cpu, 5), Arg(cpu, 6)));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CE928, MH3_NetUdpSend, "NetUdpSend");

extern "C" void MH3_NetUdpReceive(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(UdpReceive(cpu, Arg(cpu, 3), int32_t(Arg(cpu, 4)), Arg(cpu, 5), int32_t(Arg(cpu, 6))));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CE9E8, MH3_NetUdpReceive, "NetUdpReceive");

extern "C" void MH3_NetUdpSocket(CpuContext* cpu) {
    cpu->gpr[3] = Read(Arg(cpu, 3) + uSocket);
}
REGISTER_NATIVE_FUNCTION_AS(0x803CEC40, MH3_NetUdpSocket, "NetUdpSocket");

extern "C" void MH3_NetUdpField0C(CpuContext* cpu) {
    cpu->gpr[3] = Read(Arg(cpu, 3) + 0xC);
}
REGISTER_NATIVE_FUNCTION_AS(0x803CEC48, MH3_NetUdpField0C, "NetUdpField0C");

extern "C" void MH3_NetStreamWriteAddress(CpuContext* cpu) {
    StreamWriteAddress(cpu, Arg(cpu, 3), Arg(cpu, 4));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CEC50, MH3_NetStreamWriteAddress, "NetStreamWriteAddress");

extern "C" void MH3_NetStreamWriteBlock(CpuContext* cpu) {
    StreamWriteBlock(cpu, Arg(cpu, 3), Arg(cpu, 4));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CECC0, MH3_NetStreamWriteBlock, "NetStreamWriteBlock");

extern "C" void MH3_NetStreamWriteU32(CpuContext* cpu) {
    StreamWriteU32(cpu, Arg(cpu, 3), Arg(cpu, 4));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CED4C, MH3_NetStreamWriteU32, "NetStreamWriteU32");

extern "C" void MH3_NetStreamWriteU16(CpuContext* cpu) {
    StreamWriteU16(cpu, Arg(cpu, 3), Arg(cpu, 4));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CEDD4, MH3_NetStreamWriteU16, "NetStreamWriteU16");

extern "C" void MH3_NetStreamWriteU8(CpuContext* cpu) {
    StreamWriteU8(Arg(cpu, 3), Arg(cpu, 4));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CEE5C, MH3_NetStreamWriteU8, "NetStreamWriteU8");

extern "C" void MH3_NetStreamReadAddress(CpuContext* cpu) {
    StreamReadAddress(cpu, Arg(cpu, 3), Arg(cpu, 4));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CEE88, MH3_NetStreamReadAddress, "NetStreamReadAddress");

extern "C" void MH3_NetStreamReadBlock(CpuContext* cpu) {
    StreamReadBlock(cpu, Arg(cpu, 3), Arg(cpu, 4));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CEF00, MH3_NetStreamReadBlock, "NetStreamReadBlock");

extern "C" void MH3_NetStreamReadU32(CpuContext* cpu) {
    StreamReadU32(cpu, Arg(cpu, 3), Arg(cpu, 4));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CEFB8, MH3_NetStreamReadU32, "NetStreamReadU32");

extern "C" void MH3_NetStreamReadU16(CpuContext* cpu) {
    StreamReadU16(cpu, Arg(cpu, 3), Arg(cpu, 4));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CF044, MH3_NetStreamReadU16, "NetStreamReadU16");

extern "C" void MH3_NetStreamReadU8(CpuContext* cpu) {
    StreamReadU8(cpu, Arg(cpu, 3), Arg(cpu, 4));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CF0D0, MH3_NetStreamReadU8, "NetStreamReadU8");

extern "C" void MH3_NetResolverConstruct(CpuContext* cpu) {
    ResolverConstruct(cpu, Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CF10C, MH3_NetResolverConstruct, "NetResolverConstruct");

extern "C" void MH3_NetResolverDestroy(CpuContext* cpu) {
    const uint32_t resolver = Arg(cpu, 3);

    if (resolver && int16_t(Arg(cpu, 4)) > 0)
        GuestCall{cpu}.Call<OperatorDelete>({resolver});

    cpu->gpr[3] = resolver;
}
REGISTER_NATIVE_FUNCTION_AS(0x803CF16C, MH3_NetResolverDestroy, "NetResolverDestroy");

extern "C" void MH3_NetResolverThread(CpuContext* cpu) {
    ResolverLookup(cpu, Arg(cpu, 3));
    cpu->gpr[3] = 0;
}
REGISTER_NATIVE_FUNCTION_AS(0x803CF1B0, MH3_NetResolverThread, "NetResolverThread");

extern "C" void MH3_NetResolverWiiConstruct(CpuContext* cpu) {
    const uint32_t resolver = Arg(cpu, 3);
    ResolverConstruct(cpu, resolver);
    Write(resolver, kResolverWiiVtable);
    Memory::Write8(resolver + rState, kIdle);
    Write(resolver + rInfo, 0);
    cpu->gpr[3] = resolver;
}
REGISTER_NATIVE_FUNCTION_AS(0x803CF1D4, MH3_NetResolverWiiConstruct, "NetResolverWiiConstruct");

extern "C" void MH3_NetResolverWiiDestroy(CpuContext* cpu) {
    const uint32_t resolver = Arg(cpu, 3);
    ResolverWiiDestroy(cpu, resolver, int16_t(Arg(cpu, 4)));
    cpu->gpr[3] = resolver;
}
REGISTER_NATIVE_FUNCTION_AS(0x803CF220, MH3_NetResolverWiiDestroy, "NetResolverWiiDestroy");

extern "C" void MH3_NetResolverSetName(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(ResolverSetName(cpu, Arg(cpu, 3), Arg(cpu, 4)));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CF310, MH3_NetResolverSetName, "NetResolverSetName");

extern "C" void MH3_NetResolverCancel(CpuContext* cpu) {
    const uint32_t resolver = Arg(cpu, 3);

    if (Memory::Read8(resolver + rState) == kStart)
        Memory::Write8(resolver + rState, kIdle);
}
REGISTER_NATIVE_FUNCTION_AS(0x803CF3C4, MH3_NetResolverCancel, "NetResolverCancel");

extern "C" void MH3_NetResolverLookup(CpuContext* cpu) {
    ResolverLookup(cpu, Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CF3DC, MH3_NetResolverLookup, "NetResolverLookup");

extern "C" void MH3_NetResolverCheck(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(ResolverCheck(cpu, Arg(cpu, 3)));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CF41C, MH3_NetResolverCheck, "NetResolverCheck");

extern "C" void MH3_NetResolverAddress(CpuContext* cpu) {
    const uint32_t resolver = Arg(cpu, 3), out = Arg(cpu, 5);
    const int32_t index = int32_t(Arg(cpu, 4));

    if (index >= 0 && index < ReadInt(resolver + rCount))
        GuestCall{cpu}.Call<Memcpy>({out, resolver + rAddresses + uint32_t(index) * 4, 4});
}
REGISTER_NATIVE_FUNCTION_AS(0x803CF684, MH3_NetResolverAddress, "NetResolverAddress");

extern "C" void MH3_NetMutexUnlock(CpuContext* cpu) {
    GuestCall{cpu}.Call<OSUnlockMutex>({Arg(cpu, 3) + 4});
}
REGISTER_NATIVE_FUNCTION_AS(0x803CF6B8, MH3_NetMutexUnlock, "NetMutexUnlock");

extern "C" void MH3_NetMutexLock(CpuContext* cpu) {
    GuestCall{cpu}.Call<OSLockMutex>({Arg(cpu, 3) + 4});
}
REGISTER_NATIVE_FUNCTION_AS(0x803CF6C0, MH3_NetMutexLock, "NetMutexLock");

extern "C" void MH3_NetLockedFlag(CpuContext* cpu) {
    cpu->gpr[3] = Memory::Read8(Arg(cpu, 3) + 0x6134);
}
REGISTER_NATIVE_FUNCTION_AS(0x803CF6C8, MH3_NetLockedFlag, "NetLockedFlag");

extern "C" void MH3_NetSessionSetFlag(CpuContext* cpu) {
    Write(kSessionFlag, Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CF6D0, MH3_NetSessionSetFlag, "NetSessionSetFlag");

extern "C" void MH3_NetSessionBaseConstruct(CpuContext* cpu) {
    const uint32_t session = Arg(cpu, 3);
    Write(session, kSessionBaseVtable);
    Write(session + 4, 0);
    Write(session + 8, 0);
    Memory::Write8(session + 0xC, 0);
}
REGISTER_NATIVE_FUNCTION_AS(0x803CF6D8, MH3_NetSessionBaseConstruct, "NetSessionBaseConstruct");

extern "C" void MH3_NetSessionBaseDestroy(CpuContext* cpu) {
    const uint32_t session = Arg(cpu, 3);

    if (session && int16_t(Arg(cpu, 4)) > 0)
        GuestCall{cpu}.Call<OperatorDelete>({session});

    cpu->gpr[3] = session;
}
REGISTER_NATIVE_FUNCTION_AS(0x803CF6F8, MH3_NetSessionBaseDestroy, "NetSessionBaseDestroy");

extern "C" void MH3_NetSessionSetDefaults(CpuContext* cpu) {
    Write(kSessionA, Arg(cpu, 4));
    Write(kSessionB, Arg(cpu, 5));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CF73C, MH3_NetSessionSetDefaults, "NetSessionSetDefaults");

extern "C" void MH3_NetSessionSetC(CpuContext* cpu) {
    Memory::Write32(kSessionC, std::bit_cast<uint32_t>(float(cpu->fpr[1].d)));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CF748, MH3_NetSessionSetC, "NetSessionSetC");

extern "C" void MH3_NetSessionSetD(CpuContext* cpu) {
    Memory::Write32(kSessionD, std::bit_cast<uint32_t>(float(cpu->fpr[1].d)));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CF750, MH3_NetSessionSetD, "NetSessionSetD");

extern "C" void MH3_NetSessionSetRate(CpuContext* cpu) {
    const int32_t total = int32_t(Arg(cpu, 4)), parts = int32_t(Arg(cpu, 5));
    Write(kSessionQuota, uint32_t(total / parts));
    Memory::Write32(kSessionRate, std::bit_cast<uint32_t>(1.0f / float(double(parts))));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CF758, MH3_NetSessionSetRate, "NetSessionSetRate");

extern "C" void MH3_NetSessionRandomId(CpuContext* cpu) {
    cpu->gpr[3] = RandomSessionId(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x803CF794, MH3_NetSessionRandomId, "NetSessionRandomId");

extern "C" void MH3_NetSessionIdValid(CpuContext* cpu) {
    const uint32_t id = Arg(cpu, 3);
    cpu->gpr[3] = id != 0 && id != 0xFFFFFFFF;
}
REGISTER_NATIVE_FUNCTION_AS(0x803CF80C, MH3_NetSessionIdValid, "NetSessionIdValid");

extern "C" void MH3_NetSessionForward(CpuContext* cpu) {
    const uint32_t a = Arg(cpu, 3), b = Arg(cpu, 4), c = Arg(cpu, 5), d = Arg(cpu, 6), e = Arg(cpu, 7), f = Arg(cpu, 8);
    cpu->gpr[3] = GuestCall{cpu}.Call<SessionForward>({f, a, uint32_t(int32_t(int8_t(b))), c, d, e});
}
REGISTER_NATIVE_FUNCTION_AS(0x803CF830, MH3_NetSessionForward, "NetSessionForward");
