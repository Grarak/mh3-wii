// The Wii socket library (SO, RMHP08 0x8051D8CC..0x805217D8; decompiled with
// Ghidra, the call arguments checked in the translated C++), ported natively.
// Each call marshals its arguments into an IPC block from the library's
// allocator, sends an ioctl or ioctlv to /dev/net/ip/top (IOS_Ioctl and
// IOS_Ioctlv are native, ios.cpp; the device is ios_net.cpp) and stores the
// result as the calling thread's socket error. GameSpy (gamespy_common.cpp)
// and the game's own connection code call these every frame.
//
// The interrupt masks the library takes around its state stay (MSR EE); guest
// threads switch only at OS calls, so nothing runs between them anyway. The
// network configuration calls (NWC24's suspend and resume around a socket
// call before SOStartup, NCD's link status) stay guest calls.
#include "abi_bridge.h"
#include "guest_call.h"
#include "memory.h"

#include <cstdio>
#include <cstring>

namespace {

enum : uint32_t {
    IOSOpen = 0x804BC500,
    IOSClose = 0x804BC810,
    IOSIoctl = 0x804BCFB0,
    IOSIoctlv = 0x804BD310,
    OSRegisterVersion = 0x804CBA00,
    OSSleepTicks = 0x804D5320,
    OSGetSystemTime = 0x804D5400,
    NcdGetLinkStatus = 0x8051CE24,
    Nwc24Startup = 0x8051E528,
    Nwc24Cleanup = 0x8051E538,
    Nwc24SuspendForSocket = 0x8051E548,
    Nwc24ResumeForSocket = 0x8051E558,
};

// The library's configuration and state (0x80769100).
enum : uint32_t {
    kAllocFunction = 0x80769100, // void* (*)(u32 kind, s32 size)
    kFreeFunction = 0x80769104,  // void (*)(u32 kind, void* block, s32 size)
    kStartState = 0x80769108,    // s32: -2 stopped, -1 starting, 0 started
    kFd = 0x8076910C,            // /dev/net/ip/top
    kAllocations = 0x80769114,   // blocks out
    kHostEntry = 0x80769110,     // SOGetHostByName's hostent (0x460 bytes)
    kNtoaBuffer = 0x80769118,    // SOInetNtoA's result
};

// Small-data globals.
enum : uint32_t {
    kState = 0x80797D98,    // u8: 0 not initialized, 2 started
    kError = 0x80797D9C,    // the socket error with no current thread
    kMem2Only = 0x80796914, // IPC blocks must lie in MEM2
    kVersionDone = 0x80797DA8,
    kVersion = 0x80796918,     // the library's version string
    kInitVersionDone = 0x80797DA0,
    kInitVersion = 0x80796910, // SOInit's version string
};

constexpr uint32_t kDevice = 0x80631B38; // "/dev/net/ip/top"
constexpr uint32_t kCurrentThread = 0x800000E4;
constexpr uint32_t kBusClock = 0x800000F8;
constexpr uint32_t kThreadSocketError = 0x30C;
constexpr uint32_t kMsrEe = 0x8000;

// The allocator's kinds: an address info list, the hostent, an IPC block, a
// receive's bounce buffer, a send's.
enum : uint32_t {
    kKindAddrInfo = 10,
    kKindHostEntry = 11,
    kKindIoctl = 12,
    kKindReceive = 13,
    kKindSend = 14,
};

// /dev/net/ip/top ioctls.
enum : uint32_t {
    kAccept = 1,
    kBind = 2,
    kClose = 3,
    kConnect = 4,
    kFcntl = 5,
    kGetPeerName = 6,
    kGetSockName = 7,
    kListen = 10,
    kPoll = 11,
    kRecvFrom = 12,
    kSendTo = 13,
    kShutdown = 14,
    kSocket = 15,
    kSetSockOpt = 9,
    kGetHostId = 16,
    kGetHostByName = 17,
    kInetAtoN = 21,
    kGetAddrInfo = 24,
    kGetInterfaceOpt = 28,
    kStartup = 31,
};

// Library errors (SO_E*).
constexpr int32_t kEAfNoSupport = -5;
constexpr int32_t kEAlready = -7;
constexpr int32_t kEInUse = -6;
constexpr int32_t kEBusy = -10;
constexpr int32_t kENoDevice = -26;
constexpr int32_t kEInval = -28;
constexpr int32_t kENetDown = -39;
constexpr int32_t kENetUnreach = -48;
constexpr int32_t kENoMem = -49;
constexpr int32_t kEConnRefused = -45;
constexpr int32_t kETimedOut = -76;
constexpr int32_t kENetReset = -112;
constexpr int32_t kELinkDown = -121;
constexpr int32_t kEAiFamily = -68;
constexpr int32_t kEFatal = int32_t(0x80000000);

constexpr uint32_t kAfInet6 = 23;
constexpr uint32_t kAddrBytes = 8; // sockaddr_in: length, family, port, address
constexpr int32_t kMaxReceive = 0x8000;
constexpr uint32_t kHostEntryBytes = 0x460;
constexpr uint32_t kAddrInfoBytes = 0x840;
constexpr uint32_t kMaxOptionBytes = 0x14;

uint32_t RoundUp32(uint32_t bytes) {
    return (bytes + 0x1F) & ~0x1Fu;
}

bool InMem2(uint32_t address) {
    const uint32_t physical = address & 0x1FFFFFFF;
    return physical >= 0x10000000 && physical < 0x18000000;
}

uint32_t GuestLength(uint32_t s) {
    uint32_t n = 0;

    while (Memory::Read8(s + n))
        ++n;

    return n;
}

// OSDisableInterrupts / OSRestoreInterrupts.
struct InterruptsOff {
    CpuContext* cpu;
    uint32_t enabled;

    explicit InterruptsOff(CpuContext* cpu) : cpu(cpu), enabled(cpu->msr & kMsrEe) {
        cpu->msr &= ~kMsrEe;
    }

    void Again() {
        enabled = cpu->msr & kMsrEe;
        cpu->msr &= ~kMsrEe;
    }

    void Restore() {
        cpu->msr = enabled ? cpu->msr | kMsrEe : cpu->msr & ~kMsrEe;
    }
};

// The current thread's socket error, or the library's with no thread.
void StoreError(int32_t error) {
    if (const uint32_t thread = Memory::Read32(kCurrentThread))
        Memory::Write32(thread + kThreadSocketError, uint32_t(error));
    else
        Memory::Write32(kError, uint32_t(error));
}

// 0x8051D8CC: the library's memmove.
void Move(uint32_t to, uint32_t from, uint32_t bytes) {
    if (bytes && to != from)
        std::memmove(Memory::GetPointer(to, bytes), Memory::GetPointer(from, bytes), bytes);
}

// 0x8051DC8C: the library's memset.
void Fill(uint32_t to, uint32_t value, uint32_t bytes) {
    if (bytes)
        std::memset(Memory::GetPointer(to, bytes), int(value & 0xFF), bytes);
}

// 0x8051F828: a block from the allocator (none for a size below 1 or with no
// allocator); one outside MEM2 goes back when the library wants MEM2.
uint32_t Alloc(CpuContext* cpu, uint32_t kind, int32_t size) {
    const uint32_t alloc = Memory::Read32(kAllocFunction);

    if (size < 1 || !alloc)
        return 0;

    GuestCall call{cpu};
    const uint32_t block = call(alloc, {kind, uint32_t(size)});

    if (!block)
        return 0;

    Memory::Write32(kAllocations, Memory::Read32(kAllocations) + 1);

    if (!Memory::Read32(kMem2Only) || InMem2(block))
        return block;

    if (const uint32_t free = Memory::Read32(kFreeFunction)) {
        Memory::Write32(kAllocations, Memory::Read32(kAllocations) - 1);
        call(free, {kind, block, uint32_t(size)});
    }

    return 0;
}

// 0x8051F910.
void Free(CpuContext* cpu, uint32_t kind, uint32_t block, int32_t size) {
    const uint32_t free = Memory::Read32(kFreeFunction);

    if (!block || !free)
        return;

    Memory::Write32(kAllocations, Memory::Read32(kAllocations) - 1);
    GuestCall{cpu}(free, {kind, block, uint32_t(size)});
}

// 0x8051F944: the device's descriptor once the library is started; else an
// error, which becomes the socket error.
int32_t GetFd(CpuContext* cpu, uint32_t& fd) {
    InterruptsOff off(cpu);
    const uint8_t state = Memory::Read8(kState);
    int32_t error = 0;

    if (state == 0)
        error = kENetDown;
    else if (state != 2)
        error = kEInval;
    else if (int32_t(Memory::Read32(kStartState)) < 0)
        error = kEBusy;
    else if (!Memory::Read32(kCurrentThread))
        error = kEFatal;
    else
        fd = Memory::Read32(kFd);

    if (error)
        StoreError(error);

    off.Restore();
    return error;
}

// 0x8051FA2C: the result becomes the socket error.
int32_t SetError(CpuContext* cpu, int32_t result) {
    InterruptsOff off(cpu);
    StoreError(result);
    off.Restore();
    return result;
}

// 0x8051FD60: a call that opened the device for itself (`opened` 1) resumes
// NWC24 and closes the device again; a failure there is the result.
int32_t FinishStartup(CpuContext* cpu, int32_t result, uint32_t opened) {
    if (opened == 1) {
        GuestCall call{cpu};
        const int32_t resumed = int32_t(call.Call<Nwc24ResumeForSocket>());

        if (resumed == -29)
            result = kENoDevice;
        else if (resumed != 0)
            result = kEFatal;

        if (int32_t(call.Call<IOSClose>({Memory::Read32(kFd)})) < 0)
            result = kEFatal;
    }

    InterruptsOff off(cpu);

    if (opened == 1 && result != kEFatal) {
        Memory::Write32(kFd, 0xFFFFFFFF);
        Memory::Write32(kStartState, uint32_t(-2));
    }

    StoreError(result);
    off.Restore();
    return result;
}

// 0x8051FA88: the descriptor for a call that also works before SOStartup.
// Started, the library's; initialized only, the device opened for this call
// (`opened` 1, with NWC24 suspended), which FinishStartup closes.
struct StartedFd {
    uint32_t fd = 0;
    uint32_t opened = 0;
    bool hasFd = false;     // written: the original stores these as it goes
    bool hasOpened = false;
};

int32_t StartFd(CpuContext* cpu, StartedFd& out) {
    InterruptsOff off(cpu);
    GuestCall call{cpu};
    const uint8_t state = Memory::Read8(kState);
    int32_t error = 0;

    if (state == 0) {
        error = kENetDown;
    } else if (state == 2) {
        if (int32_t(Memory::Read32(kStartState)) < 0)
            error = kEBusy;
        else if (!Memory::Read32(kCurrentThread))
            error = kEFatal;
        else {
            out.opened = 0;
            out.hasOpened = true;
            out.fd = Memory::Read32(kFd);
            out.hasFd = true;
        }
    } else if (int32_t(Memory::Read32(kStartState)) > -2) {
        error = kEBusy;
    } else if (!Memory::Read32(kCurrentThread)) {
        error = kEFatal;
    } else {
        Memory::Write32(kStartState, 0xFFFFFFFF);
        off.Restore();
        const int32_t device = int32_t(call.Call<IOSOpen>({kDevice, 0}));
        Memory::Write32(kFd, uint32_t(device));

        if (device < 0) {
            off.Again();

            if (device == -6) {
                error = kENoDevice;
                Memory::Write32(kStartState, uint32_t(-2));
            } else {
                error = kEFatal;
            }
        } else {
            out.fd = uint32_t(device);
            out.hasFd = true;
            const int32_t suspended = int32_t(call.Call<Nwc24SuspendForSocket>());

            if (suspended) {
                error = suspended == -29 ? kENoDevice : suspended == -15 ? kENetUnreach : suspended == -2 ? kENetDown : kEFatal;

                if (int32_t(call.Call<IOSClose>({uint32_t(device)})) < 0)
                    error = kEFatal;

                off.Again();

                if (error != kEFatal) {
                    Memory::Write32(kFd, 0xFFFFFFFF);
                    Memory::Write32(kStartState, uint32_t(-2));
                }
            } else {
                const int32_t link = int32_t(call.Call<NcdGetLinkStatus>());

                if (link >= 3 && link < 6) {
                    const int32_t started = int32_t(call.Call<IOSIoctl>({uint32_t(device), kStartup, 0, 0, 0, 0}));

                    if (started == 0) {
                        out.opened = 1;
                        out.hasOpened = true;
                    } else {
                        error = FinishStartup(cpu, started, 1);
                    }
                } else if (link == -8) {
                    error = FinishStartup(cpu, kENoDevice, 1);
                } else if (link < 0 && link >= -2) {
                    error = FinishStartup(cpu, kEFatal, 1);
                } else {
                    error = FinishStartup(cpu, kENetUnreach, 1);
                }

                off.Again();
            }
        }
    }

    if (error)
        StoreError(error);

    off.Restore();
    return error;
}

// An ioctl whose input is `words` (each 4 bytes) in a 32-byte IPC block.
int32_t SimpleIoctl(CpuContext* cpu, uint32_t command, std::initializer_list<uint32_t> words) {
    uint32_t fd;

    if (const int32_t error = GetFd(cpu, fd))
        return error;

    int32_t result = kENoMem;

    if (const uint32_t block = Alloc(cpu, kKindIoctl, 0x20)) {
        uint32_t at = block;

        for (const uint32_t word : words) {
            Memory::Write32(at, word);
            at += 4;
        }

        result = int32_t(GuestCall{cpu}.Call<IOSIoctl>({fd, command, block, uint32_t(words.size() * 4), 0, 0}));
        Free(cpu, kKindIoctl, block, 0x20);
    }

    return SetError(cpu, result);
}

// 0x8051FF7C, and SOSocket 0x80520050 (which registers the library's
// version first). AF_INET6 is not supported.
int32_t Socket(CpuContext* cpu, uint32_t domain, uint32_t type, uint32_t protocol) {
    uint32_t fd;

    if (const int32_t error = GetFd(cpu, fd))
        return error;

    if (domain == kAfInet6)
        return SetError(cpu, kEAfNoSupport);

    int32_t result = kENoMem;

    if (const uint32_t block = Alloc(cpu, kKindIoctl, 0x20)) {
        Memory::Write32(block, domain);
        Memory::Write32(block + 4, type);
        Memory::Write32(block + 8, protocol);
        result = int32_t(GuestCall{cpu}.Call<IOSIoctl>({fd, kSocket, block, 12, 0, 0}));
        Free(cpu, kKindIoctl, block, 0x20);
    }

    return SetError(cpu, result);
}

// An address in, as SOBind and SOConnect take it: 8 bytes after the socket
// and a 1.
int32_t AddressIn(CpuContext* cpu, uint32_t command, uint32_t s, uint32_t addr) {
    uint32_t fd;

    if (const int32_t error = GetFd(cpu, fd))
        return error;

    if (!addr || Memory::Read8(addr) != kAddrBytes)
        return SetError(cpu, kEInval);

    int32_t result = kENoMem;

    if (const uint32_t block = Alloc(cpu, kKindIoctl, 0x40)) {
        Memory::Write32(block, s);
        Memory::Write32(block + 4, 1);
        Move(block + 8, addr, kAddrBytes);
        result = int32_t(GuestCall{cpu}.Call<IOSIoctl>({fd, command, block, 0x24, 0, 0}));
        Free(cpu, kKindIoctl, block, 0x40);
    }

    return SetError(cpu, result);
}

// An address out, as SOGetSockName and SOGetPeerName give it (and SOAccept,
// which may have none): the device fills it after the block's first 32 bytes.
int32_t AddressOut(CpuContext* cpu, uint32_t command, uint32_t s, uint32_t addr, bool optional) {
    uint32_t fd;

    if (const int32_t error = GetFd(cpu, fd))
        return error;

    if (addr ? Memory::Read8(addr) != kAddrBytes : !optional)
        return SetError(cpu, kEInval);

    const uint32_t length = addr ? Memory::Read8(addr) : 0;
    const uint32_t size = optional ? RoundUp32(addr ? length + 0x20 : 4) : RoundUp32(length + 0x20);
    int32_t result = kENoMem;

    if (const uint32_t block = Alloc(cpu, kKindIoctl, int32_t(size))) {
        GuestCall call{cpu};
        const uint32_t out = block + 0x20;
        Memory::Write32(block, s);

        if (!addr) {
            result = int32_t(call.Call<IOSIoctl>({fd, command, block, 4, 0, 0}));
        } else {
            Move(out, addr, length);
            result = int32_t(call.Call<IOSIoctl>({fd, command, block, 4, out, length}));

            if (result >= 0)
                Move(addr, out, Memory::Read8(out));
        }

        Free(cpu, kKindIoctl, block, int32_t(size));
    }

    return SetError(cpu, result);
}

// A buffer the device can use in place: 32-byte aligned, a multiple of 32
// bytes long, and in MEM2 when the library wants MEM2.
bool Direct(uint32_t buffer, int32_t length) {
    if (length == 0)
        return true;

    return (buffer & 0x1F) == 0 && length % 32 == 0 && (!Memory::Read32(kMem2Only) || InMem2(buffer));
}

// 0x80520CF0: SORecvFrom and SORecv. At most 32 KiB; an unaligned buffer
// receives through a bounce buffer, which is copied whole.
int32_t Receive(CpuContext* cpu, uint32_t s, uint32_t buffer, int32_t length, uint32_t flags, uint32_t from) {
    if (length > kMaxReceive)
        length = kMaxReceive;

    uint32_t fd;

    if (const int32_t error = GetFd(cpu, fd))
        return error;

    if ((from && Memory::Read8(from) != kAddrBytes) || length < 0 || (length > 0 && !buffer))
        return SetError(cpu, kEInval);

    const bool direct = Direct(buffer, length);
    const uint32_t fromLength = from ? Memory::Read8(from) : 0;
    const uint32_t size = (fromLength + 0x5F) & ~0x1Fu;
    const uint32_t bounceSize = RoundUp32(uint32_t(length));
    const uint32_t block = Alloc(cpu, kKindIoctl, int32_t(size));
    const uint32_t data = direct ? buffer : Alloc(cpu, kKindReceive, int32_t(bounceSize));
    int32_t result = kENoMem;

    if (block && data) {
        GuestCall call{cpu};
        const uint32_t address = block + 0x40;
        Memory::Write32(block + 0x20, s);
        Memory::Write32(block + 0x24, flags);
        Memory::Write32(block, block + 0x20);
        Memory::Write32(block + 4, 8);
        Memory::Write32(block + 8, data);
        Memory::Write32(block + 12, uint32_t(length));

        if (!from) {
            Memory::Write32(block + 16, 0);
            Memory::Write32(block + 20, 0);
            result = int32_t(call.Call<IOSIoctlv>({fd, kRecvFrom, 1, 2, block}));
        } else {
            Move(address, from, fromLength);
            Memory::Write32(block + 16, address);
            Memory::Write32(block + 20, fromLength);
            result = int32_t(call.Call<IOSIoctlv>({fd, kRecvFrom, 1, 2, block}));

            if (result >= 0) {
                const uint8_t want = Memory::Read8(from), got = Memory::Read8(address);
                Move(from, address, got < want ? got : want);
            }
        }

        if (result >= 0 && !direct)
            Move(buffer, data, uint32_t(length));
    }

    if (!direct)
        Free(cpu, kKindReceive, data, int32_t(bounceSize));

    Free(cpu, kKindIoctl, block, int32_t(size));
    return SetError(cpu, result);
}

// 0x80520FA8: SOSendTo and SOSend.
int32_t Send(CpuContext* cpu, uint32_t s, uint32_t buffer, int32_t length, uint32_t flags, uint32_t to) {
    uint32_t fd;

    if (const int32_t error = GetFd(cpu, fd))
        return error;

    if ((to && Memory::Read8(to) != kAddrBytes) || length < 0 || (length > 0 && !buffer))
        return SetError(cpu, kEInval);

    const bool direct = Direct(buffer, length);
    const uint32_t bounceSize = RoundUp32(uint32_t(length));
    const uint32_t block = Alloc(cpu, kKindIoctl, 0x60);
    const uint32_t data = direct ? buffer : Alloc(cpu, kKindSend, int32_t(bounceSize));
    int32_t result = kENoMem;

    if (block && data) {
        Memory::Write32(block + 0x20, s);
        Memory::Write32(block + 0x24, flags);

        if (!to) {
            Memory::Write32(block + 0x28, 0);
        } else {
            Memory::Write32(block + 0x28, 1);
            Move(block + 0x2C, to, Memory::Read8(to));
        }

        if (!direct)
            Move(data, buffer, uint32_t(length));

        Memory::Write32(block, data);
        Memory::Write32(block + 4, uint32_t(length));
        Memory::Write32(block + 8, block + 0x20);
        Memory::Write32(block + 12, 0x28);
        result = int32_t(GuestCall{cpu}.Call<IOSIoctlv>({fd, kSendTo, 2, 0, block}));
    }

    if (!direct)
        Free(cpu, kKindSend, data, int32_t(bounceSize));

    Free(cpu, kKindIoctl, block, 0x60);
    return SetError(cpu, result);
}

// 0x80520A1C: SOPoll(fds, count, timeout). A negative timeout waits
// forever; others go to the device in milliseconds.
int32_t Poll(CpuContext* cpu, uint32_t fds, uint32_t count, int64_t timeout) {
    uint32_t fd;

    if (const int32_t error = GetFd(cpu, fd))
        return error;

    if (!fds)
        return SetError(cpu, kEInval);

    const uint32_t bytes = count * 12;
    const uint32_t size = (bytes + 0x3F) & ~0x1Fu;
    const uint32_t block = Alloc(cpu, kKindIoctl, int32_t(size));

    if (!block)
        return SetError(cpu, kENoMem);

    const uint32_t entries = block + 0x20;
    const int64_t ticksPerMs = Memory::Read32(kBusClock) / 4000;
    const int64_t wait = timeout < 0 ? timeout : timeout / ticksPerMs;
    Memory::Write32(block, uint32_t(uint64_t(wait) >> 32));
    Memory::Write32(block + 4, uint32_t(wait));
    Move(entries, fds, bytes);
    const int32_t result = int32_t(GuestCall{cpu}.Call<IOSIoctl>({fd, kPoll, block, 8, entries, bytes}));

    if (result >= 0)
        Move(fds, entries, bytes);

    Free(cpu, kKindIoctl, block, int32_t(size));
    return SetError(cpu, result);
}

// 0x80520B78: SOInetAtoN(name, addr), also before SOStartup.
int32_t InetAtoN(CpuContext* cpu, uint32_t name, uint32_t addr) {
    StartedFd started;

    if (const int32_t error = StartFd(cpu, started))
        return error;

    const uint32_t fd = started.fd;
    int32_t result = kEInval;

    if (name) {
        const uint32_t length = GuestLength(name);
        const uint32_t size = (length + 0x40) & ~0x1Fu;
        const uint32_t block = Alloc(cpu, kKindIoctl, int32_t(size));
        result = kENoMem;

        if (block) {
            Move(block + 0x20, name, length + 1);
            result = int32_t(GuestCall{cpu}.Call<IOSIoctl>({fd, kInetAtoN, block + 0x20, length, block, 4}));

            if (result >= 0 && addr)
                Move(addr, block, 4);

            Free(cpu, kKindIoctl, block, int32_t(size));
        }
    }

    return FinishStartup(cpu, result, started.opened);
}

// 0x805217D8: SOGetInterfaceOpt(level, option, value, length), also before
// SOStartup. A value longer than *length gives SO_EINVAL and its length.
int32_t GetInterfaceOpt(CpuContext* cpu, uint32_t level, uint32_t option, uint32_t value, uint32_t lengthPtr) {
    StartedFd started;

    if (const int32_t error = StartFd(cpu, started))
        return error;

    const uint32_t fd = started.fd;
    int32_t result = kEInval;

    if (option - 0x1001u >= 2) {
        const int32_t asked = lengthPtr ? int32_t(Memory::Read32(lengthPtr)) : -1;
        const uint32_t length = asked < 0 ? 0 : uint32_t(asked);
        const uint32_t size = (length + 0x7F) & ~0x1Fu;
        const uint32_t block = Alloc(cpu, kKindIoctl, int32_t(size));
        result = kENoMem;

        if (block) {
            const uint32_t gotLength = block + 0x40, data = block + 0x60;
            Memory::Write32(block + 0x20, level);
            Memory::Write32(block + 0x24, option);
            Memory::Write32(gotLength, length);
            Memory::Write32(block, block + 0x20);
            Memory::Write32(block + 4, 8);
            Memory::Write32(block + 8, data);
            Memory::Write32(block + 12, length);
            Memory::Write32(block + 16, gotLength);
            Memory::Write32(block + 20, 4);
            result = int32_t(GuestCall{cpu}.Call<IOSIoctlv>({fd, kGetInterfaceOpt, 1, 2, block}));

            if (result >= 0 && lengthPtr) {
                const int32_t got = int32_t(Memory::Read32(gotLength));

                if (int32_t(Memory::Read32(lengthPtr)) < got) {
                    Memory::Write32(lengthPtr, uint32_t(got));
                    result = kEInval;
                } else {
                    if (value)
                        Move(value, data, uint32_t(got));

                    Memory::Write32(lengthPtr, uint32_t(got));
                }
            }

            Free(cpu, kKindIoctl, block, int32_t(size));
        }
    }

    return FinishStartup(cpu, result, started.opened);
}

// 0x8052124C: SOGetHostByName(name), into the library's hostent; null on an
// error. The device's pointers are relative to its own copy, whose name
// starts 16 bytes in.
uint32_t GetHostByName(CpuContext* cpu, uint32_t name) {
    uint32_t fd;

    if (GetFd(cpu, fd))
        return 0;

    uint32_t entry = 0;
    int32_t result = kEInval;

    if (name) {
        const uint32_t length = GuestLength(name);
        const uint32_t size = (length + 0x20) & ~0x1Fu;
        const uint32_t copy = Alloc(cpu, kKindIoctl, int32_t(size));
        const uint32_t host = Memory::Read32(kHostEntry);
        result = kENoMem;

        if (copy) {
            Move(copy, name, length + 1);
            result = int32_t(GuestCall{cpu}.Call<IOSIoctl>({fd, kGetHostByName, copy, length + 1, host, kHostEntryBytes}));

            if (result >= 0) {
                const uint32_t delta = host + 0x10 - Memory::Read32(host);

                for (uint32_t address = host + 0x340; Memory::Read32(address); address += 4)
                    Memory::Write32(address, Memory::Read32(address) + delta);

                for (const uint32_t field : {4u, 0u, 12u})
                    Memory::Write32(host + field, Memory::Read32(host + field) + delta);

                entry = host;
            }

            Free(cpu, kKindIoctl, copy, int32_t(size));
        }
    }

    SetError(cpu, result);
    return entry;
}

// 0x8052138C: SOGetAddrInfo(node, service, hints, result). The service's
// slot is sized by the node's length, as in the original; a null node then
// counts as empty (the original would read address 0). AF_INET6 hints give
// SO_EAI_FAMILY.
int32_t GetAddrInfo(CpuContext* cpu, uint32_t node, uint32_t service, uint32_t hints, uint32_t resultOut) {
    uint32_t fd;

    if (const int32_t error = GetFd(cpu, fd))
        return error;

    const uint32_t nodeBytes = node ? GuestLength(node) + 1 : 0;
    const uint32_t serviceSlot = service ? (node ? GuestLength(node) + 1 : 1) : 0;
    const uint32_t size = RoundUp32(nodeBytes) + RoundUp32(serviceSlot) + 0x40;
    const uint32_t block = Alloc(cpu, kKindIoctl, int32_t(size));

    if (!block)
        return SetError(cpu, kENoMem);

    const uint32_t list = Alloc(cpu, kKindAddrInfo, int32_t(kAddrInfoBytes));

    if (!list) {
        Free(cpu, kKindIoctl, block, int32_t(size));
        return SetError(cpu, kENoMem);
    }

    const uint32_t nodeCopy = block + 0x20;
    const uint32_t serviceCopy = nodeCopy + RoundUp32(nodeBytes);
    const uint32_t hintsCopy = serviceCopy + RoundUp32(serviceSlot);

    if (node)
        Move(nodeCopy, node, nodeBytes);

    Memory::Write32(block, node ? nodeCopy : 0);
    Memory::Write32(block + 4, node ? nodeBytes - 1 : 0);

    if (service)
        Move(serviceCopy, service, GuestLength(service) + 1);

    Memory::Write32(block + 8, service ? serviceCopy : 0);
    Memory::Write32(block + 12, service ? GuestLength(service) : 0);

    if (hints)
        Move(hintsCopy, hints, 0x20);
    else
        Fill(hintsCopy, 0, 0x20);

    if (!Memory::Read32(hintsCopy + 4))
        Memory::Write32(hintsCopy + 4, 2); // AF_INET

    int32_t result;

    if (Memory::Read32(hintsCopy + 4) == kAfInet6) {
        Memory::Write32(resultOut, 0);
        result = kEAiFamily;
        Free(cpu, kKindAddrInfo, list, int32_t(kAddrInfoBytes));
    } else {
        Memory::Write32(block + 16, hintsCopy);
        Memory::Write32(block + 20, 0x20);
        Memory::Write32(block + 24, list);
        Memory::Write32(block + 28, 0x834); // the list's 0x840 bytes but the last 12
        result = int32_t(GuestCall{cpu}.Call<IOSIoctlv>({fd, kGetAddrInfo, 3, 1, block}));

        if (result < 0) {
            Memory::Write32(resultOut, 0);
            Free(cpu, kKindAddrInfo, list, int32_t(kAddrInfoBytes));
        } else {
            // The entries are 32 bytes apart, their addresses (28 bytes
            // each) from 0x460 on.
            Memory::Write32(resultOut, list);
            uint32_t address = list + 0x460;

            for (uint32_t entry = list; entry; entry = Memory::Read32(entry + 28)) {
                Memory::Write32(entry + 24, address);

                if (Memory::Read32(entry + 28))
                    Memory::Write32(entry + 28, entry + 32);

                address += 28;
            }
        }
    }

    Free(cpu, kKindIoctl, block, int32_t(size));
    return SetError(cpu, result);
}

// 0x805216D4: SOSetSockOpt(s, level, option, value, length), at most 20
// bytes of value; none sets zeros.
int32_t SetSockOpt(CpuContext* cpu, uint32_t s, uint32_t level, uint32_t option, uint32_t value, int32_t length) {
    uint32_t fd;

    if (const int32_t error = GetFd(cpu, fd))
        return error;

    if (length < 0 || length > int32_t(kMaxOptionBytes))
        return SetError(cpu, kEInval);

    int32_t result = kENoMem;

    if (const uint32_t block = Alloc(cpu, kKindIoctl, 0x40)) {
        Memory::Write32(block, s);
        Memory::Write32(block + 4, level);
        Memory::Write32(block + 8, option);
        Memory::Write32(block + 12, uint32_t(length));

        if (value)
            Move(block + 16, value, uint32_t(length));
        else
            Fill(block + 16, 0, uint32_t(length));

        result = int32_t(GuestCall{cpu}.Call<IOSIoctl>({fd, kSetSockOpt, block, 0x24, 0, 0}));
        Free(cpu, kKindIoctl, block, 0x40);
    }

    return SetError(cpu, result);
}

int64_t SystemTime(CpuContext* cpu) {
    GuestCall call{cpu};
    const uint32_t high = call.Call<OSGetSystemTime>();
    return int64_t((uint64_t(high) << 32) | cpu->gpr[4]);
}

int64_t TicksPerMs() {
    return Memory::Read32(kBusClock) / 4000;
}

void SleepMs(CpuContext* cpu, uint32_t ms) {
    const uint64_t ticks = uint64_t(TicksPerMs()) * ms;
    GuestCall{cpu}.Call<OSSleepTicks>({uint32_t(ticks >> 32), uint32_t(ticks)});
}

// Whether a wait with this deadline (0: none) is over.
bool Expired(CpuContext* cpu, int64_t deadline) {
    return deadline && SystemTime(cpu) > deadline;
}

// The library's state change after SOStartup's or SOCleanup's attempt.
void SetStarted(bool started) {
    Memory::Write8(kState, started ? 2 : 1);
    Memory::Write32(kStartState, started ? 0 : uint32_t(-2));
}

// NWC24's answer to the socket start and stop requests, as a library error
// (`detail` its second word).
int32_t Nwc24Result(int32_t answer, int32_t detail) {
    switch (answer) {
    case 0:
        return 0;
    case -33:
    case -2:
        return detail;
    case -29:
        return kENoDevice;
    case -22:
    case -13:
        return kENetUnreach;
    case -1:
        return kEFatal;
    default:
        return kEInval;
    }
}

// 0x8051FE44: waits (10 ms at a time, at most `ms`, 0: forever) until the
// interface has an address: the interface's error (option 0x1003) or 0, or
// SO_ETIMEDOUT.
int32_t WaitForAddress(CpuContext* cpu, uint32_t ms) {
    GuestFrame frame(cpu, 0x20);
    const uint32_t value = frame[12], length = frame[8];
    const int64_t deadline = ms ? SystemTime(cpu) + int64_t(ms) * TicksPerMs() : 0;

    do {
        SleepMs(cpu, 10);
        Memory::Write32(length, 4);

        if (const int32_t result = GetInterfaceOpt(cpu, 0xFFFE, 0x1003, value, length))
            return result;

        if (const int32_t error = int32_t(Memory::Read32(value)))
            return error;

        uint32_t fd;

        if (!GetFd(cpu, fd)) {
            const uint32_t id = GuestCall{cpu}.Call<IOSIoctl>({fd, kGetHostId, 0, 0, 0, 0});
            SetError(cpu, 0);

            if (id)
                return 0;
        }
    } while (!Expired(cpu, deadline));

    return kETimedOut;
}

// 0x8051F5E0: SOCleanup: NWC24 takes the network back and the device closes.
int32_t Cleanup(CpuContext* cpu) {
    InterruptsOff off(cpu);
    const uint8_t state = Memory::Read8(kState);
    int32_t result;

    if (state == 1) {
        result = kEAlready;
    } else if (state == 0 || state > 2) {
        result = kENetDown;
    } else if (int32_t(Memory::Read32(kStartState)) < 0) {
        result = kEBusy;
    } else if (!Memory::Read32(kCurrentThread)) {
        result = kEFatal;
    } else {
        Memory::Write32(kStartState, 0xFFFFFFFF);
        off.Restore();
        GuestFrame frame(cpu, 0x20);
        const uint32_t detail = frame[8];
        Memory::Write32(detail, 0);
        result = Nwc24Result(int32_t(GuestCall{cpu}.Call<Nwc24Cleanup>({detail})), int32_t(Memory::Read32(detail)));

        if (result == 0) {
            if (int32_t(GuestCall{cpu}.Call<IOSClose>({Memory::Read32(kFd)})) < 0)
                result = kEFatal;
            else
                Memory::Write32(kFd, 0xFFFFFFFF);
        }

        off.Again();

        if (result == 0)
            SetStarted(false);
        else if (result != kEFatal)
            SetStarted(true);
    }

    StoreError(result);
    off.Restore();
    return result;
}

// 0x8051F1B8: SOStartupEx(timeout ms, 0: none): waits for the link (100 ms
// at a time), opens the device, has NWC24 hand the network over, then waits
// for an address. A reset network (SO_ENETRESET) starts over, four times.
int32_t StartupEx(CpuContext* cpu, uint32_t timeoutMs) {
    const int64_t deadline = timeoutMs ? SystemTime(cpu) + int64_t(timeoutMs) * TicksPerMs() : 0;
    GuestCall call{cpu};

    for (int retries = 4;; --retries) {
        InterruptsOff off(cpu);
        const uint8_t state = Memory::Read8(kState);
        int32_t result;

        if (state != 1) {
            result = (state == 0 || state > 2) ? kENetDown : kEAlready;
            StoreError(result);
            off.Restore();
        } else if (int32_t(Memory::Read32(kStartState)) > -2) {
            result = kEBusy;
            StoreError(result);
            off.Restore();
        } else if (!Memory::Read32(kCurrentThread)) {
            result = kEFatal;
            StoreError(result);
            off.Restore();
        } else {
            Memory::Write32(kStartState, 0xFFFFFFFF);
            off.Restore();
            int32_t link;

            // The link: up (3..5), or none yet (-8, 1) to wait for.
            for (;;) {
                link = int32_t(call.Call<NcdGetLinkStatus>());

                if (link != -8 && link != 1)
                    break;

                SleepMs(cpu, 100);

                if (Expired(cpu, deadline))
                    break;
            }

            if (link == -8 || link == 1) {
                result = link == 1 ? kELinkDown : kEFatal;
            } else if (link < 0) {
                result = kEFatal;
            } else if (link == 2) {
                result = kEConnRefused;
            } else {
                int32_t device;
                result = kEFatal;

                for (;;) {
                    device = int32_t(call.Call<IOSOpen>({kDevice, 0}));
                    Memory::Write32(kFd, uint32_t(device));

                    if (device != -6)
                        break;

                    SleepMs(cpu, 100);

                    if (Expired(cpu, deadline))
                        break;
                }

                if (device >= 0) {
                    GuestFrame frame(cpu, 0x60);
                    const uint32_t detail = frame[8];
                    Memory::Write32(detail, 0);
                    int32_t answer;

                    for (;;) {
                        answer = int32_t(call.Call<Nwc24Startup>({detail}));

                        if (answer != -29)
                            break;

                        SleepMs(cpu, 100);

                        if (Expired(cpu, deadline))
                            break;
                    }

                    result = answer == -29 ? kEFatal : Nwc24Result(answer, int32_t(Memory::Read32(detail)));

                    if (result != 0) {
                        if (int32_t(call.Call<IOSClose>({Memory::Read32(kFd)})) < 0)
                            result = kEFatal;
                        else
                            Memory::Write32(kFd, 0xFFFFFFFF);
                    }
                }
            }

            off.Again();

            if (result == 0) {
                SetStarted(true);
            } else {
                Memory::Write8(kState, 1);

                if (result != kEFatal)
                    Memory::Write32(kStartState, uint32_t(-2));
            }

            StoreError(result);
            off.Restore();

            if (result == 0) {
                if (!deadline) {
                    result = WaitForAddress(cpu, 0);
                } else {
                    const int64_t left = deadline - SystemTime(cpu);
                    result = left > 0 ? WaitForAddress(cpu, uint32_t(left / TicksPerMs())) : kETimedOut;
                }

                if (result != 0)
                    Cleanup(cpu);
            }

            StoreError(result);
        }

        if (result != kENetReset || retries == 0)
            return result;
    }
}

// 0x8051EEE4: SOInit(config): the allocator and the hostent buffer; once,
// before SOStartup.
int32_t Init(CpuContext* cpu, uint32_t config) {
    InterruptsOff off(cpu);
    GuestCall call{cpu};

    if (!(Memory::Read32(kInitVersionDone) & 1)) {
        call.Call<OSRegisterVersion>({Memory::Read32(kInitVersion)});
        Memory::Write32(kInitVersionDone, Memory::Read32(kInitVersionDone) | 1);
    }

    const uint8_t state = Memory::Read8(kState);
    int32_t result = 0;

    if (state != 0 && state <= 2) {
        result = kEAlready;
    } else if (!config || !Memory::Read32(config) || !Memory::Read32(config + 4)) {
        result = kEInval;
    } else {
        Fill(kAllocFunction, 0, 0x18);
        Memory::Write32(kAllocFunction, Memory::Read32(config));
        Memory::Write32(kFreeFunction, Memory::Read32(config + 4));
        Memory::Write32(kAllocations, 0);
        Memory::Write32(kStartState, uint32_t(-2));
        Memory::Write32(kFd, 0xFFFFFFFF);
        const uint32_t host = Alloc(cpu, kKindHostEntry, int32_t(kHostEntryBytes));
        Memory::Write32(kHostEntry, host);

        if (!host)
            result = kENoMem;
        else
            Memory::Write8(kState, 1);
    }

    StoreError(result);
    off.Restore();
    return result;
}

// 0x8051F0B0: SOFinish: the hostent back, once stopped and with no other
// block out.
int32_t Finish(CpuContext* cpu) {
    InterruptsOff off(cpu);
    const uint8_t state = Memory::Read8(kState);
    int32_t result = 0;

    if (state == 1) {
        if (int32_t(Memory::Read32(kStartState)) >= -1) {
            result = kEBusy;
        } else if (int32_t(Memory::Read32(kAllocations)) >= 2) {
            result = kEInUse;
        } else {
            Memory::Write8(kState, 0);
            Free(cpu, kKindHostEntry, Memory::Read32(kHostEntry), int32_t(kHostEntryBytes));
        }
    } else if (state == 0) {
        result = kEAlready;
    } else if (state < 3) {
        result = kENoDevice;
    }

    StoreError(result);
    off.Restore();
    return result;
}

uint32_t Arg(CpuContext* cpu, int n) {
    return cpu->gpr[n];
}

} // namespace

extern "C" void MH3_SOMove(CpuContext* cpu) {
    Move(Arg(cpu, 3), Arg(cpu, 4), Arg(cpu, 5));
}
REGISTER_NATIVE_FUNCTION_AS(0x8051D8CC, MH3_SOMove, "SOMove");

extern "C" void MH3_SOFill(CpuContext* cpu) {
    Fill(Arg(cpu, 3), Arg(cpu, 4), Arg(cpu, 5));
}
REGISTER_NATIVE_FUNCTION_AS(0x8051DC8C, MH3_SOFill, "SOFill");

extern "C" void MH3_SOGetError(CpuContext* cpu) {
    const uint32_t thread = Memory::Read32(kCurrentThread);
    cpu->gpr[3] = thread ? Memory::Read32(thread + kThreadSocketError) : Memory::Read32(kError);
}
REGISTER_NATIVE_FUNCTION_AS(0x8051F790, MH3_SOGetError, "SOGetError");

extern "C" void MH3_SOIsInitialized(CpuContext* cpu) {
    InterruptsOff off(cpu);
    const uint8_t state = Memory::Read8(kState);
    off.Restore();
    cpu->gpr[3] = state != 0 && state < 3;
}
REGISTER_NATIVE_FUNCTION_AS(0x8051F7D8, MH3_SOIsInitialized, "SOIsInitialized");

extern "C" void MH3_SOAlloc(CpuContext* cpu) {
    cpu->gpr[3] = Alloc(cpu, Arg(cpu, 3), int32_t(Arg(cpu, 4)));
}
REGISTER_NATIVE_FUNCTION_AS(0x8051F828, MH3_SOAlloc, "SOAlloc");

extern "C" void MH3_SOFree(CpuContext* cpu) {
    Free(cpu, Arg(cpu, 3), Arg(cpu, 4), int32_t(Arg(cpu, 5)));
}
REGISTER_NATIVE_FUNCTION_AS(0x8051F910, MH3_SOFree, "SOFree");

extern "C" void MH3_SOGetFd(CpuContext* cpu) {
    const uint32_t out = Arg(cpu, 4);
    uint32_t fd = 0;
    const int32_t error = GetFd(cpu, fd);

    if (!error)
        Memory::Write32(out, fd);

    cpu->gpr[3] = uint32_t(error);
}
REGISTER_NATIVE_FUNCTION_AS(0x8051F944, MH3_SOGetFd, "SOGetFd");

extern "C" void MH3_SOSetError(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(SetError(cpu, int32_t(Arg(cpu, 4))));
}
REGISTER_NATIVE_FUNCTION_AS(0x8051FA2C, MH3_SOSetError, "SOSetError");

extern "C" void MH3_SOStartFd(CpuContext* cpu) {
    const uint32_t fdOut = Arg(cpu, 4), openedOut = Arg(cpu, 5);
    StartedFd started;
    const int32_t error = StartFd(cpu, started);

    if (started.hasFd)
        Memory::Write32(fdOut, started.fd);

    if (started.hasOpened)
        Memory::Write32(openedOut, started.opened);

    cpu->gpr[3] = uint32_t(error);
}
REGISTER_NATIVE_FUNCTION_AS(0x8051FA88, MH3_SOStartFd, "SOStartFd");

extern "C" void MH3_SOFinishStartup(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(FinishStartup(cpu, int32_t(Arg(cpu, 4)), Arg(cpu, 5)));
}
REGISTER_NATIVE_FUNCTION_AS(0x8051FD60, MH3_SOFinishStartup, "SOFinishStartup");

extern "C" void MH3_SOSocketQuiet(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(Socket(cpu, Arg(cpu, 3), Arg(cpu, 4), Arg(cpu, 5)));
}
REGISTER_NATIVE_FUNCTION_AS(0x8051FF7C, MH3_SOSocketQuiet, "SOSocketQuiet");

extern "C" void MH3_SOSocket(CpuContext* cpu) {
    const uint32_t domain = Arg(cpu, 3), type = Arg(cpu, 4), protocol = Arg(cpu, 5);

    if (!Memory::Read32(kVersionDone)) {
        GuestCall{cpu}.Call<OSRegisterVersion>({Memory::Read32(kVersion)});
        Memory::Write32(kVersionDone, 1);
    }

    cpu->gpr[3] = uint32_t(Socket(cpu, domain, type, protocol));
}
REGISTER_NATIVE_FUNCTION_AS(0x80520050, MH3_SOSocket, "SOSocket");

extern "C" void MH3_SOClose(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(SimpleIoctl(cpu, kClose, {Arg(cpu, 3)}));
}
REGISTER_NATIVE_FUNCTION_AS(0x80520140, MH3_SOClose, "SOClose");

extern "C" void MH3_SOListen(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(SimpleIoctl(cpu, kListen, {Arg(cpu, 3), Arg(cpu, 4)}));
}
REGISTER_NATIVE_FUNCTION_AS(0x805201E4, MH3_SOListen, "SOListen");

extern "C" void MH3_SOAccept(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(AddressOut(cpu, kAccept, Arg(cpu, 3), Arg(cpu, 4), true));
}
REGISTER_NATIVE_FUNCTION_AS(0x80520298, MH3_SOAccept, "SOAccept");

extern "C" void MH3_SOBind(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(AddressIn(cpu, kBind, Arg(cpu, 3), Arg(cpu, 4)));
}
REGISTER_NATIVE_FUNCTION_AS(0x805203D8, MH3_SOBind, "SOBind");

extern "C" void MH3_SOConnect(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(AddressIn(cpu, kConnect, Arg(cpu, 3), Arg(cpu, 4)));
}
REGISTER_NATIVE_FUNCTION_AS(0x805204C0, MH3_SOConnect, "SOConnect");

extern "C" void MH3_SOGetSockName(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(AddressOut(cpu, kGetSockName, Arg(cpu, 3), Arg(cpu, 4), false));
}
REGISTER_NATIVE_FUNCTION_AS(0x805205A8, MH3_SOGetSockName, "SOGetSockName");

extern "C" void MH3_SOGetPeerName(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(AddressOut(cpu, kGetPeerName, Arg(cpu, 3), Arg(cpu, 4), false));
}
REGISTER_NATIVE_FUNCTION_AS(0x805206A4, MH3_SOGetPeerName, "SOGetPeerName");

extern "C" void MH3_SORecvFrom(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(Receive(cpu, Arg(cpu, 3), Arg(cpu, 4), int32_t(Arg(cpu, 5)), Arg(cpu, 6), Arg(cpu, 7)));
}
REGISTER_NATIVE_FUNCTION_AS(0x805207A0, MH3_SORecvFrom, "SORecvFrom");

extern "C" void MH3_SORecv(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(Receive(cpu, Arg(cpu, 3), Arg(cpu, 4), int32_t(Arg(cpu, 5)), Arg(cpu, 6), 0));
}
REGISTER_NATIVE_FUNCTION_AS(0x805207C8, MH3_SORecv, "SORecv");

extern "C" void MH3_SOSendTo(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(Send(cpu, Arg(cpu, 3), Arg(cpu, 4), int32_t(Arg(cpu, 5)), Arg(cpu, 6), Arg(cpu, 7)));
}
REGISTER_NATIVE_FUNCTION_AS(0x805207EC, MH3_SOSendTo, "SOSendTo");

extern "C" void MH3_SOSend(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(Send(cpu, Arg(cpu, 3), Arg(cpu, 4), int32_t(Arg(cpu, 5)), Arg(cpu, 6), 0));
}
REGISTER_NATIVE_FUNCTION_AS(0x80520814, MH3_SOSend, "SOSend");

// SOFcntl(s, command, ...): the one variadic integer is r5.
extern "C" void MH3_SOFcntl(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(SimpleIoctl(cpu, kFcntl, {Arg(cpu, 3), Arg(cpu, 4), Arg(cpu, 5)}));
}
REGISTER_NATIVE_FUNCTION_AS(0x80520838, MH3_SOFcntl, "SOFcntl");

extern "C" void MH3_SOShutdown(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(SimpleIoctl(cpu, kShutdown, {Arg(cpu, 3), Arg(cpu, 4)}));
}
REGISTER_NATIVE_FUNCTION_AS(0x80520968, MH3_SOShutdown, "SOShutdown");

extern "C" void MH3_SOPoll(CpuContext* cpu) {
    const int64_t timeout = int64_t((uint64_t(Arg(cpu, 5)) << 32) | Arg(cpu, 6));
    cpu->gpr[3] = uint32_t(Poll(cpu, Arg(cpu, 3), Arg(cpu, 4), timeout));
}
REGISTER_NATIVE_FUNCTION_AS(0x80520A1C, MH3_SOPoll, "SOPoll");

extern "C" void MH3_SOInetAtoN(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(InetAtoN(cpu, Arg(cpu, 3), Arg(cpu, 4)));
}
REGISTER_NATIVE_FUNCTION_AS(0x80520B78, MH3_SOInetAtoN, "SOInetAtoN");

// SOInetNtoA(addr): "%d.%d.%d.%d" of the address's bytes in a static buffer.
extern "C" void MH3_SOInetNtoA(CpuContext* cpu) {
    const uint32_t addr = Arg(cpu, 3);
    char text[16];
    const int length = std::snprintf(text, sizeof(text), "%u.%u.%u.%u", Memory::Read8(addr), Memory::Read8(addr + 1), Memory::Read8(addr + 2), Memory::Read8(addr + 3));
    std::memcpy(Memory::GetPointer(kNtoaBuffer, uint32_t(length + 1)), text, size_t(length + 1));
    cpu->gpr[3] = kNtoaBuffer;
}
REGISTER_NATIVE_FUNCTION_AS(0x80520C84, MH3_SOInetNtoA, "SOInetNtoA");

extern "C" void MH3_SOGetHostId(CpuContext* cpu) {
    uint32_t fd;

    if (GetFd(cpu, fd)) {
        cpu->gpr[3] = 0;
        return;
    }

    const uint32_t id = GuestCall{cpu}.Call<IOSIoctl>({fd, kGetHostId, 0, 0, 0, 0});
    SetError(cpu, 0);
    cpu->gpr[3] = id;
}
REGISTER_NATIVE_FUNCTION_AS(0x805211D4, MH3_SOGetHostId, "SOGetHostId");

extern "C" void MH3_SOGetInterfaceOpt(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(GetInterfaceOpt(cpu, Arg(cpu, 4), Arg(cpu, 5), Arg(cpu, 6), Arg(cpu, 7)));
}
REGISTER_NATIVE_FUNCTION_AS(0x805217D8, MH3_SOGetInterfaceOpt, "SOGetInterfaceOpt");

extern "C" void MH3_SOGetHostByName(CpuContext* cpu) {
    cpu->gpr[3] = GetHostByName(cpu, Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x8052124C, MH3_SOGetHostByName, "SOGetHostByName");

extern "C" void MH3_SOGetAddrInfo(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(GetAddrInfo(cpu, Arg(cpu, 3), Arg(cpu, 4), Arg(cpu, 5), Arg(cpu, 6)));
}
REGISTER_NATIVE_FUNCTION_AS(0x8052138C, MH3_SOGetAddrInfo, "SOGetAddrInfo");

// SOFreeAddrInfo(list): back to the allocator while the library is
// initialized.
extern "C" void MH3_SOFreeAddrInfo(CpuContext* cpu) {
    const uint32_t list = Arg(cpu, 3);
    InterruptsOff off(cpu);
    const uint8_t state = Memory::Read8(kState);

    if (state != 0 && state < 3 && list)
        Free(cpu, kKindAddrInfo, list, int32_t(kAddrInfoBytes));

    off.Restore();
}
REGISTER_NATIVE_FUNCTION_AS(0x80521670, MH3_SOFreeAddrInfo, "SOFreeAddrInfo");

extern "C" void MH3_SOSetSockOpt(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(SetSockOpt(cpu, Arg(cpu, 3), Arg(cpu, 4), Arg(cpu, 5), Arg(cpu, 6), int32_t(Arg(cpu, 7))));
}
REGISTER_NATIVE_FUNCTION_AS(0x805216D4, MH3_SOSetSockOpt, "SOSetSockOpt");

extern "C" void MH3_SOInit(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(Init(cpu, Arg(cpu, 3)));
}
REGISTER_NATIVE_FUNCTION_AS(0x8051EEE4, MH3_SOInit, "SOInit");

extern "C" void MH3_SOFinish(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(Finish(cpu));
}
REGISTER_NATIVE_FUNCTION_AS(0x8051F0B0, MH3_SOFinish, "SOFinish");

extern "C" void MH3_SOStartup(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(StartupEx(cpu, 60000));
}
REGISTER_NATIVE_FUNCTION_AS(0x8051F1AC, MH3_SOStartup, "SOStartup");

extern "C" void MH3_SOStartupEx(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(StartupEx(cpu, Arg(cpu, 3)));
}
REGISTER_NATIVE_FUNCTION_AS(0x8051F1B8, MH3_SOStartupEx, "SOStartupEx");

extern "C" void MH3_SOCleanup(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(Cleanup(cpu));
}
REGISTER_NATIVE_FUNCTION_AS(0x8051F5E0, MH3_SOCleanup, "SOCleanup");

extern "C" void MH3_SOWaitForAddress(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(WaitForAddress(cpu, Arg(cpu, 3)));
}
REGISTER_NATIVE_FUNCTION_AS(0x8051FE44, MH3_SOWaitForAddress, "SOWaitForAddress");

// The cores behind SORecvFrom/SORecv and SOSendTo/SOSend: (0, s, buffer,
// length, flags, address).
extern "C" void MH3_SOReceive(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(Receive(cpu, Arg(cpu, 4), Arg(cpu, 5), int32_t(Arg(cpu, 6)), Arg(cpu, 7), Arg(cpu, 8)));
}
REGISTER_NATIVE_FUNCTION_AS(0x80520CF0, MH3_SOReceive, "SOReceive");

extern "C" void MH3_SOSendData(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(Send(cpu, Arg(cpu, 4), Arg(cpu, 5), int32_t(Arg(cpu, 6)), Arg(cpu, 7), Arg(cpu, 8)));
}
REGISTER_NATIVE_FUNCTION_AS(0x80520FA8, MH3_SOSendData, "SOSendData");
