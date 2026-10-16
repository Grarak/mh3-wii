// The game's reliable connection (Capcom's net library, RMHP08
// 0x803CA238..0x803CD0E0; decompiled with Ghidra, the call arguments and
// float math checked in the translated C++), ported natively:
// NetworkConnectionStable over a transport. The connect handshake (0x81 /
// 0x82 carrying the sequences and the local name, 0x83 to confirm),
// keepalive and timeouts, round trip pings (0x91 / 0x92) that set the resend
// time, the MTU probe (0x95), two channels of unit packets (0 reliable:
// resent until acknowledged, 1 unreliable), the receive path that unscrambles,
// checks and files them, and the closing handshake (0x84 close, 0x85 refuse).
// Every interval is in seconds of the platform's clock, not in steps.
//
// The packet layer (game_net_packet.cpp), the transports' virtuals and the
// caller's callback stay guest calls.
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
    Memset = 0x80004350,
    OperatorNew = 0x800404DC,
    OperatorDelete = 0x80040564,
    ConstructArray = 0x80457198,
    DestroyArray = 0x80457290,
    CvtFloatToUnsigned = 0x80457360,
    Rand = 0x8045E620,
    OSInitMutex = 0x804D2560,
};

// The packet layer (native in game_net_packet.cpp).
enum : uint32_t {
    UnitDestroy = 0x803CB960,
    ReaderDestroy = 0x803CB9BC,
    ReaderConstruct = 0x803CBA18,
    UnitConstruct = 0x803CBA54,
    WriterInit = 0x803F8A34,
    WriterBegin = 0x803F8A78,
    WriteBytes = 0x803F8CC0,
    WriteU32 = 0x803F8D2C,
    WriteU16 = 0x803F8DC0,
    WriteU8 = 0x803F8E54,
    SetLength = 0x803F8E8C,
    SetTimestamp = 0x803F8EEC,
    HasPacket = 0x803F9010,
    ReaderNext = 0x803F909C,
    ReadBytes = 0x803F947C,
    ReadU32 = 0x803F94F0,
    ReadU16 = 0x803F9590,
    ReadU8 = 0x803F9630,
    PacketSize = 0x803F966C,
    Timestamp = 0x803F9834,
    UnitInit = 0x803F98E8,
    UnitBegin = 0x803F9924,
    UnitAppend = 0x803F99D4,
    UnitLength16 = 0x803F9A68,
    UnitSkip = 0x803F9A74,
    UnitSetPayloadLength = 0x803F9AA4,
    UnitSetSequence = 0x803F9B08,
    UnitSetAck = 0x803F9B6C,
    UnitSetSender = 0x803F9BD0,
    UnitSetReceiver = 0x803F9C34,
    UnitSetFlagA = 0x803F9C98,
    UnitSetFlagB = 0x803F9CD4,
    UnitSetFlagC = 0x803F9D10,
    UnitSeal = 0x803F9D70,
    UnitAttach = 0x803F9E68,
    UnitHasPacket = 0x803F9EA4,
    UnitCopy = 0x803F9F20,
    UnitConsume = 0x803F9FD8,
    UnitPacket = 0x803FA0FC,
    UnitLength = 0x803FA14C,
    UnitHeaderSize = 0x803FA1C0,
    UnitFlagA = 0x803FA2F8,
    UnitFlagB = 0x803FA328,
    UnitFlagC = 0x803FA358,
    UnitEncode = 0x803FA3F4,
    UnitDecode = 0x803FA49C,
    UnitTest = 0x803FA514,
    PoolConstruct = 0x803FA658,
    PoolDestroy = 0x803FA6A0,
    PoolAppend = 0x803FA6FC,
    PoolLoadInto = 0x803FA7E0,
    PoolDrop = 0x803FA800,
    PoolReset = 0x803FA8E0,
    PoolSetSequence = 0x803FA8EC,
    PoolSequence = 0x803FA8F4,
    PoolPutTop = 0x803FA8FC,
    PoolPutAll = 0x803FAB60,
    PoolAck = 0x803FAC24,
};

constexpr uint32_t kPlatformPtr = 0x80797180;

// The defaults the setters change for every new connection (f32 seconds).
constexpr uint32_t kDefaultKeepalive = 0x80795DC0;
constexpr uint32_t kDefaultTimeout = 0x80795DC4;
constexpr uint32_t kDefaultNoReceive = 0x80795DC8;

// Constants (sda2, f32).
constexpr float kPingStart = 3.0f;   // 0x8079EB50: a ping's first round trip, seconds
constexpr float kRtoScale = 2.0f;    // 0x8079EB60: the resend time over the round trip
constexpr float kSecond = 1.0f;      // 0x8079EB64
constexpr float kMinute = 60.0f;     // 0x8079EB68
constexpr float kResendMark = -1.0f; // 0x8079EB6C: a timestamp times this, a resend
constexpr float kTinyRtt = 1e-4f;    // 0x8079EB70: a ping answered within its own clock tick
constexpr float kRttKeep = 0.9f;     // 0x8079EB80
constexpr float kRttTake = 0.1f;     // 0x8079EB84: and the shortest resend time
constexpr float kMilliseconds = 1000.0f;

// Vtables.
enum : uint32_t {
    kConnectionVtable = 0x805F9950,
    kStableVtable = 0x805F9C50,
    kTransportVtable = 0x805F9CA0,
    kLoopbackVtable = 0x805F9CD0,
    kNullVtable = 0x805F9D00,
    kMcsVtable = 0x805F9DA0,
    kMutexVtable = 0x805F99A0,
    kLockedVtable = 0x80603FD4,
};

// Errors.
constexpr uint32_t kErrorNoTransport = 0x80030001;
constexpr uint32_t kErrorTransport = 0x80030011;
constexpr uint32_t kErrorLoopbackFull = 0x80030021;
constexpr uint32_t kErrorPoolFull = 0x80030032;
constexpr uint32_t kErrorConnectTimeout = 0x80030033;
constexpr uint32_t kErrorNoReceive = 0x80030035;
constexpr uint32_t kNoValue = 0x80000000;

// Callback events.
enum : uint32_t {
    kEventConnected = 1,
    kEventError = 2,
    kEventData = 3,
    kEventClosed = 4,
    kEventRefused = 5,
    kEventShutdown = 6,
};

// The platform object's hooks.
enum : uint32_t {
    vInfo = 0xC,
    vError = 0x10,
    vWarn = 0x14,
    vSeconds = 0x60,
};

// Strings for the log hooks.
enum : uint32_t {
    kMsgConnectTimeout = 0x805F99B0,
    kMsgMinute = 0x805F99EC,
    kMsgNoReceive = 0x805F9A24,
    kMsgMtuChanged = 0x805F9A58,
    kMsgShutdown = 0x805F9A94,
    kMsgClose = 0x805F9AC0,
    kMsgCloseSend = 0x805F9AEC,
    kMsgRefuse = 0x805F9B24,
    kMsgRefuseSend = 0x805F9B50,
    kMsgPutFailed = 0x805F9B88,
    kMsgUnitTooLong = 0x805F9BC0,
    kMsgMtuDown = 0x805F9BF8,
    kMsgMtuFixed = 0x805F9C24,
};

// A connection: vtable, its callback and parameter, the transport, the
// first error (code and two values), the send scratch, two receive
// buffers, the batch buffer for raw sends, the four unit packet pools
// (send and receive per channel), then the stable state.
enum : uint32_t {
    cCallback = 0x4,
    cParam = 0x8,
    cTransport = 0xC,
    cError = 0x10,
    cScratch = 0x1C,
    cRecvA = 0x41C,
    cRecvB = 0x81C,
    cBatch = 0xC1C,
    cBatchLength = 0x101C,
    cBatchFull = 0x1020, // u8
    cSendPools = 0x1024, // channel 0, channel 1 (0x18 each)
    cRecvPools = 0x1054,
    cProbeFlag = 0x1084,     // u8
    cProbeValue = 0x1088,    // f32: what 0x803CC068 keeps
    cResendNow = 0x108C,     // u8: send a reliable unit packet (an acknowledgement) even when empty
    cLastSend = 0x1090,      // f32 per channel
    cRto = 0x1098,           // f32: the resend time
    cSendInterval = 0x109C,  // f32: the shortest time between two sends of a channel
    cBatchLimit = 0x10A0,
    cPoolBuffers = 0x10A4,   // 4 x 0x400
    cId = 0x20A4,            // s8
    cName = 0x20A5,          // 0x80 bytes
    cNameLength = 0x2128,
    cPeerName = 0x212C,      // 0x80 bytes
    cPeerNameLength = 0x21AC,
    cState = 0x21B0,
    cNow = 0x21B4,           // f32: the platform's clock at this update
    cStarted = 0x21B8,
    cKeepaliveAt = 0x21BC,
    cKeepalive = 0x21C0,
    cConnectAt = 0x21C4,
    cConnectTimeout = 0x21C8,
    cHelloAt = 0x21CC,
    cPingIndex = 0x21D0,     // u8
    cPings = 0x21D4,         // 8 f32: each ping's round trip, 0 while unanswered
    cRtt = 0x21F4,
    cPingsLost = 0x21F8,
    cPingsAnswered = 0x21FC,
    cPingAt = 0x2200,
    cMinuteAt = 0x2204,
    cSent = 0x2208,
    cReceived = 0x220C,
    cSentSecond = 0x2210,
    cReceivedSecond = 0x2214,
    cSentMinute = 0x2218,
    cReceivedMinute = 0x221C,
    cSends = 0x2220,
    cSendsSecond = 0x2224,
    cSendsMinute = 0x2228,
    cSentTotal = 0x222C,
    cReceivedTotal = 0x2230,
    cLastReceive = 0x2234,
    cNoReceive = 0x2238,
    cReceivedSince = 0x223C, // u8
    cNoReceiveCheck = 0x223D,
    cMtuProbeDue = 0x223E,
    cMtuAnswered = 0x223F,
    cMtuLargest = 0x2240,
    cMtu = 0x2244,
    cMtuFixed = 0x2248, // u8
    cMtuTries = 0x224C,
};

constexpr uint32_t kChannelPool = 0x18;
constexpr uint32_t kUnitMtuDefault = 0x200;
constexpr uint32_t kBuffer = 0x400;

// The states.
enum : uint32_t {
    kIdle = 0,
    kConnecting = 1,
    kHello = 2,
    kConfirming = 3,
    kEstablished = 4,
    kClosing = 5,
    kClosed = 6,
};

// Transport virtuals.
enum : uint32_t {
    tDestroy = 0x8,
    tOpen = 0xC,
    tSend = 0x10,
    tReceive = 0x14,
    tPoll = 0x1C,
    tKeepalive = 0x20,
    tClosed = 0x24,
    tClose = 0x28,
};

// A transport: vtable, then its first error (code and two values).
constexpr uint32_t trError = 0x4;

// Connection virtuals (NetworkConnectionStable's vtable 0x805F9C50).
enum : uint32_t {
    vDestroy = 0x8,
    vOpen = 0xC,
    vCloseTransport = 0x10,
    vReset = 0x14,
    vUpdate = 0x18,
};

// Control packet tags.
enum : uint32_t {
    kTagHello = 0x81,
    kTagHelloAnswer = 0x82,
    kTagConfirm = 0x83,
    kTagClose = 0x84,
    kTagRefuse = 0x85,
    kTagPing = 0x91,
    kTagPong = 0x92,
    kTagMtu = 0x95,
};

constexpr uint32_t kCrFloatArgs = 1u << (31 - 6);

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

uint32_t Platform() {
    return Read(kPlatformPtr);
}

int32_t Id(uint32_t connection) {
    return int8_t(Memory::Read8(connection + cId));
}

uint32_t State(uint32_t connection) {
    return Read(connection + cState);
}

void SetState(uint32_t connection, uint32_t state) {
    Write(connection + cState, state);
}

// PPC fctiwz: toward zero, saturating, NaN as INT_MIN.
int32_t Fctiwz(double value) {
    if (std::isnan(value) || value <= -2147483648.0)
        return INT32_MIN;

    if (value >= 2147483647.0)
        return INT32_MAX;

    return int32_t(value);
}

// The platform's clock in seconds (its single-precision answer).
float Seconds(CpuContext* cpu) {
    GuestCall{cpu}.Virtual(Platform(), vSeconds, {});
    return float(cpu->fpr[1].d);
}

// The platform's log hooks (variadic): info (level, message, ...), error and
// warning (message, ...).
void LogInfo(CpuContext* cpu, uint32_t level, uint32_t message, std::initializer_list<uint32_t> values) {
    uint32_t args[8] = {level, message};
    uint32_t n = 2;

    for (uint32_t v : values)
        args[n++] = v;

    cpu->cr &= ~kCrFloatArgs;
    const uint32_t platform = Platform();
    const uint32_t fn = Read(Read(platform) + vInfo);
    cpu->gpr[3] = platform;

    for (uint32_t i = 0; i < n; ++i)
        cpu->gpr[4 + i] = args[i];

    InvokeIndirectCpu(fn, cpu);
}

void Log(CpuContext* cpu, uint32_t slot, uint32_t message, uint32_t a = 0, uint32_t b = 0) {
    cpu->cr &= ~kCrFloatArgs;
    GuestCall{cpu}.Virtual(Platform(), slot, {message, a, b});
}

// 0x803CC0A4: keeps the first error.
void SetError(uint32_t connection, uint32_t code, uint32_t a, uint32_t b) {
    if (Read(connection + cError))
        return;

    Write(connection + cError, code);
    Write(connection + cError + 4, a);
    Write(connection + cError + 8, b);
}

// 0x803CC078: the unit packet size: the probed MTU once fixed, else 0x200.
uint32_t Mtu(uint32_t connection) {
    return Memory::Read8(connection + cMtuFixed) ? Read(connection + cMtu) : kUnitMtuDefault;
}

// 0x803CC0C0: channel 0 or 1 (else an error).
bool BadChannel(uint32_t connection, int32_t channel) {
    if (channel >= 0 && channel <= 1)
        return false;

    SetError(connection, kErrorNoTransport, uint32_t(channel), 0);
    return true;
}

// The caller's callback: (event, id, a, b, data, parameter).
void Notify(CpuContext* cpu, uint32_t connection, uint32_t event, uint32_t a, uint32_t b, uint32_t data) {
    GuestCall{cpu}(Read(connection + cCallback), {event, uint32_t(Id(connection)), a, b, data, Read(connection + cParam)});
}

// The error to the callback, then the connection resets.
void FailAndClose(CpuContext* cpu, uint32_t connection) {
    Notify(cpu, connection, kEventError, Read(connection + cError), 1, connection + cError);
    GuestCall{cpu}.Virtual(connection, vReset, {});
}

// 0x803CCF44 / 0x803CCF68: takes over the transport's error, if it has one.
void TakeTransportError(CpuContext* cpu, uint32_t connection) {
    const uint32_t transport = Read(connection + cTransport);

    if (!transport) {
        SetError(connection, kErrorNoTransport, 0xFF, kNoValue);
        return;
    }

    if (const uint32_t code = Read(transport + trError)) {
        SetError(connection, code, Read(transport + trError + 4), Read(transport + trError + 8));
        GuestCall{cpu}.Call<Memset>({transport + trError, 0, 0xC});
    }
}

// 0x803CCE08: the resend time, twice the round trip (or twice the send
// interval if longer), between 0.1 and 1 second.
void UpdateRto(uint32_t connection) {
    const float rtt = kRtoScale * F(connection + cRtt);
    const float interval = kRtoScale * F(connection + cSendInterval);
    const float rto = std::max(rtt, interval);
    SetF(connection + cRto, std::min(std::max(rto, kRttTake), kSecond));
}

// A packet object (reader, writer or unit packet) on the guest stack.
struct Stacked {
    GuestFrame frame;
    uint32_t object;

    explicit Stacked(CpuContext* cpu) : frame(cpu, 0x60), object(frame[0x10]) {}
};

// A control packet: a writer over the send scratch, the tag and its fields,
// filed into a channel's send pool (0x803CB480).
struct Control {
    CpuContext* cpu;
    GuestCall game;
    Stacked writer;
    uint32_t length = 0;

    Control(CpuContext* cpu_, uint32_t connection, uint32_t tag) : cpu(cpu_), game{cpu_}, writer(cpu_) {
        game.Call<ReaderConstruct>({writer.object});
        game.Call<WriterInit>({writer.object, connection + cScratch, kBuffer});
        game.Call<WriterBegin>({writer.object, 0});
        U8(tag);
    }

    void U8(uint32_t value) {
        length += int16_t(game.Call<WriteU8>({writer.object, value & 0xFF}));
    }

    void U16(uint32_t value) {
        length += int16_t(game.Call<WriteU16>({writer.object, value & 0xFFFF}));
    }

    void U32(uint32_t value) {
        length += int16_t(game.Call<WriteU32>({writer.object, value}));
    }

    void Bytes(uint32_t from, uint32_t count) {
        length += int16_t(game.Call<WriteBytes>({writer.object, from, count}));
    }
};

void Put(CpuContext* cpu, uint32_t connection, uint32_t packet, int32_t channel);

void Send(Control& control, uint32_t connection, int32_t channel) {
    control.game.Call<SetLength>({control.writer.object, control.length & 0xFFFF});
    Put(control.cpu, connection, control.writer.object, channel);
    control.game.Call<ReaderDestroy>({control.writer.object, 0xFFFFFFFF});
}

// 0x803CB480: a packet into a channel's send pool.
void Put(CpuContext* cpu, uint32_t connection, uint32_t packet, int32_t channel) {
    if (BadChannel(connection, channel))
        return;

    GuestCall game{cpu};
    game.Call<SetTimestamp>({packet}, {0.0});
    const int32_t put = int32_t(game.Call<PoolAppend>({connection + cSendPools + uint32_t(channel) * kChannelPool, packet, 0xFF}));

    if (put < 0) {
        Log(cpu, vWarn, kMsgPutFailed, uint32_t(int32_t(int8_t(Memory::Read8(connection + cId)))), uint32_t(put));
        SetError(connection, kErrorPoolFull, uint32_t(put), 0);
    }
}

// 0x803CC10C: the hello (0x81) or its answer (0x82): both send sequences,
// the name's length and the name.
void SendHello(CpuContext* cpu, uint32_t connection, bool answer) {
    Control control(cpu, connection, answer ? kTagHelloAnswer : kTagHello);
    control.U16(control.game.Call<PoolSequence>({connection + cSendPools}));
    control.U16(control.game.Call<PoolSequence>({connection + cSendPools + kChannelPool}));
    control.U32(Read(connection + cNameLength));
    control.Bytes(connection + cName, Read(connection + cNameLength));
    Send(control, connection, 1);
}

// 0x803CC224: the confirmation (0x83), reliable.
void SendConfirm(CpuContext* cpu, uint32_t connection) {
    Control control(cpu, connection, kTagConfirm);
    Send(control, connection, 0);
}

// 0x803CC2A4: ping `index` (0x91) with the time in milliseconds.
void SendPing(CpuContext* cpu, uint32_t connection, uint32_t index) {
    GuestCall game{cpu};
    const uint32_t ms = game.Call<CvtFloatToUnsigned>({}, {double(kMilliseconds * F(connection + cNow))});
    SetF(connection + cPings + (index & 7) * 4, 0.0f);
    Control control(cpu, connection, kTagPing);
    control.U8(index & 7);
    control.U32(ms);
    Send(control, connection, 1);
}

// 0x803CC3AC: the answer (0x92) to a ping, its index and time.
void SendPong(CpuContext* cpu, uint32_t connection, uint32_t index, uint32_t ms) {
    Control control(cpu, connection, kTagPong);
    control.U8(index & 7);
    control.U32(ms);
    Send(control, connection, 1);
}

// 0x803CC494: the close (0x84) or refusal (0x85), unreliable.
void SendClose(CpuContext* cpu, uint32_t connection, bool refuse) {
    Control control(cpu, connection, refuse ? kTagRefuse : kTagClose);
    Send(control, connection, 1);
}

// 0x803CC784: the largest unit packet that arrived (0x95).
void SendMtu(CpuContext* cpu, uint32_t connection, uint32_t mtu) {
    Control control(cpu, connection, kTagMtu);
    control.U32(mtu);
    Send(control, connection, 1);
}

// 0x803CC530: when due, a unit packet the size of the MTU being tried,
// flagged as a probe, sent straight through the transport.
void SendMtuProbe(CpuContext* cpu, uint32_t connection) {
    if (!Memory::Read8(connection + cMtuProbeDue))
        return;

    Memory::Write8(connection + cMtuProbeDue, 0);
    GuestCall game{cpu};
    Stacked unit(cpu);
    const uint32_t mtu = Read(connection + cMtu);
    game.Call<UnitConstruct>({unit.object});
    game.Call<UnitInit>({unit.object, connection + cScratch, mtu});
    game.Call<UnitBegin>({unit.object});
    game.Call<UnitSkip>({unit.object, (mtu - (game.Call<UnitHeaderSize>() & 0xFFFF)) & 0xFFFF});
    game.Call<UnitSetPayloadLength>({unit.object, (mtu - (game.Call<UnitHeaderSize>() & 0xFFFF)) & 0xFFFF});
    game.Call<UnitSetSequence>({unit.object, game.Call<PoolSequence>({connection + cSendPools + kChannelPool})});
    game.Call<UnitSetAck>({unit.object, game.Call<PoolSequence>({connection + cRecvPools + kChannelPool})});
    game.Call<UnitSetSender>({unit.object, 0});
    game.Call<UnitSetReceiver>({unit.object, 0});
    game.Call<UnitSetFlagA>({unit.object, 0});
    game.Call<UnitSetFlagB>({unit.object, 1});
    game.Call<UnitSetFlagC>({unit.object, 1});
    game.Call<UnitSeal>({unit.object});
    game.Call<UnitEncode>({unit.object});
    const uint32_t length = game.Call<UnitLength16>({unit.object}) & 0xFFFF;
    game.Virtual(Read(connection + cTransport), tSend, {connection + cScratch, length, 0, 0, 0});
    game.Call<UnitDestroy>({unit.object, 0xFFFFFFFF});
}

// 0x803CC694: once a second until fixed: below 0x200 bytes the MTU is fixed
// at 0x200; after eight unanswered probes it drops by 0x80; then another
// probe is due.
void MtuStep(CpuContext* cpu, uint32_t connection) {
    if (Memory::Read8(connection + cMtuFixed))
        return;

    if (ReadInt(connection + cMtuTries) < 1) {
        if (Read(connection + cMtu) > kUnitMtuDefault) {
            Write(connection + cMtu, Read(connection + cMtu) - 0x80);
            Write(connection + cMtuTries, 8);
            LogInfo(cpu, 1, kMsgMtuDown, {uint32_t(Id(connection)), Read(connection + cMtu)});
        }

        if (Read(connection + cMtu) <= kUnitMtuDefault) {
            Write(connection + cMtu, kUnitMtuDefault);
            Memory::Write8(connection + cMtuFixed, 1);
            LogInfo(cpu, 1, kMsgMtuFixed, {uint32_t(Id(connection)), Read(connection + cMtu)});
            return;
        }
    }

    Write(connection + cMtuTries, Read(connection + cMtuTries) - 1);
    Memory::Write8(connection + cMtuProbeDue, 1);
}

// 0x803CCD40: ping `index`'s round trip into the smoothed one (an
// unanswered ping counts as 3 seconds), then the resend time.
void PingResult(uint32_t connection, uint32_t index) {
    const uint32_t ping = connection + cPings + (index & 7) * 4;
    float rtt = F(ping);

    if (rtt <= 0.0f || kPingStart < rtt)
        rtt = kPingStart;

    SetF(connection + cRtt, kRttKeep * F(connection + cRtt) + kRttTake * rtt);
    UpdateRto(connection);

    if (F(ping) == 0.0f)
        Write(connection + cPingsLost, Read(connection + cPingsLost) + 1);
    else
        Write(connection + cPingsAnswered, Read(connection + cPingsAnswered) + 1);
}

// 0x803CB544: sends a channel's waiting packets in one unit packet: those
// whose time is up (never sent, marked for resend, or older than the resend
// time) up to the MTU, at most once per send interval. The reliable channel
// keeps them until they are acknowledged; the unreliable one drops them.
void Flush(CpuContext* cpu, uint32_t connection, int32_t channel) {
    GuestCall game{cpu};
    Stacked unit(cpu);
    Stacked reader(cpu);
    game.Call<UnitConstruct>({unit.object});
    game.Call<ReaderConstruct>({reader.object});

    auto done = [&] {
        game.Call<ReaderDestroy>({reader.object, 0xFFFFFFFF});
        game.Call<UnitDestroy>({unit.object, 0xFFFFFFFF});
    };

    if (BadChannel(connection, channel)) {
        done();
        return;
    }

    if (channel == 1)
        SendMtuProbe(cpu, connection);

    const float now = F(connection + cNow);

    if (!(F(connection + cSendInterval) + F(connection + cLastSend + uint32_t(channel) * 4) < now)) {
        done();
        return;
    }

    const uint32_t pool = connection + cSendPools + uint32_t(channel) * kChannelPool;
    game.Call<UnitInit>({unit.object, connection + cScratch, Mtu(connection)});
    game.Call<UnitBegin>({unit.object});
    game.Call<PoolLoadInto>({pool, reader.object});
    uint32_t sequence = game.Call<PoolSequence>({pool});

    auto stamp = [&] {
        game.Call<Timestamp>({reader.object});
        return float(cpu->fpr[1].d);
    };

    auto due = [&](float t) {
        return 0.0f <= t || kResendMark * t <= now;
    };

    // Packets already sent and still within the resend time keep their place.
    while (game.Call<HasPacket>({reader.object})) {
        const float t = stamp();

        if (!(due(t) && now < F(connection + cRto) + t))
            break;

        sequence += game.Call<PacketSize>({reader.object, 0x80});
        game.Call<ReaderNext>({reader.object});
    }

    uint32_t added = 0;

    while (game.Call<HasPacket>({reader.object}) && due(stamp())) {
        const uint32_t size = game.Call<PacketSize>({reader.object, 0x80}) & 0xFFFF;
        const uint32_t header = game.Call<UnitHeaderSize>() & 0xFFFF;

        if (header + size > kUnitMtuDefault) {
            const uint32_t whole = game.Call<PacketSize>({reader.object, 0xFF}) & 0xFFFF;
            Log(cpu, vError, kMsgUnitTooLong, uint32_t(Id(connection)), (game.Call<UnitHeaderSize>() & 0xFFFF) + whole);
        }

        const int32_t appended = int32_t(game.Call<UnitAppend>({unit.object, reader.object, 0x80}));

        if (appended < 0)
            break;

        added += uint32_t(appended);
        game.Call<SetTimestamp>({reader.object}, {double(now)});
        game.Call<ReaderNext>({reader.object});
    }

    if (channel == 0 && Memory::Read8(connection + cResendNow)) {
        Memory::Write8(connection + cResendNow, 0);
    } else if (!added) {
        done();
        return;
    }

    game.Call<UnitSetPayloadLength>({unit.object, added & 0xFFFF});
    game.Call<UnitSetSequence>({unit.object, sequence & 0xFFFF});
    game.Call<UnitSetAck>({unit.object, game.Call<PoolSequence>({connection + cRecvPools + uint32_t(channel) * kChannelPool})});
    game.Call<UnitSetSender>({unit.object, uint32_t(Id(connection))});
    game.Call<UnitSetReceiver>({unit.object, uint32_t(Id(connection))});
    game.Call<UnitSetFlagA>({unit.object, 0});
    game.Call<UnitSetFlagB>({unit.object, uint32_t(channel) & 0xFF});
    game.Call<UnitSeal>({unit.object});
    game.Call<UnitEncode>({unit.object});
    const uint32_t length = game.Call<UnitLength16>({unit.object}) & 0xFFFF;
    const int32_t sent = int32_t(game.Virtual(Read(connection + cTransport), tSend, {connection + cScratch, length, 0, 0, 0}));

    if (sent >= 0) {
        Write(connection + cSent, Read(connection + cSent) + uint32_t(sent));
        Write(connection + cSentTotal, Read(connection + cSentTotal) + uint32_t(sent));
        Write(connection + cSends, Read(connection + cSends) + 1);

        if (channel == 1 && int32_t(added) > 0) {
            game.Call<PoolDrop>({connection + cSendPools + kChannelPool, added, 0x80});
            game.Call<PoolSetSequence>({connection + cSendPools + kChannelPool, (sequence + added) & 0xFFFF});
        }

        Write(connection + cLastSend + uint32_t(channel) * 4, Read(connection + cNow));
    }

    done();
}

// 0x803CBC9C: receives everything the transport has: unit packets are
// unscrambled and checked, then filed: data on channel 0 or 1 (acknowledging
// what the peer received), an MTU probe noted, a raw packet to the callback.
void Receive(CpuContext* cpu, uint32_t connection) {
    GuestCall game{cpu};
    GuestFrame frame(cpu, 0x80);
    const uint32_t unit = frame[0x10];
    const uint32_t lengthA = frame[0x50];
    const uint32_t dataA = frame[0x54];
    const uint32_t lengthB = frame[0x58];
    const uint32_t dataB = frame[0x5C];
    const uint32_t raw = frame[0x60];
    game.Call<UnitConstruct>({unit});
    int32_t got;

    do {
        Write(lengthA, kBuffer);
        Write(lengthB, kBuffer);
        Write(dataA, connection + cRecvA);
        Write(dataB, connection + cRecvB);
        got = int32_t(game.Virtual(Read(connection + cTransport), tReceive, {connection + cRecvA, lengthA, connection + cRecvB, lengthB, raw}));

        if (got == 0)
            break;

        if (got < 0) {
            game.Call<UnitDestroy>({unit, 0xFFFFFFFF});
            return;
        }

        Write(connection + cReceived, Read(connection + cReceived) + uint32_t(got));
        Write(connection + cReceivedTotal, Read(connection + cReceivedTotal) + uint32_t(got));
        game.Call<UnitAttach>({unit, Read(dataA), Read(lengthA)});

        while (game.Call<UnitHasPacket>({unit})) {
            game.Call<UnitDecode>({unit});
            const int32_t test = int32_t(game.Call<UnitTest>({unit}));

            if (test < 0) {
                SetError(connection, uint32_t(test), 0, kNoValue);
                game.Call<UnitDestroy>({unit, 0xFFFFFFFF});
                return;
            }

            if (test) {
                const int8_t kind = int8_t(game.Call<UnitFlagA>({unit, uint32_t(test)}));
                const int8_t channel = int8_t(game.Call<UnitFlagB>({unit}));
                uint32_t filed = 0;

                if (kind == 0) {
                    if (!game.Call<UnitFlagC>({unit})) {
                        if (channel == 0) {
                            filed = game.Call<PoolPutTop>({connection + cRecvPools, unit});
                            game.Call<PoolAck>({connection + cSendPools, unit});
                        } else if (channel == 1) {
                            filed = game.Call<PoolPutAll>({connection + cRecvPools + kChannelPool, unit});

                            if (int32_t(filed) < 0)
                                filed = 0;
                        }
                    } else {
                        Memory::Write8(connection + cMtuAnswered, 1);
                        const uint32_t length = game.Call<UnitLength>({unit}) & 0xFFFF;

                        if (Read(connection + cMtuLargest) < length)
                            Write(connection + cMtuLargest, length);
                    }
                } else if (kind == 1) {
                    Notify(cpu, connection, kEventData, 0, 1, lengthA);
                }

                if (int32_t(filed) < 4 && filed > 2) {
                    if (filed != 3) {
                        SetError(connection, filed, 0, kNoValue);
                        game.Call<UnitDestroy>({unit, 0xFFFFFFFF});
                        return;
                    }

                    Memory::Write8(connection + cResendNow, 1);
                }
            }

            game.Call<UnitConsume>({unit});
            Write(connection + cLastReceive, Read(connection + cNow));
        }
    } while (got > 0);

    game.Call<UnitDestroy>({unit, 0xFFFFFFFF});
}

// 0x803CC830: the control packets that arrived on the unreliable channel,
// then the confirmations on the reliable one.
void HandleControl(CpuContext* cpu, uint32_t connection) {
    GuestCall game{cpu};
    GuestFrame frame(cpu, 0x80);
    const uint32_t reader = frame[0x10];
    const uint32_t tag = frame[0x40];
    const uint32_t value16 = frame[0x44];
    const uint32_t value8 = frame[0x48];
    const uint32_t value32 = frame[0x4C];
    game.Call<ReaderConstruct>({reader});
    Memory::Write8(tag, 0);
    game.Call<PoolLoadInto>({connection + cRecvPools + kChannelPool, reader});

    auto hello = [&] {
        if (State(connection) != kHello)
            return;

        game.Call<ReadU16>({reader, value16});
        game.Call<PoolSetSequence>({connection + cRecvPools, Memory::Read16(value16)});
        game.Call<ReadU16>({reader, value16});
        game.Call<PoolSetSequence>({connection + cRecvPools + kChannelPool, Memory::Read16(value16)});
        game.Call<ReadU32>({reader, connection + cPeerNameLength});
        Write(connection + cPeerNameLength, uint32_t(std::min<int32_t>(ReadInt(connection + cPeerNameLength), 0x80)));
        game.Call<ReadBytes>({reader, connection + cPeerName, Read(connection + cPeerNameLength)});
        SetState(connection, kConfirming);
    };

    auto finish = [&] {
        game.Call<ReaderDestroy>({reader, 0xFFFFFFFF});
    };

    while (game.Call<HasPacket>({reader})) {
        game.Call<ReadU8>({reader, tag});
        const uint32_t kind = Memory::Read8(tag);

        if (kind == kTagHello) {
            hello();

            if (State(connection) == kConfirming)
                SendHello(cpu, connection, true);
        } else if (kind == kTagHelloAnswer) {
            hello();

            if (State(connection) == kConfirming) {
                SendConfirm(cpu, connection);
                SetState(connection, kEstablished);
                Notify(cpu, connection, kEventConnected, 0, Read(connection + cPeerNameLength), connection + cPeerName);
            }
        } else if (kind == kTagPing) {
            if (State(connection) == kEstablished || State(connection) == kConfirming) {
                Memory::Write8(value8, 0);
                Write(value32, 0);
                game.Call<ReadU8>({reader, value8});
                game.Call<ReadU32>({reader, value32});
                SendPong(cpu, connection, Memory::Read8(value8), Read(value32));
            }
        } else if (kind == kTagPong) {
            if (State(connection) == kEstablished) {
                Memory::Write8(value8, 0);
                Write(value32, 0);
                game.Call<ReadU8>({reader, value8});
                game.Call<ReadU32>({reader, value32});
                const uint32_t index = Memory::Read8(value8) & 7;
                const float sent = float(double(float(double(Read(value32)))) / double(kMilliseconds));
                const float now = F(connection + cNow);

                if (sent <= now) {
                    const uint32_t ping = connection + cPings + index * 4;
                    SetF(ping, now - sent);

                    if (F(ping) == 0.0f)
                        SetF(ping, kTinyRtt);
                }
            }
        } else if (kind == kTagMtu) {
            if (State(connection) == kEstablished && !Memory::Read8(connection + cMtuFixed)) {
                Write(value32, 0);
                game.Call<ReadU32>({reader, value32});
                Write(connection + cMtu, Read(value32));
                Memory::Write8(connection + cMtuFixed, 1);
                LogInfo(cpu, 1, kMsgMtuFixed, {uint32_t(Id(connection)), Read(connection + cMtu)});
            }
        } else if (kind == kTagClose || kind == kTagRefuse) {
            SetState(connection, kIdle);
            Notify(cpu, connection, kind == kTagClose ? kEventClosed : kEventRefused, 0, 0, 0);
            finish();
            return;
        }

        game.Call<ReaderNext>({reader});
    }

    game.Call<PoolReset>({connection + cRecvPools + kChannelPool});
    game.Call<PoolLoadInto>({connection + cRecvPools, reader});

    while (game.Call<HasPacket>({reader})) {
        game.Call<ReadU8>({reader, tag});

        if (Memory::Read8(tag) == kTagConfirm && State(connection) == kConfirming) {
            SetState(connection, kEstablished);
            Notify(cpu, connection, kEventConnected, 0, Read(connection + cPeerNameLength), connection + cPeerName);
        }

        game.Call<ReaderNext>({reader});
    }

    game.Call<PoolReset>({connection + cRecvPools});
    finish();
}

// 0x803CAA70: a step of the connection.
void Update(CpuContext* cpu, uint32_t connection) {
    GuestCall game{cpu};
    const float before = F(connection + cNow);
    const float elapsed = Seconds(cpu) - before;
    SetF(connection + cNow, Seconds(cpu));
    const float now = F(connection + cNow);
    const uint32_t state = State(connection);
    const uint32_t transport = Read(connection + cTransport);

    if (state == kHello || state == kConfirming) {
        TakeTransportError(cpu, connection);

        if (Read(connection + cError)) {
            FailAndClose(cpu, connection);
            return;
        }

        if (now <= F(connection + cConnectAt) + F(connection + cConnectTimeout)) {
            if (kSecond + F(connection + cHelloAt) <= now) {
                SetF(connection + cHelloAt, now);
                SendHello(cpu, connection, false);
            }

            Flush(cpu, connection, 0);
            Flush(cpu, connection, 1);
            Receive(cpu, connection);
            HandleControl(cpu, connection);
        } else {
            const uint32_t timeout = uint32_t(Fctiwz(F(connection + cConnectTimeout)));
            Log(cpu, vWarn, kMsgConnectTimeout, uint32_t(Id(connection)), timeout);
            SetError(connection, kErrorConnectTimeout, timeout, kNoValue);
        }

        return;
    }

    if (state == kConnecting) {
        if (!transport) {
            SetError(connection, kErrorNoTransport, 0xFF, kNoValue);
            FailAndClose(cpu, connection);
            return;
        }

        if (F(connection + cKeepaliveAt) + F(connection + cKeepalive) < now) {
            Write(connection + cKeepaliveAt, Read(connection + cNow));
            game.Virtual(transport, tKeepalive, {});
        }

        const int32_t polled = int32_t(game.Virtual(transport, tPoll, {}));

        if (polled < 0) {
            if (Read(transport + trError)) {
                SetError(connection, Read(transport + trError), Read(transport + trError + 4), Read(transport + trError + 8));
                game.Call<Memset>({transport + trError, 0, 0xC});
            } else {
                SetError(connection, kErrorTransport, 0, uint32_t(polled));
            }

            FailAndClose(cpu, connection);
        } else if (polled > 0) {
            SetState(connection, kHello);
            Write(connection + cConnectAt, Read(connection + cNow));
            SetF(connection + cHelloAt, 0.0f);
        }

        return;
    }

    if (state == kEstablished) {
        TakeTransportError(cpu, connection);

        if (Read(connection + cError)) {
            FailAndClose(cpu, connection);
            return;
        }

        if (kSecond + F(connection + cPingAt) <= now) {
            Write(connection + cPingAt, Read(connection + cNow));
            const uint32_t index = Memory::Read8(connection + cPingIndex);
            PingResult(connection, index);
            SendPing(cpu, connection, index);
            Memory::Write8(connection + cPingIndex, uint8_t((index + 1) & 7));
            MtuStep(cpu, connection);
            Write(connection + cSentSecond, Read(connection + cSent));
            Write(connection + cReceivedSecond, Read(connection + cReceived));
            Write(connection + cSentMinute, Read(connection + cSentMinute) + Read(connection + cSent));
            Write(connection + cReceivedMinute, Read(connection + cReceivedMinute) + Read(connection + cReceived));
            Write(connection + cSent, 0);
            Write(connection + cReceived, 0);
            Write(connection + cSendsSecond, Read(connection + cSends));
            Write(connection + cSendsMinute, Read(connection + cSendsMinute) + Read(connection + cSends));
            Write(connection + cSends, 0);
        }

        if (kMinute + F(connection + cMinuteAt) <= now) {
            Write(connection + cMinuteAt, Read(connection + cNow));
            LogInfo(cpu, 1, kMsgMinute, {uint32_t(Id(connection)), Read(connection + cSentMinute), Read(connection + cSendsMinute), Read(connection + cReceivedMinute)});
            Write(connection + cSentMinute, 0);
            Write(connection + cReceivedMinute, 0);
            Write(connection + cSendsMinute, 0);
        }

        if (Memory::Read8(connection + cNoReceiveCheck)) {
            if (Memory::Read8(connection + cReceivedSince)) {
                Memory::Write8(connection + cReceivedSince, 0);
                SetF(connection + cLastReceive, float(double(F(connection + cLastReceive)) + double(elapsed)));
            }

            if (F(connection + cLastReceive) + F(connection + cNoReceive) <= now) {
                const uint32_t limit = uint32_t(Fctiwz(F(connection + cNoReceive)));
                Log(cpu, vWarn, kMsgNoReceive, uint32_t(Id(connection)), limit);
                SetError(connection, kErrorNoReceive, limit, 0);
                return;
            }
        }

        Flush(cpu, connection, 0);
        Flush(cpu, connection, 1);
        Receive(cpu, connection);

        if (Memory::Read8(connection + cMtuAnswered)) {
            Memory::Write8(connection + cMtuAnswered, 0);
            SendMtu(cpu, connection, Read(connection + cMtuLargest));
            LogInfo(cpu, 3, kMsgMtuChanged, {uint32_t(Id(connection)), Read(connection + cMtuLargest)});
        }

        HandleControl(cpu, connection);
        return;
    }

    if (state == kClosing) {
        if (transport && !game.Virtual(transport, tClosed, {}))
            return;

        SetState(connection, kClosed);
    } else if (state != kClosed) {
        return;
    }

    Notify(cpu, connection, kEventShutdown, 0, 0, 0);
    game.Virtual(connection, vReset, {});
    LogInfo(cpu, 3, kMsgShutdown, {uint32_t(Id(connection))});
}

// 0x803CA5D4 / 0x803CA54C: the four unit packet pools (two arrays of two).
void PoolsConstruct(CpuContext* cpu, uint32_t pools) {
    GuestCall game{cpu};
    game.Call<ConstructArray>({pools, PoolConstruct, PoolDestroy, kChannelPool, 2});
    game.Call<ConstructArray>({pools + 2 * kChannelPool, PoolConstruct, PoolDestroy, kChannelPool, 2});
}

void PoolsDestroy(CpuContext* cpu, uint32_t pools, int16_t flags) {
    if (!pools)
        return;

    GuestCall game{cpu};
    game.Call<DestroyArray>({pools + 2 * kChannelPool, PoolDestroy, kChannelPool, 2});
    game.Call<DestroyArray>({pools, PoolDestroy, kChannelPool, 2});

    if (flags > 0)
        game.Call<OperatorDelete>({pools});
}

// 0x803CCEB8: a transport's base: no error.
void TransportConstruct(uint32_t transport) {
    Write(transport, kTransportVtable);

    for (uint32_t i = 0; i < 0xC; i += 4)
        Write(transport + trError + i, 0);
}

void LoopbackConstruct(uint32_t transport);
void UdpPeerConstruct(uint32_t transport);
void McsConstruct(CpuContext* cpu, uint32_t transport, uint32_t listening);

// 0x803CA238: the base connection over a new transport of `kind`: 1 a
// loopback, 2 a UDP peer, 4 and 5 an MCS peer (listening or not), 6 the
// locked transport (0x803CA358).
void ConnectionConstruct(CpuContext* cpu, uint32_t connection, uint32_t kind) {
    GuestCall game{cpu};
    Write(connection, kConnectionVtable);
    Write(connection + cCallback, 0);
    Write(connection + cParam, 0);
    Write(connection + cTransport, 0);
    uint32_t transport = 0;

    if (kind == 1) {
        transport = game.Call<OperatorNew>({0x2014});

        if (transport)
            LoopbackConstruct(transport);
    } else if (kind == 2) {
        transport = game.Call<OperatorNew>({0x18});

        if (transport)
            UdpPeerConstruct(transport);
    } else if (kind == 4 || kind == 5) {
        transport = game.Call<OperatorNew>({0x2470});

        if (transport)
            McsConstruct(cpu, transport, kind == 4);
    } else if (kind == 6) {
        transport = game.Call<OperatorNew>({0x663C});

        if (transport)
            game.Call<0x803CA358>({transport});
    } else {
        return;
    }

    Write(connection + cTransport, transport);
}

// 0x803CA45C: the base connection's destructor: its transport goes too.
void ConnectionDestroy(CpuContext* cpu, uint32_t connection, int16_t flags) {
    if (!connection)
        return;

    GuestCall game{cpu};
    Write(connection, kConnectionVtable);

    if (const uint32_t transport = Read(connection + cTransport)) {
        game.Virtual(transport, tDestroy, {1});
        Write(connection + cTransport, 0);
    }

    if (flags > 0)
        game.Call<OperatorDelete>({connection});
}

// 0x803CA500: NetworkConnectionStable.
void StableConstruct(CpuContext* cpu, uint32_t connection, uint32_t kind) {
    ConnectionConstruct(cpu, connection, kind);
    Write(connection, kStableVtable);
    PoolsConstruct(cpu, connection + cSendPools);
    SetState(connection, kIdle);
}

// 0x803CA644.
void StableDestroy(CpuContext* cpu, uint32_t connection, int16_t flags) {
    if (!connection)
        return;

    PoolsDestroy(cpu, connection + cSendPools, -1);
    ConnectionDestroy(cpu, connection, 0);

    if (flags > 0)
        GuestCall{cpu}.Call<OperatorDelete>({connection});
}

// A pool's first sequence: the clock in milliseconds plus rand().
uint32_t FirstSequence(CpuContext* cpu, float now) {
    GuestCall game{cpu};
    const uint32_t ms = game.Call<CvtFloatToUnsigned>({}, {double(kMilliseconds * now)});
    return (game.Call<Rand>() + ms) & 0xFFFF;
}

// 0x803CA6B0 (virtual 0xC): opens the connection with a callback, its
// parameter, an id and a name {data, length}; then resets it.
void Open(CpuContext* cpu, uint32_t connection, uint32_t callback, uint32_t param, uint32_t id, uint32_t name) {
    GuestCall game{cpu};
    const uint32_t send0 = connection + cSendPools;
    const uint32_t send1 = send0 + kChannelPool;
    const uint32_t recv0 = connection + cRecvPools;
    const uint32_t recv1 = recv0 + kChannelPool;
    game.Virtual(send0, 0x14, {connection + cPoolBuffers, kBuffer});
    game.Virtual(send1, 0x14, {connection + cPoolBuffers + kBuffer, kBuffer});
    game.Call<PoolSetSequence>({send0, FirstSequence(cpu, Seconds(cpu))});
    game.Call<PoolSetSequence>({send1, FirstSequence(cpu, Seconds(cpu))});
    game.Virtual(recv0, 0x14, {connection + cPoolBuffers + 2 * kBuffer, kBuffer});
    game.Virtual(recv1, 0x14, {connection + cPoolBuffers + 3 * kBuffer, kBuffer});
    Memory::Write8(connection + cProbeFlag, 0);
    Write(connection + cProbeValue, 0);
    Memory::Write8(connection + cId, uint8_t(id));
    const uint32_t length = std::min<int32_t>(ReadInt(name + 4), 0x80);
    Write(connection + cNameLength, length);
    game.Call<Memcpy>({connection + cName, Read(name), length});
    SetF(connection + cNow, Seconds(cpu));
    SetF(connection + cStarted, Seconds(cpu));
    Write(connection + cKeepalive, Read(kDefaultKeepalive));
    Write(connection + cConnectTimeout, Read(kDefaultTimeout));
    Write(connection + cPingsLost, 0);
    Write(connection + cPingsAnswered, 0);
    SetF(connection + cPingAt, 0.0f);
    SetF(connection + cMinuteAt, 0.0f);
    Write(connection + cSentTotal, 0);
    Write(connection + cReceivedTotal, 0);
    SetF(connection + cLastReceive, 0.0f);
    Write(connection + cNoReceive, Read(kDefaultNoReceive));
    Memory::Write8(connection + cReceivedSince, 0);
    Write(connection + cCallback, callback);
    Write(connection + cParam, param);
    game.Virtual(connection, vReset, {});
}

// 0x803CA894 (virtual 0x10): closes the transport at once.
void CloseTransport(CpuContext* cpu, uint32_t connection) {
    if (const uint32_t transport = Read(connection + cTransport)) {
        GuestCall{cpu}.Virtual(transport, tClose, {});
        SetState(connection, kIdle);
    }
}

// 0x803CA8E4 (virtual 0x14): back to the state of a new connection.
void Reset(CpuContext* cpu, uint32_t connection) {
    GuestCall game{cpu};
    game.Call<Memset>({connection + cError, 0, 0xC});
    game.Call<Memset>({connection + cScratch, 0, kBuffer});
    game.Call<Memset>({connection + cRecvA, 0, kBuffer});
    game.Call<Memset>({connection + cRecvB, 0, kBuffer});
    game.Call<Memset>({connection + cBatch, 0, kBuffer});
    Write(connection + cBatchLength, 0);
    Memory::Write8(connection + cBatchFull, 0);

    for (uint32_t i = 0; i < 4; ++i)
        game.Call<PoolReset>({connection + cSendPools + i * kChannelPool});

    Memory::Write8(connection + cResendNow, 0);
    game.Call<Memset>({connection + cLastSend, 0, 8});
    SetF(connection + cRto, kRtoScale * kPingStart);
    SetF(connection + cSendInterval, 0.0f);
    Write(connection + cBatchLimit, kBuffer);
    Write(connection + cPeerNameLength, 0);
    game.Call<Memset>({connection + cPeerName, 0, 0x80});
    SetState(connection, kIdle);
    SetF(connection + cKeepaliveAt, 0.0f);
    SetF(connection + cConnectAt, 0.0f);
    SetF(connection + cHelloAt, 0.0f);
    Memory::Write8(connection + cPingIndex, 0);

    for (uint32_t i = 0; i < 9; ++i)
        SetF(connection + cPings + i * 4, kPingStart);

    for (uint32_t at = cSent; at <= cSendsMinute; at += 4)
        Write(connection + at, 0);

    Memory::Write8(connection + cNoReceiveCheck, 0);
    Memory::Write8(connection + cMtuProbeDue, 0);
    Memory::Write8(connection + cMtuAnswered, 0);
    Write(connection + cMtuLargest, kUnitMtuDefault);
    Write(connection + cMtu, kBuffer);
    Memory::Write8(connection + cMtuFixed, 0);
    Write(connection + cMtuTries, 8);
}

// 0x803CB158 (virtual 0x1C): opens the transport toward an address; with
// no MTU probe asked for, the MTU is fixed.
void Connect(CpuContext* cpu, uint32_t connection, uint32_t address, uint32_t probe) {
    if (const uint32_t transport = Read(connection + cTransport)) {
        GuestCall{cpu}.Virtual(transport, tOpen, {address, probe});
        Memory::Write8(connection + cMtuFixed, probe == 0);
    }
}

// 0x803CB1B4 (virtual 0x20): an idle connection starts connecting, with a
// first guess at the round trip.
void Start(uint32_t connection, double rtt) {
    if (State(connection) != kIdle)
        return;

    SetState(connection, kConnecting);
    SetF(connection + cRtt, float(rtt));
    UpdateRto(connection);
    Write(connection + cKeepaliveAt, Read(connection + cNow));
}

// 0x803CB220 / 0x803CB30C (virtuals 0x24, 0x28): closes, telling a
// connected peer (0x84) or refusing it (0x85) first.
void Close(CpuContext* cpu, uint32_t connection, bool refuse) {
    const uint32_t transport = Read(connection + cTransport);

    if (!transport)
        return;

    LogInfo(cpu, 3, refuse ? kMsgRefuse : kMsgClose, {uint32_t(Id(connection))});
    const uint32_t state = State(connection);

    if (state != kIdle && state != kConnecting) {
        LogInfo(cpu, 3, refuse ? kMsgRefuseSend : kMsgCloseSend, {uint32_t(Id(connection))});
        SendClose(cpu, connection, refuse);
        Flush(cpu, connection, 1);
    }

    GuestCall game{cpu};
    game.Virtual(Read(connection + cTransport), tClose, {});
    game.Virtual(connection, vReset, {});
}

// 0x803CB3F8 (virtual 0x2C): flushes both channels of a live connection
// and lets the transport finish (state 5) or ends at once (state 6).
void Shutdown(CpuContext* cpu, uint32_t connection) {
    const uint32_t state = State(connection);

    if (state != kIdle && state != kConnecting) {
        Flush(cpu, connection, 0);
        Flush(cpu, connection, 1);
    }

    SetState(connection, State(connection) == kIdle ? kClosed : kClosing);
}

// 0x803CBBFC: sends the raw batch through the transport.
void BatchFlush(CpuContext* cpu, uint32_t connection, uint32_t a, int32_t b, uint32_t c) {
    if (ReadInt(connection + cBatchLength) <= 0 && b <= 0)
        return;

    const int32_t sent = int32_t(GuestCall{cpu}.Virtual(Read(connection + cTransport), tSend,
        {connection + cBatch, Read(connection + cBatchLength), a, uint32_t(b), uint32_t(int32_t(int8_t(c)))}));
    Write(connection + cBatchLength, 0);

    if (sent >= 0) {
        Write(connection + cSent, Read(connection + cSent) + uint32_t(sent));
        Write(connection + cSentTotal, Read(connection + cSentTotal) + uint32_t(sent));
        Write(connection + cSends, Read(connection + cSends) + 1);
    }
}

// 0x803CBB00: a unit packet into the raw batch, flushing it first when it
// would not fit (the batch then counts as full).
void BatchPut(CpuContext* cpu, uint32_t connection, uint32_t unit) {
    GuestCall game{cpu};
    const uint32_t length = game.Call<UnitLength>({unit}) & 0xFFFF;
    auto fits = [&] {
        const int32_t total = ReadInt(connection + cBatchLength) + int32_t(length);
        return total <= int32_t(kBuffer) && total <= int32_t(Mtu(connection));
    };

    if (!fits()) {
        BatchFlush(cpu, connection, 0, 0, 0);
        Memory::Write8(connection + cBatchFull, 1);
    }

    if (fits()) {
        game.Call<Memcpy>({connection + cBatch + Read(connection + cBatchLength), game.Call<UnitPacket>({unit}), length});
        Write(connection + cBatchLength, Read(connection + cBatchLength) + length);
    } else {
        SetError(connection, kErrorPoolFull, 9, kNoValue);
    }
}

// 0x803CBA9C: the room left in the raw batch (none once it was full).
uint32_t BatchRoom(uint32_t connection) {
    if (Memory::Read8(connection + cBatchFull))
        return 0;

    const int32_t room = int32_t(kBuffer) - ReadInt(connection + cBatchLength);
    const int32_t mtuRoom = int32_t(Mtu(connection)) - ReadInt(connection + cBatchLength);
    return uint32_t(std::min(room, mtuRoom));
}

// 0x803CBF20: twice the number of the last few pings still unanswered.
uint32_t UnansweredPings(uint32_t connection) {
    uint32_t count = 0;
    const int32_t span = 7 - Fctiwz(kPingStart);

    for (int32_t i = 0; i < span; ++i) {
        const uint32_t index = (Memory::Read8(connection + cPingIndex) + uint32_t(i)) & 7;

        if (F(connection + cPings + index * 4) == 0.0f)
            count += 2;
    }

    return count;
}

// The transports: vtable, the first error (code and two values), then
// their own state.
enum : uint32_t {
    kErrorNoSocket = 0x80030002,
    kErrorSend = 0x80030021,
    kErrorReceive = 0x80030022,
    kErrorStopped = 0x80030012,
};

// The platform's byte order hooks: identities on the Wii, taken directly.
enum : uint32_t {
    vNtohs = 0x48,
    vHtons = 0x4C,
};

uint32_t Convert(CpuContext* cpu, uint32_t slot, uint32_t value) {
    const uint32_t platform = Platform();
    const uint32_t fn = Read(Read(platform) + slot);

    if (fn == 0x80418430 || fn == 0x80418438 || fn == 0x80418440 || fn == 0x80418448)
        return value;

    return GuestCall{cpu}(fn, {platform, value});
}

// 0x803CCF78: keeps a transport's first error.
void TransportError(uint32_t transport, uint32_t code, uint32_t a, uint32_t b) {
    if (Read(transport + trError))
        return;

    Write(transport + trError, code);
    Write(transport + trError + 4, a);
    Write(transport + trError + 8, b);
}

// 0x803CCF00 and the derived destructors: delete when asked.
void TransportDestroy(CpuContext* cpu, uint32_t transport, int16_t flags) {
    if (transport && flags > 0)
        GuestCall{cpu}.Call<OperatorDelete>({transport});
}

// The loopback (kind 1): what is sent is received, through a 0x2000-byte
// buffer of unit packets.
enum : uint32_t {
    lBuffer = 0x10,
    lLength = 0x2010,
};

constexpr uint32_t kLoopbackSize = 0x2000;

void LoopbackClear(CpuContext* cpu, uint32_t transport) {
    GuestCall{cpu}.Call<Memset>({transport + lBuffer, 0, kLoopbackSize});
    Write(transport + lLength, 0);
}

// 0x803CCF94.
void LoopbackConstruct(uint32_t transport) {
    TransportConstruct(transport);
    Write(transport, kLoopbackVtable);

    for (uint32_t i = 0; i < kLoopbackSize; i += 4)
        Write(transport + lBuffer + i, 0);

    Write(transport + lLength, 0);
}

// 0x803CCFEC (virtual 0x10).
int32_t LoopbackSend(CpuContext* cpu, uint32_t transport, uint32_t data, int32_t length) {
    if (!data || length < 1)
        return 0;

    if (ReadInt(transport + lLength) + length > int32_t(kLoopbackSize)) {
        TransportError(transport, kErrorSend, kLoopbackSize, kNoValue);
        return -1;
    }

    GuestCall{cpu}.Call<Memcpy>({transport + lBuffer + Read(transport + lLength), data, uint32_t(length)});
    Write(transport + lLength, Read(transport + lLength) + uint32_t(length));
    return length;
}

// 0x803CD080 (virtual 0x14): the next unit packet, if it fits.
int32_t LoopbackReceive(CpuContext* cpu, uint32_t transport, uint32_t to, uint32_t length, uint32_t length2, uint32_t flag) {
    GuestCall game{cpu};
    Stacked unit(cpu);
    const int32_t room = ReadInt(length);
    Write(length, 0);
    Write(length2, 0);
    Memory::Write8(flag, 0);
    game.Call<UnitConstruct>({unit.object});
    game.Call<UnitAttach>({unit.object, transport + lBuffer, Read(transport + lLength)});
    int32_t copied = 0;

    if (game.Call<UnitHasPacket>({unit.object}) && int32_t(game.Call<UnitLength>({unit.object}) & 0xFFFF) <= room) {
        copied = int32_t(game.Call<UnitCopy>({unit.object, to, uint32_t(room)}));

        if (copied < 0) {
            copied = 0;
        } else {
            const uint32_t consumed = game.Call<UnitConsume>({unit.object});
            Write(transport + lLength, Read(transport + lLength) - consumed);
            Write(length, uint32_t(copied));
        }
    }

    game.Call<UnitDestroy>({unit.object, 0xFFFFFFFF});
    return copied;
}

// The UDP peer (kind 2): one of a NetworkMultipleUdp's four peers. A packet
// is the unit packets (u16 length first) and then the raw data (u16 length
// + 1, a flag byte), through the shared buffer at 0x806D50E0.
enum : uint32_t {
    uPeer = 0x10,
    uSocket = 0x14,
};

constexpr uint32_t kUdpBuffer = 0x806D50E0;
constexpr uint32_t kUdpLimit = 0x5DC;

enum : uint32_t {
    UdpReset = 0x803CE888,
    UdpSend = 0x803CE928,
    UdpReceive = 0x803CE9E8,
    UdpErrorB = 0x803CEC10,
    UdpErrorA = 0x803CEC28,
};

// 0x803CA420.
void UdpPeerConstruct(uint32_t transport) {
    TransportConstruct(transport);
    Write(transport, kNullVtable);
}

// 0x803CD2AC (virtual 0xC): {peer, socket}.
void UdpPeerOpen(uint32_t transport, uint32_t address) {
    Write(transport + uPeer, Read(address));
    Write(transport + uSocket, Read(address + 4));
}

// 0x803CD2C0 (virtual 0x10).
int32_t UdpPeerSend(CpuContext* cpu, uint32_t transport, uint32_t data, int32_t length, uint32_t data2, int32_t length2, uint32_t flag) {
    GuestCall game{cpu};
    const uint32_t socket = Read(transport + uSocket);

    if (!socket) {
        TransportError(transport, kErrorNoSocket, 0, 0);
        return -1;
    }

    uint32_t at = kUdpBuffer;
    uint32_t first = 0;

    if (!data || length < 1) {
        Memory::Write16(at, 0);
        at += 2;
    } else {
        first = Convert(cpu, vHtons, uint32_t(length) & 0xFFFF) & 0xFFFF;
        Memory::Write16(at, uint16_t(first));
        game.Call<Memcpy>({at + 2, data, uint32_t(length)});
        at += 2 + uint32_t(length);
    }

    uint32_t second = 0;

    if (!data2 || length2 < 1) {
        Memory::Write16(at, 0);
        at += 2;
    } else {
        second = Convert(cpu, vHtons, uint32_t(length2 + 1) & 0xFFFF) & 0xFFFF;
        Memory::Write16(at, uint16_t(second));
        Memory::Write8(at + 2, uint8_t(flag));
        game.Call<Memcpy>({at + 3, data2, uint32_t(length2)});
        at += 3 + uint32_t(length2);
    }

    if (!(Convert(cpu, vNtohs, first) & 0xFFFF) && !(Convert(cpu, vNtohs, second) & 0xFFFF))
        return 0;

    const int32_t sent = int32_t(game.Call<UdpSend>({socket, Read(transport + uPeer), kUdpBuffer, at - kUdpBuffer}));

    if (sent < 0) {
        const uint32_t a = game.Call<UdpErrorA>({Read(transport + uSocket)});
        const uint32_t b = game.Call<UdpErrorB>({Read(transport + uSocket)});
        TransportError(transport, kErrorSend, b, a);
        return -1;
    }

    return sent;
}

// 0x803CD4D8 (virtual 0x14).
int32_t UdpPeerReceive(CpuContext* cpu, uint32_t transport, uint32_t to, uint32_t length, uint32_t to2, uint32_t length2, uint32_t flag) {
    GuestCall game{cpu};
    const int32_t room = ReadInt(length);
    const int32_t room2 = ReadInt(length2);
    Write(length, 0);
    Write(length2, 0);
    Memory::Write8(flag, 0);
    const uint32_t socket = Read(transport + uSocket);

    if (!socket) {
        TransportError(transport, kErrorNoSocket, 0, 0);
        return -1;
    }

    const int32_t got = int32_t(game.Call<UdpReceive>({socket, Read(transport + uPeer), kUdpBuffer, kUdpLimit}));

    if (got < 0) {
        const uint32_t a = game.Call<UdpErrorA>({Read(transport + uSocket)});
        const uint32_t b = game.Call<UdpErrorB>({Read(transport + uSocket)});
        TransportError(transport, kErrorReceive, b, a);
        return -1;
    }

    if (!got)
        return 0;

    uint32_t at = kUdpBuffer + 2;
    const uint32_t first = Convert(cpu, vNtohs, Memory::Read16(kUdpBuffer)) & 0xFFFF;

    if (first && int32_t(first) <= room) {
        game.Call<Memcpy>({to, kUdpBuffer + 2, first});
        at += first;
        Write(length, first);
    }

    const uint32_t second = (Convert(cpu, vNtohs, Memory::Read16(at)) - 1) & 0xFFFF;

    if (second && int32_t(second) <= room2) {
        Memory::Write8(flag, Memory::Read8(at + 2));
        game.Call<Memcpy>({to2, at + 3, second});
        Write(length2, second);
    }

    return got;
}

// 0x803CD6B0 (virtual 0x1C) and 0x803CD73C (virtual 0x28): forget what
// the peer had waiting.
int32_t UdpPeerReset(CpuContext* cpu, uint32_t transport) {
    if (!Read(transport + uSocket)) {
        TransportError(transport, kErrorNoSocket, 0, 0);
        return -1;
    }

    GuestCall{cpu}.Call<UdpReset>({Read(transport + uSocket), Read(transport + uPeer)});
    return 1;
}

// The MCS peer (kinds 4 and 5): a stream through a NetworkSingleTcp, framed
// by u16 lengths; a listening one waits for the server, then sends its
// hello.
enum : uint32_t {
    mListening = 0x10, // u8
    mStep = 0x14,
    mStop = 0x18,      // u8
    mBuffer = 0x19,    // 0x2400 bytes received
    mLength = 0x241C,
    mAddress = 0x2420, // 6 bytes
    mSocket = 0x2428,
    mHello = 0x242C,   // 0x40 bytes
    mHelloLength = 0x246C,
};

constexpr uint32_t kMcsSendBuffer = 0x806D56C0;
constexpr uint32_t kMcsSendLimit = 0x410;
constexpr uint32_t kMcsBuffer = 0x2400;
constexpr uint32_t kMcsRetryAt = 0x80797158; // f32: the last retry
constexpr float kMcsRetry = 0.5f;

enum : uint32_t {
    TcpOpen = 0x803CE2AC,
    TcpPoll = 0x803CE380,
    TcpConnected = 0x803CE3A8,
    TcpClose = 0x803CE3D0,
    TcpAdd = 0x803CE428,
    TcpRemove = 0x803CE4D4,
    TcpClear = 0x803CE57C,
    TcpSend = 0x803CE5BC,
    TcpErrorA = 0x803CE620,
    TcpErrorB = 0x803CE638,
};

constexpr uint32_t kMsgMcsPutOverflow = 0x805F9D30;

// 0x803CD824.
void McsConstruct(CpuContext* cpu, uint32_t transport, uint32_t listening) {
    GuestCall game{cpu};
    TransportConstruct(transport);
    Write(transport, kMcsVtable);
    Memory::Write8(transport + mListening, uint8_t(listening));
    Write(transport + mStep, 0);
    Memory::Write8(transport + mStop, 0);
    game.Call<Memset>({kMcsSendBuffer, 0, kMcsSendLimit});
    game.Call<Memset>({transport + mBuffer, 0, kMcsBuffer});
    Write(transport + mLength, 0);
    game.Call<Memset>({transport + mAddress, 0, 6});
    Write(transport + mSocket, 0);
}

// 0x803CD8B8 (virtual 0xC): {address (6 bytes), _, socket, hello (0x40),
// hello length}.
void McsOpen(CpuContext* cpu, uint32_t transport, uint32_t address) {
    GuestCall game{cpu};
    game.Call<Memcpy>({transport + mAddress, address, 6});
    Write(transport + mSocket, Read(address + 8));
    game.Call<Memset>({transport + mHello, 0, 0x40});
    const uint32_t length = std::min<int32_t>(ReadInt(address + 0x4C), 0x40);
    Write(transport + mHelloLength, length);
    game.Call<Memcpy>({transport + mHello, address + 0xC, length});
}

// 0x803CD934 (virtual 0x10): a framed packet; a listening peer sends only
// once connected. What was sent is also kept in the receive buffer.
int32_t McsSend(CpuContext* cpu, uint32_t transport, uint32_t data, uint32_t length) {
    GuestCall game{cpu};
    const uint32_t socket = Read(transport + mSocket);

    if (!socket || !data || int32_t(length) < 1)
        return 0;

    if (length + 2 > kMcsSendLimit) {
        TransportError(transport, kErrorSend, kMcsSendLimit, kNoValue);
        return -1;
    }

    if (length + Read(transport + mLength) + 2 > kMcsBuffer) {
        TransportError(transport, kErrorSend, kMcsBuffer, kNoValue);
        return -1;
    }

    Memory::Write16(kMcsSendBuffer, uint16_t(Convert(cpu, vHtons, length & 0xFFFF)));
    game.Call<Memcpy>({kMcsSendBuffer + 2, data, length});

    if (!Memory::Read8(transport + mListening) || game.Call<TcpConnected>({socket})) {
        if (int32_t(game.Call<TcpSend>({Read(transport + mSocket), kMcsSendBuffer, length + 2})) < 0) {
            const uint32_t b = game.Call<TcpErrorB>({Read(transport + mSocket)});
            const uint32_t a = game.Call<TcpErrorA>({Read(transport + mSocket)});
            TransportError(transport, kErrorSend, a, b);
            return -1;
        }
    }

    game.Call<Memcpy>({transport + mBuffer + Read(transport + mLength), kMcsSendBuffer, length + 2});
    Write(transport + mLength, Read(transport + mLength) + length + 2);
    return int32_t(length + 2);
}

// 0x803CDAE8 (virtual 0x14): the next framed packet from the buffer.
int32_t McsReceive(CpuContext* cpu, uint32_t transport, uint32_t to, uint32_t length, uint32_t length2, uint32_t flag) {
    GuestCall game{cpu};
    const uint32_t socket = Read(transport + mSocket);

    if (!socket)
        return 0;

    if (!Memory::Read8(transport + mListening) && game.Call<TcpErrorA>({socket})) {
        const uint32_t b = game.Call<TcpErrorB>({Read(transport + mSocket)});
        const uint32_t a = game.Call<TcpErrorA>({Read(transport + mSocket)});
        TransportError(transport, kErrorReceive, a, b);
        return -1;
    }

    const int32_t room = ReadInt(length);
    Write(length, 0);

    if (length2)
        Write(length2, 0);

    if (flag)
        Memory::Write8(flag, 0);

    if (ReadInt(transport + mLength) < 2)
        return 0;

    const uint32_t size = Convert(cpu, vNtohs, Memory::Read16(transport + mBuffer)) & 0xFFFF;

    if (!size) {
        TransportError(transport, kErrorReceive, 0, kNoValue);
        return -1;
    }

    if (ReadInt(transport + mLength) < int32_t(size + 2) || room < int32_t(size))
        return 0;

    game.Call<Memcpy>({to, transport + mBuffer + 2, size});
    Write(length, size);
    const int32_t left = ReadInt(transport + mLength) - int32_t(size + 2);
    Write(transport + mLength, uint32_t(left));

    if (left > 0)
        game.Call<0x8045BC18>({transport + mBuffer, transport + mBuffer + size + 2, uint32_t(left)});

    return int32_t(size + 2);
}

// 0x803CDC90 (virtual 0x18): bytes from the socket into the buffer.
int32_t McsPut(CpuContext* cpu, uint32_t transport, uint32_t data, int32_t length) {
    if (length < 1)
        return 0;

    const int32_t had = ReadInt(transport + mLength);

    if (uint32_t(had + length) > kMcsBuffer) {
        Log(cpu, vWarn, kMsgMcsPutOverflow, kMcsBuffer, uint32_t(had));
        TransportError(transport, kErrorPoolFull, kMcsBuffer, kNoValue);
        return -1;
    }

    GuestCall{cpu}.Call<Memcpy>({transport + mBuffer + uint32_t(had), data, uint32_t(length)});
    Write(transport + mLength, Read(transport + mLength) + uint32_t(length));
    return length;
}

void McsClearBuffer(CpuContext* cpu, uint32_t transport) {
    GuestCall{cpu}.Call<Memset>({transport + mBuffer, 0, kMcsBuffer});
    Write(transport + mLength, 0);
}

// 0x803CDD54 (virtual 0x1C): connecting. A connecting peer registers with
// the socket once it is up (retrying every half second); a listening one
// opens it, then sends its hello once connected.
int32_t McsPoll(CpuContext* cpu, uint32_t transport) {
    GuestCall game{cpu};
    const uint32_t socket = Read(transport + mSocket);

    auto fail = [&](uint32_t code) {
        const uint32_t b = game.Call<TcpErrorB>({Read(transport + mSocket)});
        const uint32_t a = game.Call<TcpErrorA>({Read(transport + mSocket)});
        TransportError(transport, code, a, b);
        return -1;
    };

    if (!Memory::Read8(transport + mListening)) {
        if (!socket) {
            TransportError(transport, kErrorTransport, 0, kNoValue);
            return -1;
        }

        if (game.Call<TcpErrorA>({socket}))
            return fail(kErrorTransport);

        if (!game.Call<TcpConnected>({socket}))
            return 0;

        const float now = Seconds(cpu);

        if (!(double(kMcsRetry + F(kMcsRetryAt)) <= double(now)))
            return 0;

        SetF(kMcsRetryAt, Seconds(cpu));
        McsClearBuffer(cpu, transport);
        game.Call<TcpAdd>({Read(transport + mSocket), transport});
        return 1;
    }

    if (!socket) {
        TransportError(transport, kErrorTransport, 0, kNoValue);
        Write(transport + mStep, 0);
        return -1;
    }

    if (Memory::Read8(transport + mStop)) {
        TransportError(transport, kErrorStopped, 0, kNoValue);
        Write(transport + mStep, 0);
        return -1;
    }

    if (Read(transport + mStep) == 0) {
        game.Call<TcpClear>({socket});

        if (int32_t(game.Call<TcpOpen>({Read(transport + mSocket), transport + mAddress})) < 0) {
            fail(kErrorTransport);
            Write(transport + mStep, 0);
            return -1;
        }

        McsClearBuffer(cpu, transport);
        Write(transport + mStep, Read(transport + mStep) + 1);
    } else if (Read(transport + mStep) == 1) {
        const int32_t polled = int32_t(game.Call<TcpPoll>({socket}));

        if (polled < 0) {
            fail(kErrorTransport);
            Write(transport + mStep, 0);
            return -1;
        }

        if (polled > 0) {
            SetF(kMcsRetryAt, Seconds(cpu));
            game.Call<TcpSend>({Read(transport + mSocket), transport + mHello, Read(transport + mHelloLength)});
            Write(transport + mStep, 0);
            return 1;
        }
    }

    return 0;
}

// 0x803CE00C (virtual 0x20): a listening peer stops trying.
void McsStop(uint32_t transport) {
    if (Memory::Read8(transport + mListening))
        Memory::Write8(transport + mStop, 1);
}

// 0x803CE054 (virtual 0x28): leaves the socket (a listening peer closes it).
void McsClose(CpuContext* cpu, uint32_t transport) {
    GuestCall game{cpu};

    if (const uint32_t socket = Read(transport + mSocket)) {
        game.Call<TcpRemove>({socket, transport});

        if (Memory::Read8(transport + mListening)) {
            game.Call<TcpClose>({Read(transport + mSocket)});
            game.Call<TcpClear>({Read(transport + mSocket)});
        }
    }

    McsClearBuffer(cpu, transport);
}

uint32_t Arg(CpuContext* cpu, int n) {
    return cpu->gpr[n];
}

} // namespace

extern "C" void MH3_NetConnectionConstruct(CpuContext* cpu) {
    ConnectionConstruct(cpu, Arg(cpu, 3), Arg(cpu, 4));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CA238, MH3_NetConnectionConstruct, "NetConnectionConstruct");

extern "C" void MH3_NetLockedTransportConstruct(CpuContext* cpu) {
    const uint32_t transport = Arg(cpu, 3);
    TransportConstruct(transport);
    Write(transport, kLockedVtable);
    Write(transport + 0x6614, kMutexVtable);
    GuestCall{cpu}.Call<OSInitMutex>({transport + 0x6618});
    cpu->gpr[3] = transport;
}
REGISTER_NATIVE_FUNCTION_AS(0x803CA358, MH3_NetLockedTransportConstruct, "NetLockedTransportConstruct");

extern "C" void MH3_NetMutexDestroy(CpuContext* cpu) {
    const uint32_t object = Arg(cpu, 3);
    TransportDestroy(cpu, object, int16_t(Arg(cpu, 4)));
    cpu->gpr[3] = object;
}
REGISTER_NATIVE_FUNCTION_AS(0x803CA39C, MH3_NetMutexDestroy, "NetMutexDestroy");

extern "C" void MH3_NetMutexConstruct(CpuContext* cpu) {
    const uint32_t mutex = Arg(cpu, 3);
    Write(mutex, kMutexVtable);
    GuestCall{cpu}.Call<OSInitMutex>({mutex + 4});
    cpu->gpr[3] = mutex;
}
REGISTER_NATIVE_FUNCTION_AS(0x803CA3E0, MH3_NetMutexConstruct, "NetMutexConstruct");

extern "C" void MH3_NetUdpPeerConstruct(CpuContext* cpu) {
    UdpPeerConstruct(Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CA420, MH3_NetUdpPeerConstruct, "NetUdpPeerConstruct");

extern "C" void MH3_NetConnectionDestroy(CpuContext* cpu) {
    const uint32_t connection = Arg(cpu, 3);
    ConnectionDestroy(cpu, connection, int16_t(Arg(cpu, 4)));
    cpu->gpr[3] = connection;
}
REGISTER_NATIVE_FUNCTION_AS(0x803CA45C, MH3_NetConnectionDestroy, "NetConnectionDestroy");

extern "C" void MH3_NetDefaultKeepalive(CpuContext* cpu) {
    SetF(kDefaultKeepalive, float(cpu->fpr[1].d));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CA4E8, MH3_NetDefaultKeepalive, "NetDefaultKeepalive");

extern "C" void MH3_NetDefaultTimeout(CpuContext* cpu) {
    SetF(kDefaultTimeout, float(cpu->fpr[1].d));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CA4F0, MH3_NetDefaultTimeout, "NetDefaultTimeout");

extern "C" void MH3_NetDefaultNoReceive(CpuContext* cpu) {
    SetF(kDefaultNoReceive, float(cpu->fpr[1].d));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CA4F8, MH3_NetDefaultNoReceive, "NetDefaultNoReceive");

extern "C" void MH3_NetStableConstruct(CpuContext* cpu) {
    const uint32_t connection = Arg(cpu, 3);
    StableConstruct(cpu, connection, Arg(cpu, 4));
    cpu->gpr[3] = connection;
}
REGISTER_NATIVE_FUNCTION_AS(0x803CA500, MH3_NetStableConstruct, "NetStableConstruct");

extern "C" void MH3_NetPoolsDestroy(CpuContext* cpu) {
    const uint32_t pools = Arg(cpu, 3);
    PoolsDestroy(cpu, pools, int16_t(Arg(cpu, 4)));
    cpu->gpr[3] = pools;
}
REGISTER_NATIVE_FUNCTION_AS(0x803CA54C, MH3_NetPoolsDestroy, "NetPoolsDestroy");

extern "C" void MH3_NetPoolsConstruct(CpuContext* cpu) {
    const uint32_t pools = Arg(cpu, 3);
    PoolsConstruct(cpu, pools);
    cpu->gpr[3] = pools;
}
REGISTER_NATIVE_FUNCTION_AS(0x803CA5D4, MH3_NetPoolsConstruct, "NetPoolsConstruct");

extern "C" void MH3_NetStableDestroy(CpuContext* cpu) {
    const uint32_t connection = Arg(cpu, 3);
    StableDestroy(cpu, connection, int16_t(Arg(cpu, 4)));
    cpu->gpr[3] = connection;
}
REGISTER_NATIVE_FUNCTION_AS(0x803CA644, MH3_NetStableDestroy, "NetStableDestroy");

extern "C" void MH3_NetStableOpen(CpuContext* cpu) {
    Open(cpu, Arg(cpu, 3), Arg(cpu, 4), Arg(cpu, 5), Arg(cpu, 6), Arg(cpu, 7));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CA6B0, MH3_NetStableOpen, "NetStableOpen");

extern "C" void MH3_NetStableCloseTransport(CpuContext* cpu) {
    CloseTransport(cpu, Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CA894, MH3_NetStableCloseTransport, "NetStableCloseTransport");

extern "C" void MH3_NetStableReset(CpuContext* cpu) {
    Reset(cpu, Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CA8E4, MH3_NetStableReset, "NetStableReset");

extern "C" void MH3_NetStableUpdate(CpuContext* cpu) {
    Update(cpu, Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CAA70, MH3_NetStableUpdate, "NetStableUpdate");

extern "C" void MH3_NetStableConnect(CpuContext* cpu) {
    Connect(cpu, Arg(cpu, 3), Arg(cpu, 4), Arg(cpu, 5));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CB158, MH3_NetStableConnect, "NetStableConnect");

extern "C" void MH3_NetStableStart(CpuContext* cpu) {
    Start(Arg(cpu, 3), cpu->fpr[1].d);
}
REGISTER_NATIVE_FUNCTION_AS(0x803CB1B4, MH3_NetStableStart, "NetStableStart");

extern "C" void MH3_NetStableClose(CpuContext* cpu) {
    Close(cpu, Arg(cpu, 3), false);
}
REGISTER_NATIVE_FUNCTION_AS(0x803CB220, MH3_NetStableClose, "NetStableClose");

extern "C" void MH3_NetStableRefuse(CpuContext* cpu) {
    Close(cpu, Arg(cpu, 3), true);
}
REGISTER_NATIVE_FUNCTION_AS(0x803CB30C, MH3_NetStableRefuse, "NetStableRefuse");

extern "C" void MH3_NetStableShutdown(CpuContext* cpu) {
    Shutdown(cpu, Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CB3F8, MH3_NetStableShutdown, "NetStableShutdown");

extern "C" void MH3_NetStablePut(CpuContext* cpu) {
    Put(cpu, Arg(cpu, 3), Arg(cpu, 4), int32_t(Arg(cpu, 5)));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CB480, MH3_NetStablePut, "NetStablePut");

extern "C" void MH3_NetStableFlush(CpuContext* cpu) {
    Flush(cpu, Arg(cpu, 3), int32_t(Arg(cpu, 4)));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CB544, MH3_NetStableFlush, "NetStableFlush");

extern "C" void MH3_NetBatchReopen(CpuContext* cpu) {
    Memory::Write8(Arg(cpu, 3) + cBatchFull, 0);
}
REGISTER_NATIVE_FUNCTION_AS(0x803CBA90, MH3_NetBatchReopen, "NetBatchReopen");

extern "C" void MH3_NetBatchRoom(CpuContext* cpu) {
    cpu->gpr[3] = BatchRoom(Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CBA9C, MH3_NetBatchRoom, "NetBatchRoom");

extern "C" void MH3_NetBatchPut(CpuContext* cpu) {
    BatchPut(cpu, Arg(cpu, 3), Arg(cpu, 4));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CBB00, MH3_NetBatchPut, "NetBatchPut");

extern "C" void MH3_NetBatchFlush(CpuContext* cpu) {
    BatchFlush(cpu, Arg(cpu, 3), Arg(cpu, 4), int32_t(Arg(cpu, 5)), Arg(cpu, 6));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CBBFC, MH3_NetBatchFlush, "NetBatchFlush");

extern "C" void MH3_NetStableReceive(CpuContext* cpu) {
    Receive(cpu, Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CBC9C, MH3_NetStableReceive, "NetStableReceive");

extern "C" void MH3_NetStableId(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(Id(Arg(cpu, 3)));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CBF14, MH3_NetStableId, "NetStableId");

extern "C" void MH3_NetStableUnansweredPings(CpuContext* cpu) {
    cpu->gpr[3] = UnansweredPings(Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CBF20, MH3_NetStableUnansweredPings, "NetStableUnansweredPings");

extern "C" void MH3_NetStableNoReceiveCheck(CpuContext* cpu) {
    Memory::Write8(Arg(cpu, 3) + cNoReceiveCheck, uint8_t(Arg(cpu, 4)));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CBF98, MH3_NetStableNoReceiveCheck, "NetStableNoReceiveCheck");

extern "C" void MH3_NetStableMarkReceived(CpuContext* cpu) {
    Memory::Write8(Arg(cpu, 3) + cReceivedSince, 1);
}
REGISTER_NATIVE_FUNCTION_AS(0x803CBFA0, MH3_NetStableMarkReceived, "NetStableMarkReceived");

extern "C" void MH3_NetStableKeepalive(CpuContext* cpu) {
    SetF(kDefaultKeepalive, float(cpu->fpr[1].d));
    SetF(Arg(cpu, 3) + cKeepalive, float(cpu->fpr[1].d));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CBFAC, MH3_NetStableKeepalive, "NetStableKeepalive");

extern "C" void MH3_NetStableTimeout(CpuContext* cpu) {
    SetF(kDefaultTimeout, float(cpu->fpr[1].d));
    SetF(Arg(cpu, 3) + cConnectTimeout, float(cpu->fpr[1].d));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CBFE8, MH3_NetStableTimeout, "NetStableTimeout");

extern "C" void MH3_NetStableNoReceive(CpuContext* cpu) {
    SetF(kDefaultNoReceive, float(cpu->fpr[1].d));
    SetF(Arg(cpu, 3) + cNoReceive, float(cpu->fpr[1].d));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CC024, MH3_NetStableNoReceive, "NetStableNoReceive");

extern "C" void MH3_NetStableRtt(CpuContext* cpu) {
    cpu->fpr[1].d = double(F(Arg(cpu, 3) + cRtt));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CC060, MH3_NetStableRtt, "NetStableRtt");

extern "C" void MH3_NetStableSetProbeValue(CpuContext* cpu) {
    SetF(Arg(cpu, 3) + cProbeValue, float(cpu->fpr[1].d));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CC068, MH3_NetStableSetProbeValue, "NetStableSetProbeValue");

extern "C" void MH3_NetStableProbeValue(CpuContext* cpu) {
    cpu->fpr[1].d = double(F(Arg(cpu, 3) + cProbeValue));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CC070, MH3_NetStableProbeValue, "NetStableProbeValue");

extern "C" void MH3_NetStableMtu(CpuContext* cpu) {
    cpu->gpr[3] = Mtu(Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CC078, MH3_NetStableMtu, "NetStableMtu");

extern "C" void MH3_NetStableSetState(CpuContext* cpu) {
    SetState(Arg(cpu, 3), Arg(cpu, 4));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CC094, MH3_NetStableSetState, "NetStableSetState");

extern "C" void MH3_NetStableState(CpuContext* cpu) {
    cpu->gpr[3] = State(Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CC09C, MH3_NetStableState, "NetStableState");

extern "C" void MH3_NetStableSetError(CpuContext* cpu) {
    SetError(Arg(cpu, 3), Arg(cpu, 4), Arg(cpu, 5), Arg(cpu, 6));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CC0A4, MH3_NetStableSetError, "NetStableSetError");

extern "C" void MH3_NetStableBadChannel(CpuContext* cpu) {
    cpu->gpr[3] = BadChannel(Arg(cpu, 3), int32_t(Arg(cpu, 4))) ? 1 : 0;
}
REGISTER_NATIVE_FUNCTION_AS(0x803CC0C0, MH3_NetStableBadChannel, "NetStableBadChannel");

extern "C" void MH3_NetStableSendHello(CpuContext* cpu) {
    SendHello(cpu, Arg(cpu, 3), Arg(cpu, 4) != 0);
}
REGISTER_NATIVE_FUNCTION_AS(0x803CC10C, MH3_NetStableSendHello, "NetStableSendHello");

extern "C" void MH3_NetStableSendConfirm(CpuContext* cpu) {
    SendConfirm(cpu, Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CC224, MH3_NetStableSendConfirm, "NetStableSendConfirm");

extern "C" void MH3_NetStableSendPing(CpuContext* cpu) {
    SendPing(cpu, Arg(cpu, 3), Arg(cpu, 4));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CC2A4, MH3_NetStableSendPing, "NetStableSendPing");

extern "C" void MH3_NetStableSendPong(CpuContext* cpu) {
    SendPong(cpu, Arg(cpu, 3), Arg(cpu, 4), Arg(cpu, 5));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CC3AC, MH3_NetStableSendPong, "NetStableSendPong");

extern "C" void MH3_NetStableSendClose(CpuContext* cpu) {
    SendClose(cpu, Arg(cpu, 3), Arg(cpu, 4) == 0);
}
REGISTER_NATIVE_FUNCTION_AS(0x803CC494, MH3_NetStableSendClose, "NetStableSendClose");

extern "C" void MH3_NetStableSendMtuProbe(CpuContext* cpu) {
    SendMtuProbe(cpu, Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CC530, MH3_NetStableSendMtuProbe, "NetStableSendMtuProbe");

extern "C" void MH3_NetStableMtuStep(CpuContext* cpu) {
    MtuStep(cpu, Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CC694, MH3_NetStableMtuStep, "NetStableMtuStep");

extern "C" void MH3_NetStableSendMtu(CpuContext* cpu) {
    SendMtu(cpu, Arg(cpu, 3), Arg(cpu, 4));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CC784, MH3_NetStableSendMtu, "NetStableSendMtu");

extern "C" void MH3_NetStableHandleControl(CpuContext* cpu) {
    HandleControl(cpu, Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CC830, MH3_NetStableHandleControl, "NetStableHandleControl");

extern "C" void MH3_NetStablePingResult(CpuContext* cpu) {
    PingResult(Arg(cpu, 3), Arg(cpu, 4));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CCD40, MH3_NetStablePingResult, "NetStablePingResult");

extern "C" void MH3_NetStableUpdateRto(CpuContext* cpu) {
    UpdateRto(Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CCE08, MH3_NetStableUpdateRto, "NetStableUpdateRto");

extern "C" void MH3_NetTransportConstruct(CpuContext* cpu) {
    TransportConstruct(Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CCEB8, MH3_NetTransportConstruct, "NetTransportConstruct");

extern "C" void MH3_NetTransportDestroy(CpuContext* cpu) {
    const uint32_t transport = Arg(cpu, 3);
    TransportDestroy(cpu, transport, int16_t(Arg(cpu, 4)));
    cpu->gpr[3] = transport;
}
REGISTER_NATIVE_FUNCTION_AS(0x803CCF00, MH3_NetTransportDestroy, "NetTransportDestroy");

extern "C" void MH3_NetTransportGetError(CpuContext* cpu) {
    const uint32_t transport = Arg(cpu, 3), out = Arg(cpu, 4);

    if (out) {
        Write(out, Read(transport + trError));
        Write(out + 4, Read(transport + trError + 4));
        Write(out + 8, Read(transport + trError + 8));
    }
}
REGISTER_NATIVE_FUNCTION_AS(0x803CCF44, MH3_NetTransportGetError, "NetTransportGetError");

extern "C" void MH3_NetTransportClearError(CpuContext* cpu) {
    GuestCall{cpu}.Call<Memset>({Arg(cpu, 3) + trError, 0, 0xC});
}
REGISTER_NATIVE_FUNCTION_AS(0x803CCF68, MH3_NetTransportClearError, "NetTransportClearError");

extern "C" void MH3_NetTransportSetError(CpuContext* cpu) {
    TransportError(Arg(cpu, 3), Arg(cpu, 4), Arg(cpu, 5), Arg(cpu, 6));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CCF78, MH3_NetTransportSetError, "NetTransportSetError");

extern "C" void MH3_NetLoopbackConstruct(CpuContext* cpu) {
    LoopbackConstruct(Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CCF94, MH3_NetLoopbackConstruct, "NetLoopbackConstruct");

extern "C" void MH3_NetLoopbackSend(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(LoopbackSend(cpu, Arg(cpu, 3), Arg(cpu, 4), int32_t(Arg(cpu, 5))));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CCFEC, MH3_NetLoopbackSend, "NetLoopbackSend");

extern "C" void MH3_NetLoopbackReceive(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(LoopbackReceive(cpu, Arg(cpu, 3), Arg(cpu, 4), Arg(cpu, 5), Arg(cpu, 7), Arg(cpu, 8)));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CD080, MH3_NetLoopbackReceive, "NetLoopbackReceive");

extern "C" void MH3_NetLoopbackPoll(CpuContext* cpu) {
    LoopbackClear(cpu, Arg(cpu, 3));
    cpu->gpr[3] = 1;
}
REGISTER_NATIVE_FUNCTION_AS(0x803CD198, MH3_NetLoopbackPoll, "NetLoopbackPoll");

extern "C" void MH3_NetLoopbackClose(CpuContext* cpu) {
    LoopbackClear(cpu, Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CD210, MH3_NetLoopbackClose, "NetLoopbackClose");

extern "C" void MH3_NetUdpPeerOpen(CpuContext* cpu) {
    UdpPeerOpen(Arg(cpu, 3), Arg(cpu, 4));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CD2AC, MH3_NetUdpPeerOpen, "NetUdpPeerOpen");

extern "C" void MH3_NetUdpPeerSend(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(UdpPeerSend(cpu, Arg(cpu, 3), Arg(cpu, 4), int32_t(Arg(cpu, 5)), Arg(cpu, 6), int32_t(Arg(cpu, 7)), Arg(cpu, 8)));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CD2C0, MH3_NetUdpPeerSend, "NetUdpPeerSend");

extern "C" void MH3_NetUdpPeerReceive(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(UdpPeerReceive(cpu, Arg(cpu, 3), Arg(cpu, 4), Arg(cpu, 5), Arg(cpu, 6), Arg(cpu, 7), Arg(cpu, 8)));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CD4D8, MH3_NetUdpPeerReceive, "NetUdpPeerReceive");

extern "C" void MH3_NetUdpPeerPoll(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(UdpPeerReset(cpu, Arg(cpu, 3)));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CD6B0, MH3_NetUdpPeerPoll, "NetUdpPeerPoll");

extern "C" void MH3_NetUdpPeerClose(CpuContext* cpu) {
    UdpPeerReset(cpu, Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CD73C, MH3_NetUdpPeerClose, "NetUdpPeerClose");

extern "C" void MH3_NetMcsConstruct(CpuContext* cpu) {
    const uint32_t transport = Arg(cpu, 3);
    McsConstruct(cpu, transport, Arg(cpu, 4) & 0xFF);
    cpu->gpr[3] = transport;
}
REGISTER_NATIVE_FUNCTION_AS(0x803CD824, MH3_NetMcsConstruct, "NetMcsConstruct");

extern "C" void MH3_NetMcsOpen(CpuContext* cpu) {
    McsOpen(cpu, Arg(cpu, 3), Arg(cpu, 4));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CD8B8, MH3_NetMcsOpen, "NetMcsOpen");

extern "C" void MH3_NetMcsSend(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(McsSend(cpu, Arg(cpu, 3), Arg(cpu, 4), Arg(cpu, 5)));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CD934, MH3_NetMcsSend, "NetMcsSend");

extern "C" void MH3_NetMcsReceive(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(McsReceive(cpu, Arg(cpu, 3), Arg(cpu, 4), Arg(cpu, 5), Arg(cpu, 7), Arg(cpu, 8)));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CDAE8, MH3_NetMcsReceive, "NetMcsReceive");

extern "C" void MH3_NetMcsPut(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(McsPut(cpu, Arg(cpu, 3), Arg(cpu, 4), int32_t(Arg(cpu, 5))));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CDC90, MH3_NetMcsPut, "NetMcsPut");

extern "C" void MH3_NetMcsPoll(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(McsPoll(cpu, Arg(cpu, 3)));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CDD54, MH3_NetMcsPoll, "NetMcsPoll");

extern "C" void MH3_NetMcsStop(CpuContext* cpu) {
    McsStop(Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CE00C, MH3_NetMcsStop, "NetMcsStop");

extern "C" void MH3_NetMcsClose(CpuContext* cpu) {
    McsClose(cpu, Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CE054, MH3_NetMcsClose, "NetMcsClose");

extern "C" void MH3_NetLoopbackClosed(CpuContext* cpu) {
    GuestCall{cpu}.Virtual(Arg(cpu, 3), tClose, {});
    cpu->gpr[3] = 1;
}
REGISTER_NATIVE_FUNCTION_AS(0x803CD1E0, MH3_NetLoopbackClosed, "NetLoopbackClosed");

extern "C" void MH3_NetUdpPeerClosed(CpuContext* cpu) {
    GuestCall{cpu}.Virtual(Arg(cpu, 3), tClose, {});
    cpu->gpr[3] = 1;
}
REGISTER_NATIVE_FUNCTION_AS(0x803CD70C, MH3_NetUdpPeerClosed, "NetUdpPeerClosed");

extern "C" void MH3_NetMcsClosed(CpuContext* cpu) {
    GuestCall{cpu}.Virtual(Arg(cpu, 3), tClose, {});
    cpu->gpr[3] = 1;
}
REGISTER_NATIVE_FUNCTION_AS(0x803CE024, MH3_NetMcsClosed, "NetMcsClosed");

extern "C" void MH3_NetLoopbackZero(CpuContext* cpu) {
    cpu->gpr[3] = 0;
}
REGISTER_NATIVE_FUNCTION_AS(0x803CD190, MH3_NetLoopbackZero, "NetLoopbackZero");

extern "C" void MH3_NetUdpPeerZero(CpuContext* cpu) {
    cpu->gpr[3] = 0;
}
REGISTER_NATIVE_FUNCTION_AS(0x803CD6A8, MH3_NetUdpPeerZero, "NetUdpPeerZero");

extern "C" void MH3_NetLoopbackOpen(CpuContext* cpu) {
    (void)cpu;
}
REGISTER_NATIVE_FUNCTION_AS(0x803CCFE8, MH3_NetLoopbackOpen, "NetLoopbackOpen");

extern "C" void MH3_NetLoopbackKeepalive(CpuContext* cpu) {
    (void)cpu;
}
REGISTER_NATIVE_FUNCTION_AS(0x803CD1DC, MH3_NetLoopbackKeepalive, "NetLoopbackKeepalive");

extern "C" void MH3_NetUdpPeerKeepalive(CpuContext* cpu) {
    (void)cpu;
}
REGISTER_NATIVE_FUNCTION_AS(0x803CD708, MH3_NetUdpPeerKeepalive, "NetUdpPeerKeepalive");

extern "C" void MH3_NetLoopbackDestroy(CpuContext* cpu) {
    const uint32_t transport = Arg(cpu, 3);
    TransportDestroy(cpu, transport, int16_t(Arg(cpu, 4)));
    cpu->gpr[3] = transport;
}
REGISTER_NATIVE_FUNCTION_AS(0x803CD250, MH3_NetLoopbackDestroy, "NetLoopbackDestroy");

extern "C" void MH3_NetUdpPeerDestroy(CpuContext* cpu) {
    const uint32_t transport = Arg(cpu, 3);
    TransportDestroy(cpu, transport, int16_t(Arg(cpu, 4)));
    cpu->gpr[3] = transport;
}
REGISTER_NATIVE_FUNCTION_AS(0x803CD76C, MH3_NetUdpPeerDestroy, "NetUdpPeerDestroy");

extern "C" void MH3_NetMcsDestroy(CpuContext* cpu) {
    const uint32_t transport = Arg(cpu, 3);
    TransportDestroy(cpu, transport, int16_t(Arg(cpu, 4)));
    cpu->gpr[3] = transport;
}
REGISTER_NATIVE_FUNCTION_AS(0x803CD7C8, MH3_NetMcsDestroy, "NetMcsDestroy");
