// The game's net library functions that run every frame or every packet
// (Capcom's net library, RMHP08; decompiled with Ghidra, the call arguments
// taken from the translated C++), ported natively: the platform clock and
// the library's update, the GameSpy peer transport (its think, send and
// receive buffers), the lobby layer's and the circle layer's updates and
// member lookups, the Pat lobby connection's update and command I/O, and the
// game manager's update and connection events. Their callees elsewhere in
// the library stay guest calls.
#include "abi_bridge.h"
#include "guest_call.h"
#include "memory.h"

#include <bit>
#include <cstdint>
#include <initializer_list>

namespace {

enum : uint32_t {
    Memcpy = 0x80004000,
    Memset = 0x80004350,
    Memmove = 0x8045BC18,
    OSGetTick = 0x804D53F0,
    OSLockMutex = 0x804D25A0,
    OSUnlockMutex = 0x804D2680,
    MutexUnlock = 0x803CF6B8,
    MutexLock = 0x803CF6C0,
    LockedFlag = 0x803CF6C8,
    IdConstruct = 0x803F881C,
    IdValid = 0x803F8874,
    IdEquals = 0x803F8968,
    IdDestroy = 0x803CCE5C,
    TransportSetError = 0x803CCF78,
    TcpMove = 0x803CE0C8,
    UdpMove = 0x803CE654,
    UdpRemove = 0x803CE7B0,
    ManagerUpdate = 0x803D4E48,
    SORecvFrom = 0x805207A0,
    SORecv = 0x805207C8,
    SSLRead = 0x8051C148,
    SOGetInterfaceOpt = 0x805217D8,
    NtoHs = 0x80520CDC,
    gt2Send = 0x8050E7D0,
    gt2Think = 0x8050E5A0,
    gt2Connect = 0x8050E620,
    gt2CloseConnectionHard = 0x8050E8D0,
    gt2GetSocketSOCKET = 0x8050E910,
    gt2NetworkToHostShort = 0x80512880,
    gt2AddressToString = 0x80512890,
    GSIAvailableCheckThink = 0x8050CDF0,
    NNBeginNegotiationWithSocket = 0x80513940,
    NNFreeNegotiateList = 0x805132D0,
    NNThink = 0x80514230,
};

// Singletons (r13 data).
constexpr uint32_t kPlatformPtr = 0x80797180;
constexpr uint32_t kPat = 0x80797170;     // the Pat lobby client (0xD640 bytes)
constexpr uint32_t kLibrary = 0x80797184; // the net library's system object
constexpr uint32_t kVoice = 0x80797200;
constexpr uint32_t kFmp = 0x80797178;
constexpr uint32_t kDwc = 0x80797198;
constexpr uint32_t kLayers = 0x80797190;  // the lobby layers' holder
constexpr uint32_t kGameSpy = 0x807971A4; // the GameSpy interface (0x44A0 bytes)
constexpr uint32_t kGt2Socket = 0x807971A0;
constexpr uint32_t kGt2Connections = 0x806D5AD0; // 3
constexpr uint32_t kNatResult = 0x806D5AE0;
constexpr uint32_t kNatDone = 0x806D5AE4;
constexpr uint32_t kBusClock = 0x800000F8;

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

uint8_t Byte(uint32_t address) {
    return Memory::Read8(address);
}

int8_t Signed(uint32_t address) {
    return int8_t(Memory::Read8(address));
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

uint32_t Platform() {
    return Read(kPlatformPtr);
}

double Now(CpuContext* cpu) {
    GuestCall{cpu}.Virtual(Platform(), 0x60);
    return cpu->fpr[1].d;
}

void Log(CpuContext* cpu, uint32_t slot, std::initializer_list<uint32_t> values) {
    cpu->cr &= ~kCrFloatArgs;
    GuestCall{cpu}.Virtual(Platform(), slot, values);
}

uint32_t ToHost16(CpuContext* cpu, uint32_t value) {
    return GuestCall{cpu}.Virtual(Platform(), 0x48, {value & 0xFFFF}) & 0xFFFF;
}

uint32_t ToNet16(CpuContext* cpu, uint32_t value) {
    return GuestCall{cpu}.Virtual(Platform(), 0x4C, {value & 0xFFFF}) & 0xFFFF;
}

// The platform (+0xA0 clock in seconds, +0xA4 its last tick).

// 0x80419DB8: the clock advances by the microseconds since its last tick,
// once more than a millisecond passed.
void ClockAdvance(CpuContext* cpu, uint32_t clock, uint32_t last) {
    GuestCall game{cpu};

    if (F(clock) == 0.0f && Read(last) == 0) {
        Write(last, game.Call<OSGetTick>());
        return;
    }

    const uint32_t tick = game.Call<OSGetTick>();
    const uint32_t micros = ((tick - Read(last)) << 3) / (Read(kBusClock) / 500000);

    if (micros <= 1000)
        return;

    SetF(clock, F(clock) + 1e-6f * float(double(micros)));
    Write(last, tick);
}

// 0x8041A184: the library's update: the platform (virtual 0x40), the system
// object (0xC), the Pat client, then the flagged layers (0x18).
void LibraryUpdate(CpuContext* cpu, uint32_t library) {
    GuestCall game{cpu};

    if (Platform())
        game.Virtual(Platform(), 0x40);

    if (Read(kLibrary))
        game.Virtual(Read(kLibrary), 0xC);

    if (Read(kPat))
        game.Call<0x803FD3A8>({Read(kPat)});

    const uint32_t flags = Read(library + 0x10);

    for (uint32_t i = 0; i < 4; ++i)
        if ((flags & (1u << i)) && Read(library + i * 4))
            game.Virtual(Read(library + i * 4), 0x18);
}

// The system object (0x80797184): four members at +0x6DB0 (8 bytes each:
// present, ready), their states at +0x6DA8.

// 0x804177D4 / 0x80417888.
uint32_t MemberReady(uint32_t system, int32_t member) {
    if (member < 0 || member > 3)
        return 0;

    const uint32_t entry = system + uint32_t(member) * 8;
    return Byte(entry + 0x6DB0) ? Byte(entry + 0x6DB1) : 0;
}

uint32_t MemberActive(uint32_t system, int32_t member) {
    if (member < 0 || member > 3 || !Byte(system + uint32_t(member) * 8 + 0x6DB0))
        return 0;

    return (Byte(system + uint32_t(member) + 0x6DA8) & 0xF) != 0;
}

// 0x80417790: a present member's ready flag; clearing it notifies.
void SetMemberReady(CpuContext* cpu, uint32_t system, int32_t member, uint8_t ready) {
    if (member < 0 || member > 3)
        return;

    const uint32_t entry = system + uint32_t(member) * 8;

    if (!Byte(entry + 0x6DB0))
        return;

    Memory::Write8(entry + 0x6DB1, ready);

    if (!Byte(entry + 0x6DB1))
        GuestCall{cpu}.Call<0x804171D4>({system, uint32_t(member), uint32_t(ready), entry});
}

// 0x80416D54 / 0x80417728 / 0x80416E68: the voice object's state.
uint32_t VoiceState(CpuContext* cpu, uint32_t function) {
    if (!Read(kVoice))
        return 0;

    return GuestCall{cpu}(function, {Read(kVoice)});
}

// The GameSpy interface (0x807971A4): members' ids at +0x44 (count +0x28),
// their states at +0x54, the NAT negotiation at +0x446C.., its mutex +0x4450.

// 0x8041CE6C: a member's state by id.
uint32_t PeerState(uint32_t gs, uint32_t id) {
    for (uint32_t i = 0; i < Byte(gs + 0x28); ++i)
        if (Read(gs + i * 4 + 0x44) == id)
            return Byte(gs + i + 0x54);

    if (Byte(gs + 0x446D) && id != Read(gs + 0x30) && (id == Read(gs + 0x4470) || id == Read(gs + 0x4474)))
        return Byte(gs + 0x446E);

    return 0;
}

// 0x8041CEEC: a member's slot by id, or -1.
int32_t PeerSlot(uint32_t gs, uint32_t id) {
    for (int32_t i = 0; int32_t(int8_t(i)) < int32_t(Byte(gs + 0x28)); ++i)
        if (Read(gs + uint32_t(i) * 4 + 0x44) == id)
            return i;

    return -1;
}

// 0x8041C1F8: a received packet to the slot's peer (virtual 0x18).
void PeerReceived(CpuContext* cpu, uint32_t gs, uint32_t slot, uint32_t data, uint32_t length) {
    const uint32_t peer = Read(gs + uint32_t(Byte(gs + (slot & 0xFF) + 0x24)) * 4 + 0x14);

    if (peer)
        GuestCall{cpu}.Virtual(peer, 0x18, {data, length, 0, 0, 0});
}

// 0x8041BD28: gt2's receive callback: the connection's slot.
void ReceivedCallback(CpuContext* cpu, uint32_t connection, uint32_t data, int32_t length) {
    if (length < 1) {
        Log(cpu, 0xC, {3, 0x80603B80});
        return;
    }

    if (!Read(kGameSpy)) {
        Log(cpu, 0xC, {3, 0x80603A90});
        return;
    }

    for (uint32_t slot = 0; slot < 3; ++slot) {
        if (Read(kGt2Connections + slot * 4) == connection) {
            PeerReceived(cpu, Read(kGameSpy), slot, data, uint32_t(length));
            return;
        }
    }
}

// 0x8041D6A0: an unreliable gt2 send to a slot (under the mutex when the
// interface runs threaded); the bytes sent or 0.
uint32_t SendUnreliable(CpuContext* cpu, uint32_t gs, uint32_t slot, uint32_t data, uint32_t length) {
    GuestCall game{cpu};

    if (ReadInt(gs + 0x68) < 0 || ReadInt(gs + 0x2C) != 1 || ReadInt(gs + 0x74) < 1) {
        Log(cpu, 0x14, {0x80603DB4, length});
        return 0;
    }

    const bool locked = Byte(gs + 0x444C) != 0;
    uint32_t sent = 0;

    if (locked)
        game.Call<OSLockMutex>({gs + 0x4450});

    if (const uint32_t connection = Read(kGt2Connections + (slot & 0xFF) * 4)) {
        game.Call<gt2Send>({connection, data, length, 0});
        sent = length;
    }

    if (locked)
        game.Call<OSUnlockMutex>({gs + 0x4450});

    return sent;
}

// 0x8041CF28: the interface's think: the availability check, the NAT
// negotiation toward the session host and gt2 (under the mutex); then the
// closed members' connections and a pending update.
void GameSpyThink(CpuContext* cpu, uint32_t gs) {
    GuestCall game{cpu};

    if (Read(gs + 0x6C) && ReadInt(gs + 0x2C) >= 0) {
        game.Call<OSLockMutex>({gs + 0x4450});

        if (ReadInt(gs + 0x70) == 1 && ReadInt(gs + 0x2C) == 0) {
            const int32_t available = int32_t(game.Call<GSIAvailableCheckThink>());

            if (available)
                game.Call<0x8041C2A4>({gs, available == 1 ? 0u : 0xFFFFD25Fu, 0});
        }

        if (Read(gs + 0x68) == 0 && ReadInt(gs + 0x2C) == 1 && ReadInt(gs + 0x74) == 1 && Read(kGt2Socket)) {
            bool think = true;

            if (Byte(gs + 0x446C)) {
                const uint8_t step = Byte(gs + 0x4480);

                if (step == 0) {
                    const uint32_t host = Read(gs + 0x4470);
                    const uint32_t self = Read(gs + 0x30);
                    Write(gs + 0x4478, ((host - self) | (self - host)) >> 31);

                    if (!Read(kNatDone)) {
                        Write(gs + 0x447C, host ^ Read(gs + 0x4474));
                        const uint32_t socket = game.Call<gt2GetSocketSOCKET>({Read(kGt2Socket)});
                        const int32_t started = int32_t(game.Call<NNBeginNegotiationWithSocket>({socket, Read(gs + 0x447C), Read(gs + 0x4478), 0x8041B700, 0x8041B7C8, kNatResult}));

                        if (started) {
                            int32_t code = 0;

                            if (started == 1)
                                code = -0x2DA3;
                            else if (started == 2)
                                code = -0x2DA4;
                            else if (started == 3)
                                code = -0x2DA5;

                            Write(kNatDone, 1);
                            Write(kNatResult, 0);
                            game.Call<0x8041D678>({gs, 0x80000007, 0x5F, uint32_t(-code)});
                        }

                        Memory::Write8(gs + 0x4484, 1);
                        Memory::Write8(gs + 0x4480, 1);
                    } else {
                        Memory::Write8(gs + 0x4480, 2);
                    }
                } else if (step == 1) {
                    if (Read(kNatDone)) {
                        game.Call<NNFreeNegotiateList>();
                        Memory::Write8(gs + 0x4484, 0);
                        Memory::Write8(gs + 0x4480, 2);
                    }
                } else if (step == 2) {
                    bool connected = false;

                    if (!Read(kNatResult)) {
                        Memory::Write8(gs + 0x446E, 0xFF);
                        Memory::Write8(gs + 0x446D, 1);
                    } else if (Read(gs + 0x4478) == 1) {
                        GuestFrame frame(cpu, 0x30);
                        const uint32_t connection = frame[0x8];
                        const uint32_t port = game.Call<gt2NetworkToHostShort>({Memory::Read16(kNatResult + 0xA)});
                        const uint32_t address = game.Call<gt2AddressToString>({Read(kNatResult + 0xC), port & 0xFFFF, 0});

                        if (game.Call<gt2Connect>({Read(kGt2Socket), connection, address, gs + 0x4485, 0x14, 10000, 0x80603A60, 0}) == 0) {
                            Memory::Write8(gs + 0x4480, 3);
                            connected = true;
                        } else {
                            game.Call<0x8041D678>({gs, 0x80000007, 0x5F, 0x2DAD});
                            Memory::Write8(gs + 0x446E, 0xFF);
                            Memory::Write8(gs + 0x446D, 1);
                        }
                    }

                    if (!connected)
                        Memory::Write8(gs + 0x446C, 0);
                } else if (step == 3 && Byte(gs + 0x446E)) {
                    Memory::Write8(gs + 0x446C, 0);
                }
            }

            if (think) {
                game.Call<NNThink>();
                game.Call<gt2Think>({Read(kGt2Socket)});
            }
        }

        game.Call<OSUnlockMutex>({gs + 0x4450});
    }

    if (Byte(gs + 0x123)) {
        Memory::Write8(gs + 0x123, 0);

        if (Read(gs + 0x68) == 0 && ReadInt(gs + 0x2C) > 0 && ReadInt(gs + 0x74) > 0)
            game.Call<0x8041C8B4>({gs});
    }

    for (uint32_t i = 0; int32_t(i) < int32_t(Byte(gs + 0x28)); ++i) {
        const uint32_t closing = gs + i * 4 + 0x58;

        if (!Read(closing))
            continue;

        const int32_t slot = int8_t(PeerSlot(gs, Read(closing)));

        if (slot >= 0) {
            if (const uint32_t connection = Read(kGt2Connections + uint32_t(slot) * 4))
                game.Call<gt2CloseConnectionHard>({connection});

            Write(gs + uint32_t(slot) * 4 + 0x44, 0);
            Memory::Write8(gs + uint32_t(slot) + 0x54, 0);
        }

        Write(closing, 0);
    }

    if (Byte(gs + 0x122)) {
        if (Read(gs + 0x68)) {
            Memory::Write8(gs + 0x122, 0);
        } else if (!Read(gs + 0x6C)) {
            Memory::Write8(gs + 0x122, 0);
        } else if (ReadInt(gs + 0x70) == 1 && (ReadInt(gs + 0x2C) == 2 || ReadInt(gs + 0x2C) == -2)) {
            Memory::Write8(gs + 0x122, 0);
            game.Call<0x8041C85C>({gs});
        }
    }
}

// The GameSpy peer transport: its interface (+0x6634) and member id
// (+0x6638), the send frame at +0x14, the receive buffer at +0x614 (length
// +0x10, mutex +0x6614).

// 0x8041DC54 (virtual 0x10): a frame [u16 length, data, u16 length + 1,
// flag, data] to the member.
int32_t PeerSend(CpuContext* cpu, uint32_t transport, uint32_t data, int32_t length, uint32_t data2, int32_t length2, uint32_t flag) {
    GuestCall game{cpu};
    const uint32_t gs = Read(transport + 0x6634), id = Read(transport + 0x6638);

    if (int8_t(PeerState(gs, id)) < 0) {
        game.Call<TransportSetError>({transport, 0x80030021, 0, 0xFFFFFFFF});
        return -1;
    }

    uint32_t at = transport + 0x14;
    int32_t total;
    uint32_t first = 0, second = 0;

    if (!data || length < 1) {
        Memory::Write16(at, 0);
        at += 2;
        total = 2;
    } else {
        first = ToNet16(cpu, uint32_t(length));
        Memory::Write16(at, uint16_t(first));
        game.Call<Memcpy>({at + 2, data, uint32_t(length) & 0xFFFF});
        at += 2 + uint32_t(length);
        total = length + 2;
    }

    if (!data2 || length2 < 1) {
        Memory::Write16(at, 0);
        total += 2;
    } else {
        second = ToNet16(cpu, uint32_t(length2 + 1));
        Memory::Write16(at, uint16_t(second));
        Memory::Write8(at + 2, uint8_t(flag));
        game.Call<Memcpy>({at + 3, data2, uint32_t(length2) & 0xFFFF});
        total += 3 + length2;
    }

    if (!ToHost16(cpu, first) && !ToHost16(cpu, second))
        return 0;

    const int32_t slot = int8_t(PeerSlot(gs, id));

    if (slot < 0) {
        game.Call<TransportSetError>({transport, 0x80030021, 0, 0xFFFFFFFE});
        return -1;
    }

    if (!SendUnreliable(cpu, gs, uint32_t(slot), transport + 0x14, uint32_t(total))) {
        game.Call<TransportSetError>({transport, 0x80030021, 0, 0xFFFFFFFD});
        return -1;
    }

    return total;
}

// 0x8041DEA4 (virtual 0x14): the next frame from the receive buffer.
int32_t PeerReceive(CpuContext* cpu, uint32_t transport, uint32_t to, uint32_t length, uint32_t to2, uint32_t length2, uint32_t flag) {
    GuestCall game{cpu};
    const int32_t room = ReadInt(length), room2 = ReadInt(length2);
    Write(length, 0);
    Write(length2, 0);
    Memory::Write8(flag, 0);

    if (int8_t(PeerState(Read(transport + 0x6634), Read(transport + 0x6638))) < 0) {
        game.Call<TransportSetError>({transport, 0x80030022, 0, 0});
        return -1;
    }

    if (Read(transport + 0x10) < 4)
        return 0;

    const uint32_t buffer = transport + 0x614;
    game.Call<MutexLock>({transport + 0x6614});
    const uint32_t first = ToHost16(cpu, Memory::Read16(buffer));
    uint32_t second = 0;

    if (first + 4 <= Read(transport + 0x10))
        second = ToHost16(cpu, Memory::Read16(buffer + first + 2));

    if (Read(transport + 0x10) < first + second + 4) {
        game.Call<MutexUnlock>({transport + 0x6614});
        return 0;
    }

    uint32_t at = buffer + 2;

    if (first && int32_t(first) <= room) {
        game.Call<Memcpy>({to, at, first});
        Write(length, first);
    }

    at += first + 2;

    if (second > 1 && int32_t((second - 1) & 0xFFFF) <= room2) {
        const uint32_t count = (second - 1) & 0xFFFF;
        game.Call<Memcpy>({flag, at, 1});
        game.Call<Memcpy>({to2, at + 1, count});
        at += 1 + count;
        Write(length2, count);
    }

    const int32_t used = int32_t(first + second + 4);
    const int32_t left = ReadInt(transport + 0x10) - used;
    Write(transport + 0x10, uint32_t(left));

    if (left)
        game.Call<Memmove>({buffer, at, uint32_t(left)});

    game.Call<MutexUnlock>({transport + 0x6614});
    return used;
}

// 0x8041E0BC: bytes into the receive buffer (0x6000 at most).
int32_t PeerPut(CpuContext* cpu, uint32_t transport, uint32_t data, uint32_t length) {
    GuestCall game{cpu};
    game.Call<MutexLock>({transport + 0x6614});

    if (Read(transport + 0x10) + length > 0x6000) {
        Log(cpu, 0x14, {0x80603F70});
        game.Call<MutexUnlock>({transport + 0x6614});
        return -1;
    }

    game.Call<Memcpy>({transport + Read(transport + 0x10) + 0x614, data, length});
    Write(transport + 0x10, Read(transport + 0x10) + length);
    game.Call<MutexUnlock>({transport + 0x6614});
    return 0;
}

// 0x80413F9C: whether the network interface is up (SO option 0x1005); a
// stopped one reports 0x80000008/0x5D, a failed query 0x80000008/0/code.
bool InterfaceUp(CpuContext* cpu, uint32_t system, uint32_t error) {
    GuestFrame frame(cpu, 0x20);
    const uint32_t up = frame[0xC], size = frame[0x8];
    Write(up, 0);
    Write(size, 4);

    if (!Byte(system + 0x21)) {
        const int32_t result = int32_t(GuestCall{cpu}.Call<SOGetInterfaceOpt>({0, 0xFFFE, 0x1005, up, size}));

        if ((result < 0 || !Read(up)) && error) {
            Write(error, 0x80000008);
            Write(error + 4, 0);
            Write(error + 8, uint32_t(result));
        }
    } else if (error) {
        Write(error, 0x80000008);
        Write(error + 4, 0x5D);
        Write(error + 8, 0);
    }

    return Read(up) == 1;
}

// 0x80413D70: the system object's update: FMP, voice, then DWC or one of
// its two connection steps.
void SystemUpdate(CpuContext* cpu, uint32_t system) {
    GuestCall game{cpu};

    if (Read(kFmp))
        game.Call<0x80412CC0>({Read(kFmp)});

    if (Read(kVoice))
        game.Call<0x804473A0>({Read(kVoice)});

    if (Byte(system + 0x1D))
        game.Call<0x80417DD8>({system});
    else if (Byte(system + 0x1C))
        game.Call<0x804178DC>({system});
    else if (Read(kDwc))
        game.Call<0x8041A878>({Read(kDwc)});
}

// NetworkSocketWii (+0xC socket, +0x14 kind: 1 TCP, 2 UDP, 4 SSL; +0x18 the
// SSL context; +4 / +8 / +0x10 its error).

// 0x803F82D0: received bytes (UDP: the sender's address and port into `from`),
// 0 when nothing waits, -1 on an error or a closed stream.
int32_t SocketReceive(CpuContext* cpu, uint32_t socket, uint32_t to, int32_t length, uint32_t from) {
    GuestCall game{cpu};
    int32_t got = -1;
    const uint32_t kind = Read(socket + 0x14);

    if (length < 0) {
        Write(socket + 8, 0);
        Write(socket + 4, 0x80010003);
        return -1;
    }

    GuestFrame frame(cpu, 0x30);
    const uint32_t address = frame[0x8];

    if (kind == 2) {
        if (!from) {
            Write(socket + 8, 0);
            Write(socket + 4, 0x80010003);
            return -1;
        }

        game.Call<Memset>({address, 0, 8});
        Memory::Write8(address, 8);
        got = int32_t(game.Call<SORecvFrom>({Read(socket + 0xC), to, uint32_t(length), 0, address}));

        if (got == -6)
            return 0;
    }

    if (kind == 1) {
        got = int32_t(game.Call<SORecv>({Read(socket + 0xC), to, uint32_t(length), 0}));

        if (got == -6)
            return 0;
    } else if (kind == 4) {
        got = int32_t(game.Call<SSLRead>({Read(socket + 0x18), to, uint32_t(length)}));

        if (got == -2)
            return 0;
    }

    if (got < 0) {
        Write(socket + 8, uint32_t(got));
        Write(socket + 4, game.Call<0x803F77CC>({socket, uint32_t(got)}));
        Write(socket + 0x10, 0);
        return -1;
    }

    if (kind == 2) {
        if (got == 0)
            return 0;

        game.Call<Memcpy>({from, address + 4, 4});
        Memory::Write16(from + 4, uint16_t(game.Call<NtoHs>({Memory::Read16(address + 2)})));
    }

    if ((kind == 1 || kind == 4) && got == 0) {
        Write(socket + 8, 0);
        Write(socket + 4, 0x80010015);
        Write(socket + 0x10, 0);
        return -1;
    }

    return got;
}

// The Pat connection (+0x94 its socket, +0x98 open, +0x9C/+0xA0 the send
// command, +0x20A0/+0x20A8 the receive buffer, +0x60AA the command header).

void PatFail(uint32_t error, uint32_t code, uint32_t value) {
    if (Read(error))
        return;

    Write(error, 0x80000008);
    Write(error + 4, code);
    Write(error + 8, value);
}

// 0x803FB698: socket bytes into the receive buffer.
int32_t PatReceive(CpuContext* cpu, uint32_t pat, uint32_t error) {
    GuestCall game{cpu};
    const uint32_t socket = Read(pat + 0x94);

    if (!socket) {
        PatFail(error, 0, 0);
        Log(cpu, 0x10, {0x80600E38});
        Memory::Write8(pat + 0x98, 0);
        return -1;
    }

    const int32_t got = int32_t(game.Virtual(socket, 0x3C, {pat + Read(pat + 0x20A0) + 0x20A8, 0x4000 - Read(pat + 0x20A0), 0}));

    if (got < 0) {
        if (!Read(error))
            PatFail(error, 99, game.Call<0x803F75A4>({Read(pat + 0x94)}));

        const uint32_t code = game.Call<0x803F75A4>({Read(pat + 0x94)});
        Log(cpu, 0x10, {0x80600E68, code});
        Memory::Write8(pat + 0x98, 0);
    } else {
        Write(pat + 0x20A0, Read(pat + 0x20A0) + uint32_t(got));
    }

    return got;
}

// 0x803FBA14: the waiting command to the socket.
int32_t PatSend(CpuContext* cpu, uint32_t pat, uint32_t error) {
    GuestCall game{cpu};

    if (!Read(pat + 0x9C))
        return 0;

    const uint32_t socket = Read(pat + 0x94);

    if (!socket) {
        PatFail(error, 0, 0);
        Log(cpu, 0x10, {0x80600EB4});
        Memory::Write8(pat + 0x98, 0);
        return -1;
    }

    const int32_t sent = int32_t(game.Virtual(socket, 0x38, {pat + 0xA0, Read(pat + 0x9C), 0}));

    if (sent < 0) {
        if (!Read(error))
            PatFail(error, 100, game.Call<0x803F75A4>({Read(pat + 0x94)}));

        const uint32_t code = game.Call<0x803F75A4>({Read(pat + 0x94)});
        Log(cpu, 0x10, {0x80600EE0, code});
        Memory::Write8(pat + 0x98, 0);
        return sent;
    }

    Write(pat + 0x9C, 0);
    Write(pat + 0x60BC, pat + 0xA0);
    return 0;
}

// 0x803FB7E0: the complete commands in the receive buffer, each [u16 length,
// 6 header bytes, payload] (decrypted when the link is encrypted), to the
// handler (virtual 0x28C); then the next step (0x803FC5AC).
int32_t PatCommands(CpuContext* cpu, uint32_t pat, uint32_t error) {
    GuestCall game{cpu};
    const uint32_t header = pat + 0x60AA;
    const uint32_t buffer = pat + 0x20A8;

    for (;;) {
        if (Read(pat + 0x20A0) < 8)
            return 0;

        game.Call<Memcpy>({header, buffer, 8});
        const uint32_t length = ToHost16(cpu, Memory::Read16(header));

        if (Read(pat + 0x20A0) < length + 8)
            return 0;

        Write(pat + 0x60B4, pat + 0x20B0);

        if (Byte(pat + 0x60D0) && length) {
            GuestFrame frame(cpu, 0x20);
            const uint32_t size = frame[0x8];
            Memory::Write16(size, uint16_t(length));
            const int32_t result = int32_t(game.Call<0x804244C0>({pat + 0x20B0, size}));

            if (result < 0) {
                Log(cpu, 0x10, {0x80600E90, Byte(pat + 0x60AE), Byte(pat + 0x60AF)});

                if (!Read(error)) {
                    Write(error, 0x80000000);
                    Write(error + 4, 0x66);
                    Write(error + 8, uint32_t(result));
                }

                return -1;
            }

            Memory::Write16(header, uint16_t(ToNet16(cpu, Memory::Read16(size))));
        }

        const int32_t handled = int32_t(game.Virtual(pat, 0x28C, {header}));
        const uint32_t left = Read(pat + 0x20A0) - (length + 8);
        Write(pat + 0x20A0, left);

        if (left)
            game.Call<Memmove>({buffer, pat + length + 0x20B0, left});

        if (handled == -1) {
            if (!Read(error)) {
                Write(error, 0x80000000);
                Write(error + 4, 0x67);
                Write(error + 8, 0);
            }

            return -1;
        }

        if (handled == -2) {
            if (!Read(error)) {
                Write(error, 0x80000004);
                Write(error + 4, 0);
                Write(error + 8, 0);
            }

            return -1;
        }

        if (int32_t(game.Call<0x803FC5AC>({pat})) < 0)
            break;
    }

    if (!Read(error)) {
        Write(error, 0x80000000);
        Write(error + 4, 0x69);
        Write(error + 8, 0);
    }

    return -1;
}

// 0x803FD3A8: the Pat client's update: its clock, then by its state the
// connect, the command exchange (with a 90 s silence timeout) or the close.
void PatUpdate(CpuContext* cpu, uint32_t pat) {
    GuestCall game{cpu};
    SetF(pat + 0x611C, float(Now(cpu)));

    if (Byte(pat + 0x6131)) {
        game.Call<0x80412760>({pat});
        return;
    }

    if (Byte(pat + 0x6130)) {
        game.Call<0x8041260C>({pat});
        return;
    }

    const uint32_t error = pat + 0x654C;
    const uint8_t step = Byte(pat + 0x6134);
    GuestFrame frame(cpu, 0x50);
    const uint32_t status = frame[0x38], report = frame[0x20];

    if (step == 0) {
        if (game.Call<0x803FD6C8>({pat}) && !InterfaceUp(cpu, Read(kLibrary), status) && !Read(error)) {
            for (uint32_t i = 0; i < 12; i += 4)
                Write(report + i, Read(status + i));

            game.Virtual(pat, 0x290, {report});
        }
    } else if (step == 10) {
        int32_t result = PatReceive(cpu, pat, error);

        if (result > 0)
            Write(pat + 0x6124, Read(pat + 0x611C));

        if (result >= 0 && (result = PatCommands(cpu, pat, error)) == 0)
            result = PatSend(cpu, pat, error);

        if (result < 0) {
            Memory::Write8(pat + 0x6134, 0x5A);
        } else if (!InterfaceUp(cpu, Read(kLibrary), status)) {
            for (uint32_t i = 0; i < 12; i += 4)
                Write(report + i, Read(status + i));

            game.Virtual(pat, 0x290, {report});
        } else if (F(pat + 0x6124) < F(pat + 0x611C) - F(0x8079EC94)) {
            Log(cpu, 0x10, {0x80601228, 0x5A});
            Write(report, 0x80000000);
            Write(report + 4, 0x6B);
            Write(report + 8, 0x5A);
            game.Virtual(pat, 0x290, {report});
        }
    } else if (step == 0x5A) {
        Memory::Write8(pat + 0x6559, 0);

        if (!game.Call<0x803FB4F4>({pat}))
            return;

        if (!Byte(pat + 0x6558)) {
            if (Read(pat + 0x65F0) == 2 && Byte(pat + 0x6132) == 1) {
                game.Call<0x803FDDCC>({pat, 2, 0});
                game.Call<0x803FDD34>({pat, 2});
                game.Call<Memset>({error, 0, 12});
                Memory::Write8(pat + 0x6134, 0);
                Memory::Write8(pat + 0x6130, 1);
                Memory::Write8(pat + 0x6132, 0);
                return;
            }

            if (Read(pat + 0x65F0) == 1 && ReadInt(pat + 0x8044) != -1) {
                game.Call<0x803FE130>({pat});
                Memory::Write8(pat + 0x6134, 0);
                Memory::Write8(pat + 0x6130, 1);
                return;
            }
        }

        game.Call<0x804127C0>({pat, Byte(pat + 0x6136) ? 0x8006u : 0x8000u, 0, Read(error), 1, error});
        Memory::Write8(pat + 0x6134, 0);
    }
}

// The lobby layer (circle members at +0x356C, 0x38 each, present +0x35A1;
// this console's member +0xC07C; flags per member +0xC084/+0xC0E8/+0xC14C;
// session peers +0xEF00).

constexpr uint32_t kMembers = 100;

bool Present(uint32_t layer, uint32_t member) {
    return Byte(layer + member * 0x38 + 0x35A1) != 0;
}

int32_t SelfMember(uint32_t layer) {
    return Signed(layer + 0xC07C);
}

// 0x803EEAA8: a member's link flags (bit 1: the layer is connected).
uint32_t MemberFlags(CpuContext* cpu, uint32_t layer, uint8_t member) {
    if (member >= kMembers || !Present(layer, member))
        return 0;

    const uint32_t own = Byte(layer + uint32_t(SelfMember(layer)) + 0xC084);
    const bool linked = layer != 0xFFFF3F18 && GuestCall{cpu}.Virtual(layer, 0xE4, {uint32_t(int32_t(int8_t(member)))}) != 0;
    return own | (linked ? 2u : 0u);
}

// 0x803EE804: a member's ready flag to the system object.
void SetReady(CpuContext* cpu, uint32_t layer, uint8_t member, uint32_t ready) {
    if (member >= kMembers || !Present(layer, member))
        return;

    const uint32_t before = MemberFlags(cpu, layer, member);
    SetMemberReady(cpu, Read(kLibrary), int8_t(member), uint8_t(ready));

    if (int8_t(member) == SelfMember(layer)) {
        const bool linked = layer != 0xFFFF3F18 && GuestCall{cpu}.Virtual(layer, 0xE4, {uint32_t(int32_t(int8_t(member))), ready}) != 0;
        Memory::Write8(layer + uint32_t(SelfMember(layer)) + 0xC0E8, linked ? 1 : 0);
    } else if (before != MemberFlags(cpu, layer, member)) {
        GuestCall{cpu}.Call<0x803EEB5C>({layer, uint32_t(int32_t(int8_t(member))), 0});
    }
}

// 0x803EE90C / 0x803EEA0C.
uint32_t MemberTalking(uint32_t layer, uint8_t member) {
    if (member >= kMembers)
        return 0;

    return MemberReady(Read(kLibrary), int8_t(member)) && Byte(layer + int8_t(member) + 0xC14C);
}

uint32_t MemberLinked(uint32_t layer, uint8_t member) {
    if (member >= kMembers || !Present(layer, member))
        return 0;

    return Byte(layer + int8_t(member) + 0xC0E8);
}

// 0x803EF074: the member using session peer `peer`, or -1.
int32_t MemberOfPeer(uint32_t layer, int8_t peer) {
    for (uint32_t member = 0; member < kMembers; ++member)
        if (Present(layer, member) && Signed(layer + uint32_t(int8_t(member)) + 0xEF00) == peer)
            return int32_t(member);

    return -1;
}

// 0x803EF19C: the member with an id, or -1.
int32_t MemberOfId(CpuContext* cpu, uint32_t layer, uint32_t id) {
    if (!id)
        return -1;

    for (uint32_t member = 0; member < kMembers; ++member)
        if (Present(layer, member) && GuestCall{cpu}.Call<IdEquals>({id, layer + member * 0x38 + 0x356C}))
            return int32_t(member);

    return -1;
}

// 0x803EF238: the session peer of the member with an id, or -1.
uint32_t PeerOfId(CpuContext* cpu, uint32_t layer, uint32_t id) {
    const int32_t member = int8_t(MemberOfId(cpu, layer, id));
    return member < 0 ? 0xFFFFFFFF : Byte(layer + uint32_t(member) + 0xEF00);
}

// 0x803EF284: the id of the member using a session peer (reset first).
void IdOfPeer(CpuContext* cpu, uint32_t layer, int8_t peer, uint32_t id) {
    if (!id)
        return;

    GuestCall game{cpu};
    game.Virtual(id, 0x18);
    const int32_t member = int8_t(MemberOfPeer(layer, peer));

    if (member >= 0)
        game.Virtual(id, 0x28, {layer + uint32_t(member) * 0x38 + 0x356C});
}

// A request table's update (0x803EFA60: 12 at +0xC, 0x803F0EB0: 9 at
// +0x184): each request steps and its slot frees when it is done.
void StepTable(CpuContext* cpu, uint32_t layer, uint32_t slots, uint32_t count, uint32_t step, uint32_t active, uint32_t release) {
    GuestCall game{cpu};

    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t slot = layer + slots + i * 4;

        if (const uint32_t request = Read(slot)) {
            game(step, {request});

            if (!game(active, {request}))
                game(release, {layer, slot});
        }
    }
}

// 0x803F0CC4: the circle layer's update: its requests, then (with the Pat
// client up) a pending binary sent at most every ec68 seconds.
void CircleUpdate(CpuContext* cpu, uint32_t layer) {
    GuestCall game{cpu};
    StepTable(cpu, layer, 0xC, 12, 0x803EFAF4, 0x803EFAE0, 0x803F0288);
    StepTable(cpu, layer, 0x184, 9, 0x803F0F30, 0x803F0E2C, 0x803F0E40);

    if (!Read(kPat) || !game.Call<LockedFlag>({Read(kPat)}) || !game.Call<0x803F0DB0>({Read(kPat)}))
        return;

    if (!Byte(layer + 0x39D) || Read(layer + 0x34))
        return;

    if (!(double(F(layer + 0x398) + F(0x8079EC68)) < Now(cpu)))
        return;

    SetF(layer + 0x398, float(Now(cpu)));
    const uint32_t at = Read(layer + 0x3F8);
    game.Call<0x804039AC>({Read(kPat), at, layer + at + 0x400, Read(layer + 0x3FC) & 0xFFFF});
    Memory::Write8(layer + 0x39D, 0);
}

// 0x803E25E0: the lobby layer's request table (21 at +0xC with flags at
// +0x60, one more at +0x16C with its flag at +0x1C4): a waiting request
// starts when the others allow it, a running one steps until it is done.
bool LobbyRunning(uint32_t layer, uint32_t kind) {
    return Byte(layer + (kind < 21 ? 0x60 + kind : 0x1AF + kind)) != 0;
}

bool LobbyMayStart(CpuContext* cpu, uint32_t layer, uint32_t kind) {
    if (kind >= 16)
        return true;

    switch (kind) {
    case 1:
        return !LobbyRunning(layer, 2);

    case 2: {
        uint32_t first = 0;

        while (first < 22 && !LobbyRunning(layer, first))
            ++first;

        if (first != 22) {
            Log(cpu, 0x14, {0x805FBF44, first});
            return false;
        }

        return true;
    }

    case 4:
    case 5:
    case 6:
    case 15:
        return !LobbyRunning(layer, 7) && !LobbyRunning(layer, 8) && !LobbyRunning(layer, 9) && !LobbyRunning(layer, 10) && !LobbyRunning(layer, 14);

    case 7:
    case 8:
    case 9:
    case 10:
    case 14:
        return !LobbyRunning(layer, 4) && !LobbyRunning(layer, 5) && !LobbyRunning(layer, 6) && !LobbyRunning(layer, 15);

    default:
        return true;
    }
}

void LobbyRequests(CpuContext* cpu, uint32_t layer) {
    GuestCall game{cpu};

    for (uint32_t kind = 0; kind < 22; ++kind) {
        const uint32_t slot = layer + (kind < 21 ? 0xC : 0x16C) + kind * 4;
        const uint32_t flag = layer + (kind < 21 ? 0x60 + kind : 0x1AF + kind);

        if (Read(slot) && !Byte(flag)) {
            if (kind != 2 && Read(layer + 0x14))
                continue;

            if (!LobbyMayStart(cpu, layer, kind))
                continue;

            Memory::Write8(flag, 1);
        }

        const uint32_t request = Read(slot);

        if (!request || !Byte(flag))
            continue;

        if (kind < 21) {
            game.Call<0x803DF9B0>({request});

            if (!game.Call<0x803DF99C>({Read(slot)})) {
                game.Call<0x803E0BC4>({layer, slot});
                Memory::Write8(flag, 0);
            }
        } else {
            game.Call<0x803E28A0>({request});

            if (!game.Call<0x803E255C>({Read(slot)})) {
                game.Call<0x803E2570>({layer, layer + 0x1C0});
                Memory::Write8(flag, 0);
            }
        }
    }
}

// The game manager (vtable 0x805FB8B0): members at +0x538 (0x48 each, present
// +0x538, joined +0x539, id +0x540), the TCP and UDP sockets at +0x658/+0x65C.

// 0x803DC04C: a member's id into `id`.
bool MemberId(CpuContext* cpu, uint32_t manager, uint8_t member, uint32_t id) {
    if (member >= 4)
        return false;

    const uint32_t entry = manager + uint32_t(int8_t(member)) * 0x48;

    if (!Byte(entry + 0x538) || !id)
        return false;

    GuestCall{cpu}.Virtual(id, 0x28, {entry + 0x540});
    return true;
}

uint32_t Layer(CpuContext* cpu) {
    const uint32_t layers = Read(kLayers);
    return layers ? GuestCall{cpu}.Call<0x8041A5E4>({layers, 0}) : 0;
}

// 0x803DE5D0: a member's session peer, or -1.
uint32_t MemberPeer(CpuContext* cpu, uint32_t manager, uint8_t member) {
    if (member >= 4 || !Byte(manager + uint32_t(int8_t(member)) * 0x48 + 0x538))
        return 0xFFFFFFFF;

    const uint32_t layer = Layer(cpu);
    return layer ? PeerOfId(cpu, layer, manager + uint32_t(int8_t(member)) * 0x48 + 0x540) : 0xFFFFFFFF;
}

// 0x803DE658: the member using a session peer, or -1.
int32_t PeerMember(CpuContext* cpu, uint32_t manager, int8_t peer) {
    const uint32_t layer = Layer(cpu);

    if (!layer)
        return -1;

    GuestCall game{cpu};
    GuestFrame frame(cpu, 0x40);
    const uint32_t id = frame[0x8];
    game.Call<IdConstruct>({id});
    IdOfPeer(cpu, Layer(cpu), peer, id);
    int32_t found = -1;

    if (game.Call<IdValid>({id})) {
        for (uint32_t member = 0; member < 4; ++member) {
            const uint32_t entry = manager + member * 0x48;

            if (Byte(entry + 0x538) && game.Call<IdEquals>({id, entry + 0x540})) {
                found = int32_t(member);
                break;
            }
        }
    }

    game.Call<IdDestroy>({id, 0xFFFFFFFF});
    return found;
}

// 0x803DEED0: an event to the manager's owner; a failure without a code on
// some events takes the Pat client's error.
void ManagerNotify(CpuContext* cpu, uint32_t manager, uint32_t event, int8_t member, int32_t code, uint32_t value, uint32_t info, uint32_t param) {
    GuestCall game{cpu};

    if (code < 0 && info && event != 0x1C && event != 0x23 && event != 0xC && Read(kPat)) {
        if (game.Call<0x803FD818>({Read(kPat), 0})) {
            game.Call<0x803FD840>({Read(kPat), 0x80050031, info});
            code = ReadInt(info);
        }
    }

    game(Read(manager + 4), {event, uint32_t(int32_t(member)), uint32_t(code), value, info, param});
}

// 0x803DD7B4: a connection event for the manager (through 0x803D68D4).
void ManagerConnectionEvent(CpuContext* cpu, uint32_t manager, uint32_t event, int8_t peer, uint32_t a, uint32_t b, uint32_t info) {
    GuestCall game{cpu};
    const int8_t member = int8_t(game.Virtual(manager, 0x1C4, {uint32_t(int32_t(peer))}));

    switch (event) {
    case 1:
        if (const uint32_t layer = Layer(cpu))
            game.Call<0x803EEF34>({layer, uint32_t(int32_t(peer))});

        break;

    case 2:
        if (member >= 0 && Byte(manager + uint32_t(member) * 0x48 + 0x539))
            ManagerNotify(cpu, manager, 0x1D, member, int32_t(a), b, info, Read(manager + 8));

        break;

    case 3: {
        if (Layer(cpu)) {
            GuestFrame frame(cpu, 0x60);
            const uint32_t id = frame[0x18];
            game.Call<IdConstruct>({id});
            IdOfPeer(cpu, Layer(cpu), peer, id);

            if (game.Call<IdEquals>({id, manager + 0x3CC})) {
                game.Call<0x803DEE44>({manager, info});
                Write(manager + 0x794, Read(info));
                Write(manager + 0x798, Read(info + 4));
                Write(manager + 0x79C, Read(info + 8));
            }

            game.Call<0x803EEF80>({Layer(cpu), uint32_t(int32_t(peer)), info});
            game.Call<IdDestroy>({id, 0xFFFFFFFF});
        }

        if (member < 0 || !Byte(manager + 0x3C1))
            break;

        if (member == Signed(manager + 0x536)) {
            Memory::Write8(manager + 0x66C, 0);

            if (game.Call<0x803DF20C>({manager}))
                ManagerNotify(cpu, manager, 0x23, 0, ReadInt(manager + 0x794), 1, manager + 0x794, Read(manager + 8));
        } else {
            if (Read(manager + 0x65C))
                game.Call<UdpRemove>({Read(manager + 0x65C), manager + uint32_t(member) * 0x48 + 0x578});

            if (Byte(manager + 0x66C) && Byte(manager + uint32_t(member) * 0x48 + 0x539))
                ManagerNotify(cpu, manager, 0x24, member, ReadInt(info), 1, info, Read(manager + 8));
        }

        break;
    }

    case 4:
        if (member < 0)
            break;

        if (Byte(info) == 1) {
            GuestFrame frame(cpu, 0x30);
            game.Call<0x803DC148>({manager, frame[0x8], info});

            if (Byte(manager + uint32_t(member) * 0x48 + 0x539))
                ManagerNotify(cpu, manager, member == Signed(manager + 0x536) ? 0x1E : 0x1F, member, 0, 0, 0, Read(manager + 8));
        } else if (Byte(info) == 2) {
            game.Call<0x803DC348>({manager, info, b});
        }

        break;

    case 5:
        if (VoiceState(cpu, 0x80416D8C) && Read(kVoice)) {
            const uint32_t system = Read(kLibrary);

            if (Byte(system + 0x6DD0) && game.Call<0x80416EAC>({Read(kVoice)}) && Layer(cpu)) {
                const int32_t user = int8_t(MemberOfPeer(Layer(cpu), peer));
                game.Call<0x804172CC>({Read(kLibrary), uint32_t(user), Read(info + 4), Read(info)});
            }
        }

        break;
    }
}

// 0x803D711C: the game manager's update: the sockets, the GameSpy error
// check, the requests, then at most every ec18 seconds the waiting status,
// user binary and member changes to the Pat client.
void GameManagerUpdate(CpuContext* cpu, uint32_t manager) {
    GuestCall game{cpu};

    if (Read(manager + 0x658))
        game.Call<TcpMove>({Read(manager + 0x658)});

    if (Read(manager + 0x65C))
        game.Call<UdpMove>({Read(manager + 0x65C)});

    if (Read(kGameSpy) && Byte(manager + 0x6E75) && int32_t(game.Call<0x8041D364>({Read(kGameSpy)})) < 0) {
        GuestFrame frame(cpu, 0x30);
        const uint32_t error = frame[0x14], copy = frame[0x8];
        game.Call<0x8041D614>({Read(kGameSpy), error});

        if (Read(error + 4) == 0x4B) {
            game(Read(manager + 4), {3, 0, Read(error), 1, error, Read(manager + 8)});

            for (uint32_t i = 0; i < 12; i += 4)
                Write(copy + i, Read(error + i));

            game.Virtual(Read(kPat), 0x290, {copy});
            game.Call<0x8041D638>({Read(kGameSpy)});
        }
    }

    game.Call<ManagerUpdate>({manager});

    if (!game.Virtual(manager, 0x28) || !game.Call<0x803DE7EC>({manager}))
        return;

    if (!Read(manager + 0x950) && !Read(manager + 0x7A4) && !Byte(manager + 0xAE6))
        return;

    if (!(double(F(0x8079EC18) + F(manager + 0x3B8)) < Now(cpu)))
        return;

    SetF(manager + 0x3B8, float(Now(cpu)));
    GuestFrame frame(cpu, 0x4C0);
    const uint32_t binary = frame[0x124], changes = frame[0x20];
    game.Call<Memset>({binary, 0, 0x37C});

    if (const uint32_t length = Read(manager + 0x950)) {
        const uint32_t count = length < 0x100 ? length : 0x100;
        Memory::Write16(binary + 0x156, uint16_t(count));
        game.Call<Memcpy>({binary + 0x56, manager + 0x954, count & 0xFFFF});
        Write(manager + 0x950, 0);
    }

    if (Byte(manager + 0xAE6)) {
        Memory::Write8(binary + 0x378, Byte(manager + 0xAE5) ? 1 : 2);
        Memory::Write8(manager + 0xAE6, 0);
    }

    game.Call<Memset>({changes, 0, 0x104});

    if (Read(manager + 0x7A4)) {
        game.Call<0x803DE588>({manager, changes, manager + 0x7A0});
        Write(manager + 0x7A4, 0);
    }

    game.Call<0x80402C08>({Read(kPat), Read(manager + 0x41C), binary, changes});
}

// Members' ids at +0xC1B0 (16 bytes each) and the pair states at +0xC7F0
// (100 x 100: 0 none, 1 asked, 2.. linked, 4 failed).
uint32_t MemberIdOf(uint32_t layer, uint32_t member) {
    return Read(layer + 0xC1B0 + member * 16);
}

uint32_t PairState(uint32_t layer, uint32_t from, uint32_t to) {
    return layer + from * 100 + to + 0xC7F0;
}

bool VoiceTarget(CpuContext* cpu, uint32_t layer, uint32_t member) {
    GuestCall game{cpu};
    const uint32_t m = uint32_t(int32_t(int8_t(member)));
    return int8_t(member) != SelfMember(layer) && Present(layer, member) && game.Virtual(layer, 0xE4, {m}) && game.Virtual(layer, 0xF0, {m});
}

// The circle's GameSpy pairs (0x803E1B90's part for a known own member):
// asked pairs whose peer is gone fail, and the pair being linked finishes.
void CheckPairs(CpuContext* cpu, uint32_t layer, int32_t self, uint32_t from, uint32_t to, uint32_t done) {
    GuestCall game{cpu};

    for (uint32_t member = 0; member < kMembers; ++member) {
        if (int32_t(member) == self || !Present(layer, member) || !MemberIdOf(layer, member))
            continue;

        if (Byte(PairState(layer, uint32_t(self), member)) == 1 && Read(kGameSpy) && int8_t(PeerState(Read(kGameSpy), MemberIdOf(layer, member))) < 1) {
            Memory::Write8(PairState(layer, uint32_t(self), member), 4);
            Memory::Write8(PairState(layer, member, uint32_t(self)), 4);
            game.Call<0x803EED04>({layer, MemberIdOf(layer, uint32_t(self)), MemberIdOf(layer, member), 4});
        }
    }

    if (!Read(to)) {
        if (Read(from) && Read(from) == MemberIdOf(layer, uint32_t(SelfMember(layer))) && Read(kGameSpy) && !game.Call<0x8041DABC>({Read(kGameSpy)})) {
            game.Call<0x803EED04>({layer, Read(from), Read(to), 3});
            Write(from, 0);
            Write(to, 0);
        }
    } else if (!Byte(done) && Read(to) == MemberIdOf(layer, uint32_t(SelfMember(layer))) && Read(kGameSpy) && !game.Call<0x8041DABC>({Read(kGameSpy)})) {
        const int32_t other = int8_t(game.Call<0x803EEE30>({layer, Read(from)}));

        if (other < 0 || SelfMember(layer) < 0) {
            game.Call<0x803EED04>({layer, Read(from), Read(to), 3});
            Write(from, 0);
            Write(to, 0);
        } else {
            const uint32_t own = uint32_t(SelfMember(layer));
            Memory::Write8(PairState(layer, uint32_t(other), own), uint8_t(game.Call<0x8041DAC4>({Read(kGameSpy)})));
            Memory::Write8(PairState(layer, own, uint32_t(other)), uint8_t(game.Call<0x8041DAC4>({Read(kGameSpy)})));
            const int32_t state = int8_t(game.Call<0x8041DAC4>({Read(kGameSpy)}));
            game.Call<0x803EED04>({layer, Read(from), Read(to), uint32_t(state)});
            Memory::Write8(done, 1);
        }
    }
}

// 0x803E1B90: the lobby layer's update: its requests; a pending member
// status to the Pat client at most every ec4c seconds; in a circle (state 2)
// the voice: own state changes notify everyone, the members' voice data is
// mixed and the own sent to them; then the GameSpy pairs between members are
// checked, and the circle's host links the next unlinked pair.
void LobbyUpdate(CpuContext* cpu, uint32_t layer) {
    GuestCall game{cpu};
    LobbyRequests(cpu, layer);

    if (ReadInt(layer + 0x4C8) < 0)
        return;

    if (Byte(layer + 0x3D2) && double(F(0x8079EC4C) + F(layer + 0x3C8)) < Now(cpu)) {
        SetF(layer + 0x3C8, float(Now(cpu)));
        GuestFrame frame(cpu, 0x140);
        const uint32_t status = frame[0x8];
        game.Call<0x803EB694>({layer, status, layer + 0x450});
        game.Call<0x80403948>({Read(kPat), status});
        Memory::Write8(layer + 0x3D2, 0);
    }

    if (Read(layer + 0x4C8) != 2)
        return;

    const uint32_t system = Read(kLibrary);

    if (!Byte(layer + 0x3D1)) {
        const uint32_t state = VoiceState(cpu, 0x80416EAC);
        const uint32_t own = layer + uint32_t(SelfMember(layer)) + 0xC084;

        if (state != Byte(own)) {
            Memory::Write8(own, uint8_t(state));

            for (uint32_t member = 0; member < kMembers; ++member)
                game.Call<0x803EEB5C>({layer, member, 0});
        }

        if (VoiceState(cpu, 0x80416D8C) && Byte(system + 0x6DD0) && Read(kVoice) && game.Call<0x80416EAC>({Read(kVoice)})) {
            const uint32_t mixed = layer + 0x6EB94, input = layer + 0x6E5AC, output = layer + 0x6E8A0;
            game.Call<Memset>({mixed, 0, 0x2F4});
            Write(layer + 0x6EE88, 0);

            for (uint32_t member = 0; member < kMembers; ++member) {
                if (!VoiceTarget(cpu, layer, member))
                    continue;

                const int32_t got = int32_t(game.Call<0x804173B4>({system, member, input, 0x2F4, 0}));

                if (got > 0) {
                    const int32_t length = int32_t(game.Call<0x803E1A78>({layer, mixed, 0x2F4, input, uint32_t(got)}));

                    if (ReadInt(layer + 0x6EE88) < length)
                        Write(layer + 0x6EE88, uint32_t(length));
                }
            }

            int32_t recorded = int32_t(game.Call<0x80417244>({system, input, 0x210}));

            if (game.Call<0x80417638>({system, input, uint32_t(recorded)}))
                recorded = 0;

            const int32_t length = ReadInt(layer + 0x6EE88);

            if (length == 0x210) {
                if (recorded > 0 && Read(kVoice))
                    game.Call<0x80448108>({Read(kVoice), input, mixed, 0x210});

                game.Call<0x803EBB54>({layer, 0x27, 0, 1, mixed, Read(layer + 8)});
            } else if (length) {
                Log(cpu, 0x10, {0x805FBED8, uint32_t(length)});
            }

            int32_t encoded = 0;

            if (recorded > 0)
                encoded = int32_t(game.Call<0x803E1978>({layer, output, 0x2F4, input, uint32_t(recorded)}));

            if (encoded > 0 && Read(kLayers) && game.Call<0x8041A358>({Read(kLayers), 0})) {
                for (uint32_t member = 0; member < kMembers; ++member)
                    if (VoiceTarget(cpu, layer, member))
                        game.Call<0x803DF1E4>({game.Call<0x8041A358>({Read(kLayers), 0}), output, uint32_t(encoded), 0, uint32_t(int32_t(Signed(layer + member + 0xEF00)))});
            }
        }

        SetF(layer + 0x3CC, float(Now(cpu)));
    }

    const int32_t self = SelfMember(layer);
    const uint32_t from = layer + 0x438, to = layer + 0x43C, done = layer + 0x440;

    if (self >= 0)
        CheckPairs(cpu, layer, self, from, to, done);

    if (SelfMember(layer) != ReadInt(layer + 0xC080) || ReadInt(layer + 0xF02C) < 2)
        return;

    if (!Byte(done) && (Read(to) || Read(from)))
        return;

    if (!Read(kGameSpy) || game.Call<0x8041C838>({Read(kGameSpy)}) != 1)
        return;

    // The host's own unlinked pair first.
    for (uint32_t member = 0; member < kMembers; ++member) {
        const int32_t own = SelfMember(layer);

        if (int32_t(member) == own || !Present(layer, member) || !MemberIdOf(layer, member))
            continue;

        const uint8_t state = Byte(PairState(layer, uint32_t(own), member));

        if (state == 0 || state == 4) {
            const uint32_t id = layer + 0xC1B0 + member * 16;
            game.Call<0x803EED04>({layer, MemberIdOf(layer, uint32_t(own)), Read(id), 2});
            Write(from, MemberIdOf(layer, uint32_t(SelfMember(layer))));
            Write(to, Read(id));
            Memory::Write8(done, 0);
            game.Call<0x8041D9C4>({Read(kGameSpy), layer + 0xC1B0 + uint32_t(SelfMember(layer)) * 16, id});
            break;
        }
    }

    if (Read(to) || Read(from))
        return;

    // Then any other unlinked pair.
    for (uint32_t a = 0; a < 99; ++a) {
        if (int32_t(int8_t(a)) == SelfMember(layer) || !Present(layer, a) || !MemberIdOf(layer, a))
            continue;

        for (uint32_t b = a + 1; b < kMembers; ++b) {
            if (int32_t(int8_t(b)) == SelfMember(layer) || !Present(layer, b) || !MemberIdOf(layer, b))
                continue;

            const uint8_t state = Byte(PairState(layer, a, b));

            if (state == 0 || state == 4) {
                game.Call<0x803EED04>({layer, MemberIdOf(layer, a), MemberIdOf(layer, b), 2});
                Write(from, MemberIdOf(layer, a));
                Write(to, MemberIdOf(layer, b));
                Memory::Write8(done, 0);
                break;
            }
        }

        if (Read(to) || Read(from))
            break;
    }
}

} // namespace

// 0x80419CBC / 0x80419CCC (platform virtuals 0x40, 0x60): the clock's
// advance and its seconds.
extern "C" void MH3_NetPlatformTick(CpuContext* cpu) {
    const uint32_t platform = Arg(cpu, 3);
    ClockAdvance(cpu, platform + 0xA0, platform + 0xA4);
}
REGISTER_NATIVE_FUNCTION_AS(0x80419CBC, MH3_NetPlatformTick, "NetPlatformTick");

extern "C" void MH3_NetPlatformNow(CpuContext* cpu) {
    cpu->fpr[1].d = double(F(Arg(cpu, 3) + 0xA0));
}
REGISTER_NATIVE_FUNCTION_AS(0x80419CCC, MH3_NetPlatformNow, "NetPlatformNow");

extern "C" void MH3_NetClockAdvance(CpuContext* cpu) {
    ClockAdvance(cpu, Arg(cpu, 4), Arg(cpu, 5));
}
REGISTER_NATIVE_FUNCTION_AS(0x80419DB8, MH3_NetClockAdvance, "NetClockAdvance");

extern "C" void MH3_NetLibraryUpdate(CpuContext* cpu) {
    LibraryUpdate(cpu, Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x8041A184, MH3_NetLibraryUpdate, "NetLibraryUpdate");

// 0x8041A5E4: the holder's first layer.
extern "C" void MH3_NetLayerAt(CpuContext* cpu) {
    cpu->gpr[3] = Arg(cpu, 4) == 0 ? Read(Arg(cpu, 3) + 0xC) : 0;
}
REGISTER_NATIVE_FUNCTION_AS(0x8041A5E4, MH3_NetLayerAt, "NetLayerAt");

extern "C" void MH3_NetSystemUpdate(CpuContext* cpu) {
    SystemUpdate(cpu, Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x80413D70, MH3_NetSystemUpdate, "NetSystemUpdate");

extern "C" void MH3_NetInterfaceUp(CpuContext* cpu) {
    cpu->gpr[3] = InterfaceUp(cpu, Arg(cpu, 3), Arg(cpu, 4));
}
REGISTER_NATIVE_FUNCTION_AS(0x80413F9C, MH3_NetInterfaceUp, "NetInterfaceUp");

extern "C" void MH3_NetVoiceActive(CpuContext* cpu) {
    cpu->gpr[3] = VoiceState(cpu, 0x80416D8C);
}
REGISTER_NATIVE_FUNCTION_AS(0x80416D54, MH3_NetVoiceActive, "NetVoiceActive");

extern "C" void MH3_NetVoiceTalking(CpuContext* cpu) {
    const uint32_t system = Arg(cpu, 3);
    cpu->gpr[3] = Byte(system + 0x6DD0) ? VoiceState(cpu, 0x80416EAC) : 0;
}
REGISTER_NATIVE_FUNCTION_AS(0x80416E68, MH3_NetVoiceTalking, "NetVoiceTalking");

extern "C" void MH3_NetVoiceState(CpuContext* cpu) {
    cpu->gpr[3] = VoiceState(cpu, 0x80416EAC);
}
REGISTER_NATIVE_FUNCTION_AS(0x80417728, MH3_NetVoiceState, "NetVoiceState");

extern "C" void MH3_NetSetMemberReady(CpuContext* cpu) {
    SetMemberReady(cpu, Arg(cpu, 3), int8_t(Arg(cpu, 4)), uint8_t(Arg(cpu, 5)));
}
REGISTER_NATIVE_FUNCTION_AS(0x80417790, MH3_NetSetMemberReady, "NetSetMemberReady");

extern "C" void MH3_NetMemberReady(CpuContext* cpu) {
    cpu->gpr[3] = MemberReady(Arg(cpu, 3), int8_t(Arg(cpu, 4)));
}
REGISTER_NATIVE_FUNCTION_AS(0x804177D4, MH3_NetMemberReady, "NetMemberReady");

extern "C" void MH3_NetMemberActive(CpuContext* cpu) {
    cpu->gpr[3] = MemberActive(Arg(cpu, 3), int8_t(Arg(cpu, 4)));
}
REGISTER_NATIVE_FUNCTION_AS(0x80417888, MH3_NetMemberActive, "NetMemberActive");

extern "C" void MH3_NetGsReceived(CpuContext* cpu) {
    ReceivedCallback(cpu, Arg(cpu, 3), Arg(cpu, 4), int32_t(Arg(cpu, 5)));
}
REGISTER_NATIVE_FUNCTION_AS(0x8041BD28, MH3_NetGsReceived, "NetGsReceived");

extern "C" void MH3_NetGsPeerReceived(CpuContext* cpu) {
    PeerReceived(cpu, Arg(cpu, 3), Arg(cpu, 4), Arg(cpu, 5), Arg(cpu, 6));
}
REGISTER_NATIVE_FUNCTION_AS(0x8041C1F8, MH3_NetGsPeerReceived, "NetGsPeerReceived");

extern "C" void MH3_NetGsPeerState(CpuContext* cpu) {
    cpu->gpr[3] = PeerState(Arg(cpu, 3), Arg(cpu, 4));
}
REGISTER_NATIVE_FUNCTION_AS(0x8041CE6C, MH3_NetGsPeerState, "NetGsPeerState");

extern "C" void MH3_NetGsPeerSlot(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(PeerSlot(Arg(cpu, 3), Arg(cpu, 4)));
}
REGISTER_NATIVE_FUNCTION_AS(0x8041CEEC, MH3_NetGsPeerSlot, "NetGsPeerSlot");

extern "C" void MH3_NetGsThink(CpuContext* cpu) {
    GameSpyThink(cpu, Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x8041CF28, MH3_NetGsThink, "NetGsThink");

extern "C" void MH3_NetGsSendUnreliable(CpuContext* cpu) {
    cpu->gpr[3] = SendUnreliable(cpu, Arg(cpu, 3), Arg(cpu, 4), Arg(cpu, 5), Arg(cpu, 6));
}
REGISTER_NATIVE_FUNCTION_AS(0x8041D6A0, MH3_NetGsSendUnreliable, "NetGsSendUnreliable");

extern "C" void MH3_NetGsPeerSend(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(PeerSend(cpu, Arg(cpu, 3), Arg(cpu, 4), int32_t(Arg(cpu, 5)), Arg(cpu, 6), int32_t(Arg(cpu, 7)), Arg(cpu, 8)));
}
REGISTER_NATIVE_FUNCTION_AS(0x8041DC54, MH3_NetGsPeerSend, "NetGsPeerSend");

extern "C" void MH3_NetGsPeerReceive(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(PeerReceive(cpu, Arg(cpu, 3), Arg(cpu, 4), Arg(cpu, 5), Arg(cpu, 6), Arg(cpu, 7), Arg(cpu, 8)));
}
REGISTER_NATIVE_FUNCTION_AS(0x8041DEA4, MH3_NetGsPeerReceive, "NetGsPeerReceive");

extern "C" void MH3_NetGsPeerPut(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(PeerPut(cpu, Arg(cpu, 3), Arg(cpu, 4), Arg(cpu, 5)));
}
REGISTER_NATIVE_FUNCTION_AS(0x8041E0BC, MH3_NetGsPeerPut, "NetGsPeerPut");

extern "C" void MH3_NetSocketReceive(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(SocketReceive(cpu, Arg(cpu, 3), Arg(cpu, 4), int32_t(Arg(cpu, 5)), Arg(cpu, 6)));
}
REGISTER_NATIVE_FUNCTION_AS(0x803F82D0, MH3_NetSocketReceive, "NetSocketReceive");

extern "C" void MH3_NetPatReceive(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(PatReceive(cpu, Arg(cpu, 3), Arg(cpu, 4)));
}
REGISTER_NATIVE_FUNCTION_AS(0x803FB698, MH3_NetPatReceive, "NetPatReceive");

extern "C" void MH3_NetPatCommands(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(PatCommands(cpu, Arg(cpu, 3), Arg(cpu, 4)));
}
REGISTER_NATIVE_FUNCTION_AS(0x803FB7E0, MH3_NetPatCommands, "NetPatCommands");

extern "C" void MH3_NetPatSend(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(PatSend(cpu, Arg(cpu, 3), Arg(cpu, 4)));
}
REGISTER_NATIVE_FUNCTION_AS(0x803FBA14, MH3_NetPatSend, "NetPatSend");

extern "C" void MH3_NetPatUpdate(CpuContext* cpu) {
    PatUpdate(cpu, Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x803FD3A8, MH3_NetPatUpdate, "NetPatUpdate");

extern "C" void MH3_NetLobbyUpdate(CpuContext* cpu) {
    LobbyUpdate(cpu, Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x803E1B90, MH3_NetLobbyUpdate, "NetLobbyUpdate");

extern "C" void MH3_NetLobbyRequests(CpuContext* cpu) {
    LobbyRequests(cpu, Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x803E25E0, MH3_NetLobbyRequests, "NetLobbyRequests");

extern "C" void MH3_NetLobbyIdConstruct(CpuContext* cpu) {
    const uint32_t object = Arg(cpu, 3);
    GuestCall{cpu}.Call<IdConstruct>({object});
    cpu->gpr[3] = object;
}
REGISTER_NATIVE_FUNCTION_AS(0x803E11E4, MH3_NetLobbyIdConstruct, "NetLobbyIdConstruct");

extern "C" void MH3_NetSetReady(CpuContext* cpu) {
    SetReady(cpu, Arg(cpu, 3), uint8_t(Arg(cpu, 4)), Arg(cpu, 5));
}
REGISTER_NATIVE_FUNCTION_AS(0x803EE804, MH3_NetSetReady, "NetSetReady");

extern "C" void MH3_NetMemberTalking(CpuContext* cpu) {
    cpu->gpr[3] = MemberTalking(Arg(cpu, 3), uint8_t(Arg(cpu, 4)));
}
REGISTER_NATIVE_FUNCTION_AS(0x803EE90C, MH3_NetMemberTalking, "NetMemberTalking");

extern "C" void MH3_NetMemberActiveOf(CpuContext* cpu) {
    cpu->gpr[3] = MemberActive(Read(kLibrary), int8_t(Arg(cpu, 4)));
}
REGISTER_NATIVE_FUNCTION_AS(0x803EE994, MH3_NetMemberActiveOf, "NetMemberActiveOf");

extern "C" void MH3_NetMemberLinked(CpuContext* cpu) {
    cpu->gpr[3] = MemberLinked(Arg(cpu, 3), uint8_t(Arg(cpu, 4)));
}
REGISTER_NATIVE_FUNCTION_AS(0x803EEA0C, MH3_NetMemberLinked, "NetMemberLinked");

extern "C" void MH3_NetMemberFlags(CpuContext* cpu) {
    cpu->gpr[3] = MemberFlags(cpu, Arg(cpu, 3), uint8_t(Arg(cpu, 4)));
}
REGISTER_NATIVE_FUNCTION_AS(0x803EEAA8, MH3_NetMemberFlags, "NetMemberFlags");

extern "C" void MH3_NetMemberOfPeer(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(MemberOfPeer(Arg(cpu, 3), int8_t(Arg(cpu, 4))));
}
REGISTER_NATIVE_FUNCTION_AS(0x803EF074, MH3_NetMemberOfPeer, "NetMemberOfPeer");

extern "C" void MH3_NetMemberOfId(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(MemberOfId(cpu, Arg(cpu, 3), Arg(cpu, 4)));
}
REGISTER_NATIVE_FUNCTION_AS(0x803EF19C, MH3_NetMemberOfId, "NetMemberOfId");

extern "C" void MH3_NetPeerOfId(CpuContext* cpu) {
    cpu->gpr[3] = PeerOfId(cpu, Arg(cpu, 3), Arg(cpu, 4));
}
REGISTER_NATIVE_FUNCTION_AS(0x803EF238, MH3_NetPeerOfId, "NetPeerOfId");

extern "C" void MH3_NetIdOfPeer(CpuContext* cpu) {
    IdOfPeer(cpu, Arg(cpu, 3), int8_t(Arg(cpu, 4)), Arg(cpu, 5));
}
REGISTER_NATIVE_FUNCTION_AS(0x803EF284, MH3_NetIdOfPeer, "NetIdOfPeer");

extern "C" void MH3_NetCircleRequests(CpuContext* cpu) {
    StepTable(cpu, Arg(cpu, 3), 0xC, 12, 0x803EFAF4, 0x803EFAE0, 0x803F0288);
}
REGISTER_NATIVE_FUNCTION_AS(0x803EFA60, MH3_NetCircleRequests, "NetCircleRequests");

extern "C" void MH3_NetCircleUpdate(CpuContext* cpu) {
    CircleUpdate(cpu, Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x803F0CC4, MH3_NetCircleUpdate, "NetCircleUpdate");

extern "C" void MH3_NetCircleRequests2(CpuContext* cpu) {
    StepTable(cpu, Arg(cpu, 3), 0x184, 9, 0x803F0F30, 0x803F0E2C, 0x803F0E40);
}
REGISTER_NATIVE_FUNCTION_AS(0x803F0EB0, MH3_NetCircleRequests2, "NetCircleRequests2");

extern "C" void MH3_NetGameManagerUpdate(CpuContext* cpu) {
    GameManagerUpdate(cpu, Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x803D711C, MH3_NetGameManagerUpdate, "NetGameManagerUpdate");

extern "C" void MH3_NetGameMemberId(CpuContext* cpu) {
    cpu->gpr[3] = MemberId(cpu, Arg(cpu, 3), uint8_t(Arg(cpu, 4)), Arg(cpu, 5));
}
REGISTER_NATIVE_FUNCTION_AS(0x803DC04C, MH3_NetGameMemberId, "NetGameMemberId");

extern "C" void MH3_NetGameConnectionEvent(CpuContext* cpu) {
    ManagerConnectionEvent(cpu, Arg(cpu, 3), Arg(cpu, 4), int8_t(Arg(cpu, 5)), Arg(cpu, 6), Arg(cpu, 7), Arg(cpu, 8));
}
REGISTER_NATIVE_FUNCTION_AS(0x803DD7B4, MH3_NetGameConnectionEvent, "NetGameConnectionEvent");

extern "C" void MH3_NetGameMemberPeer(CpuContext* cpu) {
    cpu->gpr[3] = MemberPeer(cpu, Arg(cpu, 3), uint8_t(Arg(cpu, 4)));
}
REGISTER_NATIVE_FUNCTION_AS(0x803DE5D0, MH3_NetGameMemberPeer, "NetGameMemberPeer");

extern "C" void MH3_NetGamePeerMember(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(PeerMember(cpu, Arg(cpu, 3), int8_t(Arg(cpu, 4))));
}
REGISTER_NATIVE_FUNCTION_AS(0x803DE658, MH3_NetGamePeerMember, "NetGamePeerMember");

extern "C" void MH3_NetGameNotify(CpuContext* cpu) {
    ManagerNotify(cpu, Arg(cpu, 3), Arg(cpu, 4), int8_t(Arg(cpu, 5)), int32_t(Arg(cpu, 6)), Arg(cpu, 7), Arg(cpu, 8), Arg(cpu, 9));
}
REGISTER_NATIVE_FUNCTION_AS(0x803DEED0, MH3_NetGameNotify, "NetGameNotify");

// 0x803DEF9C: whether the manager has a positive id at +0x41C.
extern "C" void MH3_NetGameHasId(CpuContext* cpu) {
    cpu->gpr[3] = ReadInt(Arg(cpu, 3) + 0x41C) > 0;
}
REGISTER_NATIVE_FUNCTION_AS(0x803DEF9C, MH3_NetGameHasId, "NetGameHasId");

// 0x803DF20C: the session's virtual 0x84 with the caller's registers, or 0.
extern "C" void MH3_NetGameSessionRelay(CpuContext* cpu) {
    const uint32_t session = Read(Arg(cpu, 3) + 0xC);
    cpu->gpr[3] = session;

    if (session)
        GuestCall{cpu}(Read(Read(session) + 0x84));
}
REGISTER_NATIVE_FUNCTION_AS(0x803DF20C, MH3_NetGameSessionRelay, "NetGameSessionRelay");
