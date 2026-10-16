// The game's session (Capcom's net library, RMHP08 0x803CF860..0x803D4968;
// decompiled with Ghidra, the call arguments and float math checked in the
// translated C++), ported natively: NetworkSessionStable, up to four peers
// (this console one of them) over NetworkConnectionStable. Each peer is
// reached directly or through a relay peer; the session keeps two channels
// of unit packets per peer (0 reliable, resent until acknowledged; 1
// unreliable), its own ids, and watches each link's load to tune its send
// rate or give up on it.
#include "abi_bridge.h"
#include "guest_call.h"
#include "memory.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <initializer_list>

namespace {

enum : uint32_t {
    Memcpy = 0x80004000,
    UnitSetFlagD = 0x803F9D40,
    Memset = 0x80004350,
    OperatorNew = 0x800404DC,
    OperatorDelete = 0x80040564,
    CvtFloatToUnsigned = 0x80457360,
    Memmove = 0x8045BC18,
    Rand = 0x8045E620,
};

constexpr uint32_t kPlatformPtr = 0x80797180;
constexpr uint32_t kCrFloatArgs = 1u << (31 - 6);

// The session: vtable, callback, its parameter, a flag, the send scratch,
// then four peers and the session's own state.
enum : uint32_t {
    sCallback = 0x4,
    sParam = 0x8,
    sFlag = 0xC,        // u8
    sScratch = 0xD,     // 0x400 bytes
    sScratchB = 0x410,  // 0x400 bytes
    sOption10 = 0x14810, // u8
    sOption11 = 0x14811, // u8
    sIds = 0x14814,     // 4 x u32: the peers' session ids
    sHost = 0x14824,    // s8: the host's peer
    sRelayHost = 0x14825, // s8
    sSelf = 0x14826,    // s8: this console's peer
    sPeers = 0x14828,   // 4 x 0x924
    sIdObject = 0x16CB8,
    sId = 0x16CD8,      // this session's id
    sNow = 0x16CDC,     // f32
    sHostSeen = 0x16CE0,
    sRelaySeen = 0x16CE4,
    sLossDrop = 0x16CE8,
    sLossWarn = 0x16CEC,
    sHostTimeout = 0x16CF0,
    sRetryLimit = 0x16CF4,
    sRate = 0x16CF8,
    sQuota = 0x16CFC,
    sClosing = 0x16D00, // u8
};

constexpr uint32_t kPeer = 0x924;

// A peer.
enum : uint32_t {
    pState = 0x0,
    pRelayState = 0x4,
    pEnding = 0x8,      // how an error ends the peer: 1 leave, 2 drop, 3 fail
    pHostTimeout = 0xC, // seconds the peer may go unrouted (its host id message)
    pRelay = 0x10,      // s8: the peer it is reached through, 0xFF directly, 0xFE pending
    pRelayRtt = 0x14,   // f32
    pJoined = 0x18,     // u8
    pDirect = 0x19,     // u8: the link to the peer itself is up
    pDropped = 0x1A,    // u8
    pRelayReady = 0x1B, // u8: the relay said it is ready
    pFailed = 0x1C,     // u8: the error is to be reported
    pError = 0x20,      // code and two values
    pConnection = 0x2C,
    pIdObject = 0x30,
    pId = 0x50,
    pPools = 0x54,      // send 0, send 1, receive 0, receive 1 (0x18 each)
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

float F(uint32_t address) {
    return std::bit_cast<float>(Memory::Read32(address));
}

void SetF(uint32_t address, float value) {
    Memory::Write32(address, std::bit_cast<uint32_t>(value));
}

uint32_t Arg(CpuContext* cpu, int n) {
    return cpu->gpr[n];
}

uint32_t Peer(uint32_t session, int32_t index) {
    return session + sPeers + uint32_t(index) * kPeer;
}

bool ValidPeer(int32_t index) {
    return index >= 0 && index <= 3;
}

// 0x803D2A40: a live peer is marked for a reset.
void MarkReset(uint32_t session, int32_t index) {
    if (!ValidPeer(index))
        return;

    const uint32_t peer = Peer(session, index);

    if (!Read(peer + pConnection))
        return;

    Memory::Write8(peer + pDropped, 1);
    Memory::Write8(peer + pDirect, 0);
}

// 0x803D2A84: a live peer's first error and how to end it (1 or 2, else 3).
void PeerError(uint32_t session, int32_t index, uint32_t code, uint32_t a, uint32_t b, int32_t how) {
    if (!ValidPeer(index))
        return;

    const uint32_t peer = Peer(session, index);

    if (!Read(peer + pConnection))
        return;

    if (!Read(peer + pEnding))
        Write(peer + pEnding, uint32_t(how - 1) < 2 ? uint32_t(how) : 3);

    if (Read(peer + pError))
        return;

    Write(peer + pError, code);
    Write(peer + pError + 4, a);
    Write(peer + pError + 8, b);
}

// The packet layer (native in game_net_packet.cpp).
enum : uint32_t {
    UnitDestroy = 0x803CB960,
    ReaderDestroy = 0x803CB9BC,
    ReaderConstruct = 0x803CBA18,
    UnitConstruct = 0x803CBA54,
    WriterInit = 0x803F8A34,
    WriterBegin = 0x803F8A78,
    WriteId = 0x803F8C40,
    WriteBytes = 0x803F8CC0,
    WriteU32 = 0x803F8D2C,
    WriteU8 = 0x803F8E54,
    SetLength = 0x803F8E8C,
    Flag80 = 0x803F97FC,
    PayloadLength = 0x803F972C,
    ReadBytes = 0x803F947C,
    UnitInit = 0x803F98E8,
    UnitBegin = 0x803F9924,
    UnitSetPayloadLength = 0x803F9AA4,
    UnitSetSequence = 0x803F9B08,
    UnitSetAck = 0x803F9B6C,
    UnitSetSender = 0x803F9BD0,
    UnitSetReceiver = 0x803F9C34,
    UnitSetFlagA = 0x803F9C98,
    UnitSetFlagB = 0x803F9CD4,
    UnitSeal = 0x803F9D70,
    UnitEncode = 0x803FA3F4,
    PoolLoadAtCursor = 0x803FADEC,
    PoolRewind = 0x803FACE4,
    PoolHasNext = 0x803FACF0,
    PoolNext = 0x803FAD58,
    PoolRemove = 0x803FAE1C,
    PoolSequence = 0x803FA8F4,
    PoolLoadInto = 0x803FA7E0,
    Length16 = 0x803F98D0,
};

// The connection (native in game_net_connection.cpp).
enum : uint32_t {
    ConnectionRtt = 0x803CC060,
    ConnectionBatchPut = 0x803CBB00,
    ConnectionBatchFlush = 0x803CBBFC,
};

// Session virtuals (NetworkSessionStable).
enum : uint32_t {
    vReport = 0x1C,  // (session, peer): reports a peer's error
    vJoined = 0x4C,  // (session, peer): whether the peer has joined
    vPeerRtt = 0x8C, // (session, peer): the round trip to a peer
};

// Control messages.
enum : uint32_t {
    kTagHostId = 1,
    kTagHostGone = 2,
    kTagGone = 3,
    kTagRelayData = 4,
    kTagLeave = 5,
    kTagKeepalive = 6,
    kTagEnd = 7,
    kTagReady = 8,
    kTagReadyRelayed = 9,
    kTagWho = 10,
    kTagSeen = 11,
};

// Recipients of a control message.
constexpr uint8_t kAll = 0xFF;
constexpr uint8_t kOthers = 0xFE;

constexpr float kSecond = 1.0f;
constexpr float kMilliseconds = 1000.0f;
constexpr float kRttStart = 3.0f;
constexpr float kQuick = 0.017f;
constexpr float kTuneShare = 0.5f;
constexpr float kTuneStep = 0.008f;
constexpr float kUpStep = 0.002f;
constexpr float kUpBlend = 0.25f;
constexpr uint32_t kSendIntervalFloor = 0x80795DEC; // f32: the shortest send interval

void Log(CpuContext* cpu, uint32_t slot, std::initializer_list<uint32_t> values) {
    cpu->cr &= ~kCrFloatArgs;
    GuestCall{cpu}.Virtual(Read(kPlatformPtr), slot, values);
}

// A log hook with a float in f1 (variadic, floats flagged).
void LogFloat(CpuContext* cpu, uint32_t slot, double value, std::initializer_list<uint32_t> values) {
    cpu->cr |= kCrFloatArgs;
    GuestCall{cpu}.Virtual(Read(kPlatformPtr), slot, values, {value});
}

void LogInfoFloat(CpuContext* cpu, double value, std::initializer_list<uint32_t> values) {
    LogFloat(cpu, 0xC, value, values);
}

uint32_t Connection(uint32_t session, int32_t index) {
    return Read(Peer(session, index) + pConnection);
}

int32_t Self(uint32_t session) {
    return int8_t(Memory::Read8(session + sSelf));
}

// 0x803D48BC: the peer a peer's packets go through: itself when linked, else
// its relay when that is linked (or has said it is ready); -1 when none.
int32_t Route(uint32_t session, int32_t index) {
    if (!ValidPeer(index) || !Connection(session, index))
        return -1;

    const uint32_t peer = Peer(session, index);

    if (Memory::Read8(peer + pDirect))
        return index;

    const int32_t relay = int8_t(Memory::Read8(peer + pRelay));

    if (relay < 0)
        return -1;

    const uint32_t through = Peer(session, relay);

    if (!Read(through + pConnection))
        return -1;

    if (Memory::Read8(through + pDirect))
        return relay;

    return Memory::Read8(through + pRelayReady) ? relay : -1;
}

void Broadcast(CpuContext* cpu, uint32_t session, uint32_t packet, int32_t channel, int32_t count, uint32_t targets, uint32_t limit);

// A control message: a writer over the session's scratch, the tag and its
// fields, sent unreliably to `target` (one peer, kAll or kOthers).
struct Message {
    CpuContext* cpu;
    GuestCall game;
    GuestFrame frame;
    uint32_t writer;
    uint32_t length = 0;

    Message(CpuContext* cpu_, uint32_t session, uint32_t tag) : cpu(cpu_), game{cpu_}, frame(cpu_, 0x60), writer(frame[0x10]) {
        game.Call<ReaderConstruct>({writer});
        game.Call<WriterInit>({writer, session + sScratch, 0x400});
        game.Call<WriterBegin>({writer, 0});
        U8(tag);
    }

    void U8(uint32_t value) {
        length += int16_t(game.Call<WriteU8>({writer, value & 0xFF}));
    }

    void U32(uint32_t value) {
        length += int16_t(game.Call<WriteU32>({writer, value}));
    }

    void Id(uint32_t id) {
        length += int16_t(game.Call<WriteId>({writer, id}));
    }

    void Bytes(uint32_t from, uint32_t count) {
        length += int16_t(game.Call<WriteBytes>({writer, from, count}));
    }

    void Send(uint32_t session, uint8_t target) {
        game.Call<SetLength>({writer, length & 0xFFFF});
        const uint32_t targets = frame[0x50];
        Memory::Write8(targets, target);
        Broadcast(cpu, session, writer, 0, 1, targets, 0xFF);
        game.Call<ReaderDestroy>({writer, 0xFFFFFFFF});
    }
};

// 0x803D3D4C / 0x803D1968: the host's session id to everyone.
void SendHostId(CpuContext* cpu, uint32_t session, uint32_t id) {
    Message message(cpu, session, kTagHostId);
    message.U32(id);
    message.Send(session, kAll);
}

// 0x803D3E0C / 0x803D196C: the host is gone.
void SendHostGone(CpuContext* cpu, uint32_t session) {
    Message message(cpu, session, kTagHostGone);
    message.Send(session, kAll);
}

// 0x803D3EA0: a keepalive, to one peer.
void SendKeepalive(CpuContext* cpu, uint32_t session, uint8_t target) {
    Message message(cpu, session, kTagKeepalive);
    message.Send(session, target);
}

// 0x803D3F3C: asks the others who reaches a peer.
void SendWho(CpuContext* cpu, uint32_t session, int32_t index) {
    Message message(cpu, session, kTagWho);
    message.Id(Peer(session, index) + pIdObject);
    message.Send(session, kOthers);
}

// 0x803D400C: answers it: the peer's id, its session id, the round trip in
// milliseconds and how it is reached.
void SendSeen(CpuContext* cpu, uint32_t session, uint8_t target, uint32_t id, uint32_t sessionId, uint32_t how, double rtt) {
    GuestCall game{cpu};
    const uint32_t ms = game.Call<CvtFloatToUnsigned>({}, {double(float(double(kMilliseconds) * rtt))});
    Message message(cpu, session, kTagSeen);
    message.Id(id);
    message.U32(sessionId);
    message.U32(ms);
    message.U8(how);
    message.Send(session, target);
}

// 0x803D4130: ready (8) or ready through a relay (9), to one peer.
void SendReady(CpuContext* cpu, uint32_t session, bool relayed, int32_t index) {
    Message message(cpu, session, relayed ? kTagReadyRelayed : kTagReady);
    message.U32(Read(Peer(session, index) + pId));
    message.U32(Read(session + sId));
    message.Send(session, uint8_t(index));
}

// 0x803D4234: data relayed to a peer.
void SendRelayData(CpuContext* cpu, uint32_t session, uint32_t data, uint32_t length, uint8_t target) {
    Message message(cpu, session, kTagRelayData);
    message.Bytes(data, length);
    message.Send(session, target);
}

// 0x803D2FA8: a peer's received control and data: with `data`, the data
// packets go to the callback (event 2); without, the control packets are
// executed (a bad one fails the peer).
int32_t Execute(CpuContext* cpu, uint32_t session, int32_t index, uint32_t reader);

void Drain(CpuContext* cpu, uint32_t session, int32_t index, bool data) {
    GuestCall game{cpu};

    if (data && !game.Virtual(session, vJoined, {uint32_t(index)}))
        return;

    GuestFrame frame(cpu, 0x60);
    const uint32_t reader = frame[0x10];
    game.Call<ReaderConstruct>({reader});

    for (uint32_t channel = 0; channel < 2; ++channel) {
        const uint32_t pool = Peer(session, index) + pPools + 0x30 + channel * 0x18;
        game.Call<PoolRewind>({pool});

        while (game.Call<PoolHasNext>({pool})) {
            game.Call<PoolLoadAtCursor>({pool, reader});
            const bool isData = game.Call<Flag80>({reader}) != 0;

            if (data && isData) {
                const uint32_t length = game.Call<PayloadLength>({reader}) & 0xFFFF;
                game.Call<ReadBytes>({reader, session + sScratchB, length});
                GuestCall{cpu}(Read(session + sCallback), {2, uint32_t(index), 0, game.Call<PayloadLength>({reader}) & 0xFFFF, session + sScratchB, Read(session + sParam)});
                game.Call<PoolRemove>({pool});
            } else if (!data && !isData) {
                const int32_t result = Execute(cpu, session, index, reader);

                if (result < 0) {
                    PeerError(session, index, 0x80030003, uint32_t(-result), 0x80000000, 1);
                    game.Call<ReaderDestroy>({reader, 0xFFFFFFFF});
                    return;
                }

                game.Call<PoolRemove>({pool});
            } else {
                game.Call<PoolNext>({pool});
            }
        }
    }

    game.Call<ReaderDestroy>({reader, 0xFFFFFFFF});
}

// 0x803D315C: drops a peer's waiting data packets.
void DropData(CpuContext* cpu, uint32_t session, int32_t index) {
    GuestCall game{cpu};
    GuestFrame frame(cpu, 0x40);
    const uint32_t reader = frame[0x10];
    game.Call<ReaderConstruct>({reader});

    for (uint32_t channel = 0; channel < 2; ++channel) {
        const uint32_t pool = Peer(session, index) + pPools + 0x30 + channel * 0x18;
        game.Call<PoolRewind>({pool});

        while (game.Call<PoolHasNext>({pool})) {
            game.Call<PoolLoadAtCursor>({pool, reader});

            if (game.Call<Flag80>({reader}))
                game.Call<PoolRemove>({pool});
            else
                game.Call<PoolNext>({pool});
        }
    }

    game.Call<ReaderDestroy>({reader, 0xFFFFFFFF});
}

// A peer's link tuning (peer +0xC8 ..): the resend time, the send interval
// and quota, their saved values while lowered, and when each last changed.
enum : uint32_t {
    pRto = 0xC8,
    pInterval = 0xCC,
    pQuota = 0xD0,
    pTunedAt = 0xD4,
    pSavedInterval = 0xD8,
    pSavedQuota = 0xDC,
    pLowered = 0xE0, // u8
    pUpAt = 0xE4,
    pDownAt = 0xE8,
};

// 0x803D42F4: a routed peer's resend time: twice the round trip (or twice
// the interval), between 0.1 and 1 second.
void TuneRto(CpuContext* cpu, uint32_t session, int32_t index) {
    if (Route(session, index) < 0)
        return;

    GuestCall{cpu}.Virtual(session, vPeerRtt, {uint32_t(index)});
    const float rtt = float(2.0 * cpu->fpr[1].d);
    const uint32_t peer = Peer(session, index);
    const float interval = float(2.0 * double(F(peer + pInterval)));
    const float rto = std::max(rtt, interval);
    SetF(peer + pRto, std::min(std::max(rto, 0.1f), kSecond));
}

// 0x803D43B8: once a second, a struggling peer gets a longer send interval
// (toward the session's rate) and a smaller quota (toward the session's).
void TuneDown(CpuContext* cpu, uint32_t session, int32_t index) {
    const uint32_t peer = Peer(session, index);
    const float now = F(session + sNow);

    if (!(kSecond + F(peer + pDownAt) <= now))
        return;

    SetF(peer + pDownAt, now);

    if (!Memory::Read8(peer + pLowered)) {
        Memory::Write8(peer + pLowered, 1);
        Write(peer + pSavedInterval, Read(peer + pInterval));
        Write(peer + pSavedQuota, Read(peer + pQuota));
    }

    Write(peer + pTunedAt, Read(session + sNow));
    const float interval = F(peer + pInterval);
    const float rate = F(session + sRate);

    if (interval < rate) {
        const float step = (rate - interval) * kTuneShare;
        SetF(peer + pInterval, kTuneStep <= step ? interval + step : rate);
        LogInfoFloat(cpu, double(F(peer + pInterval)), {1, 0x805FAD10, uint32_t(index)});
    }

    const int32_t quota = ReadInt(session + sQuota);
    const int32_t own = ReadInt(peer + pQuota);

    if (quota < own) {
        const int32_t step = (own - quota) / 2;
        Write(peer + pQuota, uint32_t(step < 0x10 ? quota : own - step));
        Log(cpu, 0xC, {1, 0x805FAD50, uint32_t(index), Read(peer + pQuota)});
    }
}

// 0x803D451C: once a second, a healthy peer's interval and quota recover
// toward their saved values (which themselves improve over time).
void TuneUp(CpuContext* cpu, uint32_t session, int32_t index) {
    const uint32_t peer = Peer(session, index);
    const float now = F(session + sNow);

    if (!(kSecond + F(peer + pUpAt) <= now))
        return;

    SetF(peer + pUpAt, now);

    if (kSecond + F(peer + pTunedAt) < now) {
        SetF(peer + pTunedAt, now);
        SetF(peer + pSavedInterval, std::max(F(peer + pSavedInterval) - kUpStep, F(kSendIntervalFloor)));

        if (F(peer + pSavedInterval) < F(kSendIntervalFloor))
            SetF(peer + pSavedInterval, F(kSendIntervalFloor));

        Write(peer + pSavedQuota, uint32_t(std::min<int32_t>(ReadInt(peer + pSavedQuota) + 4, 0x400)));
    }

    if (Memory::Read8(peer + pLowered)) {
        Memory::Write8(peer + pLowered, 0);
        SetF(peer + pSavedInterval, F(peer + pSavedInterval) + (F(peer + pInterval) - F(peer + pSavedInterval)) * kUpBlend);
        const int32_t gap = ReadInt(peer + pSavedQuota) - ReadInt(peer + pQuota);
        Write(peer + pSavedQuota, uint32_t(ReadInt(peer + pSavedQuota) - gap / 4));
    }

    const float saved = F(peer + pSavedInterval);
    const float interval = F(peer + pInterval);

    if (saved < interval) {
        SetF(peer + pInterval, kQuick + saved <= interval ? interval - (interval - saved) * kTuneShare : saved);
        LogInfoFloat(cpu, double(F(peer + pInterval)), {1, 0x805FAD94, uint32_t(index)});
    }

    const int32_t savedQuota = ReadInt(peer + pSavedQuota);
    const int32_t quota = ReadInt(peer + pQuota);

    if (quota < savedQuota) {
        Write(peer + pQuota, uint32_t(savedQuota - 0x20 < quota ? savedQuota : quota + (savedQuota - quota) / 2));
        Log(cpu, 0xC, {1, 0x805FADD0, uint32_t(index), Read(peer + pQuota)});
    }
}

// 0x803D46F4: an empty reliable unit packet straight to a peer (through its
// route), so the peer learns the sequences.
void SendSequences(CpuContext* cpu, uint32_t session, int32_t index) {
    GuestCall game{cpu};
    GuestFrame frame(cpu, 0x60);
    const uint32_t unit = frame[0x10];
    game.Call<UnitConstruct>({unit});
    const int32_t route = Route(session, index);

    if (route >= 0) {
        const uint32_t connection = Connection(session, route);
        const uint32_t peer = Peer(session, index);
        game.Call<UnitInit>({unit, session + sScratch, 0x400});
        game.Call<UnitBegin>({unit});
        game.Call<UnitSetPayloadLength>({unit, 0});
        game.Call<UnitSetSequence>({unit, game.Call<PoolSequence>({peer + pPools})});
        game.Call<UnitSetAck>({unit, game.Call<PoolSequence>({peer + pPools + 0x18})});
        game.Call<UnitSetSender>({unit, Read(session + sId)});
        game.Call<UnitSetReceiver>({unit, Read(peer + pId)});
        game.Call<UnitSetFlagA>({unit, 1});
        game.Call<UnitSetFlagB>({unit, 1});
        game.Call<UnitSetFlagD>({unit, 1});
        game.Call<UnitSeal>({unit});
        game.Call<UnitEncode>({unit});
        game.Call<ConnectionBatchPut>({connection, unit});
        game.Call<ConnectionBatchFlush>({connection, 0, 0, 0});
        const uint32_t ack = game.Call<PoolSequence>({peer + pPools + 0x18});
        const uint32_t sequence = game.Call<PoolSequence>({peer + pPools});
        Log(cpu, 0xC, {3, 0x805FAE0C, uint32_t(Self(session)), uint32_t(route), uint32_t(index), sequence, ack});
    }

    game.Call<UnitDestroy>({unit, 0xFFFFFFFF});
}

// More of a peer.
enum : uint32_t {
    pKnown = 0xB4,        // u8: the peer's sequences are known
    pDelay = 0xB8,        // f32: packets wait this long before they go (0: off)
    pForce = 0xBC,        // u8: send a reliable unit even when empty
    pSentAt = 0xC0,       // 2 x f32: when each channel last sent
    pClosedAt = 0xEC,     // f32: when the link closed
    pRelayAt = 0xF0,      // f32
    pRelayPings = 0xF4,   // the best relay's unanswered pings (0xFFFF: none yet)
    pAskedAt = 0xF8,      // f32
    pAsked = 0xFC,        // times the others were asked who reaches the peer
    pHostKnown = 0x100,   // u8: the peer sent its host timeout
    pRoutedAt = 0x104,    // f32: when the peer was last routed
    pLoadAt = 0x108,      // f32
    pQueue = 0x10C,       // f32: the smoothed reliable queue
    pCongested = 0x110,   // seconds the reliable queue has grown
    pKeepaliveAt = 0x114, // f32: when a reliable packet was last queued
    pDirectData = 0x118,  // 0x400 bytes of length-prefixed data for the batch
    pDirectLength = 0x518,
    pDirectSlot = 0x51C,
};

// The session's pool buffers (per peer: send 0, send 1, receive 0, receive 1).
constexpr uint32_t kSendBuffers = 0x810;
constexpr uint32_t kSendLowBuffers = 0x8810;
constexpr uint32_t kReceiveBuffers = 0xA810;
constexpr uint32_t kReceiveLowBuffers = 0x12810;

// More of the packet layer and the runtime.
enum : uint32_t {
    PoolSetSequence = 0x803FA8EC,
    PoolReset = 0x803FA8E0,
    PoolAppend = 0x803FA6FC,
    PoolDrop = 0x803FA800,
    PoolPutTop = 0x803FA8FC,
    PoolPutLow = 0x803FAA34,
    PoolAck = 0x803FAC24,
    PoolsConstruct = 0x803CA5D4,
    PoolsDestroy = 0x803CA54C,
    ReaderHasPacket = 0x803F9010,
    ReaderNext = 0x803F909C,
    ReadId = 0x803F93EC,
    ReadU32 = 0x803F94F0,
    ReadU8 = 0x803F9630,
    PacketSize = 0x803F966C,
    PacketTimestamp = 0x803F9834,
    SetTimestamp = 0x803F8EEC,
    MaxHeader = 0x803F98DC,
    UnitAppend = 0x803F99D4,
    UnitAttach = 0x803F9E68,
    UnitHeaderSize = 0x803FA1C0,
    UnitSequence = 0x803FA1C8,
    UnitAck = 0x803FA214,
    UnitSender = 0x803FA260,
    UnitReceiver = 0x803FA2AC,
    UnitFlagB = 0x803FA328,
    UnitFlagD = 0x803FA374,
    IdConstruct = 0x803F881C,
    IdDestroy = 0x803CCE5C,
    IdEquals = 0x803F8968,
    StreamConstruct = 0x803C98E0,
    StreamDestroy = 0x803C9900,
    StreamWriteId = 0x803CEC50,
    StreamWriteU32 = 0x803CED4C,
    StreamWriteU16 = 0x803CEDD4,
    StreamData = 0x803CEC40,
    StreamLength = 0x803CEC48,
    StreamReadId = 0x803CEE88,
    StreamReadU32 = 0x803CEFB8,
    StreamReadU16 = 0x803CF044,
    ConnectionConstruct = 0x803CA500,
    ConnectionBatchReopen = 0x803CBA90,
    ConnectionBatchRoom = 0x803CBA9C,
    ConnectionPings = 0x803CBF20,
    ConnectionMtu = 0x803CC078,
    SessionBaseConstruct = 0x803CF6D8,
    SessionBaseDestroy = 0x803CF6F8,
    SessionSetLoss = 0x803CF73C,
    SessionSetHostTimeout = 0x803CF748,
    SessionSetRetries = 0x803CF750,
    SessionSetRate = 0x803CF758,
    RandomSessionId = 0x803CF794,
    ValidSessionId = 0x803CF80C,
    SessionForward = 0x803CF830,
    ConstructArray = 0x80457198,
    DestroyArray = 0x80457290,
};

// More session virtuals.
enum : uint32_t {
    vCloseAll = 0x10,
    vSetConnection = 0x18,
    vResetPeer = 0x1C,
    vConnectPeer = 0x20,
};

// Connection virtuals.
enum : uint32_t {
    cDestroy = 0x8,
    cOpen = 0xC,
    cCloseTransport = 0x10,
    cUpdate = 0x18,
    cConnect = 0x1C,
    cStart = 0x20,
    cClose = 0x24,
    cRefuse = 0x28,
    cShutdown = 0x2C,
    cWatchReceive = 0x34,
    cMarkReceived = 0x38,
    cSetKeepalive = 0x3C,
    cSetTimeout = 0x40,
    cSetNoReceive = 0x44,
    cSetProbe = 0x48,
    cProbe = 0x4C,
};

// Platform virtuals.
enum : uint32_t {
    kInfo = 0xC,
    kError = 0x10,
    kWarning = 0x14,
    kToHost16 = 0x48,
    kToNet16 = 0x4C,
    kNow = 0x60,
};

// Session defaults (r13 data).
constexpr uint32_t kDirectRoutes = 0x80795DD0; // nonzero: direct data may go through a relay
constexpr uint32_t kDefaultLossDrop = 0x80795DD4;
constexpr uint32_t kDefaultLossWarn = 0x80795DD8;
constexpr uint32_t kDefaultHostTimeout = 0x80795DDC;
constexpr uint32_t kDefaultRetries = 0x80795DE0;
constexpr uint32_t kDefaultRate = 0x80795DE4;
constexpr uint32_t kDefaultQuota = 0x80795DE8;

constexpr float kWait = 10.0f;
constexpr float kAskShare = 0.7f;
constexpr float kRecheck = 35.0f;
constexpr float kKeep = 0.9f;
constexpr float kTake = 0.1f;
constexpr float kMinInterval = 0.017f;
constexpr float kLimitScale = 256.0f;
constexpr float kLimitUnit = 4096.0f;

// Error codes.
enum : uint32_t {
    kErrTooLarge = 0x80030004,
    kErrSendFull = 0x80030032,
    kErrHostTimeout = 0x80030036,
    kErrLeft = 0x80030037,
    kErrCongested = 0x80030039,
    kErrClosed = 0x8003003A,
    kErrUnreachable = 0x8003003B,
    kErrHostLost = 0x8003003F,
    kErrEnded = 0x80030041,
    kErrTooFew = 0x80030042,
    kErrRemoteEnd = 0x80030044,
};

uint32_t Platform() {
    return Read(kPlatformPtr);
}

double Now(CpuContext* cpu) {
    GuestCall{cpu}.Virtual(Platform(), kNow);
    return cpu->fpr[1].d;
}

bool ValidId(CpuContext* cpu, uint32_t id) {
    return GuestCall{cpu}.Call<ValidSessionId>({id}) != 0;
}

// A first sequence: the clock in milliseconds plus a random number.
uint32_t InitialSequence(CpuContext* cpu) {
    GuestCall game{cpu};
    const double now = Now(cpu);
    const uint32_t ms = game.Call<CvtFloatToUnsigned>({}, {double(float(double(kMilliseconds) * now))});
    return (game.Call<Rand>() + ms) & 0xFFFF;
}

float ToFloat(int32_t value) {
    return float(double(value));
}

// 0x803CF928 / 0x803CF8CC: an id object.
uint32_t IdObjectConstruct(CpuContext* cpu, uint32_t object) {
    GuestCall{cpu}.Call<IdConstruct>({object});
    return object;
}

uint32_t IdObjectDestroy(CpuContext* cpu, uint32_t object, int16_t how) {
    if (object) {
        GuestCall{cpu}.Call<IdDestroy>({object, 0xFFFFFFFF});

        if (how > 0)
            GuestCall{cpu}.Call<OperatorDelete>({object});
    }

    return object;
}

// 0x803CF9C4 / 0x803CF958: a peer's id and pools.
uint32_t PeerConstruct(CpuContext* cpu, uint32_t peer) {
    IdObjectConstruct(cpu, peer + pIdObject);
    GuestCall{cpu}.Call<PoolsConstruct>({peer + pPools});
    return peer;
}

uint32_t PeerDestroy(CpuContext* cpu, uint32_t peer, int16_t how) {
    if (peer) {
        GuestCall{cpu}.Call<PoolsDestroy>({peer + pPools, 0xFFFFFFFF});
        IdObjectDestroy(cpu, peer + pIdObject, -1);

        if (how > 0)
            GuestCall{cpu}.Call<OperatorDelete>({peer});
    }

    return peer;
}

constexpr uint32_t kSessionVtable = 0x805FAEA8;
constexpr uint32_t kStreamVtable = 0x805FAE68;

// 0x803CF860: the session object.
uint32_t Construct(CpuContext* cpu, uint32_t session) {
    GuestCall game{cpu};
    game.Call<SessionBaseConstruct>({session});
    Write(session, kSessionVtable);
    game.Call<ConstructArray>({session + sPeers, 0x803CF9C4, 0x803CF958, kPeer, 4});
    IdObjectConstruct(cpu, session + sIdObject);
    return session;
}

// 0x803CFA00.
uint32_t Destroy(CpuContext* cpu, uint32_t session, int16_t how) {
    if (session) {
        GuestCall game{cpu};
        Write(session, kSessionVtable);
        game.Virtual(session, vCloseAll);
        IdObjectDestroy(cpu, session + sIdObject, -1);
        game.Call<DestroyArray>({session + sPeers, 0x803CF958, kPeer, 4});
        game.Call<SessionBaseDestroy>({session, 0});

        if (how > 0)
            game.Call<OperatorDelete>({session});
    }

    return session;
}

// A peer's link tuning back to the start.
void ResetTuning(uint32_t peer) {
    Memory::Write8(peer + pKnown, 0);
    SetF(peer + pDelay, 0.0f);
    Memory::Write8(peer + pForce, 0);
}

void ResetRates(uint32_t peer) {
    SetF(peer + pRto, 2.0f * kRttStart);
    Write(peer + pInterval, Read(kSendIntervalFloor));
    Write(peer + pQuota, 0x400);
    SetF(peer + pTunedAt, 0.0f);
    Memory::Write8(peer + pLowered, 0);
    Write(peer + pSavedInterval, Read(kSendIntervalFloor));
    Write(peer + pSavedQuota, 0x400);
    SetF(peer + pUpAt, 0.0f);
    SetF(peer + pDownAt, 0.0f);
}

// 0x803CFAA8: starts the session: cleared peers over the session's pool
// buffers with random first sequences, the defaults, this console's id, and
// its own peer set and connected.
void Init(CpuContext* cpu, uint32_t session, uint8_t flag, uint32_t callback, uint32_t param, uint32_t address, uint32_t connectArg) {
    GuestCall game{cpu};
    game.Call<Memset>({session + sScratch, 0, 0x400});
    game.Call<Memset>({session + sScratchB, 0, 0x400});
    Memory::Write8(session + sFlag, flag);
    Write(session + sCallback, callback);
    Write(session + sParam, param);
    Memory::Write8(session + sOption10, 0);
    Memory::Write8(session + sOption11, 0);

    for (uint32_t i = 0; i < 4; ++i)
        Write(session + sIds + i * 4, 0);

    Memory::Write8(session + sHost, 0xFF);
    Memory::Write8(session + sRelayHost, 0xFF);
    Memory::Write8(session + sSelf, 0xFF);

    for (int32_t index = 0; index < 4; ++index) {
        const uint32_t peer = Peer(session, index);
        Write(peer + pState, 0);
        Write(peer + pRelayState, 0);
        Write(peer + pEnding, 0);
        Write(peer + pHostTimeout, 0);
        Memory::Write8(peer + pRelay, 0xFF);
        SetF(peer + pRelayRtt, kRttStart);
        Memory::Write8(peer + pJoined, 0);
        Memory::Write8(peer + pDirect, 0);
        Memory::Write8(peer + pDropped, 0);
        Memory::Write8(peer + pRelayReady, 0);
        Memory::Write8(peer + pFailed, 0);
        game.Call<Memset>({peer + pError, 0, 0xC});
        Write(peer + pConnection, 0);
        game.Virtual(peer + pIdObject, 0x18);
        Write(peer + pId, 0);
        SetF(peer + pClosedAt, 0.0f);
        SetF(peer + pRelayAt, 0.0f);
        Write(peer + pRelayPings, 0xFFFF);
        SetF(peer + pAskedAt, 0.0f);
        Write(peer + pAsked, 0);
        Memory::Write8(peer + pHostKnown, 0);
        SetF(peer + pRoutedAt, 0.0f);
        SetF(peer + pLoadAt, 0.0f);
        SetF(peer + pQueue, 0.0f);
        Write(peer + pCongested, 0);
        SetF(peer + pKeepaliveAt, 0.0f);
        game.Call<Memset>({peer + pDirectData, 0, 0x400});
        game.Call<Memset>({peer + pDirectLength, 0, 4});
        Write(peer + pDirectSlot, 0);
        game.Call<Memset>({peer + 0x520, 0, 0x400});
        game.Call<Memset>({peer + 0x920, 0, 4});
        game.Virtual(peer + pPools, 0x14, {session + kSendBuffers + uint32_t(index) * 0x2000, 0x2000});
        game.Virtual(peer + pPools + 0x18, 0x14, {session + kSendLowBuffers + uint32_t(index) * 0x800, 0x800});
        game.Call<PoolSetSequence>({peer + pPools, InitialSequence(cpu)});
        game.Call<PoolSetSequence>({peer + pPools + 0x18, InitialSequence(cpu)});
        game.Virtual(peer + pPools + 0x30, 0x14, {session + kReceiveBuffers + uint32_t(index) * 0x2000, 0x2000});
        game.Virtual(peer + pPools + 0x48, 0x14, {session + kReceiveLowBuffers + uint32_t(index) * 0x800, 0x800});
        ResetTuning(peer);
        game.Call<Memset>({peer + pSentAt, 0, 8});
        ResetRates(peer);
    }

    game.Virtual(session + sIdObject, 0x18);
    Write(session + sId, 0);
    SetF(session + sNow, float(Now(cpu)));
    SetF(session + sHostSeen, 0.0f);
    SetF(session + sRelaySeen, 0.0f);
    Write(session + sLossDrop, Read(kDefaultLossDrop));
    Write(session + sLossWarn, Read(kDefaultLossWarn));
    Write(session + sHostTimeout, Read(kDefaultHostTimeout));
    Write(session + sRetryLimit, Read(kDefaultRetries));
    Write(session + sRate, Read(kDefaultRate));
    Write(session + sQuota, Read(kDefaultQuota));
    Memory::Write8(session + sClosing, 0);
    game.Virtual(session + sIdObject, 0x28, {address});
    Write(session + sId, game.Call<RandomSessionId>({session}));
    Log(cpu, kInfo, {3, 0x805FA200, Read(session + sId)});
    const int8_t self = int8_t(game.Virtual(session, vSetConnection, {1, address}));
    Memory::Write8(session + sSelf, uint8_t(self));
    game.Virtual(session, vConnectPeer, {uint32_t(int32_t(self)), connectArg, 0});
}

// 0x803CFEF8: resets every peer.
void CloseAll(CpuContext* cpu, uint32_t session) {
    for (int32_t index = 0; index < 4; ++index)
        GuestCall{cpu}.Virtual(session, vResetPeer, {uint32_t(index)});
}

// 0x803D2B00: sends a channel's waiting packets to a peer through its route,
// in unit packets of up to the route's MTU. Reliable packets (channel 0) go
// again once their resend time passes and only after the send interval, up to
// the quota per update; unreliable ones (1) go once and are dropped. A unit
// is randomly held back on a lossy link. Whether a unit went.
bool SendChannel(CpuContext* cpu, uint32_t session, int32_t index, uint32_t channel, bool force) {
    GuestCall game{cpu};
    GuestFrame frame(cpu, 0x100);
    const uint32_t unit = frame[0x40];
    const uint32_t reader = frame[0x28];
    game.Call<UnitConstruct>({unit});
    game.Call<ReaderConstruct>({reader});
    bool sent = false;
    float quota = 0.0f, interval = 0.0f, rto = 0.0f;
    const int32_t route = Route(session, index);

    if (route < 0) {
        game.Call<ReaderDestroy>({reader, 0xFFFFFFFF});
        game.Call<UnitDestroy>({unit, 0xFFFFFFFF});
        return false;
    }

    const uint32_t through = Peer(session, route);
    const uint32_t connection = Read(through + pConnection);

    if (channel == 0) {
        interval = F(through + pInterval);
        rto = F(through + pRto);
        quota = ToFloat(ReadInt(through + pQuota));
    }

    const uint32_t peer = Peer(session, index);
    const uint32_t pool = peer + pPools + channel * 0x18;
    const uint32_t sentAt = peer + pSentAt + channel * 4;

    if (!(interval + F(sentAt) < F(session + sNow))) {
        game.Call<ReaderDestroy>({reader, 0xFFFFFFFF});
        game.Call<UnitDestroy>({unit, 0xFFFFFFFF});
        return false;
    }

    // A packet may go when it has been sent (timestamp >= 0) or its hold
    // (the negated timestamp) has passed.
    const auto ready = [&](double stamp) {
        return 0.0 <= stamp || float(-1.0 * stamp) <= F(session + sNow);
    };

    int32_t total = 0;

    for (;;) {
        game.Call<UnitInit>({unit, session + sScratch, game.Call<ConnectionMtu>({connection})});
        game.Call<UnitBegin>({unit});
        game.Call<PoolLoadInto>({pool, reader});
        int32_t count = 0;
        uint32_t sequence = game.Call<PoolSequence>({pool});

        // Skip the packets still waiting for their acknowledgement.
        while (game.Call<ReaderHasPacket>({reader})) {
            game.Call<PacketTimestamp>({reader});

            if (!ready(cpu->fpr[1].d))
                break;

            game.Call<PacketTimestamp>({reader});

            if (!(F(session + sNow) < float(double(rto) + cpu->fpr[1].d)))
                break;

            sequence += game.Call<PacketSize>({reader, 0xBF});
            game.Call<ReaderNext>({reader});
        }

        while (game.Call<ReaderHasPacket>({reader})) {
            game.Call<PacketTimestamp>({reader});

            if (!ready(cpu->fpr[1].d))
                break;

            const uint32_t size = game.Call<PacketSize>({reader, 0xBF});
            const uint32_t header = game.Call<UnitHeaderSize>();

            if (0x200 < (header & 0xFFFF) + (size & 0xFFFF)) {
                const uint32_t full = game.Call<PacketSize>({reader, 0xFF});
                const uint32_t fullHeader = game.Call<UnitHeaderSize>();
                Log(cpu, kError, {0x805FA880, uint32_t(index), (fullHeader & 0xFFFF) + (full & 0xFFFF)});
            }

            const int32_t appended = int32_t(game.Call<UnitAppend>({unit, reader, 0xBF}));

            if (appended < 0)
                break;

            count += appended;
            game.Call<SetTimestamp>({reader}, {double(F(session + sNow))});
            game.Call<ReaderNext>({reader});
        }

        if (count == 0 && !force)
            break;

        force = false;
        game.Call<UnitSetPayloadLength>({unit, uint32_t(count) & 0xFFFF});
        game.Call<UnitSetSequence>({unit, sequence & 0xFFFF});
        game.Call<UnitSetAck>({unit, game.Call<PoolSequence>({pool + 0x30})});
        game.Call<UnitSetSender>({unit, Read(session + sId)});
        game.Call<UnitSetReceiver>({unit, Read(peer + pId)});
        game.Call<UnitSetFlagA>({unit, 1});
        game.Call<UnitSetFlagB>({unit, channel & 0xFF});
        game.Call<UnitSeal>({unit});
        game.Call<UnitEncode>({unit});
        int32_t loss = 0;

        if (channel == 1)
            loss = int32_t(game.Call<ConnectionPings>({connection})) + ReadInt(peer + pCongested);

        loss = std::max(loss, 1);

        if ((game.Call<Rand>() & 0xFF) < uint32_t(0x200 / loss)) {
            game.Call<ConnectionBatchPut>({connection, unit});
            sent = true;
        }

        total += count;
        Write(sentAt, Read(session + sNow));

        if (channel == 1 && count > 0) {
            game.Call<PoolDrop>({pool, uint32_t(count), 0xBF});
            game.Call<PoolSetSequence>({pool, (sequence + uint32_t(count)) & 0xFFFF});
        }

        if (channel != 0 || !(ToFloat(total) < quota))
            break;
    }

    game.Call<ReaderDestroy>({reader, 0xFFFFFFFF});
    game.Call<UnitDestroy>({unit, 0xFFFFFFFF});
    return sent;
}

// 0x803D3A20: queues a packet on a channel for the peers named in `targets`
// (kAll, kOthers or peer numbers) that are connected, know the sequences and,
// for data, have joined. An unreliable packet with a limit skips a peer whose
// reliable queue is already longer than the limit allows.
void Broadcast(CpuContext* cpu, uint32_t session, uint32_t packet, int32_t channel, int32_t count, uint32_t targets, uint32_t limit) {
    if (channel < 0 || channel > 1)
        return;

    GuestCall game{cpu};
    bool chosen[4] = {};

    for (int32_t index = 0; index < 4; ++index) {
        const uint32_t peer = Peer(session, index);

        if (!Read(peer + pConnection) || !Memory::Read8(peer + pKnown))
            continue;

        if (game.Call<Flag80>({packet}) && !Memory::Read8(peer + pJoined))
            continue;

        for (int32_t i = 0; i < count; ++i) {
            const int8_t target = int8_t(Memory::Read8(targets + uint32_t(i)));

            if (target == -1 || (target == -2 && index != Self(session)) || (target != -2 && target == index)) {
                chosen[index] = true;
                break;
            }
        }
    }

    for (int32_t index = 0; index < 4; ++index) {
        if (!chosen[index])
            continue;

        const uint32_t peer = Peer(session, index);

        if (channel == 1 && (limit & 0xFF) < 0xFE) {
            const float maxHeader = float(game.Call<MaxHeader>() & 0xFFFF);
            const float unitHeader = float(game.Call<UnitHeaderSize>() & 0xFFFF);
            const float allowed = (F(peer + pQueue) - unitHeader - maxHeader) * kLimitScale / kLimitUnit;

            if (float(limit & 0xFF) < allowed)
                continue;
        }

        if (F(peer + pDelay) <= 0.0f)
            game.Call<SetTimestamp>({packet}, {0.0});
        else
            game.Call<SetTimestamp>({packet}, {double(float(-1.0 * double(F(session + sNow) + F(peer + pDelay))))});

        const int32_t result = int32_t(game.Call<PoolAppend>({peer + pPools + uint32_t(channel) * 0x18, packet, 0xFF}));

        if (channel != 0)
            continue;

        if (result < 0) {
            Log(cpu, kWarning, {0x805FACDC, uint32_t(index), uint32_t(result)});
            PeerError(session, index, kErrSendFull, uint32_t(result), 0x80000000, 1);
        } else {
            Write(peer + pKeepaliveAt, Read(session + sNow));
        }
    }
}

// 0x803D3220: executes a control packet from a peer: host ids, gone ids,
// relayed data, leave/end, ready and the relay search. 0, or minus the tag
// of a bad one.
int32_t Execute(CpuContext* cpu, uint32_t session, int32_t index, uint32_t reader) {
    GuestCall game{cpu};
    GuestFrame frame(cpu, 0x80);
    const uint32_t id = frame[0x30];
    const uint32_t tagAt = frame[0x20];
    const uint32_t valueAt = frame[0x2C];
    const uint32_t value2At = frame[0x24];
    game.Call<IdConstruct>({id});
    const uint32_t peer = Peer(session, index);
    game.Call<ReadU8>({reader, tagAt});
    const uint32_t tag = Memory::Read8(tagAt);
    int32_t result = 0;

    const auto fail = [&] {
        result = -int32_t(tag);
    };

    switch (tag) {
    case kTagHostId: {
        game.Call<ReadU32>({reader, valueAt});
        const uint32_t timeout = Read(valueAt);
        Log(cpu, kInfo, {3, 0x805FA8B8, uint32_t(index), timeout});
        Memory::Write8(peer + pHostKnown, 1);
        Write(peer + pRoutedAt, Read(session + sNow));
        Write(peer + pHostTimeout, timeout);
        break;
    }

    case kTagHostGone:
        Memory::Write8(peer + pHostKnown, 0);
        Log(cpu, kInfo, {3, 0x805FA904, uint32_t(index)});
        break;

    case kTagGone: {
        game.Call<ReadU32>({reader, valueAt});
        const uint32_t gone = Read(valueAt);

        if (!ValidId(cpu, gone)) {
            fail();
            break;
        }

        Log(cpu, kInfo, {3, 0x805FA954, uint32_t(index), gone});
        bool known = false;

        for (uint32_t i = 0; i < 4; ++i)
            known = known || Read(session + sIds + i * 4) == gone;

        if (!known) {
            game.Call<Memmove>({session + sIds + 4, session + sIds, 0xC});
            Write(session + sIds, gone);
        }

        break;
    }

    case kTagRelayData: {
        const uint32_t length = (game.Call<PayloadLength>({reader}) & 0xFFFF) - 1;
        game.Call<ReadBytes>({reader, session + sScratchB, length});
        const uint32_t delivered = (game.Call<PayloadLength>({reader}) & 0xFFFF) - 1;
        GuestCall{cpu}(Read(session + sCallback), {4, uint32_t(index), 0, delivered, session + sScratchB, Read(session + sParam)});
        break;
    }

    case kTagLeave:
        Log(cpu, kInfo, {3, 0x805FA99C, uint32_t(index)});
        PeerError(session, Self(session), kErrLeft, 0, 0x80000000, 3);
        break;

    case kTagKeepalive:
        break;

    case kTagEnd:
        Log(cpu, kInfo, {3, 0x805FA9E8, uint32_t(index)});
        PeerError(session, index, kErrRemoteEnd, 0, 0x80000000, 2);
        break;

    case kTagReady:
    case kTagReadyRelayed: {
        game.Call<ReadU32>({reader, valueAt});
        uint32_t value = Read(valueAt);

        if (!ValidId(cpu, value)) {
            Log(cpu, kError, {0x805FAA20, uint32_t(index), value});
            fail();
            break;
        }

        if (Read(session + sId) != value) {
            Log(cpu, kError, {0x805FAA70, uint32_t(index), Read(session + sId), value});
            fail();
            break;
        }

        game.Call<ReadU32>({reader, valueAt});
        value = Read(valueAt);

        if (!ValidId(cpu, value)) {
            Log(cpu, kError, {0x805FAAD0, uint32_t(index), value});
            fail();
            break;
        }

        if (!ValidId(cpu, Read(peer + pId)))
            break;

        if (Read(peer + pId) != value) {
            Log(cpu, kError, {0x805FAB1C, uint32_t(index), Read(peer + pId), value});
            fail();
            break;
        }

        if (tag == kTagReady) {
            if (int8_t(Memory::Read8(peer + pRelay)) < 0) {
                if (Memory::Read8(peer + pDirect))
                    MarkReset(session, index);

                break;
            }

            SendReady(cpu, session, true, index);
        }

        Memory::Write8(peer + pRelayReady, 1);

        if (!Memory::Read8(peer + pJoined)) {
            Memory::Write8(peer + pJoined, 1);
            GuestCall{cpu}(Read(session + sCallback), {1, uint32_t(index), 0, 0, 0, Read(session + sParam)});
        }

        break;
    }

    case kTagWho: {
        game.Call<ReadId>({reader, id});
        int32_t found = 0;

        while (found < 4 && !game.Call<IdEquals>({Peer(session, found) + pIdObject, id}))
            ++found;

        if (found == 4) {
            SendSeen(cpu, session, uint8_t(index), id, 0, 0, 0.0);
            break;
        }

        double rtt = kRttStart;
        uint32_t how = 0;

        if (found != Self(session)) {
            const uint32_t other = Peer(session, found);

            if (Memory::Read8(other + pDirect)) {
                how = 2;

                if (Read(other + pConnection)) {
                    game.Call<ConnectionRtt>({Read(other + pConnection)});
                    rtt = cpu->fpr[1].d;
                }
            }
        }

        SendSeen(cpu, session, uint8_t(index), id, Read(Peer(session, found) + pId), how, rtt);
        break;
    }

    case kTagSeen: {
        game.Call<ReadId>({reader, id});
        game.Call<ReadU32>({reader, valueAt});
        game.Call<ReadU32>({reader, value2At});
        game.Call<ReadU8>({reader, tagAt});
        const uint32_t sessionId = Read(valueAt);
        const uint32_t ms = Read(value2At);
        const uint32_t how = Memory::Read8(tagAt);

        if (!Memory::Read8(peer + pDirect))
            break;

        int32_t found = 0;

        while (found < 4 && !game.Call<IdEquals>({Peer(session, found) + pIdObject, id}))
            ++found;

        if (found >= 4)
            break;

        if (!ValidId(cpu, sessionId)) {
            result = -int32_t(how);
            break;
        }

        const uint32_t other = Peer(session, found);

        if (!ValidId(cpu, Read(other + pId))) {
            Write(other + pId, sessionId);
            Log(cpu, kInfo, {3, 0x805FAB7C, uint32_t(index), uint32_t(found), Read(other + pId), sessionId});
        } else if (sessionId != Read(other + pId)) {
            Log(cpu, kWarning, {0x805FABD8, uint32_t(index), Read(other + pId), sessionId});
        }

        if (how != 2)
            break;

        const int32_t pings = int32_t(game.Call<ConnectionPings>({Read(peer + pConnection)}));

        if (pings < ReadInt(other + pRelayPings)) {
            Write(other + pRelayPings, game.Call<ConnectionPings>({Read(peer + pConnection)}));
            Memory::Write8(other + pRelay, uint8_t(index));
            SetF(other + pRelayRtt, float(double(ms)) / kMilliseconds);
            Log(cpu, kInfo, {3, 0x805FAC34, uint32_t(Self(session)), Read(session + sId), uint32_t(index), Read(peer + pId), uint32_t(found), Read(other + pId)});
        }

        break;
    }

    default:
        fail();
        break;
    }

    game.Call<IdDestroy>({id, 0xFFFFFFFF});
    return result;
}

// A control message with only its tag on `channel` to `target`.
void SendTag(CpuContext* cpu, uint32_t session, uint32_t tag, int32_t channel, uint8_t target) {
    GuestCall game{cpu};
    GuestFrame frame(cpu, 0x60);
    const uint32_t writer = frame[0x10];
    game.Call<ReaderConstruct>({writer});
    game.Call<WriterInit>({writer, session + sScratch, 0x400});
    game.Call<WriterBegin>({writer, 0});
    game.Call<SetLength>({writer, game.Call<WriteU8>({writer, tag}) & 0xFFFF});
    const uint32_t targets = frame[0x50];
    Memory::Write8(targets, target);
    Broadcast(cpu, session, writer, channel, 1, targets, 0xFF);
    game.Call<ReaderDestroy>({writer, 0xFFFFFFFF});
}

// A peer's ending (pEnding 1..3, one step per update): it is told to leave,
// the others that its id is gone, and when this console ends the session
// everyone that it is over; then its link shuts down.
void EndPeer(CpuContext* cpu, uint32_t session, int32_t index, uint32_t connection) {
    GuestCall game{cpu};
    const uint32_t peer = Peer(session, index);
    uint32_t step = Read(peer + pEnding);

    if (step == 1) {
        SendTag(cpu, session, kTagLeave, 1, uint8_t(index));
        Write(peer + pEnding, 2);
        step = 2;
    }

    if (step == 2) {
        if (ValidId(cpu, Read(peer + pId))) {
            Message message(cpu, session, kTagGone);
            message.U32(Read(peer + pId));
            message.Send(session, kOthers);
        }

        Write(peer + pEnding, 3);
        step = 3;
    }

    if (step == 3) {
        if (Self(session) == index) {
            GuestFrame frame(cpu, 0x60);
            const uint32_t writer = frame[0x10];
            game.Call<ReaderConstruct>({writer});
            game.Call<WriterInit>({writer, session + sScratch, 0x400});
            game.Call<WriterBegin>({writer, 0});
            game.Call<SetLength>({writer, game.Call<WriteU8>({writer, kTagEnd}) & 0xFFFF});
            const uint32_t targets = frame[0x50];
            Memory::Write8(targets, kOthers);
            Broadcast(cpu, session, writer, 1, 1, targets, 0xFF);

            for (int32_t other = 0; other < 4; ++other)
                if (Self(session) != other && Connection(session, other))
                    PeerError(session, other, kErrEnded, 0, 0x80000000, 3);

            game.Call<ReaderDestroy>({writer, 0xFFFFFFFF});
        }

        Memory::Write8(peer + pFailed, 0);
        game.Virtual(connection, cShutdown);
        Write(peer + pEnding, 4);
    }
}

// The link to a peer: started (with the round trip known so far), up once
// the peer itself answers, closed when dropped; a closed link to another
// peer waits ten seconds (or fails when fewer than three peers are left)
// and starts again, looking for a relay meanwhile.
void UpdateLink(CpuContext* cpu, uint32_t session, int32_t index, uint32_t connection) {
    GuestCall game{cpu};
    const uint32_t peer = Peer(session, index);
    const float now = F(session + sNow);

    switch (Read(peer + pState)) {
    case 1:
        Memory::Write8(peer + pDropped, 0);
        Memory::Write8(peer + pDirect, 0);
        game.Virtual(session, vPeerRtt, {uint32_t(index)});
        game.Virtual(connection, cStart, {}, {cpu->fpr[1].d});
        Write(peer + pState, 2);
        break;

    case 2:
        if (Memory::Read8(peer + pDropped)) {
            Write(peer + pState, 4);
        } else if (Memory::Read8(peer + pDirect)) {
            game.Virtual(connection, cWatchReceive, {1});
            Write(peer + pState, 3);
        }

        break;

    case 3:
        if (Memory::Read8(peer + pDropped))
            Write(peer + pState, 4);

        break;

    case 4: {
        game.Virtual(connection, cClose);

        if (index == Self(session)) {
            Write(peer + pState, 1);
            break;
        }

        uint32_t linked = 0;

        for (int32_t other = 0; other < 4; ++other)
            linked += Connection(session, other) != 0;

        if (linked < 3) {
            PeerError(session, index, kErrTooFew, 1, 0x80000000, 1);
        } else {
            Write(peer + pClosedAt, Read(session + sNow));
            Write(peer + pState, 5);

            if (!Read(peer + pRelayState))
                Write(peer + pRelayState, 1);
        }

        break;
    }

    case 5:
        if (kWait + F(peer + pClosedAt) <= now)
            Write(peer + pState, 1);

        break;
    }
}

// The search for a relay to a peer not reached directly: the others are
// asked who reaches it (once a second, up to the retry limit), the best
// answer becomes its relay, which is then sent the sequences until it says
// it is ready; a lost relay starts the search again.
void UpdateRelay(CpuContext* cpu, uint32_t session, int32_t index, uint32_t connection) {
    GuestCall game{cpu};
    const uint32_t peer = Peer(session, index);
    const float now = F(session + sNow);

    if (Memory::Read8(peer + pDirect)) {
        Write(peer + pRelayState, 0);
        Memory::Write8(peer + pRelay, 0xFF);
        SetF(peer + pRelayRtt, kRttStart);
        Write(peer + pRelayPings, 0xFFFF);
        Memory::Write8(peer + pRelayReady, 0);
    }

    switch (Read(peer + pRelayState)) {
    case 1:
        if (index == Self(session)) {
            Write(peer + pRelayState, 0);
            break;
        }

        Log(cpu, kInfo, {3, 0x805FA28C, uint32_t(index)});
        game.Virtual(connection, cWatchReceive, {0});
        Memory::Write8(peer + pRelay, 0xFE);
        SetF(peer + pRelayRtt, kRttStart);
        Write(peer + pRelayPings, 0xFFFF);
        Memory::Write8(peer + pRelayReady, 0);
        SetF(peer + pAskedAt, 0.0f);
        Write(peer + pAsked, 0);
        Write(peer + pRelayState, 2);
        break;

    case 2: {
        if (int8_t(Memory::Read8(peer + pRelay)) >= 0) {
            SetF(peer + pAskedAt, 0.0f);
            Write(peer + pAsked, 0);
            Write(peer + pRelayState, 3);
            break;
        }

        if (!(kSecond + F(peer + pAskedAt) <= now))
            break;

        Write(peer + pAskedAt, Read(session + sNow));
        Write(peer + pAsked, Read(peer + pAsked) + 1);
        const int32_t asked = ReadInt(peer + pAsked);
        const float limit = F(session + sRetryLimit);

        if (kAskShare * limit < ToFloat(asked))
            Log(cpu, kWarning, {0x805FA2C4, uint32_t(index), uint32_t(asked), uint32_t(int32_t(limit))});

        if (ToFloat(ReadInt(peer + pAsked)) <= F(session + sRetryLimit)) {
            SendWho(cpu, session, index);
        } else if (!Memory::Read8(session + sClosing)) {
            Log(cpu, kInfo, {3, 0x805FA2FC, uint32_t(index), Read(peer + pId)});
            PeerError(session, Self(session), kErrUnreachable, Read(peer + pAsked), 0x80000000, 3);
        } else if (!ValidId(cpu, Read(peer + pId))) {
            if (!Memory::Read8(session + sFlag) && Self(session) == int8_t(Memory::Read8(session + sHost))) {
                Log(cpu, kInfo, {3, 0x805FA3A0, uint32_t(index)});
                PeerError(session, index, kErrUnreachable, Read(peer + pAsked), 0x80000000, 3);
            }
        } else {
            Log(cpu, kInfo, {3, 0x805FA358, uint32_t(index), Read(peer + pId)});
            PeerError(session, index, kErrTooFew, 0, 0x80000000, 1);
        }

        break;
    }

    case 3:
        if (Route(session, index) < 0) {
            Log(cpu, kInfo, {3, 0x805FA3F0, uint32_t(index), uint32_t(int32_t(int8_t(Memory::Read8(peer + pRelay))))});
            Write(peer + pRelayState, 1);
        } else if (!Memory::Read8(peer + pRelayReady)) {
            if (kSecond + F(peer + pAskedAt) <= now) {
                Write(peer + pAskedAt, Read(session + sNow));
                SendSequences(cpu, session, index);

                if (Memory::Read8(peer + pKnown))
                    SendReady(cpu, session, false, index);
            }
        } else {
            Log(cpu, kInfo, {3, 0x805FA428, uint32_t(Self(session)), uint32_t(int32_t(int8_t(Memory::Read8(peer + pRelay)))), uint32_t(index)});
            Write(peer + pRelayState, 4);
        }

        break;

    case 4:
        if (Route(session, index) < 0) {
            Log(cpu, kInfo, {3, 0x805FA3F0, uint32_t(index), uint32_t(int32_t(int8_t(Memory::Read8(peer + pRelay))))});
            Write(peer + pRelayState, 1);
        } else if (kRecheck + F(peer + pAskedAt) <= now) {
            Write(peer + pRelayPings, 0xFFFF);
            Write(peer + pAskedAt, Read(session + sNow));
            SendWho(cpu, session, index);
        }

        break;

    case 5:
        Write(peer + pRelayAt, Read(session + sNow));
        Write(peer + pRelayState, 6);
        break;

    case 6:
        if (F(peer + pRelayAt) + kRttStart <= now)
            Write(peer + pRelayState, 1);

        break;
    }
}

// The sends to a peer through its route: the reliable channel (forced when
// asked), the unreliable one (forced while direct data waits), then the
// first waiting direct data rides along with the batch. False when the
// direct data does not fit: the original then skips the rest of this update.
bool SendToPeer(CpuContext* cpu, uint32_t session, int32_t index) {
    GuestCall game{cpu};
    const int32_t route = Route(session, index);

    if (route < 0)
        return true;

    const uint32_t peer = Peer(session, index);
    const uint32_t slot = Read(peer + pDirectSlot);
    const uint32_t connection = Connection(session, route);
    const uint32_t lengthAt = peer + pDirectLength + slot * 4;
    const uint32_t data = peer + pDirectData + slot * 0x400;
    game.Call<ConnectionBatchReopen>({connection});

    if (SendChannel(cpu, session, index, 0, Memory::Read8(peer + pForce) != 0))
        Memory::Write8(peer + pForce, 0);

    const bool sent = SendChannel(cpu, session, index, 1, ReadInt(lengthAt) > 0);
    const int32_t room = int32_t(game.Call<ConnectionBatchRoom>({connection}));

    if (ReadInt(lengthAt) < 1 || room < 1 || !sent) {
        game.Call<ConnectionBatchFlush>({connection, 0, 0, 0});
    } else {
        const uint32_t length = game.Virtual(Platform(), kToHost16, {Memory::Read16(data)}) & 0xFFFF;

        if (length == 0 || room < int32_t(length))
            return false;

        game.Call<Memcpy>({session + sScratch, data + 2, length});
        const int32_t used = int32_t(length) + 2;
        game.Call<ConnectionBatchFlush>({connection, session + sScratch, length, uint32_t(int32_t(int8_t(slot)))});
        Write(lengthAt, uint32_t(ReadInt(lengthAt) - used));

        if (ReadInt(lengthAt) > 0)
            game.Call<Memmove>({data, data + uint32_t(used), Read(lengthAt)});
    }

    Write(peer + pDirectSlot, 0);
    return true;
}

// The reliable queue to each routed peer, once a second: smoothed, and
// counted while it grows; a peer's load is that count plus its route's
// unanswered pings. Too many peers over the drop limit end the session; a
// load near it marks all links received; over the warning limit the send
// rates go down, all clear they recover.
void WatchLoad(CpuContext* cpu, uint32_t session) {
    GuestCall game{cpu};
    int32_t linked = 0, over = 0, near = 0, overWarn = 0, nearWarn = 0;
    int32_t load[4] = {};
    const float now = F(session + sNow);

    for (int32_t index = 0; index < 4; ++index) {
        if (!game.Virtual(session, vJoined, {uint32_t(index)}))
            continue;

        const int32_t route = Route(session, index);

        if (route < 0)
            continue;

        const uint32_t peer = Peer(session, index);

        if (kSecond + F(peer + pLoadAt) <= now) {
            Write(peer + pLoadAt, Read(session + sNow));
            GuestFrame frame(cpu, 0x40);
            const uint32_t reader = frame[0x10];
            game.Call<ReaderConstruct>({reader});
            game.Call<PoolLoadInto>({peer + pPools, reader});
            const uint32_t queued = game.Call<Length16>({reader}) & 0xFFFF;
            const uint32_t maxHeader = game.Call<MaxHeader>() & 0xFFFF;
            const uint32_t unitHeader = game.Call<UnitHeaderSize>() & 0xFFFF;
            const float size = ToFloat(int32_t(queued));

            if (queued < unitHeader + maxHeader) {
                Write(peer + pCongested, 0);
                SetF(peer + pQueue, kKeep * F(peer + pQueue) + kTake * size);
            } else if (F(peer + pQueue) < size) {
                if (ReadInt(peer + pCongested) < 0xFFFF)
                    Write(peer + pCongested, Read(peer + pCongested) + 1);

                SetF(peer + pQueue, size);
            } else {
                if (ReadInt(peer + pCongested) > 0)
                    Write(peer + pCongested, Read(peer + pCongested) - 1);

                SetF(peer + pQueue, kKeep * F(peer + pQueue) + kTake * size);
            }

            if (ReadInt(peer + pCongested) > 7)
                LogFloat(cpu, kWarning, double(F(peer + pQueue)), {0x805FA46C, uint32_t(index), queued, Read(peer + pCongested)});

            if (kSecond + F(peer + pKeepaliveAt) <= F(session + sNow))
                SendKeepalive(cpu, session, uint8_t(index));

            game.Call<ReaderDestroy>({reader, 0xFFFFFFFF});
        }

        ++linked;
        load[index] = ReadInt(peer + pCongested) + int32_t(game.Call<ConnectionPings>({Connection(session, route)}));
        const int32_t drop = ReadInt(session + sLossDrop), warn = ReadInt(session + sLossWarn);

        if (load[index] < drop) {
            if (drop / 2 <= load[index])
                ++near;
        } else {
            ++over;
        }

        if (load[index] < warn) {
            if (warn / 2 <= load[index])
                ++nearWarn;
        } else {
            ++overWarn;
        }
    }

    if (linked > 2) {
        if (over >= linked - 1) {
            PeerError(session, Self(session), kErrCongested, 0, 0x80000000, 3);
        } else if (linked - 1 <= near) {
            for (int32_t index = 0; index < 4; ++index)
                if (const uint32_t connection = Connection(session, index))
                    game.Virtual(connection, cMarkReceived);
        }
    }

    if (linked > 1) {
        const auto tune = [&](bool down, bool overOnly) {
            for (int32_t index = 0; index < 4; ++index) {
                if (!game.Virtual(session, vJoined, {uint32_t(index)}) || index == Self(session))
                    continue;

                if (!down)
                    TuneUp(cpu, session, index);
                else if (!overOnly || ReadInt(session + sLossWarn) <= load[index])
                    TuneDown(cpu, session, index);
            }
        };

        if (overWarn < 1) {
            if (nearWarn == 0)
                tune(false, false);
        } else {
            tune(true, nearWarn < linked / 2);
        }
    }
}

// The host and the relay host must stay routed: unrouted longer than the
// timeout (the host's own, or the loss limit plus six seconds), the session
// ends.
void WatchHost(CpuContext* cpu, uint32_t session, uint32_t hostAt, uint32_t seenAt, uint32_t message) {
    if (Route(session, int8_t(Memory::Read8(session + hostAt))) >= 0) {
        if (F(session + seenAt) == 0.0f)
            Log(cpu, kInfo, {3, message, uint32_t(Self(session)), uint32_t(int32_t(int8_t(Memory::Read8(session + hostAt))))});

        Write(session + seenAt, Read(session + sNow));
    }

    if (!(0.0f < F(session + seenAt)))
        return;

    const float limit = Memory::Read8(session + sFlag) ? F(session + sHostTimeout) : ToFloat(ReadInt(session + sLossDrop) + 6);

    if (limit <= F(session + sNow) - F(session + seenAt))
        PeerError(session, Self(session), kErrHostLost, uint32_t(int32_t(limit)), 0x80000000, 3);
}

// 0x803CFF50: a step of the session: each connected peer's link updated,
// a peer whose id is gone failed, an ending peer taken down a step, then
// its link and relay state, its control packets executed, its channels
// sent, its resend time tuned and its host timeout watched; a failed peer
// is reported to the callback (event 3) and reset. Then the load and the
// hosts are watched.
void Update(CpuContext* cpu, uint32_t session) {
    GuestCall game{cpu};
    SetF(session + sNow, float(Now(cpu)));

    for (int32_t index = 0; index < 4; ++index) {
        const uint32_t peer = Peer(session, index);
        const uint32_t connection = Read(peer + pConnection);

        if (!connection)
            continue;

        game.Virtual(connection, cUpdate);

        if (ValidId(cpu, Read(peer + pId))) {
            for (uint32_t i = 0; i < 4; ++i) {
                if (Read(peer + pId) == Read(session + sIds + i * 4)) {
                    Log(cpu, kInfo, {3, 0x805FA230, uint32_t(index), Read(session + sIds + i * 4)});
                    PeerError(session, index, kErrLeft, 0, 0x80000000, 3);
                    break;
                }
            }
        }

        const uint32_t ending = Read(peer + pEnding);

        if (ending >= 1 && ending <= 3) {
            EndPeer(cpu, session, index, connection);
        } else if (ending != 4) {
            UpdateLink(cpu, session, index, connection);
            UpdateRelay(cpu, session, index, connection);
            Drain(cpu, session, index, false);

            if (!SendToPeer(cpu, session, index))
                break;

            TuneRto(cpu, session, index);

            if (Memory::Read8(peer + pHostKnown)) {
                game.Virtual(connection, cMarkReceived);

                if (Route(session, index) < 0) {
                    Write(peer + pRoutedAt, Read(session + sNow));
                } else {
                    const int32_t timeout = ReadInt(peer + pHostTimeout);

                    if (F(peer + pRoutedAt) + ToFloat(timeout) <= F(session + sNow))
                        PeerError(session, index, kErrHostTimeout, uint32_t(timeout), 0x80000000, 1);
                }
            }

            continue;
        }

        if (Memory::Read8(peer + pFailed)) {
            GuestCall{cpu}(Read(session + sCallback), {3, uint32_t(index), Read(peer + pError), 1, peer + pError, Read(session + sParam)});
            game.Virtual(session, vResetPeer, {uint32_t(index)});
        }
    }

    WatchLoad(cpu, session);
    WatchHost(cpu, session, sHost, sHostSeen, 0x805FA4AC);
    WatchHost(cpu, session, sRelayHost, sRelaySeen, 0x805FA4E0);
}

// 0x803D1580 / 0x803D1524: a byte stream over a buffer.
uint32_t StreamObjectConstruct(CpuContext* cpu, uint32_t stream) {
    GuestCall{cpu}.Call<StreamConstruct>({stream});
    Write(stream, kStreamVtable);
    return stream;
}

uint32_t StreamObjectDestroy(CpuContext* cpu, uint32_t stream, int16_t how) {
    if (stream) {
        GuestCall{cpu}.Call<StreamDestroy>({stream, 0});

        if (how > 0)
            GuestCall{cpu}.Call<OperatorDelete>({stream});
    }

    return stream;
}

// 0x803D12B8 (virtual 0x18): a new connection of `kind` in the first free
// peer, opened with the session's id, this session id and both send
// sequences as its hello name; the peer's id object takes `address`. The
// peer, or -1.
int32_t SetConnection(CpuContext* cpu, uint32_t session, uint32_t kind, uint32_t address) {
    GuestCall game{cpu};
    GuestFrame frame(cpu, 0x100);
    const uint32_t opened = frame[0x10];
    const uint32_t stream = frame[0x18];
    const uint32_t buffer = frame[0x28];
    uint32_t connection = game.Call<OperatorNew>({0x2250});

    if (connection)
        game.Call<ConnectionConstruct>({connection, kind});

    StreamObjectConstruct(cpu, stream);

    if (!connection) {
        Log(cpu, kError, {0x805FA518});
        StreamObjectDestroy(cpu, stream, -1);
        return -1;
    }

    int32_t index = 0;

    while (index < 4 && Connection(session, index))
        ++index;

    if (index == 4) {
        Log(cpu, kError, {0x805FA548});
        game.Virtual(connection, cDestroy, {1});
        StreamObjectDestroy(cpu, stream, -1);
        return -1;
    }

    const uint32_t peer = Peer(session, index);
    game.Virtual(stream, 0x14, {buffer, 0x80});
    game.Call<StreamWriteId>({stream, session + sIdObject});
    game.Call<StreamWriteU32>({stream, Read(session + sId)});
    game.Call<StreamWriteU16>({stream, game.Call<PoolSequence>({peer + pPools})});
    game.Call<StreamWriteU16>({stream, game.Call<PoolSequence>({peer + pPools + 0x18})});
    game.Call<Memset>({opened, 0, 8});
    Write(opened, game.Call<StreamData>({stream}));
    Write(opened + 4, game.Call<StreamLength>({stream}));
    game.Virtual(connection, cOpen, {SessionForward, session, uint32_t(index), opened});
    Write(peer + pConnection, connection);
    game.Virtual(peer + pIdObject, 0x28, {address});
    Write(peer + pId, 0);
    StreamObjectDestroy(cpu, stream, -1);
    return index;
}

// 0x803D15BC (virtual 0x1C): a peer's connection closed and deleted and
// the peer back to its start, with new first sequences.
void ResetPeer(CpuContext* cpu, uint32_t session, int32_t index) {
    if (!ValidPeer(index) || !Connection(session, index))
        return;

    GuestCall game{cpu};
    const uint32_t peer = Peer(session, index);
    game.Virtual(Read(peer + pConnection), cCloseTransport);

    if (const uint32_t connection = Read(peer + pConnection)) {
        game.Virtual(connection, cDestroy, {1});
        Write(peer + pConnection, 0);
    }

    Write(peer + pState, 0);
    Write(peer + pRelayState, 0);
    Write(peer + pEnding, 0);
    Write(peer + pHostTimeout, 0);
    Memory::Write8(peer + pRelay, 0xFF);
    SetF(peer + pRelayRtt, kRttStart);
    Memory::Write8(peer + pJoined, 0);
    Memory::Write8(peer + pDirect, 0);
    Memory::Write8(peer + pDropped, 0);
    Memory::Write8(peer + pRelayReady, 0);
    Memory::Write8(peer + pFailed, 0);
    game.Call<Memset>({peer + pError, 0, 0xC});
    Write(peer + pConnection, 0);
    game.Virtual(peer + pIdObject, 0x18);
    Write(peer + pId, 0);
    SetF(peer + pClosedAt, 0.0f);
    SetF(peer + pRelayAt, 0.0f);
    Write(peer + pRelayPings, 0xFFFF);
    SetF(peer + pAskedAt, 0.0f);
    Write(peer + pAsked, 0);
    Memory::Write8(peer + pHostKnown, 0);
    SetF(peer + pRoutedAt, 0.0f);
    SetF(peer + pLoadAt, 0.0f);
    SetF(peer + pQueue, 0.0f);
    Write(peer + pCongested, 0);
    SetF(peer + pKeepaliveAt, 0.0f);

    for (uint32_t pool = 0; pool < 4; ++pool)
        game.Call<PoolReset>({peer + pPools + pool * 0x18});

    game.Call<PoolSetSequence>({peer + pPools, InitialSequence(cpu)});
    game.Call<PoolSetSequence>({peer + pPools + 0x18, InitialSequence(cpu)});
    Memory::Write8(peer + pKnown, 0);
    Memory::Write8(peer + pForce, 0);
    SetF(peer + pDelay, 0.0f);
    SetF(peer + pSentAt, 0.0f);
    SetF(peer + pSentAt + 4, 0.0f);
    ResetRates(peer);
}

// 0x803D17EC (virtual 0x20): connects a peer's idle link; it is then looked
// after (pState 1) and, unless already searching, a relay search waits.
void ConnectPeer(CpuContext* cpu, uint32_t session, int32_t index, uint32_t address, uint32_t probe) {
    if (!ValidPeer(index))
        return;

    const uint32_t peer = Peer(session, index);
    const uint32_t connection = Read(peer + pConnection);

    if (!connection || Read(peer + pState))
        return;

    GuestCall{cpu}.Virtual(connection, cConnect, {address, probe});
    Write(peer + pState, 1);

    if (!Read(peer + pRelayState))
        Write(peer + pRelayState, 5);
}

// 0x803D1970 (virtual 0x38): data to peers: one packet of at most 0x200
// bytes with its headers, queued on `channel` for `targets`.
void PutData(CpuContext* cpu, uint32_t session, uint32_t data, int32_t length, int32_t channel, int32_t count, uint32_t targets, uint32_t limit) {
    GuestCall game{cpu};
    GuestFrame frame(cpu, 0x60);
    const uint32_t writer = frame[0x10];
    game.Call<ReaderConstruct>({writer});
    const uint32_t unitHeader = game.Call<UnitHeaderSize>() & 0xFFFF;
    const uint32_t maxHeader = game.Call<MaxHeader>() & 0xFFFF;

    if (int32_t(0x200 - unitHeader - maxHeader) < length) {
        PeerError(session, Self(session), kErrTooLarge, 0, 0x80000000, 3);
        Log(cpu, kError, {0x805FA580, uint32_t(length)});
        game.Call<ReaderDestroy>({writer, 0xFFFFFFFF});
        return;
    }

    game.Call<WriterInit>({writer, session + sScratch, 0x200});
    game.Call<WriterBegin>({writer, 1});

    if (int32_t(game.Call<WriteBytes>({writer, data, uint32_t(length)})) < 0) {
        PeerError(session, Self(session), kErrSendFull, 0, 0x80000000, 3);
        Log(cpu, kError, {0x805FA5B0, uint32_t(length)});
        game.Call<ReaderDestroy>({writer, 0xFFFFFFFF});
        return;
    }

    game.Call<SetLength>({writer, uint32_t(length) & 0xFFFF});
    Broadcast(cpu, session, writer, channel, count, targets, limit & 0xFF);
    game.Call<ReaderDestroy>({writer, 0xFFFFFFFF});
}

// 0x803D1B00 (virtual 0x3C): direct data for a joined peer, length-prefixed
// in its slot (only slot 0); a full slot starts over. Unless relaying is
// allowed, only for a peer reached directly.
void PutDirect(CpuContext* cpu, uint32_t session, uint32_t data, int32_t length, int8_t slot, int8_t index) {
    GuestCall game{cpu};

    if (length < 1 || !data)
        return;

    if (!game.Virtual(session, vJoined, {uint32_t(int32_t(index))}) || slot < 0 || slot >= 1 || length + 2 > 0x400)
        return;

    if (!Read(kDirectRoutes) && index != int8_t(Route(session, index)))
        return;

    const uint32_t peer = Peer(session, index);
    const uint32_t lengthAt = peer + pDirectLength + uint32_t(slot) * 4;
    const uint32_t buffer = peer + pDirectData + uint32_t(slot) * 0x400;

    if (0x400 < length + ReadInt(lengthAt) + 2)
        Write(lengthAt, 0);

    const uint32_t prefix = game.Virtual(Platform(), kToNet16, {uint32_t(length) & 0xFFFF});
    Memory::Write16(buffer + Read(lengthAt), uint16_t(prefix));
    Write(lengthAt, Read(lengthAt) + 2);
    game.Call<Memcpy>({buffer + Read(lengthAt), data, uint32_t(length)});
    Write(lengthAt, Read(lengthAt) + uint32_t(length));
}

// 0x803D1D00 (virtual 0x4C): whether a connected peer has joined.
bool Joined(uint32_t session, int32_t index) {
    if (!ValidPeer(index) || !Connection(session, index))
        return false;

    return Memory::Read8(Peer(session, index) + pJoined) != 0;
}

// 0x803D1D44 (through 0x803CF830): a connection's event. 1: connected, its
// hello carries the peer's session id and sequences (a wrong id drops it);
// 2/4: an error or a close drops the link; 5: refused, the peer ends; 6:
// shut down, the peer is reported; 3: a session unit packet arrived: for
// this session from a known peer it is filed (an empty one carries the
// sequences), for another peer it is passed on along that peer's route.
void NetworkEvent(CpuContext* cpu, uint32_t session, uint32_t event, int32_t index, uint32_t length, uint32_t info) {
    GuestCall game{cpu};
    GuestFrame frame(cpu, 0x100);
    const uint32_t stream = frame[0x30];
    const uint32_t id = frame[0x40];
    const uint32_t unit = frame[0x68];
    const uint32_t sequences = frame[0x80];
    const uint32_t address = frame[0x88];
    StreamObjectConstruct(cpu, stream);
    game.Call<IdConstruct>({id});
    game.Call<UnitConstruct>({unit});
    const uint32_t peer = Peer(session, index);

    switch (event) {
    case 2:
        MarkReset(session, index);
        Log(cpu, kWarning, {0x805FA5E0, uint32_t(index), Read(info), Read(info + 4), Read(info + 8)});
        break;

    case 4:
        MarkReset(session, index);
        Log(cpu, kWarning, {0x805FA634, uint32_t(index)});
        break;

    case 5:
        PeerError(session, index, kErrRemoteEnd, 0, 0x80000000, 2);
        Log(cpu, kWarning, {0x805FA678, uint32_t(index)});
        break;

    case 6:
        if (!Read(peer + pConnection))
            Log(cpu, kError, {0x805FA6C0, uint32_t(index)});
        else
            Memory::Write8(peer + pFailed, 1);

        break;

    case 1: {
        if (!Read(peer + pConnection)) {
            Log(cpu, kError, {0x805FA718, uint32_t(index)});
            break;
        }

        game.Virtual(stream, 0x1C, {info, length});
        game.Call<StreamReadId>({stream, id});

        if (!game.Call<IdEquals>({id, peer + pIdObject})) {
            MarkReset(session, index);
            Log(cpu, kWarning, {0x805FA774, uint32_t(index)});
            break;
        }

        game.Call<StreamReadU32>({stream, peer + pId});
        game.Call<StreamReadU16>({stream, sequences + 2});
        game.Call<StreamReadU16>({stream, sequences});

        if (!Memory::Read8(peer + pKnown)) {
            Memory::Write8(peer + pKnown, 1);
            game.Call<PoolSetSequence>({peer + pPools + 0x30, Memory::Read16(sequences + 2)});
            game.Call<PoolSetSequence>({peer + pPools + 0x48, Memory::Read16(sequences)});
        }

        const uint32_t low = game.Call<PoolSequence>({peer + pPools + 0x48});
        const uint32_t top = game.Call<PoolSequence>({peer + pPools + 0x30});
        Log(cpu, kInfo, {3, 0x805FA7B8, uint32_t(index), Read(peer + pId), top, low});
        Memory::Write8(peer + pDirect, 1);

        if (!Memory::Read8(peer + pJoined)) {
            Memory::Write8(peer + pJoined, 1);
            GuestCall{cpu}(Read(session + sCallback), {1, uint32_t(index), 0, 0, 0, Read(session + sParam)});
        }

        break;
    }

    case 3: {
        game.Call<UnitAttach>({unit, Read(info + 4), Read(info)});
        const int8_t channel = int8_t(game.Call<UnitFlagB>({unit}));

        if (Read(session + sId) == game.Call<UnitReceiver>({unit})) {
            const uint32_t sender = game.Call<UnitSender>({unit});
            int32_t from = 0;

            while (from < 4 && Read(Peer(session, from) + pId) != sender)
                ++from;

            if (from == 4)
                break;

            const uint32_t source = Peer(session, from);
            uint32_t filed = 0;

            if (!game.Call<UnitFlagD>({unit})) {
                if (Memory::Read8(source + pKnown)) {
                    if (channel == 0) {
                        filed = game.Call<PoolPutTop>({source + pPools + 0x30, unit});
                        game.Call<PoolAck>({source + pPools, unit});
                    } else if (channel == 1) {
                        filed = game.Call<PoolPutLow>({source + pPools + 0x48, unit});

                        if (int32_t(filed) < 0)
                            filed = 0;

                        if (game.Virtual(session, vJoined, {uint32_t(from)})) {
                            Write(address, Read(info + 8));
                            Write(address + 4, Read(info + 12));
                            Memory::Write8(address + 8, Memory::Read8(info + 16));
                            GuestCall{cpu}(Read(session + sCallback), {5, uint32_t(from), 0, 1, address, Read(session + sParam)});
                        }
                    }
                }
            } else {
                if (!Memory::Read8(source + pKnown)) {
                    Memory::Write8(source + pKnown, 1);
                    game.Call<PoolSetSequence>({source + pPools + 0x30, game.Call<UnitSequence>({unit})});
                    game.Call<PoolSetSequence>({source + pPools + 0x48, game.Call<UnitAck>({unit})});
                }

                if (Memory::Read8(source + pDirect))
                    MarkReset(session, from);

                const uint32_t low = game.Call<PoolSequence>({source + pPools + 0x48});
                const uint32_t top = game.Call<PoolSequence>({source + pPools + 0x30});
                Log(cpu, kInfo, {3, 0x805FA818, uint32_t(from), top, low});
            }

            if (int32_t(filed) < 4 && filed > 2) {
                if (filed == 3)
                    Memory::Write8(source + pForce, 1);
                else
                    PeerError(session, from, filed, 0, 0x80000000, 1);
            }
        } else if (game.Call<UnitReceiver>({unit})) {
            const uint32_t receiver = game.Call<UnitReceiver>({unit});
            int32_t to = 0;

            while (to < 4 && Read(Peer(session, to) + pId) != receiver)
                ++to;

            if (to == 4)
                break;

            const int32_t route = Route(session, to);

            if (route >= 0) {
                const uint32_t connection = Connection(session, route);
                game.Call<UnitSeal>({unit});
                game.Call<UnitEncode>({unit});
                game.Call<ConnectionBatchPut>({connection, unit});
                game.Call<ConnectionBatchFlush>({connection, Read(info + 12), Read(info + 8), uint32_t(int32_t(int8_t(Memory::Read8(info + 16))))});
            }
        }

        break;
    }
    }

    game.Call<UnitDestroy>({unit, 0xFFFFFFFF});
    game.Call<IdDestroy>({id, 0xFFFFFFFF});
    StreamObjectDestroy(cpu, stream, -1);
}

// 0x803D26FC (virtual 0x8C): the round trip to a peer, through its relay
// when it has one (the relay's link plus the relay's own report); 3 s when
// not joined or not routed.
double PeerRtt(CpuContext* cpu, uint32_t session, int32_t index) {
    GuestCall game{cpu};

    if (!game.Virtual(session, vJoined, {uint32_t(index)}))
        return kRttStart;

    const int32_t route = int8_t(Route(session, index));

    if (route < 0)
        return kRttStart;

    game.Call<ConnectionRtt>({Connection(session, route)});
    const double rtt = cpu->fpr[1].d;

    if (index == route)
        return rtt;

    return double(float(double(F(Peer(session, index) + pRelayRtt)) + rtt));
}

// 0x803D27C4 (virtual 0x90): the bytes per second a peer may be sent.
int32_t Throughput(CpuContext* cpu, uint32_t session, int32_t index) {
    if (!GuestCall{cpu}.Virtual(session, vJoined, {uint32_t(index)}))
        return 0;

    if (int8_t(Route(session, index)) < 0)
        return 0x400;

    const uint32_t peer = Peer(session, index);
    float interval = F(peer + pInterval);

    if (interval <= kMinInterval)
        interval = kMinInterval;

    return int32_t(kSecond / interval * ToFloat(ReadInt(peer + pQuota)));
}

// 0x803D29A4 (virtual 0x9C): the room left in a joined peer's reliable
// send pool.
int32_t SendRoom(CpuContext* cpu, uint32_t session, int32_t index) {
    GuestCall game{cpu};

    if (!game.Virtual(session, vJoined, {uint32_t(index)}))
        return 0;

    GuestFrame frame(cpu, 0x40);
    const uint32_t reader = frame[0x10];
    game.Call<ReaderConstruct>({reader});
    game.Call<PoolLoadInto>({Peer(session, index) + pPools, reader});
    const int32_t room = 0x2000 - int32_t(game.Call<Length16>({reader}) & 0xFFFF);
    game.Call<ReaderDestroy>({reader, 0xFFFFFFFF});
    return room;
}

// 0x803D2620 (virtual 0x74): the relay of a joined peer, or -1.
int32_t RelayOf(CpuContext* cpu, uint32_t session, int32_t index) {
    if (!ValidPeer(index) || !Connection(session, index))
        return -1;

    if (!GuestCall{cpu}.Virtual(session, vJoined, {uint32_t(index)}))
        return -1;

    return int8_t(Memory::Read8(Peer(session, index) + pRelay));
}

// Each connection's virtual `slot` with a float in f1.
void ForEachConnection(CpuContext* cpu, uint32_t session, uint32_t slot, double value) {
    for (int32_t index = 0; index < 4; ++index)
        if (const uint32_t connection = Connection(session, index))
            GuestCall{cpu}.Virtual(connection, slot, {}, {value});
}

} // namespace

extern "C" void MH3_NetSessionConstruct(CpuContext* cpu) {
    cpu->gpr[3] = Construct(cpu, Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CF860, MH3_NetSessionConstruct, "NetSessionConstruct");

extern "C" void MH3_NetSessionIdDestroy(CpuContext* cpu) {
    cpu->gpr[3] = IdObjectDestroy(cpu, Arg(cpu, 3), int16_t(Arg(cpu, 4)));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CF8CC, MH3_NetSessionIdDestroy, "NetSessionIdDestroy");

extern "C" void MH3_NetSessionIdConstruct(CpuContext* cpu) {
    cpu->gpr[3] = IdObjectConstruct(cpu, Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CF928, MH3_NetSessionIdConstruct, "NetSessionIdConstruct");

extern "C" void MH3_NetSessionPeerDestroy(CpuContext* cpu) {
    cpu->gpr[3] = PeerDestroy(cpu, Arg(cpu, 3), int16_t(Arg(cpu, 4)));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CF958, MH3_NetSessionPeerDestroy, "NetSessionPeerDestroy");

extern "C" void MH3_NetSessionPeerConstruct(CpuContext* cpu) {
    cpu->gpr[3] = PeerConstruct(cpu, Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CF9C4, MH3_NetSessionPeerConstruct, "NetSessionPeerConstruct");

extern "C" void MH3_NetSessionDestroy(CpuContext* cpu) {
    cpu->gpr[3] = Destroy(cpu, Arg(cpu, 3), int16_t(Arg(cpu, 4)));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CFA00, MH3_NetSessionDestroy, "NetSessionDestroy");

extern "C" void MH3_NetSessionInit(CpuContext* cpu) {
    Init(cpu, Arg(cpu, 3), uint8_t(Arg(cpu, 4)), Arg(cpu, 5), Arg(cpu, 6), Arg(cpu, 7), Arg(cpu, 8));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CFAA8, MH3_NetSessionInit, "NetSessionInit");

extern "C" void MH3_NetSessionCloseAll(CpuContext* cpu) {
    CloseAll(cpu, Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CFEF8, MH3_NetSessionCloseAll, "NetSessionCloseAll");

extern "C" void MH3_NetSessionUpdate(CpuContext* cpu) {
    Update(cpu, Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CFF50, MH3_NetSessionUpdate, "NetSessionUpdate");

extern "C" void MH3_NetSessionSetConnection(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(SetConnection(cpu, Arg(cpu, 3), Arg(cpu, 4), Arg(cpu, 5)));
}
REGISTER_NATIVE_FUNCTION_AS(0x803D12B8, MH3_NetSessionSetConnection, "NetSessionSetConnection");

extern "C" void MH3_NetSessionStreamDestroy(CpuContext* cpu) {
    cpu->gpr[3] = StreamObjectDestroy(cpu, Arg(cpu, 3), int16_t(Arg(cpu, 4)));
}
REGISTER_NATIVE_FUNCTION_AS(0x803D1524, MH3_NetSessionStreamDestroy, "NetSessionStreamDestroy");

extern "C" void MH3_NetSessionStreamConstruct(CpuContext* cpu) {
    cpu->gpr[3] = StreamObjectConstruct(cpu, Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x803D1580, MH3_NetSessionStreamConstruct, "NetSessionStreamConstruct");

extern "C" void MH3_NetSessionResetPeer(CpuContext* cpu) {
    ResetPeer(cpu, Arg(cpu, 3), int8_t(Arg(cpu, 4)));
}
REGISTER_NATIVE_FUNCTION_AS(0x803D15BC, MH3_NetSessionResetPeer, "NetSessionResetPeer");

extern "C" void MH3_NetSessionConnectPeer(CpuContext* cpu) {
    ConnectPeer(cpu, Arg(cpu, 3), int8_t(Arg(cpu, 4)), Arg(cpu, 5), Arg(cpu, 6));
}
REGISTER_NATIVE_FUNCTION_AS(0x803D17EC, MH3_NetSessionConnectPeer, "NetSessionConnectPeer");

// 0x803D1894 (virtual 0x24): every connection refuses its peer.
extern "C" void MH3_NetSessionRefuseAll(CpuContext* cpu) {
    const uint32_t session = Arg(cpu, 3);

    for (int32_t index = 0; index < 4; ++index)
        if (const uint32_t connection = Connection(session, index))
            GuestCall{cpu}.Virtual(connection, cRefuse);
}
REGISTER_NATIVE_FUNCTION_AS(0x803D1894, MH3_NetSessionRefuseAll, "NetSessionRefuseAll");

// 0x803D18F8 (virtual 0x28): this console leaves.
extern "C" void MH3_NetSessionLeave(CpuContext* cpu) {
    const uint32_t session = Arg(cpu, 3);
    PeerError(session, Self(session), kErrClosed, 0, 0x80000000, 3);
}
REGISTER_NATIVE_FUNCTION_AS(0x803D18F8, MH3_NetSessionLeave, "NetSessionLeave");

// 0x803D191C (virtual 0x2C): a connected peer is dropped.
extern "C" void MH3_NetSessionKick(CpuContext* cpu) {
    const uint32_t session = Arg(cpu, 3);
    const int32_t index = int8_t(Arg(cpu, 4));

    if (ValidPeer(index) && Connection(session, index))
        PeerError(session, index, kErrClosed, 0, 0x80000000, 1);
}
REGISTER_NATIVE_FUNCTION_AS(0x803D191C, MH3_NetSessionKick, "NetSessionKick");

extern "C" void MH3_NetSessionSendHostId(CpuContext* cpu) {
    SendHostId(cpu, Arg(cpu, 3), Arg(cpu, 4));
}
REGISTER_NATIVE_FUNCTION_AS(0x803D3D4C, MH3_NetSessionSendHostId, "NetSessionSendHostId");

extern "C" void MH3_NetSessionSendHostGone(CpuContext* cpu) {
    SendHostGone(cpu, Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x803D3E0C, MH3_NetSessionSendHostGone, "NetSessionSendHostGone");

extern "C" void MH3_NetSessionPutData(CpuContext* cpu) {
    PutData(cpu, Arg(cpu, 3), Arg(cpu, 4), int32_t(Arg(cpu, 5)), int32_t(Arg(cpu, 6)), int32_t(Arg(cpu, 7)), Arg(cpu, 8), Arg(cpu, 9));
}
REGISTER_NATIVE_FUNCTION_AS(0x803D1970, MH3_NetSessionPutData, "NetSessionPutData");

extern "C" void MH3_NetSessionPutDirect(CpuContext* cpu) {
    PutDirect(cpu, Arg(cpu, 3), Arg(cpu, 4), int32_t(Arg(cpu, 5)), int8_t(Arg(cpu, 6)), int8_t(Arg(cpu, 7)));
}
REGISTER_NATIVE_FUNCTION_AS(0x803D1B00, MH3_NetSessionPutDirect, "NetSessionPutDirect");

// 0x803D1C64 (virtual 0x44) / 0x803D1CB4 (virtual 0x48): every peer's
// waiting data delivered or dropped.
extern "C" void MH3_NetSessionDeliver(CpuContext* cpu) {
    const uint32_t session = Arg(cpu, 3);

    for (int32_t index = 0; index < 4; ++index)
        Drain(cpu, session, index, true);
}
REGISTER_NATIVE_FUNCTION_AS(0x803D1C64, MH3_NetSessionDeliver, "NetSessionDeliver");

extern "C" void MH3_NetSessionDropAll(CpuContext* cpu) {
    const uint32_t session = Arg(cpu, 3);

    for (int32_t index = 0; index < 4; ++index)
        DropData(cpu, session, index);
}
REGISTER_NATIVE_FUNCTION_AS(0x803D1CB4, MH3_NetSessionDropAll, "NetSessionDropAll");

extern "C" void MH3_NetSessionJoined(CpuContext* cpu) {
    cpu->gpr[3] = Joined(Arg(cpu, 3), int8_t(Arg(cpu, 4)));
}
REGISTER_NATIVE_FUNCTION_AS(0x803D1D00, MH3_NetSessionJoined, "NetSessionJoined");

extern "C" void MH3_NetSessionEvent(CpuContext* cpu) {
    NetworkEvent(cpu, Arg(cpu, 3), Arg(cpu, 4), int8_t(Arg(cpu, 5)), Arg(cpu, 7), Arg(cpu, 8));
}
REGISTER_NATIVE_FUNCTION_AS(0x803D1D44, MH3_NetSessionEvent, "NetSessionEvent");

// 0x803D2378 .. 0x803D2444 (virtuals 0x50..0x5C): the defaults set, then
// copied into the session (0x803D23B8 also restarts the host watch).
extern "C" void MH3_NetSessionUseLoss(CpuContext* cpu) {
    const uint32_t session = Arg(cpu, 3);
    GuestCall{cpu}.Call<SessionSetLoss>({session, Arg(cpu, 4), Arg(cpu, 5)});
    Write(session + sLossDrop, Read(kDefaultLossDrop));
    Write(session + sLossWarn, Read(kDefaultLossWarn));
}
REGISTER_NATIVE_FUNCTION_AS(0x803D2378, MH3_NetSessionUseLoss, "NetSessionUseLoss");

extern "C" void MH3_NetSessionUseHostTimeout(CpuContext* cpu) {
    const uint32_t session = Arg(cpu, 3);
    GuestCall{cpu}.Call<SessionSetHostTimeout>({session}, {cpu->fpr[1].d});
    Write(session + sHostTimeout, Read(kDefaultHostTimeout));
    SetF(session + sHostSeen, float(Now(cpu)));
}
REGISTER_NATIVE_FUNCTION_AS(0x803D23B8, MH3_NetSessionUseHostTimeout, "NetSessionUseHostTimeout");

extern "C" void MH3_NetSessionUseRetries(CpuContext* cpu) {
    const uint32_t session = Arg(cpu, 3);
    GuestCall{cpu}.Call<SessionSetRetries>({session}, {cpu->fpr[1].d});
    Write(session + sRetryLimit, Read(kDefaultRetries));
}
REGISTER_NATIVE_FUNCTION_AS(0x803D240C, MH3_NetSessionUseRetries, "NetSessionUseRetries");

extern "C" void MH3_NetSessionUseRate(CpuContext* cpu) {
    const uint32_t session = Arg(cpu, 3);
    GuestCall{cpu}.Call<SessionSetRate>({session, Arg(cpu, 4), Arg(cpu, 5)});
    Write(session + sQuota, Read(kDefaultQuota));
    Write(session + sRate, Read(kDefaultRate));
}
REGISTER_NATIVE_FUNCTION_AS(0x803D2444, MH3_NetSessionUseRate, "NetSessionUseRate");

// 0x803D2484 / 0x803D24F8 / 0x803D256C (virtuals 0x60..0x68): every
// connection's keepalive, timeout and no-receive time (f1).
extern "C" void MH3_NetSessionSetKeepalive(CpuContext* cpu) {
    ForEachConnection(cpu, Arg(cpu, 3), cSetKeepalive, cpu->fpr[1].d);
}
REGISTER_NATIVE_FUNCTION_AS(0x803D2484, MH3_NetSessionSetKeepalive, "NetSessionSetKeepalive");

extern "C" void MH3_NetSessionSetTimeout(CpuContext* cpu) {
    ForEachConnection(cpu, Arg(cpu, 3), cSetTimeout, cpu->fpr[1].d);
}
REGISTER_NATIVE_FUNCTION_AS(0x803D24F8, MH3_NetSessionSetTimeout, "NetSessionSetTimeout");

extern "C" void MH3_NetSessionSetNoReceive(CpuContext* cpu) {
    ForEachConnection(cpu, Arg(cpu, 3), cSetNoReceive, cpu->fpr[1].d);
}
REGISTER_NATIVE_FUNCTION_AS(0x803D256C, MH3_NetSessionSetNoReceive, "NetSessionSetNoReceive");

extern "C" void MH3_NetSessionSelf(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(Self(Arg(cpu, 3)));
}
REGISTER_NATIVE_FUNCTION_AS(0x803D25E0, MH3_NetSessionSelf, "NetSessionSelf");

extern "C" void MH3_NetSessionSetHost(CpuContext* cpu) {
    Memory::Write8(Arg(cpu, 3) + sHost, uint8_t(Arg(cpu, 4)));
}
REGISTER_NATIVE_FUNCTION_AS(0x803D25F0, MH3_NetSessionSetHost, "NetSessionSetHost");

extern "C" void MH3_NetSessionSetRelayHost(CpuContext* cpu) {
    Memory::Write8(Arg(cpu, 3) + sRelayHost, uint8_t(Arg(cpu, 4)));
}
REGISTER_NATIVE_FUNCTION_AS(0x803D25FC, MH3_NetSessionSetRelayHost, "NetSessionSetRelayHost");

extern "C" void MH3_NetSessionClearRelayHost(CpuContext* cpu) {
    const uint32_t session = Arg(cpu, 3);
    Memory::Write8(session + sRelayHost, 0xFF);
    SetF(session + sRelaySeen, 0.0f);
}
REGISTER_NATIVE_FUNCTION_AS(0x803D2608, MH3_NetSessionClearRelayHost, "NetSessionClearRelayHost");

extern "C" void MH3_NetSessionRelayOf(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(RelayOf(cpu, Arg(cpu, 3), int8_t(Arg(cpu, 4))));
}
REGISTER_NATIVE_FUNCTION_AS(0x803D2620, MH3_NetSessionRelayOf, "NetSessionRelayOf");

extern "C" void MH3_NetSessionSetOption10(CpuContext* cpu) {
    Memory::Write8(Arg(cpu, 3) + sOption10, uint8_t(Arg(cpu, 4)));
}
REGISTER_NATIVE_FUNCTION_AS(0x803D26BC, MH3_NetSessionSetOption10, "NetSessionSetOption10");

extern "C" void MH3_NetSessionOption10(CpuContext* cpu) {
    cpu->gpr[3] = Memory::Read8(Arg(cpu, 3) + sOption10);
}
REGISTER_NATIVE_FUNCTION_AS(0x803D26C8, MH3_NetSessionOption10, "NetSessionOption10");

extern "C" void MH3_NetSessionSetOption11(CpuContext* cpu) {
    Memory::Write8(Arg(cpu, 3) + sOption11, uint8_t(Arg(cpu, 4)));
}
REGISTER_NATIVE_FUNCTION_AS(0x803D26D4, MH3_NetSessionSetOption11, "NetSessionSetOption11");

extern "C" void MH3_NetSessionOption11(CpuContext* cpu) {
    cpu->gpr[3] = Memory::Read8(Arg(cpu, 3) + sOption11);
}
REGISTER_NATIVE_FUNCTION_AS(0x803D26E0, MH3_NetSessionOption11, "NetSessionOption11");

extern "C" void MH3_NetSessionSetClosing(CpuContext* cpu) {
    Memory::Write8(Arg(cpu, 3) + sClosing, 1);
}
REGISTER_NATIVE_FUNCTION_AS(0x803D26EC, MH3_NetSessionSetClosing, "NetSessionSetClosing");

extern "C" void MH3_NetSessionPeerRtt(CpuContext* cpu) {
    cpu->fpr[1].d = PeerRtt(cpu, Arg(cpu, 3), int8_t(Arg(cpu, 4)));
}
REGISTER_NATIVE_FUNCTION_AS(0x803D26FC, MH3_NetSessionPeerRtt, "NetSessionPeerRtt");

extern "C" void MH3_NetSessionThroughput(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(Throughput(cpu, Arg(cpu, 3), int8_t(Arg(cpu, 4))));
}
REGISTER_NATIVE_FUNCTION_AS(0x803D27C4, MH3_NetSessionThroughput, "NetSessionThroughput");

// 0x803D28A4 / 0x803D2928 (virtuals 0x94, 0x98): a joined peer's link probe
// value (f1).
extern "C" void MH3_NetSessionSetProbe(CpuContext* cpu) {
    const uint32_t session = Arg(cpu, 3);
    const int32_t index = int8_t(Arg(cpu, 4));
    const double value = cpu->fpr[1].d;

    if (GuestCall{cpu}.Virtual(session, vJoined, {uint32_t(index)}))
        GuestCall{cpu}.Virtual(Connection(session, index), cSetProbe, {}, {value});
}
REGISTER_NATIVE_FUNCTION_AS(0x803D28A4, MH3_NetSessionSetProbe, "NetSessionSetProbe");

extern "C" void MH3_NetSessionProbe(CpuContext* cpu) {
    const uint32_t session = Arg(cpu, 3);
    const int32_t index = int8_t(Arg(cpu, 4));

    if (!GuestCall{cpu}.Virtual(session, vJoined, {uint32_t(index)})) {
        cpu->fpr[1].d = 0.0;
        return;
    }

    GuestCall{cpu}.Virtual(Connection(session, index), cProbe);
}
REGISTER_NATIVE_FUNCTION_AS(0x803D2928, MH3_NetSessionProbe, "NetSessionProbe");

extern "C" void MH3_NetSessionSendRoom(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(SendRoom(cpu, Arg(cpu, 3), int8_t(Arg(cpu, 4))));
}
REGISTER_NATIVE_FUNCTION_AS(0x803D29A4, MH3_NetSessionSendRoom, "NetSessionSendRoom");

extern "C" void MH3_NetSessionMarkReset(CpuContext* cpu) {
    MarkReset(Arg(cpu, 3), int8_t(Arg(cpu, 4)));
}
REGISTER_NATIVE_FUNCTION_AS(0x803D2A40, MH3_NetSessionMarkReset, "NetSessionMarkReset");

extern "C" void MH3_NetSessionPeerError(CpuContext* cpu) {
    PeerError(Arg(cpu, 3), int8_t(Arg(cpu, 4)), Arg(cpu, 5), Arg(cpu, 6), Arg(cpu, 7), int32_t(Arg(cpu, 8)));
}
REGISTER_NATIVE_FUNCTION_AS(0x803D2A84, MH3_NetSessionPeerError, "NetSessionPeerError");

extern "C" void MH3_NetSessionSendChannel(CpuContext* cpu) {
    cpu->gpr[3] = SendChannel(cpu, Arg(cpu, 3), int8_t(Arg(cpu, 4)), Arg(cpu, 5), Arg(cpu, 6) != 0);
}
REGISTER_NATIVE_FUNCTION_AS(0x803D2B00, MH3_NetSessionSendChannel, "NetSessionSendChannel");

extern "C" void MH3_NetSessionDrain(CpuContext* cpu) {
    Drain(cpu, Arg(cpu, 3), int8_t(Arg(cpu, 4)), Arg(cpu, 5) != 0);
}
REGISTER_NATIVE_FUNCTION_AS(0x803D2FA8, MH3_NetSessionDrain, "NetSessionDrain");

extern "C" void MH3_NetSessionDropData(CpuContext* cpu) {
    DropData(cpu, Arg(cpu, 3), int8_t(Arg(cpu, 4)));
}
REGISTER_NATIVE_FUNCTION_AS(0x803D315C, MH3_NetSessionDropData, "NetSessionDropData");

extern "C" void MH3_NetSessionExecute(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(Execute(cpu, Arg(cpu, 3), int8_t(Arg(cpu, 4)), Arg(cpu, 5)));
}
REGISTER_NATIVE_FUNCTION_AS(0x803D3220, MH3_NetSessionExecute, "NetSessionExecute");

extern "C" void MH3_NetSessionBroadcast(CpuContext* cpu) {
    Broadcast(cpu, Arg(cpu, 3), Arg(cpu, 4), int32_t(Arg(cpu, 5)), int32_t(Arg(cpu, 6)), Arg(cpu, 7), Arg(cpu, 8));
}
REGISTER_NATIVE_FUNCTION_AS(0x803D3A20, MH3_NetSessionBroadcast, "NetSessionBroadcast");

extern "C" void MH3_NetSessionSendKeepalive(CpuContext* cpu) {
    SendKeepalive(cpu, Arg(cpu, 3), uint8_t(Arg(cpu, 4)));
}
REGISTER_NATIVE_FUNCTION_AS(0x803D3EA0, MH3_NetSessionSendKeepalive, "NetSessionSendKeepalive");

extern "C" void MH3_NetSessionSendWho(CpuContext* cpu) {
    SendWho(cpu, Arg(cpu, 3), int8_t(Arg(cpu, 4)));
}
REGISTER_NATIVE_FUNCTION_AS(0x803D3F3C, MH3_NetSessionSendWho, "NetSessionSendWho");

extern "C" void MH3_NetSessionSendSeen(CpuContext* cpu) {
    SendSeen(cpu, Arg(cpu, 3), uint8_t(Arg(cpu, 4)), Arg(cpu, 5), Arg(cpu, 6), Arg(cpu, 7) & 0xFF, cpu->fpr[1].d);
}
REGISTER_NATIVE_FUNCTION_AS(0x803D400C, MH3_NetSessionSendSeen, "NetSessionSendSeen");

extern "C" void MH3_NetSessionSendReady(CpuContext* cpu) {
    SendReady(cpu, Arg(cpu, 3), Arg(cpu, 4) != 0, int8_t(Arg(cpu, 5)));
}
REGISTER_NATIVE_FUNCTION_AS(0x803D4130, MH3_NetSessionSendReady, "NetSessionSendReady");

extern "C" void MH3_NetSessionSendRelayData(CpuContext* cpu) {
    SendRelayData(cpu, Arg(cpu, 3), Arg(cpu, 4), Arg(cpu, 5), uint8_t(Arg(cpu, 6)));
}
REGISTER_NATIVE_FUNCTION_AS(0x803D4234, MH3_NetSessionSendRelayData, "NetSessionSendRelayData");

extern "C" void MH3_NetSessionTuneRto(CpuContext* cpu) {
    TuneRto(cpu, Arg(cpu, 3), int8_t(Arg(cpu, 4)));
}
REGISTER_NATIVE_FUNCTION_AS(0x803D42F4, MH3_NetSessionTuneRto, "NetSessionTuneRto");

extern "C" void MH3_NetSessionTuneDown(CpuContext* cpu) {
    TuneDown(cpu, Arg(cpu, 3), int8_t(Arg(cpu, 4)));
}
REGISTER_NATIVE_FUNCTION_AS(0x803D43B8, MH3_NetSessionTuneDown, "NetSessionTuneDown");

extern "C" void MH3_NetSessionTuneUp(CpuContext* cpu) {
    TuneUp(cpu, Arg(cpu, 3), int8_t(Arg(cpu, 4)));
}
REGISTER_NATIVE_FUNCTION_AS(0x803D451C, MH3_NetSessionTuneUp, "NetSessionTuneUp");

extern "C" void MH3_NetSessionSendSequences(CpuContext* cpu) {
    SendSequences(cpu, Arg(cpu, 3), int8_t(Arg(cpu, 4)));
}
REGISTER_NATIVE_FUNCTION_AS(0x803D46F4, MH3_NetSessionSendSequences, "NetSessionSendSequences");

extern "C" void MH3_NetSessionRoute(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(Route(Arg(cpu, 3), int8_t(Arg(cpu, 4))));
}
REGISTER_NATIVE_FUNCTION_AS(0x803D48BC, MH3_NetSessionRoute, "NetSessionRoute");
