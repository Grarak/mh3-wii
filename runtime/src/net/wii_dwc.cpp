// DWC, Nintendo's online layer over GameSpy (RMHP08 0x805076D0..0x8050B050;
// decompiled with Ghidra, the call arguments checked in the translated C++),
// ported natively: its base64 (with '.', '-' and '*' padding), errors and
// debug print, the allocator wrappers it hands GameSpy, the console's friend
// code, and the account server's login (NAS "ac": acctcreate, login, svcloc)
// as a state machine over NAND (the stored user ID) and NHTTP. NHTTP, NAND,
// SC, OS and the guest's libc (sprintf, strtok, sscanf, strtol, string and
// wide-string calls, memset) stay guest calls.
#include "abi_bridge.h"
#include "guest_call.h"
#include "memory.h"

#include <cstring>
#include <initializer_list>

namespace {

enum : uint32_t {
    Memset = 0x80004350,
    Strlen = 0x80456C40,
    Div2i = 0x804575D8,
    Vprintf = 0x8045E2D0,
    Sprintf = 0x8045E54C,
    Sscanf = 0x8045F9FC,
    Strcpy = 0x8045FBD4,
    Strncpy = 0x8045FC94,
    Strncmp = 0x8045FE20,
    Strtok = 0x8045FED8,
    Strstr = 0x8045FFFC,
    Strtol = 0x80461C48,
    Wcslen = 0x804641A0,
    Wcsncpy = 0x804641D8,
    NandPrivateCreateAsync = 0x804C7970,
    NandPrivateDeleteAsync = 0x804C7CA0,
    NandReadAsync = 0x804C7DD0,
    NandWriteAsync = 0x804C7EC0,
    NandGetStatus = 0x804C8710,
    NandPrivateOpenAsync = 0x804C8B90,
    NandCloseAsync = 0x804C8D00,
    NandGetHomeDir = 0x804C96A0,
    OSRegisterVersion = 0x804CBA00,
    OSGetAppGamename = 0x804CBA10,
    OSReport = 0x804CDCA0,
    OSPanic = 0x804CDD30,
    OSGetTime = 0x804D53D0,
    OSTicksToCalendarTime = 0x804D54D0,
    OSCalendarTimeToTicks = 0x804D5800,
    SCGetLanguage = 0x804DD150,
    SCGetProductArea = 0x804DD800,
    SCGetSerialPrefix = 0x804DD890,
    SCGetSerialNumber = 0x804DD8D0,
    GsiGetHostByName = 0x8050C2C0,
    GsiMemoryCallbacksSet = 0x8050CC00,
    NhttpStartup = 0x8051AB68,
    NhttpCleanupAsync = 0x8051ABF4,
    NhttpGetError = 0x8051AC4C,
    NhttpCreateRequest = 0x8051AF24,
    NhttpAddHeaderField = 0x8051AFA8,
    NhttpAddPostDataAscii = 0x8051B06C,
    NhttpSendRequestAsync = 0x8051B130,
    NhttpCancelRequestAsync = 0x8051B18C,
    NhttpDestroyResponse = 0x8051B1CC,
    NhttpGetBodyAll = 0x8051B250,
    NhttpGetResultCode = 0x8051B2B8,
    NhttpSetProxyDefault = 0x8051B318,
    NhttpSetVerifyOption = 0x8051B4E4,
    NhttpSetRootCaDefault = 0x8051B60C,
    NhttpSetClientCertDefault = 0x8051B66C,
    NcdGetCurrentIfConfig = 0x8051CBD4,
    NcdGetWirelessMacAddress = 0x8051D8C8,
    Nwc24GetMyUserId = 0x8051DD90,
    PoolIsInitialized = 0x80522460,
    PoolInit = 0x80522320,
    PoolFinish = 0x805223F0,
};

// The ports' own addresses, handed to GameSpy, NHTTP and NAND as callbacks.
enum : uint32_t {
    DwcAlloc = 0x80507CA0,
    DwcFree = 0x80507D70,
    GsMalloc = 0x80507D90,
    GsRealloc = 0x80507E00,
    GsFree = 0x80507ED0,
    GsMemalign = 0x80507F00,
    NhttpAllocHook = 0x80509890,
    NhttpFreeHook = 0x805098B0,
    NandDone = 0x805098D0,
    AuthResponse = 0x8050A430,
    AuthEnd = 0x8050AD90,
};

// Small-data and data globals.
enum : uint32_t {
    kErrorClass = 0x80797C70,
    kErrorCode = 0x80797C74,
    kAuthServer = 0x80797C78,
    kInitialized = 0x80797C7C,
    kFreeFunction = 0x80797C80,
    kAllocFunction = 0x80797C84,
    kPrintMask = 0x80797C88,
    kFriendCodeDone = 0x80797C90,
    kFriendCode = 0x80797C98, // u64
    kFreeList = 0x80797CA0,
    kUseStoredId = 0x80797CA8,
    kEnvironment = 0x80797CAC, // 0 test servers, 1 release
    kWork = 0x80797CB0,        // the login's work (0x5B30 bytes)
    kAuthState = 0x80797CB4,
    kSvlState = 0x80797CB8,
    kSvlResult = 0x80797CC0,
    kBase64Table = 0x807966B8, // char*: "A..Za..z0..9.-"
    kUserIdFile = 0x807966C8,  // the NAND path of the stored user ID
    kAuthResult = 0x807633C0,  // 0x1D0 bytes: the result, the token at +4...
    kSvlData = 0x80763590,     // 0x174 bytes: the service locator's answer
    kStoredUserId = 0x80763720, // u64, its NAND copy's 32 bytes
    kRetryCode = 0x80763578,
    kTimeOffset = 0x80763580, // s64: the server's time less OSGetTime
    kResponse = 0x80763588,
    kSslError = 0x80763774,
    kHostHeader = 0x80763778,
    kGameName = 0x80764AB0,
    kStatsHost = 0x80631168, // "gamestats.gs.nintendowifi.net", rewritten
};

// The login work's fields.
enum : uint32_t {
    kWorkIfConfig = 0x4000,
    kWorkIngamesn = 0x415E, // UTF-16, 26 characters
    kWorkGsbrcd = 0x4192,
    kWorkSvc = 0x419E,
    kWorkWordsData = 0x41A4,
    kWorkWordsLength = 0x41A8,
    kWorkHasUserId = 0x41AC,
    kWorkWregion = 0x41B0,
    kWorkBody = 0x41B2, // the response body buffer (0x1000)
    kWorkPost = 0x51B2, // the post fields' base64 (0x800 each)
    kWorkTries = 0x59B4,
    kWorkNandDone = 0x59B8,
    kWorkNandResult = 0x59BC,
    kWorkRequest = 0x59C4,
    kWorkMode = 0x59C8, // 1 login, 2 svcloc, 3 the profanity check
    kWorkNandResultOut = 0x59CC,
    kWorkFile = 0x5A88,
    kWorkAlloc = 0x5B14,
    kWorkFree = 0x5B18,
    kWorkStartTime = 0x5B20,
    kWorkIdHigh = 0x5B28,
    kWorkIdLow = 0x5B2C,
    kWorkBytes = 0x5B30,
};

constexpr uint32_t kBusClock = 0x800000F8;
constexpr uint32_t kCrFloatArgs = 1u << (31 - 6);
constexpr uint32_t kMagic = 0x4457434D; // "DWCM"
constexpr uint32_t kHeader = 0x20;
constexpr uint32_t kPostField = 0x800;

uint32_t Read(uint32_t address) {
    return Memory::Read32(address);
}

void Write(uint32_t address, uint32_t value) {
    Memory::Write32(address, value);
}

uint32_t Work() {
    return Read(kWork);
}

uint32_t Length(uint32_t s) {
    uint32_t n = 0;

    while (Memory::Read8(s + n))
        ++n;

    return n;
}

// 0x80507F70's prefixes by level.
uint32_t PrintPrefix(uint32_t level) {
    switch (level) {
    case 0x1: return 0x806306D0; // "DWC_INFO: "
    case 0x2: return 0x806306E0; // "!!DWC_ERROR!!: "
    case 0x4: return 0x806306F0; // "DWC_DEBUG: "
    case 0x8: return 0x80630700; // "DWC_WARN: "
    case 0x10: return 0x80630710;
    case 0x20: return 0x80630720;
    case 0x40: return 0x80630730;
    case 0x80: return 0x80630740;
    case 0x100: return 0x80630750;
    case 0x200: return 0x80630760;
    case 0x400: return 0x80630770;
    case 0x8000: return 0x80630780;
    case 0x10000: return 0x80630790;
    case 0x20000: return 0x806307A0;
    case 0x40000: return 0x806307B0;
    case 0x1000000: return 0x806307C4; // "DWC_AUTH: "
    case 0x2000000: return 0x806307D4;
    case 0x4000000: return 0x806307E4;
    case 0x8000000: return 0x806307F4;
    case 0x10000000: return 0x80630804;
    case 0x20000000: return 0x80630814;
    case 0x80000000: return 0x8063082C; // "DWC_GAMESPY: "
    default: return 0x8063083C;          // "DWC_UNKNOWN: "
    }
}

// 0x80507F70: DWC_Printf(level, format, ...): with the level in the print
// mask, its prefix by OSReport, then vprintf with the arguments as the
// variadic call laid them out (r5..r10, f1..f8 when CR bit 6 says so, then
// the caller's stack).
void PrintWith(CpuContext* cpu, const uint32_t (&gprs)[8], const double* fprs, uint32_t overflow) {
    const uint32_t level = gprs[0];

    if (!(level & Read(kPrintMask)))
        return;

    GuestFrame frame(cpu, 0x80);
    GuestCall call{cpu};
    cpu->cr &= ~kCrFloatArgs;
    call.Call<OSReport>({PrintPrefix(level)});
    const uint32_t save = frame[0x10], list = frame[0x70];

    for (int i = 0; i < 8; ++i)
        Write(save + 4 * i, gprs[i]);

    if (fprs)
        for (int i = 0; i < 8; ++i) {
            uint64_t bits;
            std::memcpy(&bits, &fprs[i], 8);
            Memory::Write64(save + 0x20 + 8 * i, bits);
        }

    Write(list, 0x02000000); // two integer registers used, none of the FPRs
    Write(list + 4, overflow);
    Write(list + 8, save);
    call.Call<Vprintf>({gprs[1], list});
}

// DWC_Printf from a port: integer arguments only.
void Print(CpuContext* cpu, uint32_t level, uint32_t format, std::initializer_list<uint32_t> args = {}) {
    if (!(level & Read(kPrintMask)))
        return;

    uint32_t gprs[8] = {level, format};
    int i = 2;

    for (const uint32_t arg : args)
        gprs[i++] = arg;

    PrintWith(cpu, gprs, nullptr, cpu->gpr[1] + 8);
}

// 0x80507B10: DWCi_SetError(class, code), unless the error is fatal (9).
void SetError(uint32_t errorClass, uint32_t code) {
    if (Read(kErrorClass) == 9)
        return;

    Write(kErrorClass, errorClass);
    Write(kErrorCode, code);
}

// 0x805076D0: DWC's base64 of `length` bytes into `out` (at most `room`
// bytes, '*' padding): the bytes written, the length needed with no `out`,
// -1 with too little room.
int32_t Base64Encode(uint32_t data, uint32_t length, uint32_t out, uint32_t room) {
    const uint32_t needed = length / 3 * 4 + (length % 3 ? 4 : 0);

    if (!out)
        return int32_t(needed);

    if (room < needed)
        return -1;

    const uint32_t table = Read(kBase64Table);
    uint32_t at = out;

    for (uint32_t from = data, end = data + length; from != end;) {
        const uint32_t left = end - from;
        const uint32_t chars = (left * 8 + 5) / 6;
        const uint32_t take = left < 3 ? left : 3;
        uint8_t b[3] = {};

        for (uint32_t i = 0; i < take; ++i)
            b[i] = Memory::Read8(from + i);

        Memory::Write8(at, Memory::Read8(table + (b[0] >> 2)));
        Memory::Write8(at + 1, chars < 2 ? '*' : Memory::Read8(table + ((b[0] & 3) << 4 | b[1] >> 4)));
        Memory::Write8(at + 2, chars < 3 ? '*' : Memory::Read8(table + ((b[1] & 0xF) << 2 | b[2] >> 6)));
        Memory::Write8(at + 3, chars < 4 ? '*' : Memory::Read8(table + (b[2] & 0x3F)));
        at += 4;
        from += take;
    }

    return int32_t(at - out);
}

int8_t Base64Value(int8_t c) {
    if (c >= 'A' && c <= 'Z')
        return int8_t(c - 'A');

    if (c >= 'a' && c <= 'z')
        return int8_t(c - 'a' + 26);

    if (c >= '0' && c <= '9')
        return int8_t(c - '0' + 52);

    if (c == '.')
        return 62;

    if (c == '-')
        return 63;

    return 0;
}

// 0x80507860: DWC's base64 decoding: the bytes written (or needed with no
// `out`), -1 for a length that is no multiple of 4 or too little room.
int32_t Base64Decode(uint32_t text, uint32_t length, uint32_t out, uint32_t room) {
    if (length & 3)
        return -1;

    int32_t bits = 0;

    for (uint32_t i = 0; i < length; ++i)
        if (Memory::Read8(text + i) != '*')
            bits += 6;

    const int32_t bytes = bits / 8;

    if (!out)
        return bytes;

    if (room < uint32_t(bytes))
        return -1;

    if (!length)
        return 0;

    int32_t written = 0;

    for (uint32_t at = text;; at += 4) {
        int8_t v[4];

        for (int i = 0; i < 4; ++i)
            v[i] = Base64Value(int8_t(Memory::Read8(at + i)));

        Memory::Write8(out + written, uint8_t(v[0] << 2 | v[1] >> 4));

        if (++written >= bytes)
            return written;

        Memory::Write8(out + written, uint8_t(v[1] << 4 | v[2] >> 2));

        if (++written >= bytes)
            return written;

        Memory::Write8(out + written, uint8_t(v[2] << 6 | v[3]));

        if (++written >= bytes)
            return written;
    }
}

// The allocator the game set (DWC_SetMemFunc): blocks carry a 32-byte header
// with "DWCM" and the size asked for.
uint32_t Alloc(CpuContext* cpu, uint32_t kind, uint32_t size, uint32_t align) {
    const uint32_t block = GuestCall{cpu}(Read(kAllocFunction), {kind, size + kHeader, align});

    if (!block)
        return 0;

    Write(block, kMagic);
    Write(block + 4, size);
    return block + kHeader;
}

void Free(CpuContext* cpu, uint32_t kind, uint32_t block, uint32_t size) {
    if (block)
        GuestCall{cpu}(Read(kFreeFunction), {kind, block - kHeader, size});
}

// 0x80508CB0: the console's friend code (NWC24's user ID), fetched once with
// the pool library (0x80522320) up for it if nothing else started it.
uint64_t ConsoleFriendCode(CpuContext* cpu) {
    if (!Read(kFriendCodeDone)) {
        GuestCall call{cpu};
        const uint32_t poolUp = call.Call<PoolIsInitialized>();
        const uint32_t small = Alloc(cpu, 3, 0x4000, 0x20);
        const uint32_t big = Alloc(cpu, 3, 0x8000, 0x20);
        Write(kFriendCodeDone, 1);

        if (poolUp != 1)
            call.Call<PoolInit>({big, 0x8000});

        if (call.Call<Nwc24GetMyUserId>({kFriendCode}) == 0) {
            Print(cpu, 0x8000000, 0x80630890, {Read(kFriendCode), Read(kFriendCode + 4)});
        } else {
            Print(cpu, 0x8000000, 0x806308B4);
            Write(kFriendCode + 4, 0);
            Write(kFriendCode, 0);
        }

        if (poolUp != 1)
            call.Call<PoolFinish>();

        Free(cpu, 3, small, 0);
        Free(cpu, 3, big, 0);
    }

    return (uint64_t(Read(kFriendCode)) << 32) | Read(kFriendCode + 4);
}

// 0x80508E20: the login's state cleared; `environment` picks the servers.
void AuthInit(CpuContext* cpu, uint32_t environment) {
    GuestCall call{cpu};
    call.Call<Memset>({kAuthResult, 0, 0x1D0});
    call.Call<Memset>({kSvlData, 0, 0x174});
    call.Call<Memset>({kStoredUserId, 0, 0x20});
    Write(kWork, 0);
    Write(kAuthState, 0);
    Write(kEnvironment, environment);
    Write(kRetryCode, 0);
    Write(kUseStoredId, 0);
}

// 0x80508EB0: starts a login (ingamesn, gsbrcd, a user ID to log in with):
// the work from `alloc`, the interface config, NHTTP.
uint32_t AuthStart(CpuContext* cpu, uint32_t ingamesn, uint32_t gsbrcd, uint32_t idHigh, uint32_t idLow, uint32_t alloc, uint32_t free) {
    const uint32_t state = Read(kAuthState);

    if (state != 0 && state != 0x1A) {
        Print(cpu, 0x1000000, 0x806309F8); // "auth is processing"
        return 0;
    }

    GuestCall call{cpu};
    const uint32_t work = call(alloc, {0, kWorkBytes});
    Write(kWork, work);

    if (!work) {
        Print(cpu, 0x1000000, 0x80630A10); // "memory shortage"
        return 0;
    }

    call.Call<Memset>({work, 0, kWorkBytes});
    Write(work + kWorkAlloc, alloc);
    Write(work + kWorkFree, free);
    const int32_t config = int32_t(call.Call<NcdGetCurrentIfConfig>({work + kWorkIfConfig}));

    if (config == 0) {
        if (int32_t(call.Call<NhttpStartup>({NhttpAllocHook, NhttpFreeHook, 0x11})) >= 0) {
            call.Call<Wcsncpy>({work + kWorkIngamesn, ingamesn, 0x1A});
            call.Call<Strncpy>({work + kWorkGsbrcd, gsbrcd, 0xC});
            Write(work + kWorkMode, 1);
            call.Call<Memset>({kAuthResult, 0, 0x1D0});
            Write(kRetryCode, 0);
            Write(work + kWorkIdLow, idLow);
            Write(work + kWorkIdHigh, idHigh);
            Write(kAuthState, 1);
            return 1;
        }

        Print(cpu, 0x1000000, 0x80630A48); // "failed to start NHTTP"
    } else {
        Print(cpu, 0x1000000, 0x80630A24, {uint32_t(config)});
    }

    call(Read(Work() + kWorkFree), {0, Work(), 0});
    return 0;
}

// 0x8050AD90: the login's end: its work freed, the state 0x19 (done).
void EndAuth(CpuContext* cpu) {
    Print(cpu, 0x1000000, 0x806310B0); // "DWCi_Auth_EndProcess()"
    GuestCall{cpu}(Read(Work() + kWorkFree), {0, Work(), 0});
    Write(kWork, 0);
    Write(kAuthState, 0x19);
}

// 0x8050ADF0: a finished NAND step: on to `next`, or `retry` (five tries)
// after a busy file system (-3); a failure other than the ones `okA` and
// `okB` allow (a missing file, -12, for okA; -1 for okB; 0x1B: none) ends
// the login with NAND's error.
void NandStepDone(CpuContext* cpu, uint32_t next, uint32_t retry, uint32_t okA, uint32_t okB) {
    const uint32_t work = Work();
    Write(work + kWorkNandDone, 0);
    const int32_t result = int32_t(Read(work + kWorkNandResult));

    if (retry != 0x1B && result == -3 && int32_t(Read(work + kWorkTries)) <= 4) {
        Write(work + kWorkTries, Read(work + kWorkTries) + 1);
        Write(kAuthState, retry);
        return;
    }

    Write(work + kWorkTries, 0);
    uint32_t state = next;

    if (result != 0 && (okA == 0x1B || result != -12) && (okB == 0x1B || result != -1)) {
        Print(cpu, 0x1000000, 0x806310C8, {uint32_t(result)});
        Write(kAuthResult, uint32_t((result == -4 ? -1 : 0) - 29000));
        EndAuth(cpu);
        state = 0x19;
    } else if (result == -12 && okA != 0x1B) {
        state = okA;
    } else if (result == -1 && okB != 0x1B) {
        state = okB;
    }

    Write(kAuthState, state);
}

// Milliseconds since the request went out.
int64_t SinceRequest(CpuContext* cpu) {
    GuestCall call{cpu};
    const uint32_t high = call.Call<OSGetTime>();
    const uint64_t now = (uint64_t(high) << 32) | cpu->gpr[4];
    const uint32_t work = Work();
    const uint64_t start = (uint64_t(Read(work + kWorkStartTime)) << 32) | Read(work + kWorkStartTime + 4);
    return int64_t(now - start) / int64_t(Read(kBusClock) / 4000);
}

void StampRequest(CpuContext* cpu) {
    GuestCall call{cpu};
    const uint32_t high = call.Call<OSGetTime>();
    const uint32_t low = cpu->gpr[4];
    Write(Work() + kWorkStartTime, high);
    Write(Work() + kWorkStartTime + 4, low);
}

// A request's result in kAuthResult: done (1), a server error band
// (-20000..-20999, -29000 on) to stop at, another failure to retry three
// times, or still running, timed out after 30 s.
void WaitForRequest(CpuContext* cpu, uint32_t retry, const char* timeoutLabel, uint32_t timeoutText) {
    const int32_t result = int32_t(Read(kAuthResult));

    if (result == 1) {
        Write(kAuthState, retry == 9 ? 0xB : 0x17);
    } else if (result >= -20999 && result <= -20102) {
        Write(kAuthState, 0x17);
    } else if (result < 0) {
        const uint32_t work = Work();
        Write(work + kWorkTries, Read(work + kWorkTries) + 1);
        Write(kAuthState, int32_t(Read(work + kWorkTries)) < 3 ? retry : 0x17);
    } else if (SinceRequest(cpu) > 30000) {
        (void)timeoutLabel;
        Print(cpu, 0x1000000, timeoutText);
        Write(kAuthState, 0x15);
    }
}

void SendAuthRequest(CpuContext* cpu, uint32_t mode, uint32_t ingamesn, uint32_t gsbrcd, uint32_t unused, uint32_t idHigh, uint32_t idLow);

// 0x805090F0: one step of the login: the stored user ID read from NAND (or
// a new account made and stored), the login request, its answer, the end.
void AuthProcess(CpuContext* cpu) {
    const uint32_t state = Read(kAuthState);

    if (state >= 0x1B)
        return;

    GuestCall call{cpu};
    const uint32_t work = Work();
    const uint32_t nandDone = work + kWorkNandDone;

    switch (state) {
    case 1:
        if (Read(work + kWorkMode) == 3 || Read(kStoredUserId) || Read(kStoredUserId + 4)) {
            Write(kAuthState, 0x13);
        } else {
            call.Call<NandPrivateOpenAsync>({Read(kUserIdFile), work + kWorkFile, 1, NandDone, work + kWorkNandResultOut});
            Write(kAuthState, 2);
        }

        break;
    case 2:
        if (Read(nandDone))
            NandStepDone(cpu, 3, 1, 9, 7);

        break;
    case 3:
        call.Call<NandReadAsync>({work + kWorkFile, kStoredUserId, 0x20, NandDone, work + kWorkNandResultOut});
        Write(kAuthState, 4);
        break;
    case 4:
        if (Read(nandDone)) {
            const int32_t read = int32_t(Read(work + kWorkNandResult));

            if (read == 0x20) {
                Print(cpu, 0x1000000, 0x80630A78, {Read(kStoredUserId), Read(kStoredUserId + 4)});
                Write(work + kWorkNandResult, 0);
            } else if (read >= 0) {
                Print(cpu, 0x1000000, 0x80630A90);
                Write(work + kWorkNandResult, 0xFFFFFFFF);
            }

            NandStepDone(cpu, 0x11, 3, 0x1B, 5);
        }

        break;
    case 5:
        call.Call<NandCloseAsync>({work + kWorkFile, NandDone, work + kWorkNandResultOut});
        Write(kAuthState, 6);
        break;
    case 6:
        if (Read(nandDone))
            NandStepDone(cpu, 7, 5, 0x1B, 0x1B);

        break;
    case 7:
        call.Call<NandPrivateDeleteAsync>({Read(kUserIdFile), NandDone, work + kWorkNandResultOut});
        Write(kAuthState, 8);
        break;
    case 8:
        if (Read(nandDone)) {
            if (Read(work + kWorkNandResult) == 0)
                Print(cpu, 0x1000000, 0x80630AB0); // "delete illegal userid"

            NandStepDone(cpu, 9, 7, 9, 0x1B);
        }

        break;
    case 9:
        SendAuthRequest(cpu, 0, 0, 0, 0, 0, 0);
        StampRequest(cpu);
        Write(kAuthState, 10);
        break;
    case 10:
        WaitForRequest(cpu, 9, "acctcreate", 0x80630ACC);
        break;
    case 0xB:
        call.Call<NandPrivateCreateAsync>({Read(kUserIdFile), 0x3F, 0, NandDone, work + kWorkNandResultOut});
        Write(kAuthState, 0xC);
        break;
    case 0xC:
        if (Read(nandDone))
            NandStepDone(cpu, 0xD, 0xB, 0x1B, 0x1B);

        break;
    case 0xD:
        call.Call<NandPrivateOpenAsync>({Read(kUserIdFile), work + kWorkFile, 2, NandDone, work + kWorkNandResultOut});
        Write(kAuthState, 0xE);
        break;
    case 0xE:
        if (Read(nandDone))
            NandStepDone(cpu, 0xF, 0xD, 0x1B, 0x1B);

        break;
    case 0xF:
        call.Call<NandWriteAsync>({work + kWorkFile, kStoredUserId, 0x20, NandDone, work + kWorkNandResultOut});
        Write(kAuthState, 0x10);
        break;
    case 0x10:
        if (Read(nandDone)) {
            if (Read(work + kWorkNandResult) == 0x20) {
                Write(work + kWorkNandResult, 0);
            } else {
                Print(cpu, 0x1000000, 0x80630AE4, {Read(work + kWorkNandResult)});
                Write(work + kWorkNandResult, 0xFFFFFFFF);
            }

            NandStepDone(cpu, 0x11, 0xF, 0x1B, 0x1B);
        }

        break;
    case 0x11:
        call.Call<NandCloseAsync>({work + kWorkFile, NandDone, work + kWorkNandResultOut});
        Write(kAuthState, 0x12);
        break;
    case 0x12:
        if (Read(nandDone))
            NandStepDone(cpu, 0x13, 0x11, 0x1B, 0x1B);

        break;
    case 0x13:
        if (!Read(kUseStoredId))
            SendAuthRequest(cpu, Read(work + kWorkMode), work + kWorkIngamesn, work + kWorkGsbrcd, work, Read(kStoredUserId), Read(kStoredUserId + 4));
        else
            SendAuthRequest(cpu, Read(work + kWorkMode), work + kWorkIngamesn, work + kWorkGsbrcd, work, Read(work + kWorkIdHigh), Read(work + kWorkIdLow));

        StampRequest(cpu);
        Write(kAuthState, 0x14);
        break;
    case 0x14:
        WaitForRequest(cpu, 0x13, "login", 0x80630B08);
        break;
    case 0x15:
        Write(kAuthState, 0x16);
        call.Call<NhttpCancelRequestAsync>({Read(work + kWorkRequest)});
        break;
    case 0x16:
        if (Read(kAuthResult)) {
            Write(kAuthResult, uint32_t(-20100));
            Write(kAuthState, 0x17);
        }

        break;
    case 0x17:
        Write(kAuthState, 0x18);

        if (Read(kResponse)) {
            Print(cpu, 0x1000000, 0x80630A60); // "NHTTPDestroyResponse()"
            call.Call<NhttpDestroyResponse>({Read(kResponse)});
            Write(kResponse, 0);
        }

        call.Call<NhttpCleanupAsync>({AuthEnd});
        break;
    default:
        break;
    }
}

// Adds a post field, its value base64 encoded into the next part of the
// work's post buffer; returns the next part.
uint32_t PostField(CpuContext* cpu, uint32_t request, uint32_t name, uint32_t value, uint32_t length, uint32_t at) {
    const int32_t encoded = Base64Encode(value, length, at, kPostField);
    Memory::Write8(at + uint32_t(encoded), 0);
    GuestCall{cpu}.Call<NhttpAddPostDataAscii>({request, name, at});
    return at + uint32_t(encoded) + 1;
}

uint32_t PostString(CpuContext* cpu, uint32_t request, uint32_t name, uint32_t value, uint32_t at) {
    return PostField(cpu, request, name, value, Length(value), at);
}

// sprintf into `buffer` with integer arguments.
void Format(CpuContext* cpu, uint32_t buffer, uint32_t format, std::initializer_list<uint32_t> args) {
    uint32_t regs[8] = {buffer, format};
    int i = 2;

    for (const uint32_t arg : args)
        regs[i++] = arg;

    for (int r = 0; r < 8; ++r)
        cpu->gpr[3 + r] = regs[r];

    cpu->cr &= ~kCrFloatArgs;
    InvokeIndirectCpu(Sprintf, cpu);
}

// 0x805098F0: the account server request: acctcreate (0), login (1),
// svcloc (2) or the word check (3), with the console's and the game's
// details, sent with AuthResponse as its callback.
void SendAuthRequest(CpuContext* cpu, uint32_t mode, uint32_t ingamesn, uint32_t gsbrcd, uint32_t, uint32_t idHigh, uint32_t idLow) {
    GuestFrame frame(cpu, 0x1E0);
    GuestCall call{cpu};
    const uint32_t text = frame[0x88], status = frame[0x10], home = frame[0x48], mac = frame[0x18], calendar = frame[0x20], serial = frame[0x8];
    const uint32_t work = Work();
    const bool words = Read(work + kWorkMode) == 3;
    const uint32_t urls = words ? 0x806309EC : 0x8063096C; // per environment
    const uint32_t url = Read(urls + 4 * Read(kEnvironment));
    Print(cpu, 0x1000000, 0x80630B88, {url});
    const uint32_t request = call.Call<NhttpCreateRequest>({url, 1, work + kWorkBody, 0x1000, AuthResponse, 0});

    if (call.Call<NhttpSetRootCaDefault>({request})) {
        cpu->cr &= ~kCrFloatArgs;
        call.Call<OSPanic>({0x80630B94, 1033, 0x80630BAC});
    }

    if (call.Call<NhttpSetClientCertDefault>({request})) {
        cpu->cr &= ~kCrFloatArgs;
        call.Call<OSPanic>({0x80630B94, 1037, 0x80630BD0});
    }

    call.Call<NhttpSetVerifyOption>({request});
    call.Call<NhttpSetProxyDefault>({request, 2});
    call.Call<NhttpAddHeaderField>({request, 0x80630BF8, 0x80630C04}); // User-Agent: RVL SDK/1.0
    // Host: the URL's host, between "//" and the next '/'.
    const uint32_t host = call.Call<Strstr>({url, 0x807966D4});
    call.Call<Strcpy>({kHostHeader, host + 2});
    Memory::Write8(call.Call<Strstr>({kHostHeader, 0x807966D8}), 0);
    call.Call<NhttpAddHeaderField>({request, 0x807966DC, kHostHeader});
    call.Call<NhttpAddHeaderField>({request, 0x80630C10, call.Call<OSGetAppGamename>()});
    Print(cpu, 0x1000000, 0x80630C20, {call.Call<OSGetAppGamename>()});
    uint32_t at = work + kWorkPost;

    if (mode == 0) {
        at = PostString(cpu, request, 0x807966E4, 0x80630C38, at); // action=acctcreate
        Print(cpu, 0x1000000, 0x80630C44);
    } else if (mode == 1) {
        at = PostString(cpu, request, 0x807966E4, 0x807966EC, at); // action=login
        at = PostString(cpu, request, 0x807966F4, gsbrcd, at);
        Print(cpu, 0x1000000, 0x80630C5C);
        Print(cpu, 0x1000000, 0x80630C70, {gsbrcd});
        Format(cpu, text, 0x80796700, {idHigh, idLow}); // "%013llu"
        at = PostString(cpu, request, 0x80796708, text, at);
        Print(cpu, 0x1000000, 0x80630C80, {idHigh, idLow});
        const uint32_t characters = call.Call<Wcslen>({ingamesn});
        at = PostField(cpu, request, 0x80630C98, ingamesn, characters * 2, at); // ingamesn
    } else if (mode == 2) {
        at = PostString(cpu, request, 0x807966E4, 0x80796710, at); // action=svcloc
        Print(cpu, 0x1000000, 0x80630CA4);
        at = PostString(cpu, request, 0x80796718, work + kWorkSvc, at);
        Print(cpu, 0x1000000, 0x80630CB8, {work + kWorkSvc});
        Format(cpu, text, 0x80796700, {idHigh, idLow});
        at = PostString(cpu, request, 0x80796708, text, at);
        Print(cpu, 0x1000000, 0x80630C80, {idHigh, idLow});
    } else if (mode == 3) {
        if (Read(work + kWorkHasUserId) == 1) {
            Format(cpu, text, 0x80796700, {idHigh, idLow});
            at = PostString(cpu, request, 0x80796708, text, at);
            Print(cpu, 0x1000000, 0x80630C80, {idHigh, idLow});
        }

        at = PostString(cpu, request, 0x80796720, work + kWorkWregion, at);
        Print(cpu, 0x1000000, 0x80630CC4, {work + kWorkWregion});
        call.Call<NhttpAddPostDataAscii>({request, 0x80796728, 0x807966D0}); // wtype
        Print(cpu, 0x1000000, 0x80630CD4);
        at = PostString(cpu, request, 0x80796730, 0x80630CE0, at); // wenc=UTF-16BE
        Print(cpu, 0x1000000, 0x80630CEC);
        at = PostField(cpu, request, 0x80796738, Read(work + kWorkWordsData), Read(work + kWorkWordsLength), at);
    }

    at = PostString(cpu, request, 0x80796748, 0x80796740, at); // sdkver
    at = PostString(cpu, request, 0x80796750, call.Call<OSGetAppGamename>(), at); // gamecd
    // makercd: the home directory's title's maker, "01" and "00" on failure.
    const int32_t homeResult = int32_t(call.Call<NandGetHomeDir>({home}));

    if (homeResult == 0) {
        const int32_t statusResult = int32_t(call.Call<NandGetStatus>({home, status}));

        if (statusResult == 0) {
            const uint16_t group = Memory::Read16(status + 4);

            if (group == 2)
                call.Call<Strncpy>({text, 0x80796758, 3});
            else
                Format(cpu, text, 0x8079675C, {uint32_t(group >> 8), uint32_t(group & 0xFF)});
        } else {
            Print(cpu, 0x1000000, 0x80630D00, {uint32_t(statusResult)});
            call.Call<Strncpy>({text, 0x80796764, 3});
        }
    } else {
        Print(cpu, 0x1000000, 0x80630D1C, {uint32_t(homeResult)});
        call.Call<Strncpy>({text, 0x80796764, 3});
    }

    at = PostString(cpu, request, 0x80796768, text, at);
    Print(cpu, 0x1000000, 0x80630D3C, {text});
    at = PostString(cpu, request, 0x80796774, 0x80796770, at); // unitcd=1
    call.Call<NcdGetWirelessMacAddress>({mac});
    Format(cpu, text, 0x80630D4C, {Memory::Read8(mac), Memory::Read8(mac + 1), Memory::Read8(mac + 2), Memory::Read8(mac + 3), Memory::Read8(mac + 4), Memory::Read8(mac + 5)});
    at = PostString(cpu, request, 0x8079677C, text, at);
    Print(cpu, 0x1000000, 0x80630D68, {text});
    const uint32_t language = call.Call<SCGetLanguage>() & 0xFF;
    Format(cpu, text, 0x80796784, {language});
    Print(cpu, 0x1000000, 0x80630D78, {text});
    at = PostString(cpu, request, 0x8079678C, text, at);

    if (!words) {
        const uint32_t high = call.Call<OSGetTime>();
        const uint32_t low = cpu->gpr[4];
        call.Call<OSTicksToCalendarTime>({high, low, calendar});
        const int32_t year = int32_t(Read(calendar + 20));
        Format(cpu, text, 0x80630D84, {uint32_t(year % 100), Read(calendar + 16) + 1, Read(calendar + 12), Read(calendar + 8), Read(calendar + 4), Read(calendar)});
        at = PostString(cpu, request, 0x80796798, text, at); // devtime
        Print(cpu, 0x1000000, 0x80630DA0, {text});
    }

    if (Memory::Read8(work + kWorkIfConfig) == 1 && !words) {
        Format(cpu, text, 0x80796784, {Memory::Read8(work + kWorkIfConfig + 4)});
        Print(cpu, 0x1000000, 0x80630DB4, {text});
        at = PostString(cpu, request, 0x80630DC8, text, at); // confmethod
    }

    const uint32_t prefix = call.Call<SCGetSerialPrefix>();

    if (prefix && call.Call<SCGetSerialNumber>({serial})) {
        Format(cpu, text, 0x807967A0, {prefix, Read(serial)});
        Print(cpu, 0x1000000, 0x80630DD4, {text});
        at = PostString(cpu, request, 0x807967A8, text, at); // csnum
    }

    const uint64_t code = ConsoleFriendCode(cpu);
    Format(cpu, text, 0x807967B0, {uint32_t(code >> 32), uint32_t(code)});
    Print(cpu, 0x1000000, 0x80630DE4, {text});
    at = PostString(cpu, request, 0x807967B8, text, at); // cfc

    if (!words) {
        const int8_t region = int8_t(call.Call<SCGetProductArea>());
        Format(cpu, text, 0x80796784, {uint32_t(int32_t(region))});
        Print(cpu, 0x1000000, 0x80630DF0, {text});
        PostString(cpu, request, 0x807967BC, text, at);
    }

    Write(work + kWorkRequest, call.Call<NhttpSendRequestAsync>({request}));
    Write(kAuthResult, 0);
}

// A "name=value" token: the value decoded from base64 into `out` (0x100).
bool Field(CpuContext* cpu, uint32_t token, uint32_t name, uint32_t out, int32_t& length) {
    GuestCall call{cpu};
    const uint32_t nameLength = Length(name);

    if (call.Call<Strncmp>({token, name, nameLength}) != 0)
        return false;

    length = Base64Decode(token + nameLength, Length(token) - nameLength, out, 0x100);
    Memory::Write8(out + uint32_t(length), 0);
    return true;
}

// 0x8050A430: the account server's answer (NHTTP's callback): its fields,
// then the result: 1 for a return code below 100, -20000 less one at 100 on.
void AuthAnswer(CpuContext* cpu, uint32_t result, uint32_t response) {
    GuestFrame frame(cpu, 0x1C0);
    GuestCall call{cpu};
    const uint32_t returnCode = frame[8], body = frame[12], bodyLength = frame[16], value = frame[64];
    const uint32_t date = frame[24]; // OSCalendarTime: sec, min, hour, mday, mon, year, ...

    if (Read(kResponse)) {
        Print(cpu, 0x1000000, 0x80630A60);
        call.Call<NhttpDestroyResponse>({Read(kResponse)});
    }

    Write(kResponse, response);
    Print(cpu, 0x1000000, 0x80630E00, {result});

    if (result == 8) {
        Print(cpu, 0x1000000, 0x80630E18, {8});
        Write(kAuthResult, 2);
        return;
    }

    if (result != 0) {
        if (result == 0xE)
            Print(cpu, 0x1000000, 0x80630E30, {call.Call<NhttpGetError>()});

        Print(cpu, 0x1000000, 0x80630E40, {result});
        Write(kAuthResult, uint32_t(-20100));
        Write(kSslError, 1);
        return;
    }

    const int32_t status = int32_t(call.Call<NhttpGetResultCode>({response}));

    if (status != 200) {
        Print(cpu, 0x1000000, 0x80630E54, {uint32_t(status)});
        Write(kAuthResult, uint32_t(-23000 - status));

        if (int32_t(call.Call<NhttpGetBodyAll>({response, bodyLength})) < 1)
            Print(cpu, 0x1000000, 0x80630E78, {Read(bodyLength)});
        else
            Print(cpu, 0x1000000, 0x807967C4, {Read(bodyLength)});

        return;
    }

    bool hasReturnCode = false;

    if (int32_t(call.Call<NhttpGetBodyAll>({response, body})) > 0) {
        Print(cpu, 0x1000000, 0x807967CC, {Read(body)});
        int32_t length = 0;

        for (uint32_t token = call.Call<Strtok>({Read(body), 0x807967C8}); token; token = call.Call<Strtok>({0, 0x807967C8})) {
            if (Field(cpu, token, 0x807967D4, value, length)) { // retry=
                Print(cpu, 0x1000000, 0x80630E88, {uint32_t(length), value});
            } else if (Field(cpu, token, 0x80630E98, value, length)) { // returncd=
                call.Call<Strcpy>({returnCode, value});
                Print(cpu, 0x1000000, 0x80630EA4, {uint32_t(length), value});
                hasReturnCode = true;
            } else if (Field(cpu, token, 0x80630EB8, value, length)) { // datetime=
                cpu->cr &= ~kCrFloatArgs;
                const uint32_t parsed = call.Call<Sscanf>({value, 0x80630EC4, date + 20, date + 16, date + 12, date + 8, date + 4, date});
                uint64_t ticks = 0;

                if (parsed == 6) {
                    Write(date + 24, 0);
                    Write(date + 16, Read(date + 16) - 1);
                    Write(date + 28, 0);
                    Write(date + 32, 0);
                    Write(date + 36, 0);
                    const uint32_t high = call.Call<OSCalendarTimeToTicks>({date});
                    ticks = (uint64_t(high) << 32) | cpu->gpr[4];
                } else {
                    Print(cpu, 0x1000000, 0x80630EE0, {value});
                }

                const uint32_t high = call.Call<OSGetTime>();
                const uint64_t now = (uint64_t(high) << 32) | cpu->gpr[4];
                const uint64_t offset = ticks - now;
                Write(kTimeOffset, uint32_t(offset >> 32));
                Write(kTimeOffset + 4, uint32_t(offset));
                Print(cpu, 0x1000000, 0x80630EFC, {uint32_t(length), value});
            } else if (Field(cpu, token, 0x80630F10, value, length)) { // locator=
                call.Call<Strcpy>({0x8076353B, value});
                Print(cpu, 0x1000000, 0x80630F1C, {uint32_t(length), value});
            } else if (Field(cpu, token, 0x807967DC, value, length)) { // token=
                call.Call<Strcpy>({kAuthResult + 4, value});
                Print(cpu, 0x1000000, 0x80630F30, {uint32_t(length), value});
            } else if (Field(cpu, token, 0x80630F40, value, length)) { // challenge=
                call.Call<Strcpy>({0x807634F1, value});
                Print(cpu, 0x1000000, 0x80630F4C, {uint32_t(length), value});
            } else if (Field(cpu, token, 0x807967E8, value, length)) { // userid=
                cpu->cr &= ~kCrFloatArgs;
                call.Call<Sscanf>({value, 0x807967F0, 0x80763570});
                Print(cpu, 0x1000000, 0x80630F60, {uint32_t(length), 0, Read(0x80763570), Read(0x80763574)});
                Write(kStoredUserId + 4, Read(0x80763574));
                Write(kStoredUserId, Read(0x80763570));
            } else if (Field(cpu, token, 0x80630F74, value, length)) { // svchost=
                call.Call<Strcpy>({0x80763594, value});
                Print(cpu, 0x1000000, 0x80630F80, {uint32_t(length), value});
            } else if (Field(cpu, token, 0x80630F94, value, length)) { // servicetoken=
                call.Call<Strcpy>({0x807635D5, value});
                Print(cpu, 0x1000000, 0x80630FA4, {uint32_t(length), value});
            } else if (Field(cpu, token, 0x80630FBC, value, length)) { // statusdata=
                Write(kSvlData, Memory::Read8(value) == 'Y');
                Print(cpu, 0x1000000, 0x80630FC8, {uint32_t(length), value});
            } else if (Field(cpu, token, 0x80630FE0, value, length)) { // prwords=
                call.Call<Strcpy>({0x80763740, value});
            } else {
                Print(cpu, 0x1000000, 0x80630FEC, {token});
            }
        }
    }

    if (!hasReturnCode) {
        Print(cpu, 0x1000000, 0x80631098); // "no return code"
        Write(kAuthResult, uint32_t(-20101));
        return;
    }

    const bool words = Read(Work() + kWorkMode) == 3;
    const int32_t code = int32_t(call.Call<Strtol>({returnCode, 0, 10}));

    if (code == 0 && !words) {
        Print(cpu, 0x1000000, 0x80631004, {returnCode});
        Write(kAuthResult, uint32_t(-20101));
    } else if (code == 1 && words) {
        Print(cpu, 0x1000000, 0x80631020, {returnCode});
        Write(kAuthResult, uint32_t(-33001));
    } else {
        if (code > 99) {
            if (!words || code != 100) {
                Print(cpu, 0x1000000, 0x8063107C, {uint32_t(code)});
                Write(kAuthResult, uint32_t(-20000 - code));
                return;
            }

            Print(cpu, 0x1000000, 0x80631040, {100});
        }

        Write(kRetryCode, code == 40 ? 2 : 1);
        Write(kAuthResult, 1);
    }
}

// 0x80509800: whether no login runs; a finished one (0x19) is collected.
uint32_t AuthIdle() {
    const uint32_t state = Read(kAuthState);

    if (state == 0x19) {
        Write(kAuthState, 0x1A);
        return 1;
    }

    return state == 0 || state == 0x1A;
}

// 0x8050B050: the service locator's step (DWC_SvlProcess): 2 running, 3 done
// (the answer copied to the caller's buffer), 4 failed.
uint32_t SvlProcess(CpuContext* cpu) {
    if (!AuthIdle()) {
        AuthProcess(cpu);
        return 2;
    }

    if (Read(kAuthResult) != 1) {
        int32_t code = int32_t(Read(kAuthResult));

        if (uint32_t(code + 20110) < 11)
            code -= 4000;
        else if (uint32_t(code + 20999) < 0x379)
            code -= 4000;
        else if (uint32_t(code + 23999) < 1000)
            code -= 2000;
        else
            Print(cpu, 0x1000000, 0x806310E8, {uint32_t(code)});

        SetError(0xF, uint32_t(code));
        return 4;
    }

    if (const uint32_t out = Read(kSvlResult))
        std::memmove(Memory::GetPointer(out, 0x174), Memory::GetPointer(kSvlData, 0x174), 0x174);

    if (Length(Read(kSvlResult) + 0x45) == 0) {
        SetError(0xF, uint32_t(-24101));
        return 4;
    }

    return 3;
}

// 0x80508DD0: the blocks on the free list (linked at +0x18), freed.
void FreeList(CpuContext* cpu) {
    for (uint32_t block = Read(kFreeList); block;) {
        const uint32_t next = Read(block + 0x18);
        Free(cpu, 12, block, 0);
        block = next;
        Write(kFreeList, block);
    }
}

uint32_t Arg(CpuContext* cpu, int n) {
    return cpu->gpr[n];
}

} // namespace

extern "C" void MH3_DWCBase64Encode(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(Base64Encode(Arg(cpu, 3), Arg(cpu, 4), Arg(cpu, 5), Arg(cpu, 6)));
}
REGISTER_NATIVE_FUNCTION_AS(0x805076D0, MH3_DWCBase64Encode, "DWC_Base64Encode");

extern "C" void MH3_DWCBase64Decode(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(Base64Decode(Arg(cpu, 3), Arg(cpu, 4), Arg(cpu, 5), Arg(cpu, 6)));
}
REGISTER_NATIVE_FUNCTION_AS(0x80507860, MH3_DWCBase64Decode, "DWC_Base64Decode");

extern "C" void MH3_DWCSetError(CpuContext* cpu) {
    SetError(Arg(cpu, 3), Arg(cpu, 4));
}
REGISTER_NATIVE_FUNCTION_AS(0x80507B10, MH3_DWCSetError, "DWCi_SetError");

// DWC_Init(environment, game name, authentication server, alloc, free).
extern "C" void MH3_DWCInit(CpuContext* cpu) {
    const uint32_t environment = Arg(cpu, 3), gameName = Arg(cpu, 4), server = Arg(cpu, 5);
    GuestCall call{cpu};
    Write(kAllocFunction, Arg(cpu, 6));
    Write(kFreeFunction, Arg(cpu, 7));
    call.Call<OSRegisterVersion>({0x80630628});
    AuthInit(cpu, environment);
    Write(kAuthServer, server);
    call.Call<GsiMemoryCallbacksSet>({GsMalloc, GsFree, GsRealloc, GsMemalign});
    call.Call<Strcpy>({kGameName, gameName});
    call.Call<Strcpy>({kStatsHost, environment == 0 ? 0x80630698u : 0x806306ACu});
    Write(kInitialized, 1);
    ConsoleFriendCode(cpu);
    cpu->gpr[3] = 0;
}
REGISTER_NATIVE_FUNCTION_AS(0x80507B30, MH3_DWCInit, "DWC_Init");

extern "C" void MH3_DWCShutdown(CpuContext* cpu) {
    GuestCall call{cpu};
    call.Call<GsiMemoryCallbacksSet>({GsMalloc, GsFree, GsRealloc, GsMemalign});
    call.Call<GsiGetHostByName>({0x807966C0}); // "clear": empties the name cache
    FreeList(cpu);
    Write(kInitialized, 0);
}
REGISTER_NATIVE_FUNCTION_AS(0x80507C30, MH3_DWCShutdown, "DWC_Shutdown");

extern "C" void MH3_DWCSetMemFunc(CpuContext* cpu) {
    Write(kAllocFunction, Arg(cpu, 3));
    Write(kFreeFunction, Arg(cpu, 4));
}
REGISTER_NATIVE_FUNCTION_AS(0x80507C90, MH3_DWCSetMemFunc, "DWC_SetMemFunc");

extern "C" void MH3_DWCAlloc(CpuContext* cpu) {
    cpu->gpr[3] = Alloc(cpu, Arg(cpu, 3), Arg(cpu, 4), 0x20);
}
REGISTER_NATIVE_FUNCTION_AS(0x80507CA0, MH3_DWCAlloc, "DWC_Alloc");

extern "C" void MH3_DWCAllocEx(CpuContext* cpu) {
    cpu->gpr[3] = Alloc(cpu, Arg(cpu, 3), Arg(cpu, 4), Arg(cpu, 5));
}
REGISTER_NATIVE_FUNCTION_AS(0x80507D10, MH3_DWCAllocEx, "DWC_AllocEx");

extern "C" void MH3_DWCFree(CpuContext* cpu) {
    Free(cpu, Arg(cpu, 3), Arg(cpu, 4), Arg(cpu, 5));
}
REGISTER_NATIVE_FUNCTION_AS(0x80507D70, MH3_DWCFree, "DWC_Free");

// GameSpy's allocator (gsiMemoryCallbacksSet), kind 9.
extern "C" void MH3_DWCGsMalloc(CpuContext* cpu) {
    cpu->gpr[3] = Alloc(cpu, 9, Arg(cpu, 3), 0x20);
}
REGISTER_NATIVE_FUNCTION_AS(0x80507D90, MH3_DWCGsMalloc, "DWCi_GsMalloc");

extern "C" void MH3_DWCGsRealloc(CpuContext* cpu) {
    const uint32_t old = Arg(cpu, 3), size = Arg(cpu, 4);
    const uint32_t block = Alloc(cpu, 9, size, 0x20);

    if (block && old) {
        const uint32_t oldSize = Read(old - 0x1C);
        const uint32_t copy = size < oldSize ? size : oldSize;
        std::memmove(Memory::GetPointer(block, copy), Memory::GetPointer(old, copy), copy);
        Free(cpu, 9, old, oldSize);
    }

    cpu->gpr[3] = block;
}
REGISTER_NATIVE_FUNCTION_AS(0x80507E00, MH3_DWCGsRealloc, "DWCi_GsRealloc");

extern "C" void MH3_DWCGsFree(CpuContext* cpu) {
    Free(cpu, 9, Arg(cpu, 3), 0);
}
REGISTER_NATIVE_FUNCTION_AS(0x80507ED0, MH3_DWCGsFree, "DWCi_GsFree");

extern "C" void MH3_DWCGsMemalign(CpuContext* cpu) {
    cpu->gpr[3] = Alloc(cpu, 9, Arg(cpu, 4), Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x80507F00, MH3_DWCGsMemalign, "DWCi_GsMemalign");

// DWC_Printf(level, format, ...) from translated code: the arguments where
// the variadic call left them.
extern "C" void MH3_DWCPrintf(CpuContext* cpu) {
    uint32_t gprs[8];

    for (int i = 0; i < 8; ++i)
        gprs[i] = cpu->gpr[3 + i];

    double fprs[8];

    for (int i = 0; i < 8; ++i)
        fprs[i] = cpu->fpr[1 + i].d;

    PrintWith(cpu, gprs, (cpu->cr & kCrFloatArgs) ? fprs : nullptr, cpu->gpr[1] + 8);
}
REGISTER_NATIVE_FUNCTION_AS(0x80507F70, MH3_DWCPrintf, "DWC_Printf");

// DWC's memcpy.
extern "C" void MH3_DWCCopy(CpuContext* cpu) {
    const uint32_t to = Arg(cpu, 3), from = Arg(cpu, 4), bytes = Arg(cpu, 5);

    if (bytes && to != from)
        std::memmove(Memory::GetPointer(to, bytes), Memory::GetPointer(from, bytes), bytes);
}
REGISTER_NATIVE_FUNCTION_AS(0x805082C0, MH3_DWCCopy, "DWCi_Copy");

extern "C" void MH3_DWCGetConsoleFriendCode(CpuContext* cpu) {
    const uint64_t code = ConsoleFriendCode(cpu);
    cpu->gpr[3] = uint32_t(code >> 32);
    cpu->gpr[4] = uint32_t(code);
}
REGISTER_NATIVE_FUNCTION_AS(0x80508CB0, MH3_DWCGetConsoleFriendCode, "DWC_GetConsoleFriendCode");

extern "C" void MH3_DWCFreeList(CpuContext* cpu) {
    FreeList(cpu);
    cpu->gpr[3] = 0;
}
REGISTER_NATIVE_FUNCTION_AS(0x80508DD0, MH3_DWCFreeList, "DWCi_FreeList");

extern "C" void MH3_DWCAuthInit(CpuContext* cpu) {
    AuthInit(cpu, Arg(cpu, 3));
    cpu->gpr[3] = 1;
}
REGISTER_NATIVE_FUNCTION_AS(0x80508E20, MH3_DWCAuthInit, "DWCi_Auth_Init");

extern "C" void MH3_DWCAuthStart(CpuContext* cpu) {
    cpu->gpr[3] = AuthStart(cpu, Arg(cpu, 3), Arg(cpu, 4), Arg(cpu, 5), Arg(cpu, 6), Arg(cpu, 7), Arg(cpu, 8));
}
REGISTER_NATIVE_FUNCTION_AS(0x80508EB0, MH3_DWCAuthStart, "DWCi_Auth_Start");

// The service locator (svcloc) for `svc`, with the given allocator.
extern "C" void MH3_DWCSvlStart(CpuContext* cpu) {
    const uint32_t svc = Arg(cpu, 3), alloc = Arg(cpu, 4), free = Arg(cpu, 5);
    AuthStart(cpu, 0x807966CC, 0x807966D0, 0, 0, alloc, free);
    GuestCall call{cpu};
    call.Call<Memset>({kSvlData, 0, 0x174});
    call.Call<Strncpy>({Work() + kWorkSvc, svc, 5});
    Write(Work() + kWorkMode, 2);
    cpu->gpr[3] = 1;
}
REGISTER_NATIVE_FUNCTION_AS(0x80509070, MH3_DWCSvlStart, "DWCi_Svl_Start");

extern "C" void MH3_DWCAuthProcess(CpuContext* cpu) {
    AuthProcess(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x805090F0, MH3_DWCAuthProcess, "DWCi_Auth_Process");

extern "C" void MH3_DWCAuthIdle(CpuContext* cpu) {
    cpu->gpr[3] = AuthIdle();
}
REGISTER_NATIVE_FUNCTION_AS(0x80509800, MH3_DWCAuthIdle, "DWCi_Auth_IsIdle");

extern "C" void MH3_DWCAuthSucceeded(CpuContext* cpu) {
    cpu->gpr[3] = Read(kAuthResult) == 1;
}
REGISTER_NATIVE_FUNCTION_AS(0x80509850, MH3_DWCAuthSucceeded, "DWCi_Auth_Succeeded");

extern "C" void MH3_DWCAuthResult(CpuContext* cpu) {
    cpu->gpr[3] = Read(kAuthResult);
}
REGISTER_NATIVE_FUNCTION_AS(0x80509870, MH3_DWCAuthResult, "DWCi_Auth_GetResult");

extern "C" void MH3_DWCSvlData(CpuContext* cpu) {
    cpu->gpr[3] = kSvlData;
}
REGISTER_NATIVE_FUNCTION_AS(0x80509880, MH3_DWCSvlData, "DWCi_Svl_GetResult");

// NHTTP's allocator: the login work's, kind 13 (r5 keeps the work, as the
// original leaves it there).
extern "C" void MH3_DWCNhttpAlloc(CpuContext* cpu) {
    const uint32_t size = Arg(cpu, 3);
    cpu->gpr[3] = GuestCall{cpu}(Read(Work() + kWorkAlloc), {13, size, Work()});
}
REGISTER_NATIVE_FUNCTION_AS(0x80509890, MH3_DWCNhttpAlloc, "DWCi_NhttpAlloc");

extern "C" void MH3_DWCNhttpFree(CpuContext* cpu) {
    const uint32_t block = Arg(cpu, 3);
    GuestCall{cpu}(Read(Work() + kWorkFree), {13, block, 0, Work()});
}
REGISTER_NATIVE_FUNCTION_AS(0x805098B0, MH3_DWCNhttpFree, "DWCi_NhttpFree");

// NAND's callback: (result, block).
extern "C" void MH3_DWCNandDone(CpuContext* cpu) {
    Write(Work() + kWorkNandDone, 1);
    Write(Work() + kWorkNandResult, Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x805098D0, MH3_DWCNandDone, "DWCi_NandDone");

extern "C" void MH3_DWCAuthRequest(CpuContext* cpu) {
    SendAuthRequest(cpu, Arg(cpu, 3), Arg(cpu, 4), Arg(cpu, 5), Arg(cpu, 6), Arg(cpu, 7), Arg(cpu, 8));
}
REGISTER_NATIVE_FUNCTION_AS(0x805098F0, MH3_DWCAuthRequest, "DWCi_Auth_SendRequest");

extern "C" void MH3_DWCAuthAnswer(CpuContext* cpu) {
    AuthAnswer(cpu, Arg(cpu, 3), Arg(cpu, 4));
}
REGISTER_NATIVE_FUNCTION_AS(0x8050A430, MH3_DWCAuthAnswer, "DWCi_Auth_Answer");

extern "C" void MH3_DWCAuthEnd(CpuContext* cpu) {
    EndAuth(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050AD90, MH3_DWCAuthEnd, "DWCi_Auth_EndProcess");

extern "C" void MH3_DWCNandStepDone(CpuContext* cpu) {
    NandStepDone(cpu, Arg(cpu, 3), Arg(cpu, 4), Arg(cpu, 5), Arg(cpu, 6));
}
REGISTER_NATIVE_FUNCTION_AS(0x8050ADF0, MH3_DWCNandStepDone, "DWCi_Auth_NandStepDone");

// The login before the online menus (DWC_LoginAsync's account check).
extern "C" void MH3_DWCCheckStart(CpuContext* cpu) {
    Write(kSvlState, 4);
    AuthStart(cpu, 0x807967F8, 0x807967FC, 0, 0, DwcAlloc, DwcFree);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050AF20, MH3_DWCCheckStart, "DWC_CheckStart");

// Its step: 2 running, 3 done, 4 failed (the error set), 5 canceled.
extern "C" void MH3_DWCCheckProcess(CpuContext* cpu) {
    const uint32_t state = Read(kSvlState);
    uint32_t result;

    if (state == 1) {
        result = 3;
    } else if (state == 2) {
        result = 5;
    } else if (state == 3) {
        result = 4;
    } else if (state == 4) {
        AuthProcess(cpu);

        if (!AuthIdle()) {
            result = 2;
        } else if (Read(kAuthResult) != 1) {
            Write(kSvlState, 3);
            SetError(2, Read(kAuthResult));
            result = 4;
        } else {
            Write(kSvlState, 1);
            result = 3;
        }
    } else {
        result = 4;
    }

    cpu->gpr[3] = result;
}
REGISTER_NATIVE_FUNCTION_AS(0x8050AF50, MH3_DWCCheckProcess, "DWC_CheckProcess");

extern "C" void MH3_DWCSvlReset(CpuContext* cpu) {
    Write(kSvlResult, 0);
    cpu->gpr[3] = 1;
}
REGISTER_NATIVE_FUNCTION_AS(0x8050B010, MH3_DWCSvlReset, "DWC_SvlInit");

extern "C" void MH3_DWCSvlCleanup(CpuContext*) {
    Write(kSvlResult, 0);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050B020, MH3_DWCSvlCleanup, "DWC_SvlCleanup");

// DWC_SvlGetTokenAsync(svc, result).
extern "C" void MH3_DWCSvlGetToken(CpuContext* cpu) {
    const uint32_t svc = Arg(cpu, 3);
    Write(kSvlResult, Arg(cpu, 4));
    cpu->gpr[3] = svc;
    cpu->gpr[4] = DwcAlloc;
    cpu->gpr[5] = DwcFree;
    MH3_DWCSvlStart(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050B030, MH3_DWCSvlGetToken, "DWC_SvlGetTokenAsync");

extern "C" void MH3_DWCSvlProcess(CpuContext* cpu) {
    cpu->gpr[3] = SvlProcess(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x8050B050, MH3_DWCSvlProcess, "DWC_SvlProcess");
