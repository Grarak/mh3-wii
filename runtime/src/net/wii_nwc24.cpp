// NWC24 (WiiConnect24) in RMHP08 0x8051DD90..0x8051EE14, decompiled with
// Ghidra (the call arguments and branches checked in the translated C++) and
// ported natively: the user ID, the scheduler's suspend and resume around
// online play, the socket hand-over the socket library asks for (wii_so.cpp),
// the RTC counter and the shutdown request. Requests go to /dev/net/kd/request
// and /dev/net/kd/time (ios_net.cpp) as ioctls through 32-byte in and out
// buffers, under the library's mutexes. OS, SC and the guest's memset stay
// guest calls.
#include "abi_bridge.h"
#include "guest_call.h"
#include "memory.h"

namespace {

enum : uint32_t {
    Memset = 0x80004350,
    Div2i = 0x804575D8,
    IOSOpen = 0x804BC500,
    IOSClose = 0x804BC810,
    IOSIoctlAsync = 0x804BCE70,
    IOSIoctl = 0x804BCFB0,
    OSRegisterVersion = 0x804CBA00,
    OSGetAppType = 0x804CBAA0,
    DCFlushRange = 0x804CCCF0,
    OSInitMutex = 0x804D2560,
    OSLockMutex = 0x804D25A0,
    OSUnlockMutex = 0x804D2680,
    OSRegisterShutdownFunction = 0x804D2870,
    OSGetTime = 0x804D53D0,
    OSSetIdleMode = 0x804D6230,
    SCCheckStatus = 0x804DB770,
    SCGetIdleMode = 0x804DD140,
    SCGetCounterBias = 0x804DD2E0,
    AsyncDone = 0x8051ED38,
    ShutdownHookAddress = 0x8051EE14,
};

enum : uint32_t {
    kInfo = 0x80797D58,       // the library's work: the user ID at +8
    kState = 0x80797D60,      // 0 idle, 1 and 2 busy, 3 making a user ID
    kVersionDone = 0x80797D64,
    kVersion = 0x80796900,
    kFlags = 0x80797D68,      // bit 0: the mutexes and buffers are set up
    kSuspends = 0x80797D6C,   // suspends the scheduler counts against
    kSuspendBase = 0x80797D70,
    kRtcReady = 0x80797D78,
    kAsyncBusy = 0x80797D80,
    kShutdownTries = 0x80797D88,
    kShutdownSent = 0x80797D8C,
    kShutdownResult = 0x80797D90,
    kShutdownFd = 0x80796908,
    kRequestMutex = 0x80768E40,
    kSchedulerMutex = 0x80768E58,
    kIn = 0x80768E80,
    kOut = 0x80768EA0,
    kRtcMutex = 0x80769040,
    kRtcIn = 0x80769060,
    kRtcOut = 0x80769080,
    kShutdownInfo = 0x807690A0, // OSShutdownFunctionInfo: function, priority
    kShutdownIn = 0x807690C0,
    kShutdownOut = 0x807690E0,
    kUserIdCache = 0x800031C0,  // the user ID in low memory, 8 bytes
};

enum : uint32_t {
    kRequestDevice = 0x80631A38,  // "/dev/net/kd/request"
    kTimeDevice = 0x80631A80,     // "/dev/net/kd/time"
    kShutdownDevice = 0x80631AC0, // "/dev/net/kd/request" (its own copy)
};

constexpr uint32_t kCurrentThread = 0x800000E4;
constexpr uint32_t kBusClock = 0x800000F8;
constexpr uint32_t kMsrEe = 0x8000;

// The /dev/net/kd/request and /dev/net/kd/time ioctls.
enum : uint32_t {
    kSuspendScheduler = 1,
    kResumeScheduler = 3,
    kStartSocket = 6,
    kCleanupSocket = 7,
    kSuspendForSocket = 8,
    kResumeForSocket = 9,
    kGenerateUserId = 15,
    kSetRtcCounter = 23,
    kSetScriptMode = 34,
    kRequestShutdown = 40,
};

constexpr int32_t kEFatal = -1;
constexpr int32_t kEInval = -3;
constexpr int32_t kENoId = -5;
constexpr int32_t kEBusy = -10;
constexpr int32_t kENotReady = -9;
constexpr int32_t kEInUse = -26;
constexpr int32_t kENoDevice = -29;
constexpr int32_t kEBadId = -37;
constexpr int32_t kEIpc = -42;

// The nibble permutation the user ID is scrambled with (0x805755C8).
constexpr uint8_t kNibbles[16] = {0x0D, 0x05, 0x09, 0x07, 0x00, 0x0F, 0x0A, 0x02, 0x0C, 0x03, 0x0E, 0x01, 0x08, 0x06, 0x0B, 0x04};

uint32_t Nibble(uint32_t index) {
    return kNibbles[index & 0xF];
}

struct InterruptsOff {
    CpuContext* cpu;
    uint32_t enabled;

    explicit InterruptsOff(CpuContext* cpu) : cpu(cpu), enabled(cpu->msr & kMsrEe) {
        cpu->msr &= ~kMsrEe;
    }

    void Restore() {
        cpu->msr = enabled ? cpu->msr | kMsrEe : cpu->msr & ~kMsrEe;
    }
};

uint32_t Read(uint32_t address) {
    return Memory::Read32(address);
}

void Write(uint32_t address, uint32_t value) {
    Memory::Write32(address, value);
}

// 0x8051DEF8.
void RegisterVersion(CpuContext* cpu) {
    if (!Read(kVersionDone)) {
        GuestCall{cpu}.Call<OSRegisterVersion>({Read(kVersion)});
        Write(kVersionDone, 1);
    }
}

// The mutexes and buffers, once (inlined in each request).
void Setup(CpuContext* cpu) {
    if (Read(kFlags) & 1)
        return;

    InterruptsOff off(cpu);

    if (!(Read(kFlags) & 1)) {
        GuestCall call{cpu};
        call.Call<OSInitMutex>({kRequestMutex});
        call.Call<OSInitMutex>({kSchedulerMutex});
        call.Call<Memset>({kIn, 0, 0x20});
        call.Call<Memset>({kOut, 0, 0x20});
        Write(kFlags, Read(kFlags) | 1);
    }

    off.Restore();
}

// 0x8051EBE0: opens a device; its error as the library's.
int32_t Open(CpuContext* cpu, uint32_t path, uint32_t& fd, uint32_t mode) {
    fd = GuestCall{cpu}.Call<IOSOpen>({path, mode});
    const int32_t result = int32_t(fd);

    if (result >= 0)
        return 0;

    return result == -6 ? kENoDevice : result == -8 ? kEInUse : kEIpc;
}

// 0x8051EC58.
int32_t Close(CpuContext* cpu, uint32_t fd) {
    return int32_t(GuestCall{cpu}.Call<IOSClose>({fd})) < 0 ? kEIpc : 0;
}

// 0x8051EC8C.
int32_t Ioctl(CpuContext* cpu, uint32_t fd, uint32_t command, uint32_t in, uint32_t inLength, uint32_t out, uint32_t outLength) {
    return int32_t(GuestCall{cpu}.Call<IOSIoctl>({fd, command, in, inLength, out, outLength})) < 0 ? kEIpc : 0;
}

// 0x8051E588: a /dev/net/kd/request command with no input: the answer's
// first word, and its second for -2 and -33 (into `detail`).
int32_t Request(CpuContext* cpu, uint32_t command, uint32_t detail) {
    if (!Read(kCurrentThread))
        return kEFatal;

    Setup(cpu);
    GuestCall call{cpu};
    call.Call<OSLockMutex>({kRequestMutex});
    uint32_t fd;
    int32_t result = Open(cpu, kRequestDevice, fd, 0);

    if (result >= 0) {
        result = Ioctl(cpu, fd, command, 0, 0, kOut, 0x20);

        if (result >= 0) {
            result = int32_t(Read(kOut));

            if ((result == -2 || result == -33) && detail)
                Write(detail, Read(kOut + 4));
        }

        if (const int32_t closed = Close(cpu, fd); closed < 0)
            result = closed;
    }

    call.Call<OSUnlockMutex>({kRequestMutex});
    return result;
}

// 0x8051DF6C: marks the library busy making a user ID (state 3), or idle
// again.
int32_t SetMakingId(CpuContext* cpu, bool on) {
    InterruptsOff off(cpu);
    const int32_t state = int32_t(Read(kState));
    int32_t result = 0;

    if (!on) {
        if (state != 3)
            result = kENotReady;
        else
            Write(kState, 0);
    } else if (state == 0) {
        Write(kState, 3);
    } else {
        result = state == 1 ? kEBusy : kEInUse;
    }

    off.Restore();
    return result;
}

// 0x8051E00C: NWC24SuspendScheduler: the suspends before this one, less
// those the library counts against.
int32_t SuspendScheduler(CpuContext* cpu) {
    Setup(cpu);
    GuestCall call{cpu};
    call.Call<OSLockMutex>({kSchedulerMutex});
    int32_t result = Request(cpu, kSuspendScheduler, 0);

    if (result >= 0) {
        Write(kSuspends, Read(kSuspends) + 1);
        result -= int32_t(Read(kSuspendBase));
    }

    call.Call<OSUnlockMutex>({kSchedulerMutex});
    return result;
}

// 0x8051E0DC: NWC24ResumeScheduler.
int32_t ResumeScheduler(CpuContext* cpu) {
    Setup(cpu);
    GuestCall call{cpu};
    call.Call<OSLockMutex>({kSchedulerMutex});
    int32_t result = 0;

    if (int32_t(Read(kSuspendBase)) < 1 || Read(kSuspends) != 0) {
        result = Request(cpu, kResumeScheduler, 0);

        if (int32_t(Read(kSuspends)) > 0) {
            Write(kSuspends, Read(kSuspends) - 1);
            result -= int32_t(Read(kSuspendBase));
        }
    }

    call.Call<OSUnlockMutex>({kSchedulerMutex});
    return result;
}

// 0x8051E1CC: NWC24iSetScriptMode(mode), while no user ID is being made.
int32_t SetScriptMode(CpuContext* cpu, uint32_t mode) {
    int32_t result;

    if (!Read(kCurrentThread))
        result = kEFatal;
    else if (Read(kState) == 1 || Read(kState) == 2)
        result = kEBusy;
    else
        result = Read(kState) == 3 ? kEInUse : 0;

    if (result < 0)
        return result;

    Setup(cpu);
    GuestCall call{cpu};
    call.Call<OSLockMutex>({kRequestMutex});
    call.Call<Memset>({kIn, 0, 0x20});
    uint32_t fd;
    result = Open(cpu, kRequestDevice, fd, 0);

    if (result >= 0) {
        Write(kIn, mode);
        result = Ioctl(cpu, fd, kSetScriptMode, kIn, 0x20, kOut, 0x20);

        if (result >= 0)
            result = int32_t(Read(kOut));

        const int32_t closed = Close(cpu, fd);

        if (result >= 0)
            result = closed;
    }

    call.Call<OSUnlockMutex>({kRequestMutex});
    return result;
}

// 0x8051E36C: NWC24iRequestGenerateUserId(id, extra): a new user ID (8 bytes
// into `id`, one word into `extra`) on 0, -35 and -36.
int32_t RequestGenerateUserId(CpuContext* cpu, uint32_t id, uint32_t extra) {
    int32_t result;

    if (!Read(kCurrentThread))
        result = kEFatal;
    else
        result = (Read(kState) == 1 || Read(kState) == 2) ? kEBusy : 0;

    if (result < 0)
        return result;

    Setup(cpu);
    GuestCall call{cpu};
    call.Call<OSLockMutex>({kRequestMutex});
    uint32_t fd;
    result = Open(cpu, kRequestDevice, fd, 0);

    if (result >= 0) {
        result = Ioctl(cpu, fd, kGenerateUserId, 0, 0, kOut, 0x20);

        if (result >= 0) {
            result = int32_t(Read(kOut));

            if (result == 0 || result == -35 || result == -36) {
                if (id) {
                    Write(id + 4, Read(kOut + 8));
                    Write(id, Read(kOut + 4));
                }

                if (extra)
                    Write(extra, Read(kOut + 12));
            }
        }

        const int32_t closed = Close(cpu, fd);

        if (result >= 0)
            result = closed;
    }

    call.Call<OSUnlockMutex>({kRequestMutex});
    return result;
}

// 0x8051E780: the user ID's 53 bits unscrambled: XORs, the nibble
// permutation and a rotation (checked against the translated original).
uint64_t DecodeUserId(uint32_t high, uint32_t low) {
    const uint32_t a = (high & 0x1FFFFF) ^ 0x5E5E;
    const uint32_t b = (low ^ 0x5E5E5E5E) >> 1;
    const uint32_t c = (Nibble(b >> 12) << 4 | Nibble(b >> 8)) | (Nibble(a >> 13) << 4 | Nibble(a >> 9)) << 8 | (Nibble(b >> 4) << 4 | Nibble(b)) << 16;
    const uint32_t d = (Nibble(b >> 20) << 4 | Nibble(b >> 16)) | ((a & 0xFFFE0000) | ((low ^ 0x5E5E5E5E) & 1) << 21) >> 1 | (Nibble((high << 31 | b) >> 28) << 4 | Nibble(b >> 24)) << 8;
    const uint32_t top = (c | (Nibble((a & 0x1FF) >> 5) << 4 | Nibble(a >> 1)) << 24) >> 22 | (d & 0x7FF) << 10;
    const uint32_t bottom = c << 10 | d >> 11;
    return ((uint64_t(top) << 32) | bottom) ^ 0xB3B3B3B3B3B3ull;
}

// 0x8051E6E8: whether the user ID's check bits hold: its 53 bits divide by the
// polynomial 0x635 (11 bits) with no remainder; else NWC24_ERR_ID_CRC (-37).
int32_t CheckUserId(uint32_t high, uint32_t low) {
    uint64_t value = DecodeUserId(high, low);

    for (int i = 0; i < 43; ++i)
        if ((value >> (52 - i)) & 1)
            value ^= uint64_t(0x635) << (42 - i);

    return value ? kEBadId : 0;
}

// 0x8051DD90: NWC24GetMyUserId(id): the library's own while it is busy;
// else the one in low memory when its check bits hold, or a new one
// (generated with the scheduler suspended, then cached in low memory).
int32_t GetMyUserId(CpuContext* cpu, uint32_t id) {
    const uint32_t state = Read(kState);

    if (state == 1 || state == 2) {
        const uint32_t info = Read(kInfo);
        Write(id + 4, Read(info + 12));
        Write(id, Read(info + 8));
        return 0;
    }

    const uint32_t high = Read(kUserIdCache), low = Read(kUserIdCache + 4);
    Write(id + 4, low);
    Write(id, high);

    if ((high | low) && CheckUserId(high, low) == 0)
        return 0;

    int32_t result = SuspendScheduler(cpu);

    if (result < 0)
        return result;

    int32_t made = SetMakingId(cpu, true);

    if (made >= 0) {
        GuestFrame frame(cpu, 0x20);
        const uint32_t extra = frame[8];
        Write(extra, 0);

        if (!id) {
            made = kEInval;
        } else {
            Write(id + 4, 0x6FC0FFFF);
            Write(id, 0x002386F2);
            made = RequestGenerateUserId(cpu, id, extra);
            Write(kUserIdCache + 4, Read(id + 4));
            Write(kUserIdCache, Read(id));
            GuestCall{cpu}.Call<DCFlushRange>({kUserIdCache, 0x20});
        }

        const int32_t idle = SetMakingId(cpu, false);

        if (made >= 0)
            made = idle;
    }

    if (made == -35 || made == -36)
        made = 0;

    const int32_t resumed = ResumeScheduler(cpu);
    return resumed < 0 && made == 0 ? resumed : made;
}

// 0x8051EA04: NWC24iSetRtcCounter(counter, flag).
int32_t SetRtcCounter(CpuContext* cpu, uint32_t counter, uint32_t flag) {
    if (!Read(kCurrentThread))
        return kEFatal;

    if (!Read(kRtcReady)) {
        InterruptsOff off(cpu);

        if (!Read(kRtcReady)) {
            GuestCall call{cpu};
            call.Call<OSInitMutex>({kRtcMutex});
            call.Call<Memset>({kRtcIn, 0, 0x20});
            call.Call<Memset>({kRtcOut, 0, 0x20});
            Write(kRtcReady, 1);
        }

        off.Restore();
    }

    GuestCall call{cpu};
    call.Call<OSLockMutex>({kRtcMutex});
    uint32_t fd;
    int32_t result = Open(cpu, kTimeDevice, fd, 0);

    if (result >= 0) {
        Write(kRtcIn, counter);
        Write(kRtcIn + 4, flag);
        result = Ioctl(cpu, fd, kSetRtcCounter, kRtcIn, 0x20, kRtcOut, 0x20);

        if (result >= 0)
            result = int32_t(Read(kRtcOut));

        const int32_t closed = Close(cpu, fd);

        if (result >= 0)
            result = closed;
    }

    call.Call<OSUnlockMutex>({kRtcMutex});
    return result;
}

// 0x8051EB4C: NWC24iSynchronizeRtcCounter(force): the RTC's seconds less
// the system's counter bias, once SC has read the settings.
void SynchronizeRtcCounter(CpuContext* cpu, uint32_t force) {
    GuestCall call{cpu};
    int32_t status;

    do {
        status = int32_t(call.Call<SCCheckStatus>());

        if (status == 2)
            return;
    } while (status != 0);

    const uint32_t bias = call.Call<SCGetCounterBias>();
    const uint32_t timeHigh = call.Call<OSGetTime>();
    const uint32_t timeLow = cpu->gpr[4];
    call.Call<Div2i>({timeHigh, timeLow, 0, Read(kBusClock) >> 2});
    const uint32_t seconds = cpu->gpr[4];
    SetRtcCounter(cpu, seconds - bias, force != 0);
}

// 0x8051ECD4: an asynchronous ioctl whose completion (0x8051ED38) stores its
// result in `result` and clears the busy flag.
int32_t IoctlAsync(CpuContext* cpu, uint32_t fd, uint32_t command, uint32_t in, uint32_t inLength, uint32_t out, uint32_t outLength, uint32_t result) {
    if (int32_t(GuestCall{cpu}.Call<IOSIoctlAsync>({fd, command, in, inLength, out, outLength, AsyncDone, result})) < 0)
        return kEIpc;

    Write(kAsyncBusy, 1);
    return 0;
}

// 0x8051EE14: the shutdown function: asks NWC24 to shut down (once the
// previous ask is answered, five tries), done when it answered.
uint32_t ShutdownHook(CpuContext* cpu, uint32_t final, uint32_t event) {
    if (final)
        return 1;

    if (!Read(kShutdownSent)) {
        Write(kShutdownIn, event);

        if (IoctlAsync(cpu, Read(kShutdownFd), kRequestShutdown, kShutdownIn, 0x20, kShutdownOut, 0x20, kShutdownResult) >= 0)
            Write(kShutdownSent, 1);

        return 0;
    }

    if (Read(kAsyncBusy))
        return 0;

    if (int32_t(Read(kShutdownResult)) >= 0 || int32_t(Read(kShutdownTries)) < 1)
        return 1;

    Write(kShutdownTries, Read(kShutdownTries) - 1);
    Write(kShutdownSent, 0);
    return 0;
}

// 0x8051ED54: NWC24iPrepareShutdown: registers the shutdown function, opens
// its device, applies the idle mode SC has, and sets script mode 1 for a
// '@' application type.
int32_t PrepareShutdown(CpuContext* cpu) {
    GuestCall call{cpu};
    int32_t result = 0;
    RegisterVersion(cpu);
    Write(kShutdownInfo, ShutdownHookAddress);
    Write(kShutdownInfo + 4, 110);
    call.Call<OSRegisterShutdownFunction>({kShutdownInfo});

    if (int32_t(Read(kShutdownFd)) < 0) {
        uint32_t fd;
        result = Open(cpu, kShutdownDevice, fd, 1);
        Write(kShutdownFd, fd);
    }

    Write(kShutdownTries, 5);
    int32_t status;

    do {
        status = int32_t(call.Call<SCCheckStatus>());
    } while (status == 1);

    if (status != 2) {
        GuestFrame frame(cpu, 0x20);
        const uint32_t idle = frame[8];
        call.Call<SCGetIdleMode>({idle});
        call.Call<OSSetIdleMode>({Memory::Read8(idle + 1)});
    }

    if ((call.Call<OSGetAppType>() & 0xFF) == '@')
        SetScriptMode(cpu, 1);

    return result;
}

} // namespace

extern "C" void MH3_NWC24GetMyUserId(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(GetMyUserId(cpu, cpu->gpr[3]));
}
REGISTER_NATIVE_FUNCTION_AS(0x8051DD90, MH3_NWC24GetMyUserId, "NWC24GetMyUserId");

extern "C" void MH3_NWC24RegisterVersion(CpuContext* cpu) {
    RegisterVersion(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x8051DEF8, MH3_NWC24RegisterVersion, "NWC24iRegisterVersion");

extern "C" void MH3_NWC24IsState1(CpuContext* cpu) {
    cpu->gpr[3] = Read(kState) == 1;
}
REGISTER_NATIVE_FUNCTION_AS(0x8051DF30, MH3_NWC24IsState1, "NWC24iIsState1");

extern "C" void MH3_NWC24IsState2(CpuContext* cpu) {
    cpu->gpr[3] = Read(kState) == 2;
}
REGISTER_NATIVE_FUNCTION_AS(0x8051DF44, MH3_NWC24IsState2, "NWC24iIsState2");

extern "C" void MH3_NWC24IsMakingId(CpuContext* cpu) {
    cpu->gpr[3] = Read(kState) == 3;
}
REGISTER_NATIVE_FUNCTION_AS(0x8051DF58, MH3_NWC24IsMakingId, "NWC24iIsMakingId");

extern "C" void MH3_NWC24SetMakingId(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(SetMakingId(cpu, cpu->gpr[3] != 0));
}
REGISTER_NATIVE_FUNCTION_AS(0x8051DF6C, MH3_NWC24SetMakingId, "NWC24iSetMakingId");

extern "C" void MH3_NWC24SuspendScheduler(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(SuspendScheduler(cpu));
}
REGISTER_NATIVE_FUNCTION_AS(0x8051E00C, MH3_NWC24SuspendScheduler, "NWC24SuspendScheduler");

extern "C" void MH3_NWC24ResumeScheduler(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(ResumeScheduler(cpu));
}
REGISTER_NATIVE_FUNCTION_AS(0x8051E0DC, MH3_NWC24ResumeScheduler, "NWC24ResumeScheduler");

extern "C" void MH3_NWC24SetScriptMode(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(SetScriptMode(cpu, cpu->gpr[3]));
}
REGISTER_NATIVE_FUNCTION_AS(0x8051E1CC, MH3_NWC24SetScriptMode, "NWC24iSetScriptMode");

extern "C" void MH3_NWC24RequestGenerateUserId(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(RequestGenerateUserId(cpu, cpu->gpr[3], cpu->gpr[4]));
}
REGISTER_NATIVE_FUNCTION_AS(0x8051E36C, MH3_NWC24RequestGenerateUserId, "NWC24iRequestGenerateUserId");

// The /dev/net/kd/request commands with no input (0x8051E528..0x8051E578).
extern "C" void MH3_NWC24StartSocket(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(Request(cpu, kStartSocket, cpu->gpr[3]));
}
REGISTER_NATIVE_FUNCTION_AS(0x8051E528, MH3_NWC24StartSocket, "NWC24iStartSocket");

extern "C" void MH3_NWC24CleanupSocket(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(Request(cpu, kCleanupSocket, cpu->gpr[3]));
}
REGISTER_NATIVE_FUNCTION_AS(0x8051E538, MH3_NWC24CleanupSocket, "NWC24iCleanupSocket");

extern "C" void MH3_NWC24SuspendForSocket(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(Request(cpu, kSuspendForSocket, 0));
}
REGISTER_NATIVE_FUNCTION_AS(0x8051E548, MH3_NWC24SuspendForSocket, "NWC24iSuspendForSocket");

extern "C" void MH3_NWC24ResumeForSocket(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(Request(cpu, kResumeForSocket, 0));
}
REGISTER_NATIVE_FUNCTION_AS(0x8051E558, MH3_NWC24ResumeForSocket, "NWC24iResumeForSocket");

extern "C" void MH3_NWC24RequestSuspend(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(Request(cpu, kSuspendScheduler, 0));
}
REGISTER_NATIVE_FUNCTION_AS(0x8051E568, MH3_NWC24RequestSuspend, "NWC24iRequestSuspend");

extern "C" void MH3_NWC24RequestResume(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(Request(cpu, kResumeScheduler, 0));
}
REGISTER_NATIVE_FUNCTION_AS(0x8051E578, MH3_NWC24RequestResume, "NWC24iRequestResume");

extern "C" void MH3_NWC24Request(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(Request(cpu, cpu->gpr[4], cpu->gpr[5]));
}
REGISTER_NATIVE_FUNCTION_AS(0x8051E588, MH3_NWC24Request, "NWC24iRequest");

extern "C" void MH3_NWC24CheckUserId(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(CheckUserId(cpu->gpr[3], cpu->gpr[4]));
}
REGISTER_NATIVE_FUNCTION_AS(0x8051E6E8, MH3_NWC24CheckUserId, "NWC24iCheckUserId");

extern "C" void MH3_NWC24DecodeUserId(CpuContext* cpu) {
    const uint64_t value = DecodeUserId(cpu->gpr[3], cpu->gpr[4]);
    cpu->gpr[3] = uint32_t(value >> 32);
    cpu->gpr[4] = uint32_t(value);
}
REGISTER_NATIVE_FUNCTION_AS(0x8051E780, MH3_NWC24DecodeUserId, "NWC24iDecodeUserId");

extern "C" void MH3_NWC24SetRtcCounter(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(SetRtcCounter(cpu, cpu->gpr[3], cpu->gpr[4]));
}
REGISTER_NATIVE_FUNCTION_AS(0x8051EA04, MH3_NWC24SetRtcCounter, "NWC24iSetRtcCounter");

extern "C" void MH3_NWC24SynchronizeRtcCounter(CpuContext* cpu) {
    SynchronizeRtcCounter(cpu, cpu->gpr[3]);
}
REGISTER_NATIVE_FUNCTION_AS(0x8051EB4C, MH3_NWC24SynchronizeRtcCounter, "NWC24iSynchronizeRtcCounter");

// The device helpers take the caller's name first, for nothing.
extern "C" void MH3_NWC24Open(CpuContext* cpu) {
    const uint32_t fdOut = cpu->gpr[5];
    int32_t result = kEInval;

    if (fdOut) {
        uint32_t fd;
        result = Open(cpu, cpu->gpr[4], fd, cpu->gpr[6]);
        Write(fdOut, fd);
    }

    cpu->gpr[3] = uint32_t(result);
}
REGISTER_NATIVE_FUNCTION_AS(0x8051EBE0, MH3_NWC24Open, "NWC24iOpen");

extern "C" void MH3_NWC24Close(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(Close(cpu, cpu->gpr[4]));
}
REGISTER_NATIVE_FUNCTION_AS(0x8051EC58, MH3_NWC24Close, "NWC24iClose");

extern "C" void MH3_NWC24Ioctl(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(Ioctl(cpu, cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7], cpu->gpr[8], cpu->gpr[9]));
}
REGISTER_NATIVE_FUNCTION_AS(0x8051EC8C, MH3_NWC24Ioctl, "NWC24iIoctl");

extern "C" void MH3_NWC24IoctlAsync(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(IoctlAsync(cpu, cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7], cpu->gpr[8], cpu->gpr[9], cpu->gpr[10]));
}
REGISTER_NATIVE_FUNCTION_AS(0x8051ECD4, MH3_NWC24IoctlAsync, "NWC24iIoctlAsync");

extern "C" void MH3_NWC24IsAsyncBusy(CpuContext* cpu) {
    cpu->gpr[3] = Read(kAsyncBusy);
}
REGISTER_NATIVE_FUNCTION_AS(0x8051ED30, MH3_NWC24IsAsyncBusy, "NWC24iIsAsyncBusy");

extern "C" void MH3_NWC24AsyncDone(CpuContext* cpu) {
    if (const uint32_t result = cpu->gpr[4])
        Write(result, cpu->gpr[3]);

    Write(kAsyncBusy, 0);
    cpu->gpr[3] = 0;
}
REGISTER_NATIVE_FUNCTION_AS(0x8051ED38, MH3_NWC24AsyncDone, "NWC24iAsyncDone");

extern "C" void MH3_NWC24PrepareShutdown(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(PrepareShutdown(cpu));
}
REGISTER_NATIVE_FUNCTION_AS(0x8051ED54, MH3_NWC24PrepareShutdown, "NWC24iPrepareShutdown");

extern "C" void MH3_NWC24ShutdownHook(CpuContext* cpu) {
    cpu->gpr[3] = ShutdownHook(cpu, cpu->gpr[3], cpu->gpr[4]);
}
REGISTER_NATIVE_FUNCTION_AS(0x8051EE14, MH3_NWC24ShutdownHook, "NWC24iShutdownHook");
