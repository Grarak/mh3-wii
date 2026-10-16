// The GameSpy SDK in RMHP08 (0x8050B170..0x80514D18), ported natively:
// gamespy_common.cpp (darray, hashtable, platform sockets and time, the
// availability check), gamespy_gt2.cpp (GameSpy Transport 2, the peer
// connections) and gamespy_natneg.cpp (NAT negotiation). The ports call each
// other directly; guest callbacks, the socket library (SO) and the guest's
// memcpy, memmove, memset, sprintf and rand go through GuestCall.
#pragma once

#include "abi_bridge.h"
#include "guest_call.h"
#include "memory.h"

#include <cstdint>

namespace GameSpy {

enum : uint32_t {
    Memcpy = 0x80004000,
    Memset = 0x80004350,
    Strlen = 0x80456C40,
    Div2i = 0x804575D8,
    Memmove = 0x8045BC18,
    Sprintf = 0x8045E54C,
    Rand = 0x8045E620,
    OSGetTime = 0x804D53D0,
    OSSleepTicks = 0x804D5320,
};

// The socket library (SO).
enum : uint32_t {
    SOSocket = 0x80520050,
    SOClose = 0x80520140,
    SOBind = 0x805203D8,
    SOGetSockName = 0x805205A8,
    SORecvFrom = 0x805207A0,
    SOSendTo = 0x805207EC,
    SOPoll = 0x80520A1C,
    SOInetAtoN = 0x80520B78,
    SOInetNtoA = 0x80520C84,
    SOGetHostByName = 0x8052124C,
    SOGetInterfaceOpt = 0x805217D8,
};

// The last socket error (gsi's GOAGetLastError), r13 - 0x3610.
constexpr uint32_t kLastSocketError = 0x80797CD0;

// struct sockaddr_in as the socket library has it: length, family, port,
// address.
enum : uint32_t {
    kAddrLength = 0x0,
    kAddrFamily = 0x1,
    kAddrPort = 0x2,
    kAddrIp = 0x4,
    kAddrBytes = 0x8,
};

// CR bit 6 (cr1 eq): a variadic call's FPR arguments are in registers.
constexpr uint32_t kCrFloatArgs = 1u << (31 - 6);

inline uint32_t Guest(CpuContext* cpu, uint32_t fn, std::initializer_list<uint32_t> ints = {}) {
    return GuestCall{cpu}(fn, ints);
}

inline void Memcpy4(CpuContext* cpu, uint32_t to, uint32_t from, uint32_t bytes) {
    GuestCall{cpu}.Call<Memcpy>({to, from, bytes});
}

inline void Memmove4(CpuContext* cpu, uint32_t to, uint32_t from, uint32_t bytes) {
    GuestCall{cpu}.Call<Memmove>({to, from, bytes});
}

inline void Memset4(CpuContext* cpu, uint32_t to, uint32_t value, uint32_t bytes) {
    GuestCall{cpu}.Call<Memset>({to, value, bytes});
}

uint32_t StringLength(uint32_t s);
bool BytesEqual(uint32_t a, uint32_t b, uint32_t bytes);

// gsimalloc, gsifree, gsirealloc: the memory callbacks set at 0x80763878.
uint32_t Malloc(CpuContext* cpu, uint32_t bytes);
void Free(CpuContext* cpu, uint32_t block);
uint32_t Realloc(CpuContext* cpu, uint32_t block, uint32_t bytes);

// current_time: milliseconds of the time base.
uint32_t CurrentTime(CpuContext* cpu);

// darray.c.
uint32_t ArrayNew(CpuContext* cpu, uint32_t elemSize, int32_t count, uint32_t freeFn);
void ArrayFree(CpuContext* cpu, uint32_t array);

inline int32_t ArrayLength(uint32_t array) {
    return int32_t(Memory::Read32(array));
}

uint32_t ArrayNth(uint32_t array, int32_t n);
void ArrayAppend(CpuContext* cpu, uint32_t array, uint32_t elem);
void ArrayInsertSorted(CpuContext* cpu, uint32_t array, uint32_t elem, uint32_t compare);
void ArrayRemoveAt(CpuContext* cpu, uint32_t array, int32_t n);
void ArrayDeleteAt(CpuContext* cpu, uint32_t array, int32_t n);
void ArrayReplaceAt(CpuContext* cpu, uint32_t array, uint32_t elem, int32_t n);
int32_t ArraySearch(CpuContext* cpu, uint32_t array, uint32_t key, uint32_t compare, int32_t from, bool sorted);
void ArrayMapBackwards(CpuContext* cpu, uint32_t array, uint32_t fn, uint32_t data);
uint32_t ArrayMapBackwards2(CpuContext* cpu, uint32_t array, uint32_t fn, uint32_t data);

// hashtable.c.
uint32_t TableNew2(CpuContext* cpu, uint32_t elemSize, int32_t buckets, int32_t chains, uint32_t hash, uint32_t compare, uint32_t freeFn);
void TableFree(CpuContext* cpu, uint32_t table);
void TableEnter(CpuContext* cpu, uint32_t table, uint32_t elem);
bool TableRemove(CpuContext* cpu, uint32_t table, uint32_t elem);
uint32_t TableLookup(CpuContext* cpu, uint32_t table, uint32_t elem);
void TableMap(CpuContext* cpu, uint32_t table, uint32_t fn, uint32_t data);
uint32_t TableMap2(CpuContext* cpu, uint32_t table, uint32_t fn, uint32_t data);

// gsPlatformSocket: the BSD calls on the socket library. Errors return -1
// and keep the library's code for LastSocketError.
int32_t Socket(CpuContext* cpu, uint32_t domain, uint32_t type, uint32_t protocol);
int32_t CloseSocket(CpuContext* cpu, uint32_t s);
int32_t Bind(CpuContext* cpu, uint32_t s, uint32_t addr, uint32_t length);
int32_t RecvFrom(CpuContext* cpu, uint32_t s, uint32_t buffer, uint32_t length, uint32_t flags, uint32_t from, uint32_t fromLength);
int32_t SendTo(CpuContext* cpu, uint32_t s, uint32_t buffer, uint32_t length, uint32_t flags, uint32_t to, uint32_t toLength);
int32_t GetSockName(CpuContext* cpu, uint32_t s, uint32_t addr, uint32_t length);
uint32_t InetAddr(CpuContext* cpu, uint32_t name);

inline int32_t LastSocketError() {
    return int32_t(Memory::Read32(kLastSocketError));
}

int32_t SocketSelect(CpuContext* cpu, uint32_t s, uint32_t readable, uint32_t writable, uint32_t exception);
uint32_t CanReceiveOnSocket(CpuContext* cpu, uint32_t s);
uint32_t CanSendOnSocket(CpuContext* cpu, uint32_t s);
uint32_t GetHostByName(CpuContext* cpu, uint32_t name);

// The socket library's byte order helpers (big-endian): htonl and ntohl
// return the value, htons and ntohs its low 16 bits.
inline uint32_t Short(uint32_t value) {
    return value & 0xFFFF;
}

} // namespace GameSpy
