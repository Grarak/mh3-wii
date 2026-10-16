// The GameSpy SDK's common code in RMHP08, ported from the translated C++ and
// its Ghidra decompilation (gamespy.h):
//
//   darray.c       0x8050B170..0x8050BA40
//   hashtable.c    0x8050BAE0..0x8050BE80
//   gsPlatform     the socket calls 0x8050BF00..0x8050C8A0, the host name
//                  cache 0x8050C2C0, the local host 0x8050C8F0, private
//                  addresses 0x8050CAD0, current_time 0x8050CB70, msleep
//                  0x8050CBC0, the memory callbacks 0x8050CC00..0x8050CC50
//   gsAvailable    the availability check 0x8050CC70 and its think 0x8050CDF0
//
// current_time is OSGetTime in milliseconds: GT2's and NAT negotiation's
// timers (keep-alives, resends, timeouts) count real time, whatever the
// frame rate or step.
//
// MH3_PORT_CHECK names the ports that do no socket I/O (the array, table and
// memory ports, current_time); the socket calls and the availability check
// would send twice.
#include "gamespy.h"

#include "port_check.h"

#include <cstring>

namespace GameSpy {

namespace {

// struct DArrayImplementation.
enum : uint32_t {
    kCount = 0x0,
    kCapacity = 0x4,
    kElemSize = 0x8,
    kGrowBy = 0xC,
    kElemFree = 0x10,
    kList = 0x14,
    kArrayBytes = 0x18,
};

constexpr int32_t kDefaultGrowBy = 8;

// struct HashImplementation.
enum : uint32_t {
    kBuckets = 0x0,
    kBucketCount = 0x4,
    kTableFree = 0x8,
    kHash = 0xC,
    kCompare = 0x10,
    kTableBytes = 0x14,
};

// gsiMemoryCallbacksSet's callbacks: malloc, free, realloc, memalign.
constexpr uint32_t kMemoryCallbacks = 0x80763878;

int32_t Count(uint32_t array) {
    return int32_t(Memory::Read32(array + kCount));
}

// ArrayNth against a given count, as the inlined copies check it.
uint32_t NthOf(uint32_t array, int32_t n, int32_t count) {
    if (n < 0 || n >= count)
        return 0;

    return Memory::Read32(array + kList) + Memory::Read32(array + kElemSize) * uint32_t(n);
}

void Grow(CpuContext* cpu, uint32_t array) {
    const uint32_t capacity = Memory::Read32(array + kCapacity) + Memory::Read32(array + kGrowBy);
    Memory::Write32(array + kCapacity, capacity);
    Memory::Write32(array + kList, Realloc(cpu, Memory::Read32(array + kList), capacity * Memory::Read32(array + kElemSize)));
}

// ArrayInsertAt (inlined in the append and the sorted insert).
void InsertAt(CpuContext* cpu, uint32_t array, uint32_t elem, int32_t n) {
    if (Memory::Read32(array + kCount) == Memory::Read32(array + kCapacity))
        Grow(cpu, array);

    const int32_t count = Count(array) + 1;
    Memory::Write32(array + kCount, uint32_t(count));

    if (n < count - 1)
        Memmove4(cpu, NthOf(array, n + 1, count), NthOf(array, n, count), Memory::Read32(array + kElemSize) * uint32_t(count - 1 - n));

    Memcpy4(cpu, NthOf(array, n, Count(array)), elem, Memory::Read32(array + kElemSize));
}

// ArrayRemoveAt's move of the elements after n, then the count.
void RemoveAt(CpuContext* cpu, uint32_t array, int32_t n) {
    const int32_t count = Count(array);

    if (n < count - 1)
        Memmove4(cpu, NthOf(array, n, count), NthOf(array, n + 1, count), Memory::Read32(array + kElemSize) * uint32_t(count - 1 - n));

    Memory::Write32(array + kCount, Memory::Read32(array + kCount) - 1);
}

void FreeElement(CpuContext* cpu, uint32_t array, int32_t n) {
    if (const uint32_t fn = Memory::Read32(array + kElemFree))
        Guest(cpu, fn, {NthOf(array, n, Count(array))});
}

uint32_t Bucket(uint32_t table, uint32_t hash) {
    return Memory::Read32(Memory::Read32(table + kBuckets) + 4 * hash);
}

uint32_t HashOf(CpuContext* cpu, uint32_t table, uint32_t elem) {
    return Guest(cpu, Memory::Read32(table + kHash), {elem, Memory::Read32(table + kBucketCount)});
}

// The socket calls' error path: keep the code, return -1.
int32_t SocketResult(uint32_t result) {
    if (int32_t(result) >= 0)
        return int32_t(result);

    Memory::Write32(kLastSocketError, result);
    return -1;
}

uint32_t TicksPerMillisecond() {
    return Memory::Read32(0x800000F8) / 4000;
}

} // namespace

uint32_t StringLength(uint32_t s) {
    return uint32_t(std::strlen(reinterpret_cast<const char*>(Memory::GetPointer(s))));
}

bool BytesEqual(uint32_t a, uint32_t b, uint32_t bytes) {
    return std::memcmp(Memory::GetPointer(a, bytes), Memory::GetPointer(b, bytes), bytes) == 0;
}

// 0x8050CC20, 0x8050CC50, 0x8050CC30: tail calls into the callbacks, with
// the registers they left (r4 or r5 hold the callbacks' address).
uint32_t Malloc(CpuContext* cpu, uint32_t bytes) {
    cpu->gpr[4] = kMemoryCallbacks & 0xFFFF0000;
    return Guest(cpu, Memory::Read32(kMemoryCallbacks), {bytes});
}

void Free(CpuContext* cpu, uint32_t block) {
    if (!block)
        return;

    cpu->gpr[4] = kMemoryCallbacks;
    Guest(cpu, Memory::Read32(kMemoryCallbacks + 4), {block});
}

uint32_t Realloc(CpuContext* cpu, uint32_t block, uint32_t bytes) {
    cpu->gpr[5] = kMemoryCallbacks;
    return Guest(cpu, Memory::Read32(kMemoryCallbacks + 8), {block, bytes});
}

// 0x8050CB70: OSGetTime over the ticks of a millisecond (a signed 64-bit
// division), the low word.
uint32_t CurrentTime(CpuContext* cpu) {
    GuestCall{cpu}.Call<OSGetTime>();
    const int64_t time = int64_t((uint64_t(cpu->gpr[3]) << 32) | cpu->gpr[4]);
    return uint32_t(time / int64_t(TicksPerMillisecond()));
}

// 0x8050B170.
uint32_t ArrayNew(CpuContext* cpu, uint32_t elemSize, int32_t count, uint32_t freeFn) {
    const uint32_t array = Malloc(cpu, kArrayBytes);

    if (!count)
        count = kDefaultGrowBy;

    Memory::Write32(array + kCount, 0);
    Memory::Write32(array + kCapacity, uint32_t(count));
    Memory::Write32(array + kElemSize, elemSize);
    Memory::Write32(array + kGrowBy, uint32_t(count));
    Memory::Write32(array + kElemFree, freeFn);

    if (count)
        Memory::Write32(array + kList, Malloc(cpu, uint32_t(count) * Memory::Read32(array + kElemSize)));
    else
        Memory::Write32(array + kList, 0);

    return array;
}

// 0x8050B210.
void ArrayFree(CpuContext* cpu, uint32_t array) {
    for (int32_t i = 0; i < Count(array); ++i)
        FreeElement(cpu, array, i);

    Free(cpu, Memory::Read32(array + kList));
    Free(cpu, array);
}

// 0x8050B2C0.
uint32_t ArrayNth(uint32_t array, int32_t n) {
    return NthOf(array, n, Count(array));
}

// 0x8050B2F0: ArrayInsertAt at the end.
void ArrayAppend(CpuContext* cpu, uint32_t array, uint32_t elem) {
    if (array)
        InsertAt(cpu, array, elem, Count(array));
}

// 0x8050B420: a binary search for the place (the comparator gets the element,
// then the new one), then ArrayInsertAt.
void ArrayInsertSorted(CpuContext* cpu, uint32_t array, uint32_t elem, uint32_t compare) {
    const uint32_t size = Memory::Read32(array + kElemSize);
    const uint32_t list = Memory::Read32(array + kList);
    int32_t low = 0, high = Count(array) - 1;

    while (low <= high) {
        const int32_t mid = (low + high) >> 1;
        const int32_t order = int32_t(Guest(cpu, compare, {list + uint32_t(mid) * size, elem}));

        if (order < 0)
            low = mid + 1;
        else
            high = mid - 1;
    }

    const int32_t n = int32_t(list + uint32_t(low) * size - Memory::Read32(array + kList)) / int32_t(Memory::Read32(array + kElemSize));
    InsertAt(cpu, array, elem, n);
}

// 0x8050B5A0.
void ArrayRemoveAt(CpuContext* cpu, uint32_t array, int32_t n) {
    RemoveAt(cpu, array, n);
}

// 0x8050B650.
void ArrayDeleteAt(CpuContext* cpu, uint32_t array, int32_t n) {
    FreeElement(cpu, array, n);
    RemoveAt(cpu, array, n);
}

// 0x8050B740.
void ArrayReplaceAt(CpuContext* cpu, uint32_t array, uint32_t elem, int32_t n) {
    FreeElement(cpu, array, n);
    Memcpy4(cpu, ArrayNth(array, n), elem, Memory::Read32(array + kElemSize));
}

// 0x8050B800: a binary search (comparator(element, key); found once any
// compares equal) or a linear one (comparator(key, element)) from `from`.
int32_t ArraySearch(CpuContext* cpu, uint32_t array, uint32_t key, uint32_t compare, int32_t from, bool sorted) {
    if (!array || !Count(array))
        return -1;

    const int32_t count = Count(array);
    const uint32_t size = Memory::Read32(array + kElemSize);
    const uint32_t base = NthOf(array, from, count);
    uint32_t result = 0;
    bool found = true;

    if (sorted) {
        int32_t low = 0, high = count - from - 1;
        found = false;

        while (low <= high) {
            const int32_t mid = (low + high) >> 1;
            const int32_t order = int32_t(Guest(cpu, compare, {base + uint32_t(mid) * size, key}));

            if (order == 0)
                found = true;

            if (order < 0)
                low = mid + 1;
            else
                high = mid - 1;
        }

        result = base + uint32_t(low) * size;
    } else {
        for (int32_t i = 0; i < count - from; ++i)
            if (Guest(cpu, compare, {key, base + uint32_t(i) * size}) == 0) {
                result = base + size * uint32_t(i);
                break;
            }
    }

    if (!result || !found)
        return -1;

    return int32_t(result - Memory::Read32(array + kList)) / int32_t(Memory::Read32(array + kElemSize));
}

// 0x8050B9A0: fn(element, data) from the last element to the first.
void ArrayMapBackwards(CpuContext* cpu, uint32_t array, uint32_t fn, uint32_t data) {
    for (int32_t i = Count(array) - 1; i > -1; --i)
        Guest(cpu, fn, {i < Count(array) ? NthOf(array, i, Count(array)) : 0, data});
}

// 0x8050BA40: the same until fn returns 0; that element, or 0.
uint32_t ArrayMapBackwards2(CpuContext* cpu, uint32_t array, uint32_t fn, uint32_t data) {
    for (int32_t i = Count(array) - 1; i > -1; --i) {
        const uint32_t elem = i < Count(array) ? NthOf(array, i, Count(array)) : 0;

        if (!Guest(cpu, fn, {elem, data}))
            return elem;
    }

    return 0;
}

// 0x8050BAE0.
uint32_t TableNew2(CpuContext* cpu, uint32_t elemSize, int32_t buckets, int32_t chains, uint32_t hash, uint32_t compare, uint32_t freeFn) {
    const uint32_t table = Malloc(cpu, kTableBytes);
    Memory::Write32(table + kBuckets, Malloc(cpu, uint32_t(buckets) << 2));

    for (int32_t i = 0; i < buckets; ++i) {
        const uint32_t array = ArrayNew(cpu, elemSize, chains, freeFn);
        Memory::Write32(Memory::Read32(table + kBuckets) + 4 * uint32_t(i), array);
    }

    Memory::Write32(table + kBucketCount, uint32_t(buckets));
    Memory::Write32(table + kTableFree, freeFn);
    Memory::Write32(table + kCompare, compare);
    Memory::Write32(table + kHash, hash);
    return table;
}

// 0x8050BB90.
void TableFree(CpuContext* cpu, uint32_t table) {
    if (!table)
        return;

    for (int32_t i = 0; i < int32_t(Memory::Read32(table + kBucketCount)); ++i)
        ArrayFree(cpu, Bucket(table, uint32_t(i)));

    Free(cpu, Memory::Read32(table + kBuckets));
    Free(cpu, table);
}

// 0x8050BC10: replaces an equal element, else appends to its bucket.
void TableEnter(CpuContext* cpu, uint32_t table, uint32_t elem) {
    if (!table)
        return;

    const uint32_t hash = HashOf(cpu, table, elem);
    const int32_t n = ArraySearch(cpu, Bucket(table, hash), elem, Memory::Read32(table + kCompare), 0, false);

    if (n == -1)
        ArrayAppend(cpu, Bucket(table, hash), elem);
    else
        ArrayReplaceAt(cpu, Bucket(table, hash), elem, n);
}

// 0x8050BCC0.
bool TableRemove(CpuContext* cpu, uint32_t table, uint32_t elem) {
    if (!table)
        return false;

    const uint32_t hash = HashOf(cpu, table, elem);
    const int32_t n = ArraySearch(cpu, Bucket(table, hash), elem, Memory::Read32(table + kCompare), 0, false);

    if (n == -1)
        return false;

    ArrayDeleteAt(cpu, Bucket(table, hash), n);
    return true;
}

// 0x8050BD70.
uint32_t TableLookup(CpuContext* cpu, uint32_t table, uint32_t elem) {
    if (!table)
        return 0;

    const uint32_t hash = HashOf(cpu, table, elem);
    const int32_t n = ArraySearch(cpu, Bucket(table, hash), elem, Memory::Read32(table + kCompare), 0, false);
    return n == -1 ? 0 : ArrayNth(Bucket(table, hash), n);
}

// 0x8050BE10.
void TableMap(CpuContext* cpu, uint32_t table, uint32_t fn, uint32_t data) {
    for (int32_t i = 0; i < int32_t(Memory::Read32(table + kBucketCount)); ++i)
        ArrayMapBackwards(cpu, Bucket(table, uint32_t(i)), fn, data);
}

// 0x8050BE80.
uint32_t TableMap2(CpuContext* cpu, uint32_t table, uint32_t fn, uint32_t data) {
    for (int32_t i = 0; i < int32_t(Memory::Read32(table + kBucketCount)); ++i)
        if (const uint32_t elem = ArrayMapBackwards2(cpu, Bucket(table, uint32_t(i)), fn, data))
            return elem;

    return 0;
}

// 0x8050BF00: the protocol is not passed on.
int32_t Socket(CpuContext* cpu, uint32_t domain, uint32_t type, uint32_t) {
    return SocketResult(Guest(cpu, SOSocket, {domain, type, 0}));
}

// 0x8050BF40.
int32_t CloseSocket(CpuContext* cpu, uint32_t s) {
    return SocketResult(Guest(cpu, SOClose, {s}));
}

// 0x8050BF80: an address without a port binds nothing.
int32_t Bind(CpuContext* cpu, uint32_t s, uint32_t addr, uint32_t length) {
    if (!Memory::Read16(addr + kAddrPort))
        return 0;

    GuestFrame frame(cpu, 0x20);
    const uint32_t local = frame[8];
    Memcpy4(cpu, local, addr, kAddrBytes);
    Memory::Write8(local + kAddrLength, uint8_t(length));
    return SocketResult(Guest(cpu, SOBind, {s, local}));
}

// 0x8050C000: never blocks (SO_MSG_DONTWAIT in place of the flags).
int32_t RecvFrom(CpuContext* cpu, uint32_t s, uint32_t buffer, uint32_t length, uint32_t, uint32_t from, uint32_t fromLength) {
    Memory::Write8(from + kAddrLength, uint8_t(Memory::Read32(fromLength)));
    const uint32_t result = Guest(cpu, SORecvFrom, {s, buffer, length, 4, from});
    Memory::Write32(fromLength, Memory::Read8(from + kAddrLength));
    return SocketResult(result);
}

// 0x8050C060.
int32_t SendTo(CpuContext* cpu, uint32_t s, uint32_t buffer, uint32_t length, uint32_t flags, uint32_t to, uint32_t toLength) {
    GuestFrame frame(cpu, 0x30);
    const uint32_t local = frame[8];
    Memcpy4(cpu, local, to, kAddrBytes);
    Memory::Write8(local + kAddrLength, uint8_t(toLength));
    return SocketResult(Guest(cpu, SOSendTo, {s, buffer, length, flags, local}));
}

// 0x8050C0E0.
int32_t GetSockName(CpuContext* cpu, uint32_t s, uint32_t addr, uint32_t length) {
    Memory::Write8(addr + kAddrLength, uint8_t(Memory::Read32(length)));
    const uint32_t result = Guest(cpu, SOGetSockName, {s, addr});
    Memory::Write32(length, Memory::Read8(addr + kAddrLength));
    return SocketResult(result);
}

// 0x8050C140: INADDR_NONE (-1) for a name that is no address.
uint32_t InetAddr(CpuContext* cpu, uint32_t name) {
    GuestFrame frame(cpu, 0x10);
    const uint32_t address = frame[8];

    if (!Guest(cpu, SOInetAtoN, {name, address}))
        return 0xFFFFFFFF;

    return Memory::Read32(address);
}

// 0x8050C190: a poll that does not wait. Readable: POLLRDNORM or POLLHUP
// (0x41); writable: POLLWRNORM (8); exception: 0x20.
int32_t SocketSelect(CpuContext* cpu, uint32_t s, uint32_t readable, uint32_t writable, uint32_t exception) {
    GuestFrame frame(cpu, 0x30);
    const uint32_t poll = frame[8];
    Memory::Write32(poll, s);
    Memory::Write32(poll + 4, (readable ? 1u : 0u) | (writable ? 8u : 0u));
    Memory::Write32(poll + 8, 0);
    const int32_t result = int32_t(Guest(cpu, SOPoll, {poll, 1, 0, 0}));

    if (result < 0)
        return -1;

    const uint32_t events = Memory::Read32(poll + 8);

    if (readable)
        Memory::Write32(readable, result > 0 && (events & 0x41) ? 1 : 0);

    if (writable)
        Memory::Write32(writable, result > 0 && (events & 8) ? 1 : 0);

    if (exception)
        Memory::Write32(exception, result > 0 && (events & 0x20) ? 1 : 0);

    return result;
}

// 0x8050C850.
uint32_t CanReceiveOnSocket(CpuContext* cpu, uint32_t s) {
    GuestFrame frame(cpu, 0x10);
    const uint32_t flag = frame[8];
    Memory::Write32(flag, 0);

    if (SocketSelect(cpu, s, flag, 0, 0) != 1)
        return 0;

    return Memory::Read32(flag);
}

// 0x8050C8A0.
uint32_t CanSendOnSocket(CpuContext* cpu, uint32_t s) {
    GuestFrame frame(cpu, 0x10);
    const uint32_t flag = frame[8];
    Memory::Write32(flag, 0);

    if (SocketSelect(cpu, s, 0, flag, 0) != 1)
        return 0;

    return Memory::Read32(flag);
}

namespace {

// The host name cache: 31 entries (r13 - 0x3618), each a copied hostent with
// its name, probed quadratically from a hash of the name.
constexpr uint32_t kHostCache = 0x80797CC8;
constexpr uint32_t kHostCacheSlots = 31;
constexpr uint32_t kHostProbes = 15;

// "clear": the name that empties the cache.
constexpr uint32_t kClearName = 0x80796800;

// struct hostent and the cache's name after it.
enum : uint32_t {
    kHostName = 0x0,
    kHostAliases = 0x4,
    kHostAddrType = 0x8,
    kHostLength = 0xA,
    kHostAddrList = 0xC,
    kCachedName = 0x10,
    kCachedBytes = 0x14,
};

void CallbackFree(CpuContext* cpu, uint32_t block) {
    if (block)
        Guest(cpu, Memory::Read32(kMemoryCallbacks + 4), {block});
}

uint32_t CallbackMalloc(CpuContext* cpu, uint32_t bytes) {
    return Guest(cpu, Memory::Read32(kMemoryCallbacks), {bytes});
}

void ClearHostCache(CpuContext* cpu) {
    if (!Memory::Read32(kHostCache))
        return;

    for (uint32_t slot = 0; slot < kHostCacheSlots; ++slot) {
        if (Memory::Read32(Memory::Read32(kHostCache) + 4 * slot)) {
            uint32_t i = 0;

            for (;; ++i) {
                const uint32_t addresses = Memory::Read32(Memory::Read32(Memory::Read32(kHostCache) + 4 * slot) + kHostAddrList);
                const uint32_t address = Memory::Read32(addresses + 4 * i);

                if (!address)
                    break;

                CallbackFree(cpu, address);
            }

            const uint32_t entry = Memory::Read32(Memory::Read32(kHostCache) + 4 * slot);
            CallbackFree(cpu, Memory::Read32(Memory::Read32(entry + kHostAddrList) + 4 * i));
            CallbackFree(cpu, Memory::Read32(Memory::Read32(Memory::Read32(kHostCache) + 4 * slot) + kHostAddrList));
            CallbackFree(cpu, Memory::Read32(Memory::Read32(Memory::Read32(kHostCache) + 4 * slot) + kCachedName));
        }

        CallbackFree(cpu, Memory::Read32(Memory::Read32(kHostCache) + 4 * slot));
    }

    CallbackFree(cpu, Memory::Read32(kHostCache));
    Memory::Write32(kHostCache, 0);
}

} // namespace

// 0x8050C2C0: gethostbyname through the cache. A hit returns the cached
// hostent; a miss looks the name up and returns the socket library's hostent
// (copied into a free slot; with none in 15 probes, not cached).
uint32_t GetHostByName(CpuContext* cpu, uint32_t name) {
    const char* host = reinterpret_cast<const char*>(Memory::GetPointer(name));
    const char* clear = reinterpret_cast<const char*>(Memory::GetPointer(kClearName));

    if (std::strncmp(clear, host, std::strlen(clear)) == 0) {
        ClearHostCache(cpu);
        return 0;
    }

    if (!Memory::Read32(kHostCache)) {
        Memory::Write32(kHostCache, CallbackMalloc(cpu, 4 * kHostCacheSlots));
        Memset4(cpu, Memory::Read32(kHostCache), 0, 4 * kHostCacheSlots);
    }

    const uint32_t length = uint32_t(std::strlen(host));
    uint32_t hash = 0;

    for (uint32_t i = 0; i < length; ++i)
        hash += uint32_t(int32_t(int8_t(host[i]))) << ((i & 7) << 2);

    hash %= kHostCacheSlots;
    const uint32_t cache = Memory::Read32(kHostCache);

    for (uint32_t i = 0; i < kHostProbes; ++i) {
        const uint32_t entry = Memory::Read32(cache + 4 * ((hash + i * i) % kHostCacheSlots));

        if (entry && std::strcmp(reinterpret_cast<const char*>(Memory::GetPointer(Memory::Read32(entry + kCachedName))), host) == 0)
            return entry;
    }

    uint32_t slot = hash;

    if (Memory::Read32(cache + 4 * hash)) {
        uint32_t i = 1;

        for (; i < kHostProbes; ++i)
            if (!Memory::Read32(cache + 4 * ((hash + i * i) % kHostCacheSlots))) {
                slot = (hash + i * i) % kHostCacheSlots;
                break;
            }

        if (i == kHostProbes)
            return Guest(cpu, SOGetHostByName, {name});
    }

    const uint32_t found = Guest(cpu, SOGetHostByName, {name});

    if (!found)
        return 0;

    Memory::Write32(Memory::Read32(kHostCache) + 4 * slot, CallbackMalloc(cpu, kCachedBytes));
    uint32_t addresses = 0;

    while (Memory::Read32(Memory::Read32(found + kHostAddrList) + 4 * addresses))
        ++addresses;

    const uint32_t entry = Memory::Read32(Memory::Read32(kHostCache) + 4 * slot);
    Memory::Write16(entry + kHostAddrType, 2);
    Memory::Write16(entry + kHostLength, Memory::Read16(found + kHostLength));
    Memory::Write32(entry + kHostName, 0);
    Memory::Write32(entry + kHostAliases, 0);
    const uint32_t list = CallbackMalloc(cpu, (addresses + 1) * 4);
    Memory::Write32(Memory::Read32(Memory::Read32(kHostCache) + 4 * slot) + kHostAddrList, list);
    uint32_t copied = 0;

    for (; Memory::Read32(Memory::Read32(found + kHostAddrList) + 4 * copied); ++copied) {
        const uint32_t bytes = uint32_t(int32_t(int16_t(Memory::Read16(found + kHostLength))));
        const uint32_t address = CallbackMalloc(cpu, bytes);
        const uint32_t cached = Memory::Read32(Memory::Read32(kHostCache) + 4 * slot);
        Memory::Write32(Memory::Read32(cached + kHostAddrList) + 4 * copied, address);
        Memcpy4(cpu, Memory::Read32(Memory::Read32(cached + kHostAddrList) + 4 * copied), Memory::Read32(Memory::Read32(found + kHostAddrList) + 4 * copied), bytes);
    }

    const uint32_t cached = Memory::Read32(Memory::Read32(kHostCache) + 4 * slot);
    Memory::Write32(Memory::Read32(cached + kHostAddrList) + 4 * copied, 0);
    const uint32_t copy = CallbackMalloc(cpu, length + 1);
    Memory::Write32(Memory::Read32(Memory::Read32(kHostCache) + 4 * slot) + kCachedName, copy);
    std::strcpy(reinterpret_cast<char*>(Memory::GetPointer(copy)), host);
    return found;
}

namespace {

// 0x8050C8F0: a hostent ("localhost") of the interface's addresses (12 bytes
// each, at most 16) from SOGetInterfaceOpt 0x4002 (count) and 0x4003; its
// length field holds the count.
constexpr uint32_t kLocalHost = 0x80763888;
constexpr uint32_t kLocalAddressList = 0x80763898;
constexpr uint32_t kLocalAddresses = 0x807638D8;
constexpr uint32_t kLocalHostName = 0x80631108;
constexpr uint32_t kLocalAliases = 0x80797CCC;
constexpr uint32_t kLocalAddressBytes = 0xC;

uint32_t GetLocalHost(CpuContext* cpu) {
    GuestFrame frame(cpu, 0x20);
    const uint32_t listLength = frame[8], countLength = frame[0xC], count = frame[0x10];
    Memory::Write32(countLength, 4);
    Guest(cpu, SOGetInterfaceOpt, {0, 0xFFFE, 0x4002, count, countLength});

    if (Memory::Read32(count) - 1 > 0xF)
        return 0;

    Memset4(cpu, kLocalAddresses, 0xBE, Memory::Read32(count) * kLocalAddressBytes);
    Memory::Write32(listLength, Memory::Read32(count) * kLocalAddressBytes);
    Guest(cpu, SOGetInterfaceOpt, {0, 0xFFFE, 0x4003, kLocalAddresses, listLength});
    const int32_t addresses = int32_t(Memory::Read32(count));
    Memory::Write32(kLocalHost + kHostName, kLocalHostName);
    Memory::Write32(kLocalHost + kHostAliases, kLocalAliases);
    Memory::Write16(kLocalHost + kHostAddrType, 2);
    Memory::Write16(kLocalHost + kHostLength, uint16_t(addresses));
    int32_t i = 0;

    for (; i < addresses; ++i)
        Memory::Write32(kLocalAddressList + 4 * uint32_t(i), kLocalAddresses + kLocalAddressBytes * uint32_t(i));

    Memory::Write32(kLocalAddressList + 4 * uint32_t(i), 0);
    Memory::Write32(kLocalHost + kHostAddrList, kLocalAddressList);
    return kLocalHost;
}

// 0x8050CAD0: 10/8, 172.16/12, 192.168/16.
uint32_t IsPrivateIp(uint32_t addr) {
    const uint32_t ip = Memory::Read32(addr);
    const uint32_t first = ip >> 24, second = (ip >> 16) & 0xFF;

    if (first == 10)
        return 1;

    if (first == 172 && second - 16 <= 15)
        return 1;

    return first == 192 && second == 168 ? 1 : 0;
}

// 0x8050CBC0: OSSleepTicks for that many milliseconds.
void Msleep(CpuContext* cpu, uint32_t milliseconds) {
    const uint64_t ticks = uint64_t(milliseconds) * TicksPerMillisecond();
    Guest(cpu, OSSleepTicks, {uint32_t(ticks >> 32), uint32_t(ticks)});
}

// gsAvailable.c: one UDP packet to <gamename>.available.gs.nintendowifi.net
// (the MH3SP patch: available.mh3sp.com) port 27900, resent once after 2 s.
// The answer: FE FD 09, then a big-endian word whose bit 0 means
// unavailable (2), bit 1 temporarily unavailable (3).
constexpr uint32_t kAvailable = 0x80763998;

enum : uint32_t {
    kGameName = 0x0,
    kOverrideHost = 0x40,
    kAvailableSocket = 0x80,
    kServerAddr = 0x84,
    kPacket = 0x8C,
    kPacketName = 0x91,
    kPacketLength = 0xCC,
    kSentTime = 0xD0,
    kResends = 0xD4,
};

// __GSIACResult, r13 - 0x3608: 0 waiting, 1 available, 2 unavailable,
// 3 temporarily unavailable.
constexpr uint32_t kAvailableResult = 0x80797CD8;
constexpr uint32_t kAvailableHostFormat = 0x80631118;
constexpr uint32_t kAvailableReply = 0x80796808;
constexpr uint32_t kAvailablePort = 27900;
constexpr uint32_t kAvailableResend = 2000;

void StartAvailableCheck(CpuContext* cpu, uint32_t gameName) {
    GuestFrame frame(cpu, 0x60);
    const uint32_t hostName = frame[8];
    std::strcpy(reinterpret_cast<char*>(Memory::GetPointer(kAvailable + kGameName)), reinterpret_cast<const char*>(Memory::GetPointer(gameName)));
    Memory::Write32(kAvailable + kAvailableSocket, 0xFFFFFFFF);
    const bool overridden = Memory::Read8(kAvailable + kOverrideHost) != 0;

    if (!overridden) {
        cpu->cr &= ~kCrFloatArgs;
        Guest(cpu, Sprintf, {hostName, kAvailableHostFormat, gameName});
    }

    Memory::Write8(kAvailable + kServerAddr + kAddrFamily, 2);
    Memory::Write16(kAvailable + kServerAddr + kAddrPort, uint16_t(Short(kAvailablePort)));
    const uint32_t host = overridden ? kAvailable + kOverrideHost : hostName;
    Memory::Write32(kAvailable + kServerAddr + kAddrIp, InetAddr(cpu, host));
    bool resolved = true;

    if (Memory::Read32(kAvailable + kServerAddr + kAddrIp) == 0xFFFFFFFF) {
        const uint32_t entry = GetHostByName(cpu, overridden ? kAvailable + kOverrideHost : hostName);

        if (!entry)
            resolved = false;
        else
            Memory::Write32(kAvailable + kServerAddr + kAddrIp, Memory::Read32(Memory::Read32(Memory::Read32(entry + kHostAddrList))));
    }

    if (!resolved)
        return;

    Memory::Write32(kAvailable + kAvailableSocket, uint32_t(Socket(cpu, 2, 2, 17)));

    if (int32_t(Memory::Read32(kAvailable + kAvailableSocket)) == -1)
        return;

    Memory::Write8(kAvailable + kPacket, 9);
    const uint32_t length = StringLength(gameName);
    Memcpy4(cpu, kAvailable + kPacketName, gameName, length + 1);
    Memory::Write32(kAvailable + kPacketLength, length + 6);
    SendTo(cpu, Memory::Read32(kAvailable + kAvailableSocket), kAvailable + kPacket, length + 6, 0, kAvailable + kServerAddr, kAddrBytes);
    Memory::Write32(kAvailable + kSentTime, CurrentTime(cpu));
    Memory::Write32(kAvailable + kResends, 0);
}

uint32_t SetAvailableResult(uint32_t result) {
    Memory::Write32(kAvailableResult, result);
    return result;
}

uint32_t AvailableCheckThink(CpuContext* cpu) {
    GuestFrame frame(cpu, 0xA0);
    const uint32_t fromLength = frame[0x20], from = frame[0x28], reply = frame[0x40];
    Memory::Write32(fromLength, kAddrBytes);
    const uint32_t s = Memory::Read32(kAvailable + kAvailableSocket);

    if (int32_t(s) == -1)
        return SetAvailableResult(1);

    if (CanReceiveOnSocket(cpu, s)) {
        const int32_t received = RecvFrom(cpu, Memory::Read32(kAvailable + kAvailableSocket), reply, 0x40, 0, from, fromLength);
        uint32_t flags = 0;
        bool rejected = true;

        if (received >= 7 && BytesEqual(from + kAddrIp, kAvailable + kServerAddr + kAddrIp, 4) &&
            Memory::Read16(from + kAddrPort) == Memory::Read16(kAvailable + kServerAddr + kAddrPort) && BytesEqual(reply, kAvailableReply, 3)) {
            rejected = false;
            flags = Memory::Read32(reply + 3);
        }

        if (!rejected) {
            CloseSocket(cpu, Memory::Read32(kAvailable + kAvailableSocket));

            if (flags & 1)
                return SetAvailableResult(2);

            if (flags & 2)
                return SetAvailableResult(3);

            return SetAvailableResult(1);
        }
    }

    if (CurrentTime(cpu) > Memory::Read32(kAvailable + kSentTime) + kAvailableResend) {
        if (Memory::Read32(kAvailable + kResends) == 1) {
            CloseSocket(cpu, Memory::Read32(kAvailable + kAvailableSocket));
            return SetAvailableResult(1);
        }

        SendTo(cpu, Memory::Read32(kAvailable + kAvailableSocket), kAvailable + kPacket, Memory::Read32(kAvailable + kPacketLength), 0, kAvailable + kServerAddr, kAddrBytes);
        Memory::Write32(kAvailable + kSentTime, CurrentTime(cpu));
        Memory::Write32(kAvailable + kResends, Memory::Read32(kAvailable + kResends) + 1);
    }

    return 0;
}

// The ports' register wrappers.
void ArrayNewPort(CpuContext* cpu) {
    cpu->gpr[3] = ArrayNew(cpu, cpu->gpr[3], int32_t(cpu->gpr[4]), cpu->gpr[5]);
}

void ArrayFreePort(CpuContext* cpu) {
    ArrayFree(cpu, cpu->gpr[3]);
}

void ArrayLengthPort(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(ArrayLength(cpu->gpr[3]));
}

void ArrayNthPort(CpuContext* cpu) {
    cpu->gpr[3] = ArrayNth(cpu->gpr[3], int32_t(cpu->gpr[4]));
}

void ArrayAppendPort(CpuContext* cpu) {
    ArrayAppend(cpu, cpu->gpr[3], cpu->gpr[4]);
}

void ArrayInsertSortedPort(CpuContext* cpu) {
    ArrayInsertSorted(cpu, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5]);
}

void ArrayRemoveAtPort(CpuContext* cpu) {
    ArrayRemoveAt(cpu, cpu->gpr[3], int32_t(cpu->gpr[4]));
}

void ArrayDeleteAtPort(CpuContext* cpu) {
    ArrayDeleteAt(cpu, cpu->gpr[3], int32_t(cpu->gpr[4]));
}

void ArrayReplaceAtPort(CpuContext* cpu) {
    ArrayReplaceAt(cpu, cpu->gpr[3], cpu->gpr[4], int32_t(cpu->gpr[5]));
}

void ArraySearchPort(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(ArraySearch(cpu, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], int32_t(cpu->gpr[6]), cpu->gpr[7] != 0));
}

void ArrayMapBackwardsPort(CpuContext* cpu) {
    ArrayMapBackwards(cpu, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5]);
}

void ArrayMapBackwards2Port(CpuContext* cpu) {
    cpu->gpr[3] = ArrayMapBackwards2(cpu, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5]);
}

void TableNew2Port(CpuContext* cpu) {
    cpu->gpr[3] = TableNew2(cpu, cpu->gpr[3], int32_t(cpu->gpr[4]), int32_t(cpu->gpr[5]), cpu->gpr[6], cpu->gpr[7], cpu->gpr[8]);
}

void TableFreePort(CpuContext* cpu) {
    TableFree(cpu, cpu->gpr[3]);
}

void TableEnterPort(CpuContext* cpu) {
    TableEnter(cpu, cpu->gpr[3], cpu->gpr[4]);
}

void TableRemovePort(CpuContext* cpu) {
    cpu->gpr[3] = TableRemove(cpu, cpu->gpr[3], cpu->gpr[4]) ? 1 : 0;
}

void TableLookupPort(CpuContext* cpu) {
    cpu->gpr[3] = TableLookup(cpu, cpu->gpr[3], cpu->gpr[4]);
}

void TableMapPort(CpuContext* cpu) {
    TableMap(cpu, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5]);
}

void TableMap2Port(CpuContext* cpu) {
    cpu->gpr[3] = TableMap2(cpu, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5]);
}

void SocketPort(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(Socket(cpu, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5]));
}

void CloseSocketPort(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(CloseSocket(cpu, cpu->gpr[3]));
}

void BindPort(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(Bind(cpu, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5]));
}

void RecvFromPort(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(RecvFrom(cpu, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7], cpu->gpr[8]));
}

void SendToPort(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(SendTo(cpu, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7], cpu->gpr[8]));
}

void GetSockNamePort(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(GetSockName(cpu, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5]));
}

void InetAddrPort(CpuContext* cpu) {
    cpu->gpr[3] = InetAddr(cpu, cpu->gpr[3]);
}

void LastSocketErrorPort(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(LastSocketError());
}

void SocketSelectPort(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(SocketSelect(cpu, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6]));
}

void GetHostByNamePort(CpuContext* cpu) {
    cpu->gpr[3] = GetHostByName(cpu, cpu->gpr[3]);
}

void CanReceiveOnSocketPort(CpuContext* cpu) {
    cpu->gpr[3] = CanReceiveOnSocket(cpu, cpu->gpr[3]);
}

void CanSendOnSocketPort(CpuContext* cpu) {
    cpu->gpr[3] = CanSendOnSocket(cpu, cpu->gpr[3]);
}

void GetLocalHostPort(CpuContext* cpu) {
    cpu->gpr[3] = GetLocalHost(cpu);
}

void IsPrivateIpPort(CpuContext* cpu) {
    cpu->gpr[3] = IsPrivateIp(cpu->gpr[3]);
}

void NothingPort(CpuContext*) {
}

void CurrentTimePort(CpuContext* cpu) {
    cpu->gpr[3] = CurrentTime(cpu);
}

void MsleepPort(CpuContext* cpu) {
    Msleep(cpu, cpu->gpr[3]);
}

void MemoryCallbacksSetPort(CpuContext* cpu) {
    Memory::Write32(kMemoryCallbacks, cpu->gpr[3]);
    Memory::Write32(kMemoryCallbacks + 4, cpu->gpr[4]);
    Memory::Write32(kMemoryCallbacks + 8, cpu->gpr[5]);
    Memory::Write32(kMemoryCallbacks + 12, cpu->gpr[6]);
}

void MallocPort(CpuContext* cpu) {
    cpu->gpr[3] = Malloc(cpu, cpu->gpr[3]);
}

void ReallocPort(CpuContext* cpu) {
    cpu->gpr[3] = Realloc(cpu, cpu->gpr[3], cpu->gpr[4]);
}

void FreePort(CpuContext* cpu) {
    Free(cpu, cpu->gpr[3]);
}

void StartAvailableCheckPort(CpuContext* cpu) {
    StartAvailableCheck(cpu, cpu->gpr[3]);
}

void AvailableCheckThinkPort(CpuContext* cpu) {
    cpu->gpr[3] = AvailableCheckThink(cpu);
}

} // namespace

} // namespace GameSpy

using namespace GameSpy;

extern "C" void func_8050B170(CpuContext*);
extern "C" void func_8050B210(CpuContext*);
extern "C" void func_8050B2B0(CpuContext*);
extern "C" void func_8050B2C0(CpuContext*);
extern "C" void func_8050B2F0(CpuContext*);
extern "C" void func_8050B420(CpuContext*);
extern "C" void func_8050B5A0(CpuContext*);
extern "C" void func_8050B650(CpuContext*);
extern "C" void func_8050B740(CpuContext*);
extern "C" void func_8050B800(CpuContext*);
extern "C" void func_8050B9A0(CpuContext*);
extern "C" void func_8050BA40(CpuContext*);
extern "C" void func_8050BAE0(CpuContext*);
extern "C" void func_8050BB90(CpuContext*);
extern "C" void func_8050BC10(CpuContext*);
extern "C" void func_8050BCC0(CpuContext*);
extern "C" void func_8050BD70(CpuContext*);
extern "C" void func_8050BE10(CpuContext*);
extern "C" void func_8050BE80(CpuContext*);
extern "C" void func_8050C180(CpuContext*);
extern "C" void func_8050CAD0(CpuContext*);
extern "C" void func_8050CB70(CpuContext*);
extern "C" void func_8050CC00(CpuContext*);

extern "C" void MH3_ArrayNew(CpuContext* cpu) {
    PortCheck::Call(cpu, "ArrayNew", func_8050B170, ArrayNewPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050B170, MH3_ArrayNew, "ArrayNew");

extern "C" void MH3_ArrayFree(CpuContext* cpu) {
    PortCheck::Call(cpu, "ArrayFree", func_8050B210, ArrayFreePort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050B210, MH3_ArrayFree, "ArrayFree");

extern "C" void MH3_ArrayLength(CpuContext* cpu) {
    PortCheck::Call(cpu, "ArrayLength", func_8050B2B0, ArrayLengthPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050B2B0, MH3_ArrayLength, "ArrayLength");

extern "C" void MH3_ArrayNth(CpuContext* cpu) {
    PortCheck::Call(cpu, "ArrayNth", func_8050B2C0, ArrayNthPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050B2C0, MH3_ArrayNth, "ArrayNth");

extern "C" void MH3_ArrayAppend(CpuContext* cpu) {
    PortCheck::Call(cpu, "ArrayAppend", func_8050B2F0, ArrayAppendPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050B2F0, MH3_ArrayAppend, "ArrayAppend");

extern "C" void MH3_ArrayInsertSorted(CpuContext* cpu) {
    PortCheck::Call(cpu, "ArrayInsertSorted", func_8050B420, ArrayInsertSortedPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050B420, MH3_ArrayInsertSorted, "ArrayInsertSorted");

extern "C" void MH3_ArrayRemoveAt(CpuContext* cpu) {
    PortCheck::Call(cpu, "ArrayRemoveAt", func_8050B5A0, ArrayRemoveAtPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050B5A0, MH3_ArrayRemoveAt, "ArrayRemoveAt");

extern "C" void MH3_ArrayDeleteAt(CpuContext* cpu) {
    PortCheck::Call(cpu, "ArrayDeleteAt", func_8050B650, ArrayDeleteAtPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050B650, MH3_ArrayDeleteAt, "ArrayDeleteAt");

extern "C" void MH3_ArrayReplaceAt(CpuContext* cpu) {
    PortCheck::Call(cpu, "ArrayReplaceAt", func_8050B740, ArrayReplaceAtPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050B740, MH3_ArrayReplaceAt, "ArrayReplaceAt");

extern "C" void MH3_ArraySearch(CpuContext* cpu) {
    PortCheck::Call(cpu, "ArraySearch", func_8050B800, ArraySearchPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050B800, MH3_ArraySearch, "ArraySearch");

extern "C" void MH3_ArrayMapBackwards(CpuContext* cpu) {
    PortCheck::Call(cpu, "ArrayMapBackwards", func_8050B9A0, ArrayMapBackwardsPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050B9A0, MH3_ArrayMapBackwards, "ArrayMapBackwards");

extern "C" void MH3_ArrayMapBackwards2(CpuContext* cpu) {
    PortCheck::Call(cpu, "ArrayMapBackwards2", func_8050BA40, ArrayMapBackwards2Port);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050BA40, MH3_ArrayMapBackwards2, "ArrayMapBackwards2");

extern "C" void MH3_TableNew2(CpuContext* cpu) {
    PortCheck::Call(cpu, "TableNew2", func_8050BAE0, TableNew2Port);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050BAE0, MH3_TableNew2, "TableNew2");

extern "C" void MH3_TableFree(CpuContext* cpu) {
    PortCheck::Call(cpu, "TableFree", func_8050BB90, TableFreePort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050BB90, MH3_TableFree, "TableFree");

extern "C" void MH3_TableEnter(CpuContext* cpu) {
    PortCheck::Call(cpu, "TableEnter", func_8050BC10, TableEnterPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050BC10, MH3_TableEnter, "TableEnter");

extern "C" void MH3_TableRemove(CpuContext* cpu) {
    PortCheck::Call(cpu, "TableRemove", func_8050BCC0, TableRemovePort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050BCC0, MH3_TableRemove, "TableRemove");

extern "C" void MH3_TableLookup(CpuContext* cpu) {
    PortCheck::Call(cpu, "TableLookup", func_8050BD70, TableLookupPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050BD70, MH3_TableLookup, "TableLookup");

extern "C" void MH3_TableMap(CpuContext* cpu) {
    PortCheck::Call(cpu, "TableMap", func_8050BE10, TableMapPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050BE10, MH3_TableMap, "TableMap");

extern "C" void MH3_TableMap2(CpuContext* cpu) {
    PortCheck::Call(cpu, "TableMap2", func_8050BE80, TableMap2Port);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050BE80, MH3_TableMap2, "TableMap2");

extern "C" void MH3_GsiSocket(CpuContext* cpu) {
    SocketPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050BF00, MH3_GsiSocket, "gsiSocket");

extern "C" void MH3_GsiCloseSocket(CpuContext* cpu) {
    CloseSocketPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050BF40, MH3_GsiCloseSocket, "gsiCloseSocket");

extern "C" void MH3_GsiBind(CpuContext* cpu) {
    BindPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050BF80, MH3_GsiBind, "gsiBind");

extern "C" void MH3_GsiRecvFrom(CpuContext* cpu) {
    RecvFromPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050C000, MH3_GsiRecvFrom, "gsiRecvFrom");

extern "C" void MH3_GsiSendTo(CpuContext* cpu) {
    SendToPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050C060, MH3_GsiSendTo, "gsiSendTo");

extern "C" void MH3_GsiGetSockName(CpuContext* cpu) {
    GetSockNamePort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050C0E0, MH3_GsiGetSockName, "gsiGetSockName");

extern "C" void MH3_GsiInetAddr(CpuContext* cpu) {
    InetAddrPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050C140, MH3_GsiInetAddr, "gsiInetAddr");

extern "C" void MH3_GOAGetLastError(CpuContext* cpu) {
    PortCheck::Call(cpu, "GOAGetLastError", func_8050C180, LastSocketErrorPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050C180, MH3_GOAGetLastError, "GOAGetLastError");

extern "C" void MH3_GSISocketSelect(CpuContext* cpu) {
    SocketSelectPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050C190, MH3_GSISocketSelect, "GSISocketSelect");

extern "C" void MH3_GsiGetHostByName(CpuContext* cpu) {
    GetHostByNamePort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050C2C0, MH3_GsiGetHostByName, "gsiGetHostByName");

extern "C" void MH3_CanReceiveOnSocket(CpuContext* cpu) {
    CanReceiveOnSocketPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050C850, MH3_CanReceiveOnSocket, "CanReceiveOnSocket");

extern "C" void MH3_CanSendOnSocket(CpuContext* cpu) {
    CanSendOnSocketPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050C8A0, MH3_CanSendOnSocket, "CanSendOnSocket");

extern "C" void MH3_GsiGetLocalHost(CpuContext* cpu) {
    GetLocalHostPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050C8F0, MH3_GsiGetLocalHost, "gsiGetLocalHost");

extern "C" void MH3_IsPrivateIP(CpuContext* cpu) {
    PortCheck::Call(cpu, "IsPrivateIP", func_8050CAD0, IsPrivateIpPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050CAD0, MH3_IsPrivateIP, "IsPrivateIP");

extern "C" void MH3_GsiStartup(CpuContext* cpu) {
    NothingPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050CB50, MH3_GsiStartup, "gsiStartup");

extern "C" void MH3_GsiCleanup(CpuContext* cpu) {
    NothingPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050CB60, MH3_GsiCleanup, "gsiCleanup");

extern "C" void MH3_CurrentTime(CpuContext* cpu) {
    PortCheck::Call(cpu, "current_time", func_8050CB70, CurrentTimePort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050CB70, MH3_CurrentTime, "current_time");

extern "C" void MH3_Msleep(CpuContext* cpu) {
    MsleepPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050CBC0, MH3_Msleep, "msleep");

extern "C" void MH3_GsiMemoryCallbacksSet(CpuContext* cpu) {
    PortCheck::Call(cpu, "gsiMemoryCallbacksSet", func_8050CC00, MemoryCallbacksSetPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050CC00, MH3_GsiMemoryCallbacksSet, "gsiMemoryCallbacksSet");

extern "C" void MH3_Gsimalloc(CpuContext* cpu) {
    MallocPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050CC20, MH3_Gsimalloc, "gsimalloc");

extern "C" void MH3_Gsirealloc(CpuContext* cpu) {
    ReallocPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050CC30, MH3_Gsirealloc, "gsirealloc");

extern "C" void MH3_Gsifree(CpuContext* cpu) {
    FreePort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050CC50, MH3_Gsifree, "gsifree");

extern "C" void MH3_GSIStartAvailableCheck(CpuContext* cpu) {
    StartAvailableCheckPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050CC70, MH3_GSIStartAvailableCheck, "GSIStartAvailableCheck");

extern "C" void MH3_GSIAvailableCheckThink(CpuContext* cpu) {
    AvailableCheckThinkPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050CDF0, MH3_GSIAvailableCheckThink, "GSIAvailableCheckThink");
