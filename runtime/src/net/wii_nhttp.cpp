// NHTTP, the SDK's HTTP client (RMHP08 0x80514C38..0x8051BE1C; decompiled
// with Ghidra, the call arguments checked in the translated C++), ported
// natively. DWC's account server login is its one user. A request (URL,
// method, header fields, post data) goes on a queue that NHTTP's worker
// thread takes one at a time: it resolves the host, connects (through a
// proxy's CONNECT and SSL when asked), sends the request line, the header
// fields and the post data (URL-encoded, multipart or raw), reads the header
// byte by byte and receives the body (by length, chunked or until the server
// closes) into the caller's buffer, then calls back. The socket stays open
// between requests to the same host and port while the server keeps the
// connection alive. SO, SSL, NCD, OS and the guest's memcpy and memset stay
// guest calls.
#include "abi_bridge.h"
#include "guest_call.h"
#include "memory.h"

#include <initializer_list>
#include <utility>

namespace {

enum : uint32_t {
    Memcpy = 0x80004000,
    Memset = 0x80004350,
    DebugPrint = 0x8045E204,
    OSRegisterVersion = 0x804CBA00,
    OSReport = 0x804CDCA0,
    OSPanic = 0x804CDD30,
    OSInitMessageQueue = 0x804D1AE0,
    OSSendMessage = 0x804D1B40,
    OSReceiveMessage = 0x804D1C10,
    OSInitMutex = 0x804D2560,
    OSLockMutex = 0x804D25A0,
    OSUnlockMutex = 0x804D2680,
    OSInitCond = 0x804D27C0,
    OSSignalCond = 0x804D27D0,
    OSCreateThread = 0x804D45F0,
    OSJoinThread = 0x804D4B30,
    OSResumeThread = 0x804D4C80,
    SSLNew = 0x8051BE7C,
    SSLConnect = 0x8051BFD4,
    SSLDoHandshake = 0x8051C09C,
    SSLRead = 0x8051C148,
    SSLWrite = 0x8051C418,
    SSLShutdown = 0x8051C6D8,
    SSLSetClientCert = 0x8051C784,
    SSLSetRootCA = 0x8051C8F0,
    SSLSetBuiltinRootCA = 0x8051CA38,
    SSLSetBuiltinClientCert = 0x8051CB00,
    NCDGetCurrentIpConfig = 0x8051CCCC,
    SOSocket = 0x8051FF7C,
    SOClose = 0x80520140,
    SOConnect = 0x805204C0,
    SORecv = 0x805207C8,
    SOSend = 0x80520814,
    SOShutdown = 0x80520968,
    SOGetAddrInfo = 0x8052138C,
    SOFreeAddrInfo = 0x80521670,
    SOSetSockOpt = 0x805216D4,
};

// The ports' own addresses, handed to the guest.
enum : uint32_t {
    kAllocAddress = 0x80514C68,
    kFreeAddress = 0x80514CC4,
    kWorkerEntry = 0x80515728,
    kDispatchAddress = 0x8051AC70,
};

constexpr uint32_t kGlobals = 0x80765120;

enum : uint32_t {
    kGlobalsPtr = 0x80797D44,
    kConnections = 0x80797D40, // the connection list's head
    kStartupVersionDone = 0x80797D38,
    kStartupVersion = 0x807968D8,
    kCreateVersionDone = 0x80797D3C,
    kCreateVersion = 0x807968DC,
    kWakeReady = 0x807650E0,
    kWakeMutex = 0x807650E4,
    kWakeCond = 0x807650FC,
    kBoundary = 0x805754B0,    // "--" and 18 characters
    kDivisors = 0x805754C8,    // 1000000000 .. 10
    kBase64 = 0x806313F8,
    kCurrentThread = 0x800000E4,
};

// Text the requests are made of.
enum : uint32_t {
    kHttpScheme = 0x80796870,       // "http://"
    kHttpsScheme = 0x806313E8,      // "https://"
    kGet = 0x807968A8,              // "GET "
    kPost = 0x807968B0,             // "POST "
    kHead = 0x807968B8,             // "HEAD "
    kSlash = 0x807968C0,
    kRequestEnd = 0x8063144C,       // " HTTP/1.1\r\n"
    kHostField = 0x8079687C,        // "Host: "
    kCrLf = 0x80796884,
    kColon = 0x80796878,
    kColonSpace = 0x80796890,
    kConnectMethod = 0x80631440,    // "CONNECT "
    kConnectFields = 0x80631458,    // Content-Length 0 and Pragma no-cache
    kProxyAuthField = 0x80631480,   // "Proxy-Authorization: Basic "
    kAuthField = 0x806314B0,        // "Authorization: Basic "
    kContentLengthField = 0x8063149C,
    kFormType = 0x80575564,         // URL-encoded post data's Content-Type
    kMultipartType = 0x80575598,    // multipart's, up to the boundary
    kDisposition = 0x805754F0,      // a part's Content-Disposition, up to its name
    kNameEnd = 0x80796894,
    kOctetStream = 0x80575518,      // a binary part's Content-Type
    kBoundaryEnd = 0x80796898,      // "--\r\n"
    kEquals = 0x807968A0,
    kAmpersand = 0x807968A4,
    kUserSeparator = 0x807968E0,    // ":"
    kStatusStart = 0x80796888,      // "HTTP/"
    kHttp11 = 0x806314E4,           // "HTTP/1.1"
    kContentLength = 0x806314C8,
    kConnection = 0x806314D8,
    kKeepAlive = 0x806314F0,
    kClose = 0x807968C4,
    kTransferEncoding = 0x806314FC,
    kChunked = 0x807968D0,
};

// Diagnostics.
enum : uint32_t {
    kMsgIpConfig = 0x806312C8,
    kFileBgnEnd = 0x806312E8,
    kIpConfigName = 0x806312F8,
    kMsgConnectionsLeft = 0x80631310,
    kMsgNoMemory = 0x80631350,
    kMsgRawPostSet = 0x8063136C,
    kCheckThreadName = 0x806313A8,
    kMsgIllegalThread = 0x806313C4,
    kFileOs = 0x806313D8,
    kIllegalThread = 0x80796868,
    kAddHeaderFieldName = 0x80631608,
    kMsgAfterStart = 0x80631620,
    kAddPostDataAsciiName = 0x80631650,
    kMsgProxyTooLong = 0x80631708,
    kMsgUserTooLong = 0x80631730,
    kMsgPasswordTooLong = 0x80631754,
    kNoAuth = 0x80631778,
    kMsgUsingProxy = 0x80631784,
    kMsgSetProxyFailed = 0x806317A8,
    kFileNhttp = 0x806317C4,
    kSetProxyName = 0x806317D0,
    kMsgConnectionsReset = 0x80631808,
};

// The globals' fields: the interface config NCD answers (its proxies for
// http and https first), the allocator, the worker's socket, thread and
// stack, the request queue and the mutex over the queue and connections.
enum : uint32_t {
    gHttpProxy = 0x28,
    gHttpsProxy = 0x170,
    gAlloc = 0x7C4,
    gFree = 0x7C8,
    gStarted = 0x7CC,
    gSocket = 0x7D0,
    gSslError = 0x7D4,
    gError = 0x7D8,
    gQuit = 0x7DC,
    gStack = 0x7E0,
    gSslHook = 0x7E4,
    gQueue = 0x800,   // head, next ID
    gCurrent = 0x808, // the queue entry being worked on
    gMutex = 0x80C,   // an OSMutex, then its "initialized" word at +0x18
    gWorker = 0x840,
};

// A proxy setting: enabled, authenticated, then server, port, user, password.
enum : uint32_t {
    xEnabled = 0x0,
    xAuth = 0x1,
    xServer = 0x4,
    xPort = 0x104,
    xUser = 0x106,
    xPassword = 0x127,
};

// The worker: its message queue (3 messages), thread, the queue's
// "initialized" word and the 0x100-byte send buffer.
enum : uint32_t {
    wMessages = 0x20,
    wThread = 0x30,
    wQueueReady = 0x348,
    wSendBuffer = 0x360,
};

// A request (600 bytes).
enum : uint32_t {
    rCancel = 0x0,
    rStarted = 0x4,
    rHttps = 0x8,
    rProxy = 0xC,
    rRawPost = 0x10,
    rHostEnd = 0x14,   // in the URL
    rPathStart = 0x18,
    rMethod = 0x1C,    // 0 GET, 1 POST, 2 HEAD
    rPort = 0x20,
    rUrl = 0x24,       // decoded
    rHost = 0x28,
    rResponse = 0x2C,
    rHeaders = 0x30,
    rPosts = 0x34,
    rBoundary = 0x38,  // "--" and 18 characters
    rAuth = 0x4C,
    rAuthLength = 0xA8,
    rSsl = 0xAC,
    rClientCert = 0xB0,
    rClientCertLength = 0xB4,
    rClientKey = 0xB8,
    rClientKeyLength = 0xBC,
    rRootCa = 0xC0,
    rRootCaLength = 0xC4,
    rBuiltinClientCert = 0xC8,
    rVerify = 0xCC,
    rPostType = 0xD0,  // 0 by the parts, 2 multipart
    rSslHookArg = 0xD4,
    rRootCaId = 0xD8,
    rClientCertId = 0xDC,
    rProxyAddress = 0xE0,
    rProxyPort = 0x1E0,
    rProxyAuth = 0x1E4,
    rProxyAuthLength = 0x240,
    rRecvBuffer = 0x244,
    rSendBuffer = 0x248,
    rRawData = 0x24C,
    rRawLength = 0x250,
    rPostCallback = 0x254,
};

// A response (0x43C bytes): the header in 0x400 bytes, then in 0x200-byte
// chunks linked by their first word.
enum : uint32_t {
    pHeaderLength = 0x0,
    pReceived = 0x4,
    pTotal = 0x8,
    pContentLength = 0xC,
    pDone = 0x10,
    pHeaderDone = 0x14,
    pStatus = 0x18,
    pBufferSize = 0x1C,
    pSpare1 = 0x20,
    pSpare2 = 0x24,
    pBuffer = 0x28,
    pGrow = 0x2C,
    pRelease = 0x30,
    pChunks = 0x34,
    pInline = 0x38,
    pParam = 0x438,
};

// A connection (0x8060 bytes): the request's handle on the list, with the
// worker's receive buffer.
enum : uint32_t {
    cState = 0x0,
    cError = 0x4,
    cSslError = 0x8,
    cBusy = 0xC,
    cRequest = 0x10,
    cResponse = 0x14,
    cId = 0x18,
    cCallback = 0x1C,
    cNext = 0x20,
    cPostData = 0x24,
    cPostLength = 0x28,
    cProgress = 0x2C,
    cUserCallback = 0x30,
    cBuffer = 0x40,
    cBufferAt = 0x8040,
    cBufferCount = 0x8044,
};

// A queue entry (0x14 bytes) and a header field or post part (0x18), both
// on circular lists: previous, next, ...
enum : uint32_t {
    qId = 0x8,
    qRequest = 0xC,
    qSocket = 0x10,
    fLabel = 0x8,
    fValue = 0xC,
    fLength = 0x10,
    fBinary = 0x14,
};

// The worker's state, on its stack (0x358 bytes).
enum : uint32_t {
    sId = 0x0,
    sHost = 0x4,
    sDiscard = 0x104, // 0x200 bytes the body goes to when the caller's is full
    sStatusLine = 0x304,
    sAddress = 0x314,
    sLastAddress = 0x318,
    sPort = 0x31C,
    sLastPort = 0x320,
    sFill = 0x324,
    sHeaderLength = 0x328,
    sContentLength = 0x32C,
    sError = 0x330,
    sResume = 0x334,
    sKeepAlive = 0x338,
    sChunked = 0x33C,
    kStateSize = 0x358,
};

// Connections()'s searches.
enum : int32_t {
    kByConnection = 0,
    kByRequest = 1,
    kByResponse = 2,
    kPush = 3,
    kRemove = 4,
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

int8_t Char(uint32_t address) {
    return int8_t(Memory::Read8(address));
}

uint32_t StrLen(uint32_t text) {
    uint32_t length = 0;

    while (Memory::Read8(text + length))
        ++length;

    return length;
}

int32_t Lower(int32_t c) {
    return c > 0x40 && c < 0x5B ? c + 0x20 : c;
}

bool IsDigit(int32_t c) {
    return c >= '0' && c <= '9';
}

bool IsAlnum(int32_t c) {
    return IsDigit(c) || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

void Copy(CpuContext* cpu, uint32_t to, uint32_t from, uint32_t length) {
    GuestCall{cpu}.Call<Memcpy>({to, from, length});
}

void Clear(CpuContext* cpu, uint32_t to, uint32_t length) {
    GuestCall{cpu}.Call<Memset>({to, 0, length});
}

void Lock(CpuContext* cpu, uint32_t mutex) {
    GuestCall{cpu}.Call<OSLockMutex>({mutex});
}

void Unlock(CpuContext* cpu, uint32_t mutex) {
    GuestCall{cpu}.Call<OSUnlockMutex>({mutex});
}

// The SDK's debug print, OSReport and OSPanic (variadic: no float arguments).
void Print(CpuContext* cpu, uint32_t format, uint32_t a = 0, uint32_t b = 0) {
    cpu->cr &= ~kCrFloatArgs;
    GuestCall{cpu}.Call<DebugPrint>({format, a, b});
}

void Report(CpuContext* cpu, uint32_t format, uint32_t a = 0, uint32_t b = 0, uint32_t c = 0, uint32_t d = 0) {
    cpu->cr &= ~kCrFloatArgs;
    GuestCall{cpu}.Call<OSReport>({format, a, b, c, d});
}

void Panic(CpuContext* cpu, uint32_t file, uint32_t line, uint32_t message) {
    cpu->cr &= ~kCrFloatArgs;
    GuestCall{cpu}.Call<OSPanic>({file, line, message});
}

// 0x80514C38: the globals' fields, cleared.
void ResetGlobals(uint32_t globals) {
    Write(globals + gSslError, 0);
    Write(globals + gError, 0);
    Write(globals + gAlloc, 0);
    Write(globals + gFree, 0);
    Write(globals + gStarted, 0);
    Write(globals + gSocket, 0xFFFFFFFF);
    Write(globals + gQuit, 0);
    Write(globals + gStack, 0);
    Write(globals + gSslHook, 0);
}

// 0x8051BD68: the globals, set up on first use.
uint32_t Globals() {
    if (!Read(kGlobalsPtr)) {
        Write(kGlobalsPtr, kGlobals);
        ResetGlobals(kGlobals);
        Write(kGlobals + gQueue, 0);
        Write(kGlobals + gQueue + 4, 0);
        Write(kGlobals + gCurrent, 0);
        Write(kGlobals + gMutex + 0x18, 0);
        Write(kGlobals + gWorker + wQueueReady, 0);
    }

    return Read(kGlobalsPtr);
}

// 0x80514C68: the allocator NHTTPStartup took (none: 0).
uint32_t Alloc(CpuContext* cpu, uint32_t size, uint32_t align) {
    const uint32_t alloc = Read(Globals() + gAlloc);
    return alloc ? GuestCall{cpu}(alloc, {size, align}) : 0;
}

// 0x80514CC4.
void Free(CpuContext* cpu, uint32_t block) {
    const uint32_t free = Read(Globals() + gFree);

    if (free)
        GuestCall{cpu}(free, {block});
}

// 0x80515534: the mutex, initialized once.
void MutexSetup(CpuContext* cpu, uint32_t mutex) {
    if (!Read(mutex + 0x18)) {
        GuestCall{cpu}.Call<OSInitMutex>({mutex});
        Write(mutex + 0x18, 1);
    }
}

// Appends `node` to the circular list at `list` (as its head's previous).
void Append(uint32_t list, uint32_t node) {
    const uint32_t head = Read(list);

    if (!head) {
        Write(node, node);
        Write(node + 4, node);
        Write(list, node);
        return;
    }

    Write(node, Read(head));
    Write(node + 4, head);
    Write(Read(head) + 4, node);
    Write(head, node);
}

// 0x805150E4: takes the head off the list at `list`.
uint32_t PopHead(uint32_t list) {
    const uint32_t head = Read(list);

    if (head) {
        if (head == Read(head)) {
            Write(list, 0);
        } else {
            Write(Read(head) + 4, Read(head + 4));
            Write(Read(head + 4), Read(head));
            Write(list, Read(head + 4));
        }
    }

    return head;
}

// Frees a list from its head: the head's previous until the head is alone.
void FreeList(CpuContext* cpu, uint32_t head) {
    while (head) {
        const uint32_t previous = Read(head);

        if (previous == head) {
            Free(cpu, head);
            head = 0;
        } else {
            const uint32_t next = Read(previous);
            Free(cpu, previous);
            Write(head, next);
        }
    }
}

// 0x8051B758: the connection list under `mutex`: finds a connection by
// itself, its request or its response, pushes one or removes one.
uint32_t Connections(CpuContext* cpu, uint32_t mutex, uint32_t key, int32_t mode) {
    uint32_t found = 0;
    Lock(cpu, mutex);

    if (mode == kPush) {
        Write(key + cNext, Read(kConnections));
        Write(kConnections, key);
        found = key;
    } else {
        for (uint32_t link = kConnections, connection; (connection = Read(link)) != 0; link = connection + cNext) {
            if (mode == kByResponse) {
                if (Read(connection + cResponse) == key)
                    found = connection;
            } else if (mode < kByResponse) {
                if (mode == kByConnection) {
                    if (connection == key)
                        found = connection;
                } else if (mode >= 0 && Read(connection + cRequest) == key) {
                    found = connection;
                }
            } else if (mode == kRemove && connection == key) {
                Write(link, Read(connection + cNext));
                found = connection;
            }

            if (found)
                break;
        }
    }

    // As the original: the mutex given, not the caller's (always the same).
    Unlock(cpu, mutex);
    return found;
}

// 0x8051B8C8 / 0x8051B900: the connection's request or response.
uint32_t RequestOf(CpuContext* cpu, uint32_t mutex, uint32_t connection) {
    const uint32_t found = Connections(cpu, mutex, connection, kByConnection);
    return found ? Read(found + cRequest) : 0;
}

uint32_t ResponseOf(CpuContext* cpu, uint32_t mutex, uint32_t connection) {
    const uint32_t found = Connections(cpu, mutex, connection, kByConnection);
    return found ? Read(found + cResponse) : 0;
}

// 0x8051B950 / 0x8051B994: a connection's request or response, else the
// handle itself (a request or response).
uint32_t RequestOrSelf(CpuContext* cpu, uint32_t mutex, uint32_t handle) {
    const uint32_t found = Connections(cpu, mutex, handle, kByConnection);
    return found ? Read(found + cRequest) : handle;
}

uint32_t ResponseOrSelf(CpuContext* cpu, uint32_t mutex, uint32_t handle) {
    const uint32_t found = Connections(cpu, mutex, handle, kByConnection);
    return found ? Read(found + cResponse) : handle;
}

// 0x8051B9D8.
uint32_t ConnectionCount() {
    uint32_t count = 0;

    for (uint32_t connection = Read(kConnections); connection; connection = Read(connection + cNext))
        ++count;

    return count;
}

// 0x8051BDF4 / 0x8051BE04: the body's expected length, for the buffer grow
// callback.
void SetProgress(uint32_t connection, uint32_t value) {
    if (connection)
        Write(connection + cProgress, value);
}

uint32_t Progress(uint32_t connection) {
    return connection ? Read(connection + cProgress) : 0;
}

// 0x8051B9F8: asks the connection's callback (kind 1) for the next piece
// of streamed post data (`label`'s, at `offset`).
int32_t PostCallback(CpuContext* cpu, uint32_t mutex, uint32_t connection, uint32_t label, uint32_t offset) {
    int32_t answer = -1;

    if (!Connections(cpu, mutex, connection, kByConnection))
        return answer;

    const uint32_t found = Connections(cpu, mutex, connection, kByConnection);
    const uint32_t response = found ? Read(found + cResponse) : 0;

    if (response && Read(connection + cCallback)) {
        GuestFrame frame(cpu, 0x30);
        const uint32_t args = frame[0x10];
        Write(args, label);
        Write(args + 4, Read(connection + cPostData));
        Write(args + 8, Read(connection + cPostLength));
        Write(args + 12, offset);
        answer = int32_t(GuestCall{cpu}(Read(connection + cCallback), {connection, 1, args}));
        const uint32_t data = Read(args + 4), length = Read(args + 8);
        const uint32_t again = Connections(cpu, mutex, connection, kByConnection);

        if (again) {
            Write(again + cPostData, data);
            Write(again + cPostLength, length);
        }
    }

    return answer;
}

// 0x8051BAE0 (kind 2: grow the buffer) / 0x8051BBF8 (kind 3: the body is
// complete): the response's buffer, its size and the received length,
// through the connection's callback.
void BufferCallback(CpuContext* cpu, uint32_t mutex, uint32_t connection, uint32_t kind) {
    if (!Connections(cpu, mutex, connection, kByConnection))
        return;

    const uint32_t found = Connections(cpu, mutex, connection, kByConnection);
    const uint32_t response = found ? Read(found + cResponse) : 0;

    if (!response || !Read(connection + cCallback))
        return;

    GuestFrame frame(cpu, 0x30);
    const uint32_t args = frame[0x10];
    Write(args, Read(response + pBuffer));
    Write(args + 4, Read(response + pBufferSize));
    Write(args + 8, Read(response + pReceived));
    GuestCall{cpu}(Read(connection + cCallback), {connection, kind, args});
    const uint32_t buffer = Read(args), size = Read(args + 4), received = Read(args + 8);
    const uint32_t again = Connections(cpu, mutex, connection, kByConnection);

    if (again) {
        const uint32_t still = Connections(cpu, mutex, again, kByConnection);
        const uint32_t now = still ? Read(still + cResponse) : 0;

        if (now) {
            Write(now + pBuffer, buffer);
            Write(now + pBufferSize, size);
            Write(now + pReceived, received);
        }
    }
}

// 0x8051BD10: the request is over (kind 4).
void Complete(CpuContext* cpu, uint32_t mutex, uint32_t connection) {
    if (Connections(cpu, mutex, connection, kByConnection) && Read(connection + cCallback))
        GuestCall{cpu}(Read(connection + cCallback), {connection, 4, 0});
}

// 0x8051B6DC: the connection is no longer busy; signals its waiters.
void Wakeup(CpuContext* cpu, uint32_t connection) {
    GuestCall call{cpu};

    if (!Read(kWakeReady)) {
        call.Call<OSInitMutex>({kWakeMutex});
        call.Call<OSInitCond>({kWakeCond});
        Write(kWakeReady, 1);
    }

    call.Call<OSLockMutex>({kWakeMutex});
    Write(connection + cBusy, 0);
    call.Call<OSSignalCond>({kWakeCond});
    call.Call<OSUnlockMutex>({kWakeMutex});
}

// 0x8051BE1C: forgets the connections (warning when some are left).
void ResetConnections(CpuContext* cpu) {
    if (Read(kConnections)) {
        const uint32_t count = ConnectionCount();

        if (count)
            Print(cpu, kMsgConnectionsReset, count);
    }

    Write(kConnections, 0);
}

// 0x805178C8: compares a token case-insensitively up to `a`'s end or space.
int32_t TokenCompare(uint32_t a, uint32_t b) {
    for (;; ++a, ++b) {
        const int32_t cb = Lower(Char(b)), ca = Char(a);

        if (Lower(ca) != cb)
            return -1;

        if (ca == 0 || ca == ' ')
            return 0;
    }
}

// 0x80517338: compares `n` characters case-insensitively; 0 when equal (or
// both end), else the count left at the difference.
int32_t CaseCompare(uint32_t a, uint32_t b, int32_t n) {
    for (int32_t left = n; left > 0; --left, --n, ++a, ++b) {
        const int32_t ca = Char(a), cb = Char(b);

        if (ca == 0 && cb == 0)
            return 0;

        if (Lower(ca) != Lower(cb))
            return n;
    }

    return n;
}

// 0x80517404 / 0x80517474: a string's URL-encoded length.
int32_t EncodedLength(uint32_t text) {
    int32_t length = 0;

    for (int32_t c; (c = Char(text)) != 0; ++text)
        length += IsAlnum(c) || c == ' ' ? 1 : 3;

    return length;
}

int32_t EncodedLength(uint32_t text, int32_t n) {
    int32_t length = 0;

    for (; n > 0; --n, ++text) {
        const int32_t c = Char(text);
        length += IsAlnum(c) || c == ' ' ? 1 : 3;
    }

    return length;
}

// 0x805174E8: one character URL-encoded ('+' for space, %XX).
uint32_t EncodeChar(uint32_t out, uint32_t value) {
    const int8_t c = int8_t(value);

    if (c == ' ') {
        Memory::Write8(out, '+');
        return 1;
    }

    if (!IsAlnum(c)) {
        const uint32_t high = value >> 4 & 0xF, low = value & 0xF;
        Memory::Write8(out, '%');
        Memory::Write8(out + 1, uint8_t(high < 10 ? '0' + high : '7' + high));
        Memory::Write8(out + 2, uint8_t(low < 10 ? '0' + low : '7' + low));
        return 3;
    }

    Memory::Write8(out, uint8_t(c));
    return 1;
}

// 0x80517588: up to 8 hex digits (leading spaces skipped, a space or NUL
// ends them); -1 on anything else or a full 8 that would go negative.
int32_t ParseHex(uint32_t text, int32_t n) {
    if (n > 8)
        return -1;

    const int32_t first = Char(text);
    const uint32_t x = uint32_t(first ^ 0x37);

    if (n == 8 && ((uint32_t(int32_t(x) >> 1) - (x & uint32_t(first))) >> 31) != 0)
        return -1;

    int32_t value = 0;
    bool digits = false;

    for (; n > 0; --n, ++text) {
        const int32_t c = Lower(Char(text));

        if (IsDigit(c)) {
            digits = true;
            value = c + value * 16 - 0x30;
        } else if (c >= 'a' && c <= 'f') {
            digits = true;
            value = c + value * 16 - 0x57;
        } else {
            if (digits && (c == ' ' || c == 0))
                return value;

            if (digits || c != ' ')
                return -1;
        }
    }

    return value;
}

// 0x805176AC: up to 10 decimal digits, as ParseHex; -1 on overflow.
int32_t ParseDecimal(uint32_t text, int32_t n) {
    if (n > 10)
        return -1;

    int32_t value = 0;
    bool digits = false;

    for (; n > 0; --n, ++text) {
        const int32_t c = Char(text);

        if (digits && (c == ' ' || c == 0))
            return value;

        if (digits || c != ' ') {
            if (!IsDigit(c))
                return -1;

            digits = true;
            const int32_t next = c + value * 10 - 0x30;

            if (next < value)
                return -1;

            value = next;
        }
    }

    return value;
}

// 0x8051797C: the digits among `n` characters (at most 9); -1 for none.
int32_t ParseDigits(uint32_t text, int32_t n) {
    int32_t count = 0, value = 0;

    for (; n != 0; --n, ++text) {
        const int32_t c = Char(text);

        if (c != ' ' && IsDigit(c)) {
            ++count;
            value = c + value * 10 - 0x30;

            if (count > 9)
                return -1;
        }
    }

    return count ? value : -1;
}

// 0x8051774C: `value` in decimal; answers its length.
uint32_t Decimal(uint32_t out, uint32_t value) {
    uint32_t n = 0;
    bool digits = false;

    for (uint32_t i = 0; i < 9; ++i) {
        const uint32_t divisor = Read(kDivisors + 4 * i);

        if (value < divisor) {
            if (digits)
                Memory::Write8(out + n++, '0');
        } else {
            digits = true;
            Memory::Write8(out + n++, uint8_t(value / divisor + '0'));
            value -= value / divisor * divisor;
        }
    }

    Memory::Write8(out + n, uint8_t(value + '0'));
    return n + 1;
}

// 0x80517A0C: 0 when `needle` occurs in `haystack`, else -1.
int32_t Contains(uint32_t haystack, int32_t length, uint32_t needle, int32_t needleLength) {
    if (length < needleLength)
        return -1;

    for (int32_t at = 0; at < length - needleLength + 1; ++at) {
        if (Char(needle) != Char(haystack + at))
            continue;

        int32_t same = 1;

        while (same < needleLength && Char(haystack + at + same) == Char(needle + same))
            ++same;

        if (same == needleLength)
            return 0;
    }

    return -1;
}

// 0x80517AB8: `in` in base64 into `out`; answers the length (the input's
// bytes as signed characters, as the original).
uint32_t Base64(uint32_t out, uint32_t in) {
    const uint32_t length = StrLen(in);
    uint32_t groups = (length + 2) / 3, covered = 0, at = out;
    const auto digit = [](int32_t index) { return Memory::Read8(kBase64 + uint32_t(index)); };

    if (int32_t(length) > 0) {
        covered = groups * 3;

        for (; groups; --groups, in += 3, at += 4) {
            const int32_t a = int8_t(Memory::Read8(in)), b = int8_t(Memory::Read8(in + 1)), c = int8_t(Memory::Read8(in + 2));
            Memory::Write8(at, digit(a >> 2));
            Memory::Write8(at + 1, digit((b >> 4) + (a & 3) * 0x10));
            Memory::Write8(at + 2, digit((c >> 6) + (b & 0xF) * 4));
            Memory::Write8(at + 3, digit(c & 0x3F));
        }
    }

    if (covered == length + 1) {
        Memory::Write8(at - 1, '=');
    } else if (covered == length + 2) {
        Memory::Write8(at - 2, '=');
        Memory::Write8(at - 1, '=');
    }

    Memory::Write8(at, 0);
    return StrLen(out);
}

// 0x80517CB4: the last bytes (a ring of 4) end the header: CR CR, LF LF or
// CR LF CR LF.
bool HeaderEnd(const uint8_t (&ring)[4], uint32_t n) {
    const uint8_t c = ring[(n - 2) & 3], last = ring[(n - 1) & 3];

    if (c == '\r' && last == '\r')
        return true;

    if (c == '\n' && last == '\n')
        return true;

    return ring[(n - 4) & 3] == '\r' && ring[(n - 3) & 3] == '\n' && c == '\r' && last == '\n';
}

// The response's header bytes from `at` on.
class HeaderBytes {
  public:
    HeaderBytes(uint32_t response, int32_t at) : response_(response) {
        if (at < 0x400) {
            offset_ = uint32_t(at);
            return;
        }

        chunk_ = Read(response + pChunks);

        for (uint32_t n = uint32_t(at - 0x400) >> 9; n; --n)
            chunk_ = Read(chunk_);

        offset_ = uint32_t(at - 0x400) & 0x1FF;
    }

    int8_t Next() {
        if (!chunk_) {
            if (offset_ < 0x400)
                return Char(response_ + pInline + offset_++);

            chunk_ = Read(response_ + pChunks);
            offset_ = 0;
        } else if (offset_ == 0x200) {
            offset_ = 0;
            chunk_ = Read(chunk_);
        }

        return Char(chunk_ + 4 + offset_++);
    }

  private:
    uint32_t response_;
    uint32_t chunk_ = 0;
    uint32_t offset_ = 0;
};

// 0x8051574C: the line from `from` (below `to`): answers where the next
// starts (0 when that is `to`, -1 without an end), the first colon and the
// line end's length (1 or 2).
int32_t FindLine(uint32_t response, int32_t from, int32_t to, int32_t* colon, int32_t* eol) {
    if (colon)
        *colon = -1;

    if (to <= from)
        return -1;

    HeaderBytes bytes(response, from);
    int32_t answer = -1;
    bool cr = false;

    for (int32_t at = from; at < to; ++at) {
        const int8_t c = bytes.Next();
        const int32_t next = at == to - 1 ? 0 : at + 1;

        if (c == ':' && colon && *colon < 0)
            *colon = at;

        if (cr) {
            if (c != '\n')
                return answer;

            if (eol)
                *eol = 2;

            return next;
        }

        if (c == '\r') {
            cr = true;
            answer = next;

            if (eol)
                *eol = 1;
        }

        if (c == '\n') {
            if (eol)
                *eol = 1;

            return next;
        }
    }

    return -1;
}

// 0x80515944: the first non-space at or after `from`.
int32_t SkipSpaces(uint32_t response, int32_t from, int32_t to) {
    if (to <= from)
        return -1;

    HeaderBytes bytes(response, from);

    for (int32_t at = from; at < to; ++at) {
        if (bytes.Next() != ' ')
            return at;
    }

    return -1;
}

// 0x80515A3C: 0 when the header text at `from` matches `text`
// case-insensitively up to its end, a space, `term` or `to`.
int32_t MatchToken(uint32_t response, int32_t from, int32_t to, uint32_t text, int8_t term) {
    if (to <= from)
        return -1;

    HeaderBytes bytes(response, from);
    int8_t c = bytes.Next();

    for (int32_t at = from;; ++at, ++text) {
        const int8_t t = Char(text);

        if (Lower(c) != Lower(t))
            return -1;

        if (t == 0 || t == ' ' || t == term || uint32_t(at) == uint32_t(to - 1))
            return 0;

        c = bytes.Next();
    }
}

// 0x80515C2C: copies `length` header bytes from `from` to `to`.
bool CopyHeader(CpuContext* cpu, uint32_t response, uint32_t to, int32_t from, uint32_t length) {
    if (ReadInt(response + pHeaderLength) < int32_t(from + length))
        return false;

    if (!length)
        return true;

    if (from < 0x400) {
        uint32_t n = length;

        if (int32_t(0x400 - from) < int32_t(length))
            n = 0x400 - from;

        Copy(cpu, to, response + pInline + from, n);
        from += n;
        length -= n;
        to += n;
    }

    if (length) {
        uint32_t chunk = Read(response + pChunks);

        for (uint32_t n = uint32_t(int32_t(from - 0x400) >> 9); n; --n)
            chunk = Read(chunk);

        uint32_t offset = uint32_t(from - 0x400) & 0x1FF;

        while (length) {
            uint32_t n = length;

            if (int32_t(0x200 - offset) < int32_t(length))
                n = 0x200 - offset;

            Copy(cpu, to, chunk + offset + 4, n);
            chunk = Read(chunk);
            offset = (offset + n) & 0x1FF;
            to += n;
            length -= n;
        }
    }

    return true;
}

// 0x805168F0: the named field's value: answers its length (-1 when absent,
// 0 when the header ends at its colon) and where it starts.
int32_t FindField(uint32_t response, uint32_t name, int32_t* start) {
    const int32_t end = ReadInt(response + pHeaderLength);
    int32_t colon = 0, eol = 0;
    int32_t next = FindLine(response, 12, end, &colon, &eol), line;

    do {
        line = next;

        if (line < 1)
            return -1;

        next = FindLine(response, line, end, &colon, &eol);
    } while (colon < 1 || MatchToken(response, line, colon, name, 0) != 0);

    if (colon + 1 >= end)
        return 0;

    int32_t valueEnd = FindLine(response, colon + 1, end, nullptr, &eol);

    if (valueEnd < 1) {
        valueEnd = end;
    } else {
        if (valueEnd < eol)
            return -1;

        valueEnd -= eol;
    }

    int32_t valueStart = SkipSpaces(response, colon + 1, valueEnd);

    if (valueStart < 0)
        valueStart = valueEnd;

    *start = valueStart;
    return valueEnd - valueStart;
}

// 0x80515D70: `n` reaches the response buffer's size (unsigned).
uint32_t AtLeast(uint32_t response, uint32_t n) {
    const uint32_t size = Read(response + pBufferSize);
    return ((n | ~size) - ((n - size) >> 1)) >> 31;
}

// 0x80514FA8: sets the field `label` on the list (a header field or post
// part), appending it when new.
bool AddField(CpuContext* cpu, uint32_t list, uint32_t globals, uint32_t label, uint32_t value) {
    const uint32_t head = Read(list);
    uint32_t node = head;
    bool found = false;

    if (head) {
        if (TokenCompare(label, Read(head + fLabel)) == 0) {
            found = true;
        } else {
            for (node = Read(head + 4); node != Read(list); node = Read(node + 4)) {
                if (TokenCompare(label, Read(node + fLabel)) == 0) {
                    found = true;
                    break;
                }
            }
        }
    }

    if (found) {
        Write(node + fValue, value);
        return true;
    }

    const uint32_t added = Alloc(cpu, 0x18, 4);

    if (!added) {
        Print(cpu, kMsgNoMemory);
        Write(globals + gError, 1);
        return false;
    }

    Write(added + fLabel, label);
    Write(added + fValue, value);
    Write(added + fLength, 0);
    Write(added + fBinary, 0);
    Append(list, added);
    return true;
}

// 0x8051512C: a header field, before the request is sent.
bool AddHeader(CpuContext* cpu, uint32_t request, uint32_t globals, uint32_t label, uint32_t value) {
    if (Read(request + rStarted))
        return false;

    return AddField(cpu, request + rHeaders, globals, label, value);
}

// 0x8051514C: a text post part. The multipart boundary changes (one
// character at a time from the end) until the value does not contain it.
bool AddPost(CpuContext* cpu, uint32_t request, uint32_t globals, uint32_t label, uint32_t value) {
    if (Read(request + rStarted))
        return false;

    if (Read(request + rRawPost)) {
        Print(cpu, kMsgRawPostSet);
        return false;
    }

    const int32_t length = value ? int32_t(StrLen(value)) : 0;
    const uint32_t boundary = request + rBoundary;
    bool unique = Contains(value, length, boundary + 2, 0x12) < 0;

    for (uint32_t i = 0x13; !unique && i > 1; --i) {
        const int8_t original = Char(kBoundary + i);

        for (;;) {
            const uint32_t c = (Memory::Read8(boundary + i) + 1) & 0xFF;
            int8_t next = int8_t(c);

            if (c == 0x7B)
                next = '0';
            else if (c == 0x5B)
                next = 'a';
            else if (c == 0x3A)
                next = 'A';

            Memory::Write8(boundary + i, uint8_t(next));

            if (next == original)
                break;

            if (Contains(value, length, boundary + 2, 0x12) < 0) {
                unique = true;
                break;
            }
        }
    }

    if (!unique || !AddField(cpu, request + rPosts, globals, label, value))
        return false;

    Write(Read(Read(request + rPosts)) + fLength, uint32_t(length));
    return true;
}

// 0x805152E8: queues a request; answers its ID (-1 without memory).
int32_t QueueAdd(CpuContext* cpu, uint32_t queue, uint32_t request) {
    const uint32_t node = Alloc(cpu, 0x14, 4);

    if (!node)
        return -1;

    Append(queue, node);
    const uint32_t id = Read(queue + 4);
    Write(node + qId, id);
    Write(queue + 4, id + 1);
    Write(node + qRequest, request);
    Write(node + qSocket, 0xFFFFFFFF);

    if (ReadInt(queue + 4) < 0)
        Write(queue + 4, 0);

    return int32_t(Read(node + qId));
}

void FreeRequest(CpuContext* cpu, uint32_t mutex, uint32_t request);

// 0x805153B4: takes request `id` off the queue, frees it and completes its
// connection as canceled (error 8).
bool QueueRemove(CpuContext* cpu, uint32_t queue, uint32_t mutex, int32_t id) {
    const uint32_t head = Read(queue);
    uint32_t node = 0;

    if (head) {
        if (ReadInt(head + qId) == id) {
            node = head;
        } else {
            for (uint32_t at = Read(head + 4); at != head; at = Read(at + 4)) {
                if (ReadInt(at + qId) == id) {
                    node = at;
                    break;
                }
            }
        }
    }

    if (!node)
        return false;

    if (head == Read(head)) {
        Write(queue, 0);
    } else {
        Write(Read(node) + 4, Read(node + 4));
        Write(Read(node + 4), Read(node));

        if (Read(queue) == node)
            Write(queue, Read(node + 4));
    }

    const uint32_t request = Read(node + qRequest);
    const uint32_t connection = Connections(cpu, mutex, request, kByRequest);
    FreeRequest(cpu, mutex, request);
    Free(cpu, node);

    if (connection) {
        Write(connection + cError, 8);
        Complete(cpu, mutex, connection);
    }

    return true;
}

// 0x805154D0.
void QueueClear(CpuContext* cpu, uint32_t queue, uint32_t mutex) {
    while (Read(queue))
        QueueRemove(cpu, queue, mutex, ReadInt(Read(queue) + qId));
}

// 0x80515580: the worker's message queue (once) and thread.
void WorkerStart(CpuContext* cpu, uint32_t worker, uint32_t priority, uint32_t stack) {
    GuestCall call{cpu};

    if (!Read(worker + wQueueReady)) {
        call.Call<OSInitMessageQueue>({worker, worker + wMessages, 3});
        Write(worker + wQueueReady, 1);
    }

    call.Call<OSCreateThread>({worker + wThread, kWorkerEntry, 0, stack + 0x2000, 0x2000, priority, 0});
    call.Call<OSResumeThread>({worker + wThread});
}

// 0x80515614: tells the worker to quit and waits for it.
void WorkerStop(CpuContext* cpu, uint32_t worker, uint32_t globals) {
    Write(globals + gQuit, 1);
    GuestCall call{cpu};
    call.Call<OSSendMessage>({worker, 0, 0});
    call.Call<OSJoinThread>({worker + wThread, 0});
}

// 0x8051565C / 0x80515684: the worker waits for a request / is woken.
void WorkerWait(CpuContext* cpu, uint32_t worker) {
    GuestFrame frame(cpu, 0x20);
    GuestCall{cpu}.Call<OSReceiveMessage>({worker, frame[0x10], 1});
}

void WorkerWake(CpuContext* cpu, uint32_t worker) {
    GuestCall{cpu}.Call<OSSendMessage>({worker, 0, 0});
}

// 0x80515690: panics on the worker thread (`worker` set) or off it.
void CheckThread(CpuContext* cpu, uint32_t worker, uint32_t onWorker) {
    const uint32_t thread = Read(kCurrentThread);

    if (thread && ((!onWorker && thread != worker + wThread) || (onWorker && thread == worker + wThread))) {
        Report(cpu, kMsgIllegalThread, kCheckThreadName);
        Panic(cpu, kFileOs, 0xDF, kIllegalThread);
    }
}

// 0x80514D20: the allocator, the queue and mutex, the worker and its stack,
// then the interface's config (its proxies).
bool Startup(CpuContext* cpu, uint32_t globals, uint32_t alloc, uint32_t free, uint32_t priority) {
    Write(globals + gAlloc, alloc);
    Write(globals + gFree, free);
    Write(globals + gError, 0);
    Write(globals + gSslError, 0);
    Write(globals + gQuit, 0);
    Write(globals + gQueue, 0);
    Write(globals + gQueue + 4, 0);
    Write(globals + gCurrent, 0);
    MutexSetup(cpu, globals + gMutex);
    ResetConnections(cpu);
    Write(globals + gSocket, 0xFFFFFFFF);
    const uint32_t stack = Alloc(cpu, 0x2000, 8);
    Write(globals + gStack, stack);

    if (!stack) {
        Write(globals + gError, 1);
        return false;
    }

    WorkerStart(cpu, globals + gWorker, priority, stack);
    const int32_t result = int32_t(GuestCall{cpu}.Call<NCDGetCurrentIpConfig>({globals}));

    if (result < 0) {
        Report(cpu, kMsgIpConfig, uint32_t(result));
        Panic(cpu, kFileBgnEnd, 0xE6, kIpConfigName);
    }

    Write(globals + gStarted, 1);
    return true;
}

void CancelAll(CpuContext* cpu, uint32_t globals);

// 0x80514EB0: cancels everything, stops the worker, frees its stack, calls
// `callback` and closes the kept socket.
void Cleanup(CpuContext* cpu, uint32_t globals, uint32_t callback) {
    const uint32_t worker = globals + gWorker;
    CheckThread(cpu, worker, 1);
    CancelAll(cpu, globals);
    WorkerStop(cpu, worker, globals);
    Free(cpu, Read(globals + gStack));
    Write(globals + gStack, 0);
    Write(globals + gStarted, 0);

    if (callback)
        GuestCall{cpu}(callback, {});

    const uint32_t count = ConnectionCount();

    if (count)
        Print(cpu, kMsgConnectionsLeft, count);

    if (ReadInt(globals + gSocket) >= 0) {
        GuestCall{cpu}.Call<SOClose>({Read(globals + gSocket)});
        Write(globals + gSocket, 0xFFFFFFFF);
    }
}

// 0x80515DF4: a request for `url` (http:// or https://, %XX decoded up to
// the path, an optional port): its host, path and port, its response with
// the caller's buffer.
uint32_t CreateRequest(CpuContext* cpu, uint32_t globals, uint32_t url, int32_t method, uint32_t buffer, uint32_t size, uint32_t param, uint32_t grow, uint32_t release) {
    GuestFrame frame(cpu, 0x150);
    const uint32_t local = frame[0x10];
    uint32_t copy = local, request = 0;
    const auto fail = [&](uint32_t error) {
        Write(globals + gError, error);

        if (request) {
            if (Read(request + rUrl))
                Free(cpu, Read(request + rUrl));

            if (Read(request + rHost))
                Free(cpu, Read(request + rHost));

            if (Read(request + rResponse))
                Free(cpu, Read(request + rResponse));

            Free(cpu, request);
        }

        if (copy && copy != local)
            Free(cpu, copy);

        return 0u;
    };

    if (method >= 3 || method < 0) {
        Write(globals + gError, 0xB);
        return 0;
    }

    request = Alloc(cpu, 600, 4);

    if (!request) {
        Write(globals + gError, 1);
        return 0;
    }

    Clear(cpu, request, 600);
    const uint32_t response = Alloc(cpu, 0x43C, 4);
    Write(request + rResponse, response);

    if (!response)
        return fail(1);

    Clear(cpu, response, 0x43C);
    Write(response + pBuffer, buffer);
    Write(response + pBufferSize, size);
    Write(response + pGrow, grow);
    Write(response + pRelease, release);
    const int32_t length = int32_t(StrLen(url));

    if (length < 8)
        return fail(4);

    if (length + 1 >= 0x101) {
        copy = Alloc(cpu, uint32_t(length + 1), 4);

        if (!copy)
            return fail(4);
    }

    Clear(cpu, copy, uint32_t(length));
    Copy(cpu, copy, url, uint32_t(length));
    Write(request + rPort, 80);
    int32_t scheme = 7;

    if (CaseCompare(copy, kHttpScheme, 7) != 0) {
        if (CaseCompare(copy, kHttpsScheme, 8) != 0)
            return fail(4);

        Write(request + rHttps, 1);
        scheme = 8;
        Write(request + rPort, 443);
    }

    const int32_t rest = length - scheme;
    const uint32_t start = copy + uint32_t(scheme);

    if (rest < 1)
        return fail(4);

    // The host's escapes up to the first '/' (an escaped one included).
    int32_t escapes = 0, escape = 0;

    for (int32_t i = 0; i < rest && Char(start + i) != '/'; ++i) {
        if (escape == 2) {
            escape = 1;
            continue;
        }

        if (escape != 1) {
            if (Char(start + i) == '%') {
                escape = 2;
                ++escapes;
            }

            continue;
        }

        const int8_t c = int8_t(ParseHex(start + i - 1, 2));
        escape = 0;

        if (c < 0)
            return fail(4);

        if (c == '/') {
            --escapes;
            break;
        }
    }

    if (escape != 0)
        return fail(4);

    const uint32_t decodedSize = uint32_t(scheme + rest - escapes * 2 + 1);
    const uint32_t decoded = Alloc(cpu, decodedSize, 4);
    Write(request + rUrl, decoded);

    if (!decoded)
        return fail(1);

    Clear(cpu, decoded, decodedSize);
    Copy(cpu, decoded, copy, uint32_t(scheme));
    int32_t out = 0;
    bool path = false;
    escape = 0;

    for (int32_t i = 0; i < rest; ++i) {
        if (escape == 2) {
            escape = 1;
        } else if (escape == 1) {
            const int8_t c = int8_t(ParseHex(start + i - 1, 2));
            escape = 0;
            Memory::Write8(decoded + out + scheme - 1, uint8_t(c));

            if (c == '/')
                path = true;
        } else {
            const int8_t c = Char(start + i);

            if (c == '/')
                path = true;

            if (!path && c == '%')
                escape = 2;
            else
                Memory::Write8(decoded + out + scheme, uint8_t(c));

            ++out;
        }
    }

    Memory::Write8(decoded + scheme + out, 0);
    const uint32_t host = decoded + uint32_t(scheme);
    int32_t at = 0;

    for (; at < out; ++at) {
        const int8_t c = Char(host + at);

        if (c == '/' || c == ':') {
            Write(request + rHostEnd, uint32_t(at + scheme));
            break;
        }
    }

    if (at == out) {
        Write(request + rHostEnd, uint32_t(at + scheme));
        Write(request + rPathStart, uint32_t(at + scheme));
    } else if (Char(host + at) == '/') {
        Write(request + rPathStart, Read(request + rHostEnd));
    } else if (Char(host + at) == ':') {
        for (; at < out; ++at) {
            if (Char(host + at) == '/') {
                Write(request + rPathStart, uint32_t(at + scheme));
                break;
            }
        }

        if (at == out) {
            Write(request + rPathStart, uint32_t(at + scheme));
        } else {
            const int32_t portStart = ReadInt(request + rHostEnd) + 1;
            int32_t port = ParseDigits(decoded + uint32_t(portStart), ReadInt(request + rPathStart) - portStart);

            if (port < 0)
                port = ReadInt(request + rPort);
            else if (port > 0xFFFF)
                return fail(4);

            Write(request + rPort, uint32_t(port) & 0xFFFF);
        }
    }

    const uint32_t skip = Read(request + rHttps) ? 8 : 7;
    const uint32_t hostLength = Read(request + rHostEnd) - skip;
    const uint32_t hostCopy = Alloc(cpu, hostLength + 1, 4);
    Write(request + rHost, hostCopy);

    if (!hostCopy)
        return fail(1);

    Clear(cpu, hostCopy, hostLength + 1);
    Copy(cpu, hostCopy, decoded + skip, hostLength);
    Copy(cpu, request + rBoundary, kBoundary, 0x14);
    Write(request + rMethod, uint32_t(method));

    for (uint32_t field = rSsl; field <= rVerify; field += 4)
        Write(request + field, 0);

    Write(request + rSslHookArg, 0);
    Write(response + pParam, param);
    Write(request + rProxy, 0);
    Write(request + rRecvBuffer, 0);
    Write(request + rSendBuffer, 0);

    if (copy && copy != local)
        Free(cpu, copy);

    return request;
}

// 0x80516520: frees a request (not its response).
void FreeRequest(CpuContext* cpu, uint32_t mutex, uint32_t request) {
    const uint32_t connection = Connections(cpu, mutex, request, kByRequest);

    if (connection)
        Write(connection + cRequest, 0);

    FreeList(cpu, Read(request + rHeaders));
    FreeList(cpu, Read(request + rPosts));
    Free(cpu, Read(request + rUrl));
    Free(cpu, Read(request + rHost));
    Free(cpu, request);
}

// 0x8051641C: frees a request with its response.
void DiscardRequest(CpuContext* cpu, uint32_t globals, uint32_t request) {
    const uint32_t mutex = globals + gMutex;
    uint32_t connection = Connections(cpu, mutex, request, kByRequest);

    if (connection)
        Write(connection + cResponse, 0);

    Free(cpu, Read(request + rResponse));
    connection = Connections(cpu, mutex, request, kByRequest);

    if (connection)
        Write(connection + cRequest, 0);

    FreeList(cpu, Read(request + rHeaders));
    FreeList(cpu, Read(request + rPosts));
    Free(cpu, Read(request + rUrl));
    Free(cpu, Read(request + rHost));
    Free(cpu, request);
}

// 0x805165F8: queues the request for the worker; answers its ID.
int32_t Queue(CpuContext* cpu, uint32_t globals, uint32_t request) {
    const uint32_t mutex = globals + gMutex;

    if (Read(request + rStarted)) {
        Write(globals + gError, 0xB);
        return -1;
    }

    Lock(cpu, mutex);
    const int32_t id = QueueAdd(cpu, globals + gQueue, request);

    if (id < 0) {
        Write(globals + gError, 1);
    } else {
        Write(request + rStarted, 1);
        WorkerWake(cpu, globals + gWorker);
    }

    Unlock(cpu, mutex);
    return id;
}

// 0x80517258: ends the socket's traffic both ways.
void ShutdownSocket(CpuContext* cpu, uint32_t mutex, int32_t socket) {
    Lock(cpu, mutex);

    if (socket >= 0)
        GuestCall{cpu}.Call<SOShutdown>({uint32_t(socket), 2});

    Unlock(cpu, mutex);
}

// 0x805166B8: cancels request `id`: the one being worked on (its socket
// shut down) or a queued one.
bool Cancel(CpuContext* cpu, uint32_t globals, int32_t id) {
    const uint32_t mutex = globals + gMutex;
    const uint32_t current = Read(globals + gCurrent);
    bool found = false;
    Lock(cpu, mutex);

    if (current && ReadInt(current + qId) == id && !Read(Read(current + qRequest))) {
        Write(Read(current + qRequest), 1);
        ShutdownSocket(cpu, mutex, ReadInt(current + qSocket));
        found = true;
    }

    if (!found)
        found = QueueRemove(cpu, globals + gQueue, mutex, id);

    Unlock(cpu, mutex);
    return found;
}

// 0x8051677C: cancels every request.
void CancelAll(CpuContext* cpu, uint32_t globals) {
    const uint32_t mutex = globals + gMutex;
    const uint32_t current = Read(globals + gCurrent);
    Lock(cpu, mutex);

    if (current && !Read(Read(current + qRequest))) {
        Write(Read(current + qRequest), 1);
        ShutdownSocket(cpu, mutex, ReadInt(current + qSocket));
    }

    QueueClear(cpu, globals + gQueue, mutex);
    Unlock(cpu, mutex);
}

// 0x80516824: frees a response: its header chunks, its buffer through the
// release callback.
void FreeResponse(CpuContext* cpu, uint32_t mutex, uint32_t response) {
    for (uint32_t chunk; (chunk = Read(response + pChunks)) != 0;) {
        const uint32_t next = Read(chunk);
        Free(cpu, chunk);
        Write(response + pChunks, next);
    }

    if (Read(response + pSpare1))
        Free(cpu, Read(response + pSpare1));

    if (Read(response + pSpare2))
        Free(cpu, Read(response + pSpare2));

    if (const uint32_t release = Read(response + pRelease)) {
        GuestCall{cpu}(release, {Read(response + pBuffer), kFreeAddress, Read(response + pParam)});
        Write(response + pBuffer, 0);
        Write(response + pBufferSize, 0);
    }

    const uint32_t connection = Connections(cpu, mutex, response, kByResponse);

    if (connection)
        Write(connection + cResponse, 0);

    Free(cpu, response);
}

// 0x80516A24: 0 while the kept socket is open with nothing to read.
int32_t SocketAlive(CpuContext* cpu, int32_t socket) {
    if (socket == -1)
        return -1;

    GuestFrame frame(cpu, 0x30);
    const int32_t result = int32_t(GuestCall{cpu}.Call<SORecv>({uint32_t(socket), frame[0x10], 1, 4}));
    return result == -6 ? 0 : -1;
}

// 0x80516A7C: a TCP socket with the request's buffer sizes.
int32_t OpenSocket(CpuContext* cpu, uint32_t request) {
    GuestCall call{cpu};
    const int32_t socket = int32_t(call.Call<SOSocket>({2, 1, 0}));
    GuestFrame frame(cpu, 0x30);
    const uint32_t value = frame[0x10];
    const uint32_t recvBuffer = request ? Read(request + rRecvBuffer) : 0;

    if (socket >= 0 && recvBuffer) {
        Write(value, recvBuffer);
        call.Call<SOSetSockOpt>({uint32_t(socket), 0xFFFF, 0x1002, value, 4});
    }

    const uint32_t sendBuffer = request ? Read(request + rSendBuffer) : 0;

    if (socket >= 0 && sendBuffer) {
        Write(value + 4, sendBuffer);
        call.Call<SOSetSockOpt>({uint32_t(socket), 0xFFFF, 0x1001, value + 4, 4});
    }

    return socket;
}

// 0x80516B50: ends the request's SSL and closes the socket.
int32_t CloseSocket(CpuContext* cpu, uint32_t mutex, uint32_t request, uint32_t socket) {
    GuestCall call{cpu};
    Lock(cpu, mutex);

    if (ReadInt(request + rSsl) > 0) {
        call.Call<SSLShutdown>({Read(request + rSsl)});
        Write(request + rSsl, 0xFFFFFFFF);
    }

    Unlock(cpu, mutex);
    return int32_t(call.Call<SOClose>({socket}));
}

// 0x80516C7C: SSL over the connected socket: certificates, then the
// handshake (repeated while it would block).
int32_t SslConnect(CpuContext* cpu, uint32_t globals, uint32_t mutex, uint32_t request, uint32_t socket) {
    GuestCall call{cpu};
    const uint32_t ssl = call.Call<SSLNew>({Read(request + rVerify), Read(request + rHost)});
    Write(request + rSsl, ssl);
    const uint32_t hook = Read(globals + gSslHook);

    if (hook && Read(request + rSslHookArg))
        call(hook, {ssl, Read(request + rSslHookArg)});

    if (Read(request + rBuiltinClientCert) == 1) {
        if (call.Call<SSLSetBuiltinClientCert>({Read(request + rSsl), Read(request + rClientCertId)}))
            return -1005;
    } else if (Read(request + rClientCert) && Read(request + rClientKey)) {
        if (call.Call<SSLSetClientCert>({Read(request + rSsl), Read(request + rClientCert), Read(request + rClientCertLength), Read(request + rClientKey), Read(request + rClientKeyLength)}))
            return -1005;
    }

    if (!Read(request + rRootCa)) {
        if (call.Call<SSLSetBuiltinRootCA>({Read(request + rSsl), Read(request + rRootCaId)}))
            return -1004;
    } else if (call.Call<SSLSetRootCA>({Read(request + rSsl), Read(request + rRootCa), Read(request + rRootCaLength)})) {
        return -1004;
    }

    if (int32_t(call.Call<SSLConnect>({Read(request + rSsl), socket})) < -1)
        return -1001;

    for (;;) {
        const uint32_t connection = Connections(cpu, mutex, request, kByRequest);
        const int32_t result = int32_t(call.Call<SSLDoHandshake>({Read(request + rSsl)}));
        Write(globals + gSslError, uint32_t(result));

        if (connection)
            Write(connection + cSslError, uint32_t(result));

        if (result == 0)
            return 0;

        if (result == -1 || result > 0 || (result != -7 && result < -3))
            return -1001;
    }
}

// 0x80516BBC: connects to `address`:`port`, then SSL for https (not
// through a proxy: that comes after its CONNECT).
int32_t Connect(CpuContext* cpu, uint32_t globals, uint32_t mutex, uint32_t request, uint32_t socket, uint32_t address, uint32_t port) {
    GuestFrame frame(cpu, 0x30);
    const uint32_t sockaddr = frame[0x10];
    Memory::Write8(sockaddr, 8);
    Memory::Write8(sockaddr + 1, 2);
    Memory::Write16(sockaddr + 2, uint16_t(port));
    Write(sockaddr + 4, address);

    if (int32_t(GuestCall{cpu}.Call<SOConnect>({socket, sockaddr})) < 0)
        return Read(request + rCancel) ? -1002 : -1001;

    if (!Read(request + rHttps) || Read(request + rProxy))
        return 0;

    return SslConnect(cpu, globals, mutex, request, socket);
}

// 0x80516E2C: reads through the connection's 32 KiB buffer.
int32_t BufferedRecv(CpuContext* cpu, uint32_t connection, uint32_t socket, uint32_t to, int32_t length, uint32_t flags) {
    const uint32_t data = connection + cBuffer;

    if (length <= 0)
        return 0;

    if (!Read(connection + cBufferCount)) {
        const int32_t n = int32_t(GuestCall{cpu}.Call<SORecv>({socket, data, 0x8000, flags}));

        if (n < 1)
            return n;

        Write(connection + cBufferCount, uint32_t(n));
        Write(connection + cBufferAt, 0);
    }

    const uint32_t count = Read(connection + cBufferCount);

    if (!count)
        return 0;

    const uint32_t n = count < uint32_t(length) ? count : uint32_t(length);
    Copy(cpu, to, data + Read(connection + cBufferAt), n);
    const uint32_t left = Read(connection + cBufferCount) - n;
    Write(connection + cBufferCount, left);

    if (!left) {
        Clear(cpu, data, 0x8000);
        Write(connection + cBufferAt, 0);
    } else {
        Write(connection + cBufferAt, Read(connection + cBufferAt) + n);
    }

    return int32_t(n);
}

// 0x80516F38's and 0x805171AC's answer: a would-block is 0, a canceled
// request -1002, other errors -1001.
int32_t TransferAnswer(uint32_t request, int32_t n) {
    if (n >= 0)
        return n;

    if (Read(request + rCancel))
        return -1002;

    if (ReadInt(request + rSsl) <= 0) {
        if (n == -56)
            return 0;
    } else if (uint32_t(n + 7) < 2) {
        return 0;
    }

    return -1001;
}

// 0x80516F38: reads from the request's socket (or its SSL).
int32_t Recv(CpuContext* cpu, uint32_t mutex, uint32_t request, uint32_t socket, uint32_t to, int32_t length, uint32_t flags) {
    const int32_t ssl = ReadInt(request + rSsl);
    int32_t n;

    if (ssl <= 0) {
        const uint32_t connection = Connections(cpu, mutex, request, kByRequest);

        if (!connection)
            return -1001;

        n = BufferedRecv(cpu, connection, socket, to, length, flags);
    } else {
        n = int32_t(GuestCall{cpu}.Call<SSLRead>({uint32_t(ssl), to, uint32_t(length)}));
    }

    return TransferAnswer(request, n);
}

// 0x80515D8C: into the response buffer after what it holds, up to its end.
int32_t RecvRest(CpuContext* cpu, uint32_t mutex, uint32_t request, uint32_t socket, uint32_t received, uint32_t flags) {
    const uint32_t response = Read(request + rResponse);
    return Recv(cpu, mutex, request, socket, Read(response + pBuffer) + received, int32_t(Read(response + pBufferSize) - received), flags);
}

// 0x80515DAC: as RecvRest, at most `most`; -1003 when the buffer is full.
int32_t RecvAtMost(CpuContext* cpu, uint32_t mutex, uint32_t request, uint32_t socket, uint32_t received, int32_t most, uint32_t flags) {
    const uint32_t response = Read(request + rResponse);
    const uint32_t size = Read(response + pBufferSize);

    if (size <= received)
        return -1003;

    const int32_t room = int32_t(size - received);
    return Recv(cpu, mutex, request, socket, Read(response + pBuffer) + received, most <= room ? most : room, flags);
}

// 0x80517014: sends with the device seeing 32-byte aligned buffers: an
// unaligned head and the tail go through a bounce buffer.
int32_t AlignedSend(CpuContext* cpu, uint32_t socket, uint32_t data, uint32_t length, uint32_t flags) {
    GuestFrame frame(cpu, 0x60);
    const uint32_t bounce = (frame[0x10] + 0x1F) & ~0x1Fu;
    GuestCall call{cpu};
    uint32_t head = (data & 0x1F) ? 0x20 - (data & 0x1F) : 0, sent = 0;
    Clear(cpu, bounce, 0x20);

    if (head) {
        if (length < head)
            head = length;

        Copy(cpu, bounce, data, head);
        const uint32_t n = call.Call<SOSend>({socket, bounce, head, flags});

        if (int32_t(n) < 1 || n < head)
            return int32_t(n);

        data += n;
        length -= n;
        sent = n;
    }

    if (int32_t(length) > 0) {
        const uint32_t body = length & ~0x1Fu;

        if (body) {
            const uint32_t n = call.Call<SOSend>({socket, data, body, flags});

            if (int32_t(n) < 1)
                return int32_t(sent) > 0 ? int32_t(sent) : int32_t(n);

            if (n < body)
                return int32_t(sent + n);

            data += n;
            length -= n;
            sent += n;
        }
    }

    if (int32_t(length) > 0 && (length & 0x1F)) {
        const uint32_t tail = length & 0x1F;
        Clear(cpu, bounce, 0x20);
        Copy(cpu, bounce, data, tail);
        const uint32_t n = call.Call<SOSend>({socket, bounce, tail, flags});

        if (int32_t(n) < 1)
            return int32_t(sent) > 0 ? int32_t(sent) : int32_t(n);

        return int32_t(sent + n);
    }

    return int32_t(sent);
}

// 0x805171AC: sends on the request's socket (or its SSL).
int32_t Send(CpuContext* cpu, uint32_t request, uint32_t socket, uint32_t data, uint32_t length, uint32_t flags) {
    const int32_t ssl = ReadInt(request + rSsl);
    int32_t n;

    if (ssl < 1)
        n = AlignedSend(cpu, socket, data, length, flags);
    else
        n = int32_t(GuestCall{cpu}.Call<SSLWrite>({uint32_t(ssl), data, length}));

    return TransferAnswer(request, n);
}

// 0x805172AC: the host's IPv4 address (0 when it does not resolve).
uint32_t ResolveHost(CpuContext* cpu, uint32_t host) {
    GuestFrame frame(cpu, 0x30);
    const uint32_t result = frame[0x10], address = frame[0x14];
    GuestCall call{cpu};
    Write(address, 0);

    if (call.Call<SOGetAddrInfo>({host, 0, 0, result}) != 0)
        return 0;

    Copy(cpu, address, Read(Read(result) + 0x18) + 4, 4);
    call.Call<SOFreeAddrInfo>({Read(result)});
    return Read(address);
}

// 0x80517D7C: adds to the 0x100-byte send buffer (`fill` holds its count),
// sending it whenever it is full; answers `length`, -1 when canceled or
// what a failed send answered.
int32_t BufferedSend(CpuContext* cpu, uint32_t request, uint32_t buffer, uint32_t socket, uint32_t fill, uint32_t data, int32_t length) {
    for (int32_t left = length; left > 0;) {
        if (Read(request + rCancel))
            return -1;

        int32_t n = 0x100 - ReadInt(fill);

        if (left < n)
            n = left;

        Copy(cpu, buffer + Read(fill), data, uint32_t(n));
        const uint32_t filled = Read(fill) + uint32_t(n);
        data += uint32_t(n);
        left -= n;
        Write(fill, filled);

        if (filled == 0x100) {
            const int32_t sent = Send(cpu, request, socket, buffer, 0x100, 0);

            if (sent < 1)
                return sent;

            Write(fill, Read(fill) - uint32_t(sent));
        }
    }

    return length;
}

// 0x80517E5C: the length of streamed post data (`label`'s; URL-encoded for
// mode 2) by asking the callback for every piece.
bool PostLength(CpuContext* cpu, uint32_t mutex, uint32_t request, uint32_t label, int32_t* total, int32_t mode) {
    const uint32_t connection = Connections(cpu, mutex, request, kByRequest);

    if (!connection)
        return false;

    Write(connection + cPostData, 0);
    uint32_t offset = 0;

    while (!Read(request + rCancel)) {
        Write(connection + cPostLength, 0);

        if (PostCallback(cpu, mutex, connection, label, offset) < 0)
            return false;

        const uint32_t n = Read(connection + cPostLength);

        if (!n)
            return true;

        if (!Read(connection + cPostData))
            return false;

        offset += n;

        if (mode == 2)
            *total += EncodedLength(Read(connection + cPostData), int32_t(n));
        else if (mode < 2 && mode >= 0)
            *total += int32_t(n);
    }

    return false;
}

// 0x80517F60: sends streamed post data (URL-encoded for mode 2): 0 when
// done, 1 on a send error, 2 when a send took nothing, 3 on a callback
// error or cancel.
uint32_t PostSend(CpuContext* cpu, uint32_t mutex, uint32_t request, uint32_t buffer, uint32_t label, uint32_t socket, uint32_t fill, int32_t mode) {
    const uint32_t connection = Connections(cpu, mutex, request, kByRequest);

    if (!connection)
        return 3;

    GuestFrame frame(cpu, 0x30);
    const uint32_t encoded = frame[0x10];
    Write(connection + cPostData, 0);
    uint32_t offset = 0;

    for (;;) {
        if (Read(request + rCancel))
            return 3;

        Write(connection + cPostLength, 0);

        if (PostCallback(cpu, mutex, connection, label, offset) < 0)
            return 3;

        const uint32_t n = Read(connection + cPostLength);
        uint32_t data = Read(connection + cPostData);

        if (!n)
            return 0;

        if (!data)
            return 3;

        offset += n;

        if (mode == 2) {
            for (uint32_t i = 0; i < n; ++i, ++data) {
                Clear(cpu, encoded, 3);
                const uint32_t length = EncodeChar(encoded, uint32_t(int32_t(Char(data))));
                const int32_t sent = BufferedSend(cpu, request, buffer, socket, fill, encoded, int32_t(length));

                if (sent < 0)
                    return 1;

                if (sent == 0)
                    return 2;
            }
        } else if (mode >= 0 && mode < 2) {
            const int32_t sent = BufferedSend(cpu, request, buffer, socket, fill, data, int32_t(n));

            if (sent < 0)
                return 1;

            if (sent == 0)
                return 2;
        }
    }
}

// 0x805180F0: the response buffer has room (growing it through the
// callback when full or missing).
bool HasRoom(CpuContext* cpu, uint32_t mutex, uint32_t response) {
    const uint32_t full = AtLeast(response, Read(response + pReceived));

    if (Read(response + pBufferSize) && Read(response + pBuffer) && !full)
        return true;

    const uint32_t connection = Connections(cpu, mutex, response, kByResponse);

    if (!connection)
        return false;

    BufferCallback(cpu, mutex, connection, 2);
    return Read(response + pBuffer) && Read(response + pBufferSize) && !AtLeast(response, Read(response + pReceived));
}

// The request the worker is on.
uint32_t CurrentRequest(uint32_t globals) {
    return Read(Read(globals + gCurrent) + qRequest);
}

// 0x805181BC: request text through the send buffer: 0, 1 on a send error
// (or cancel), 2 when a send took nothing.
uint32_t SendText(CpuContext* cpu, uint32_t state, uint32_t data, uint32_t length) {
    const uint32_t globals = Globals();

    if (!length)
        return 0;

    const int32_t sent = BufferedSend(cpu, CurrentRequest(globals), globals + gWorker + wSendBuffer, Read(globals + gSocket), state + sFill, data, int32_t(length));

    if (sent < 0)
        return 1;

    return sent == 0 ? 2 : 0;
}

// Flushes what the send buffer holds: 0, 1 on an error, 2 when nothing
// went.
uint32_t Flush(CpuContext* cpu, uint32_t globals, uint32_t state, uint32_t request) {
    const uint32_t buffer = globals + gWorker + wSendBuffer;
    uint32_t answer = 0;

    if (ReadInt(state + sFill) > 0) {
        const int32_t sent = Send(cpu, request, Read(globals + gSocket), buffer, Read(state + sFill), 0);
        Write(state + sFill, 0);
        Clear(cpu, buffer, 0x100);
        answer = sent < 0 ? 1 : 0;

        if (sent == 0)
            answer = 2;
    }

    Write(state + sFill, 0);
    Clear(cpu, buffer, 0x100);
    return answer;
}

// The Authorization fields (the proxy's for kProxyAuthField).
uint32_t SendAuth(CpuContext* cpu, uint32_t state, uint32_t field, uint32_t fieldLength, uint32_t value, uint32_t length) {
    if (!Read(length))
        return 0;

    uint32_t answer = SendText(cpu, state, field, fieldLength);

    if (!answer)
        answer = SendText(cpu, state, value, Read(length));

    if (!answer)
        answer = SendText(cpu, state, kCrLf, 2);

    return answer;
}

uint32_t SendProxyAuth(CpuContext* cpu, uint32_t state) {
    const uint32_t request = CurrentRequest(Globals());
    return SendAuth(cpu, state, kProxyAuthField, 0x1B, request + rProxyAuth, request + rProxyAuthLength);
}

// 0x80518268: the proxy's CONNECT for https.
uint32_t ProxyConnect(CpuContext* cpu, uint32_t state) {
    const uint32_t globals = Globals();
    const uint32_t request = CurrentRequest(globals);
    GuestFrame frame(cpu, 0x40);
    const uint32_t port = frame[0x10];
    const uint32_t digits = Decimal(port, Read(request + rPort));
    const uint32_t host = Read(request + rUrl) + 8, hostLength = Read(request + rHostEnd) - 8;
    const std::initializer_list<std::pair<uint32_t, uint32_t>> parts = {
        {kConnectMethod, 8}, {host, hostLength}, {kColon, 1}, {port, digits}, {kRequestEnd, 0xB},
        {kHostField, 6}, {host, hostLength}, {kColon, 1}, {port, digits}, {kCrLf, 2}, {kConnectFields, 0x25},
    };

    for (const auto& [text, length] : parts) {
        if (const uint32_t answer = SendText(cpu, state, text, length))
            return answer;
    }

    if (const uint32_t answer = SendProxyAuth(cpu, state))
        return answer;

    SendText(cpu, state, kCrLf, 2);

    if (ReadInt(state + sFill) > 0) {
        const int32_t sent = Send(cpu, request, Read(globals + gSocket), globals + gWorker + wSendBuffer, Read(state + sFill), 0);

        if (sent < 0)
            return 1;

        if (sent == 0)
            return 2;
    }

    Write(state + sFill, 0);
    Clear(cpu, globals + gWorker + wSendBuffer, 0x100);
    return 0;
}

// 0x80518510: the proxy's answer to CONNECT, up to the header's end:
// whether it was 200.
bool ProxyAnswer(CpuContext* cpu, uint32_t state) {
    const uint32_t globals = Globals();
    const uint32_t request = CurrentRequest(globals);
    const uint32_t response = Read(request + rResponse);
    const uint32_t mutex = globals + gMutex, socket = Read(globals + gSocket);
    GuestFrame frame(cpu, 0x220);
    const uint32_t text = frame[0x10];
    int32_t got = 0;
    bool ok = false;

    for (;;) {
        do {
            const int32_t n = Recv(cpu, mutex, request, socket, text + uint32_t(got), 0x200 - got, 0);
            got += n;
            Write(response + pStatus, uint32_t(ParseDecimal(text + 9, 3)));

            if (CaseCompare(text, kStatusStart, 5) == 0 && Char(text + 8) == ' ' && ReadInt(response + pStatus) == 200)
                ok = true;

            bool end = false;

            for (int32_t i = 0; i < got; ++i) {
                const uint32_t at = text + uint32_t(i);

                if (i >= 2 && Char(at - 1) == '\r' && Char(at) == '\r')
                    end = true;
                else if (i >= 2 && Char(at - 1) == '\n' && Char(at) == '\n')
                    end = true;
                else if (i > 3 && Char(at - 3) == '\r' && Char(at - 2) == '\n' && Char(at - 1) == '\r' && Char(at) == '\n')
                    end = true;
            }

            if (end)
                return ok;

            if (n < 0)
                return false;
        } while (got < 0x200);

        const int32_t n = Recv(cpu, mutex, request, socket, globals + gWorker + wSendBuffer, 1, 0);

        if (n != 0)
            return false;
    }
}

// 0x80518708: the header fields ("label: value"), each freed once sent.
uint32_t SendFields(CpuContext* cpu, uint32_t state) {
    const uint32_t request = CurrentRequest(Globals());

    for (uint32_t node; (node = PopHead(request + rHeaders)) != 0;) {
        const uint32_t label = Read(node + fLabel), value = Read(node + fValue);
        uint32_t answer = SendText(cpu, state, label, StrLen(label));

        if (!answer)
            answer = SendText(cpu, state, kColonSpace, 2);

        if (!answer)
            answer = SendText(cpu, state, value, StrLen(value));

        if (!answer)
            answer = SendText(cpu, state, kCrLf, 2);

        if (answer)
            return answer;

        Free(cpu, node);
    }

    return 0;
}

// 0x80518800: raw post data (given or streamed) with its length.
uint32_t SendRawPost(CpuContext* cpu, uint32_t state) {
    const uint32_t globals = Globals();
    const uint32_t mutex = globals + gMutex, request = CurrentRequest(globals);
    int32_t total = 0;

    if (!Read(request + rRawData)) {
        if (!PostLength(cpu, mutex, request, 0, &total, 0))
            return 3;
    } else {
        total = ReadInt(request + rRawLength);
    }

    GuestFrame frame(cpu, 0x40);
    const uint32_t digits = frame[0x10];
    const uint32_t count = Decimal(digits, uint32_t(total));
    uint32_t answer = SendText(cpu, state, kContentLengthField, 0x10);

    if (!answer)
        answer = SendText(cpu, state, digits, count);

    if (!answer)
        answer = SendText(cpu, state, kCrLf, 2);

    if (!answer)
        answer = SendText(cpu, state, kCrLf, 2);

    if (answer)
        return answer;

    if (!Read(request + rRawData))
        return PostSend(cpu, mutex, request, globals + gWorker + wSendBuffer, 0, Read(globals + gSocket), state + sFill, 0);

    return SendText(cpu, state, Read(request + rRawData), Read(request + rRawLength));
}

// The post parts' last (the list head's previous).
bool LastPart(uint32_t request, uint32_t node) {
    return node == Read(Read(request + rPosts));
}

// 0x80518988: multipart/form-data.
uint32_t SendMultipart(CpuContext* cpu, uint32_t state) {
    const uint32_t globals = Globals();
    const uint32_t mutex = globals + gMutex, request = CurrentRequest(globals);
    int32_t total = 0;

    for (uint32_t node = Read(request + rPosts); node; node = Read(node + 4)) {
        total += 0x16;
        const int32_t label = int32_t(StrLen(Read(node + fLabel)));
        total = (Read(node + fBinary) ? label + total + 0x74 : label + total + 0x29) + 2;

        if (!Read(node + fValue)) {
            if (!PostLength(cpu, mutex, request, Read(node + fLabel), &total, 1))
                return 3;
        } else {
            total += ReadInt(node + fLength);
        }

        total += 2;

        if (LastPart(request, node))
            break;
    }

    total += 0x18;
    GuestFrame frame(cpu, 0x40);
    const uint32_t digits = frame[0x10];
    const uint32_t count = Decimal(digits, uint32_t(total));
    const std::initializer_list<std::pair<uint32_t, uint32_t>> header = {
        {kMultipartType, 0x2C}, {request + rBoundary + 2, 0x12}, {kCrLf, 2},
        {kContentLengthField, 0x10}, {digits, count}, {kCrLf, 2}, {kCrLf, 2},
    };

    for (const auto& [text, length] : header) {
        if (const uint32_t answer = SendText(cpu, state, text, length))
            return answer;
    }

    for (uint32_t node = Read(request + rPosts); node; node = Read(node + 4)) {
        const uint32_t label = Read(node + fLabel);
        uint32_t answer = SendText(cpu, state, request + rBoundary, 0x14);

        if (!answer)
            answer = SendText(cpu, state, kCrLf, 2);

        if (!answer)
            answer = SendText(cpu, state, kDisposition, 0x26);

        if (!answer)
            answer = SendText(cpu, state, label, StrLen(label));

        if (!answer)
            answer = SendText(cpu, state, kNameEnd, 3);

        if (!answer && Read(node + fBinary))
            answer = SendText(cpu, state, kOctetStream, 0x4B);

        if (!answer)
            answer = SendText(cpu, state, kCrLf, 2);

        if (answer)
            return answer;

        if (!Read(node + fValue))
            answer = PostSend(cpu, mutex, request, globals + gWorker + wSendBuffer, label, Read(globals + gSocket), state + sFill, 1);
        else
            answer = SendText(cpu, state, Read(node + fValue), Read(node + fLength));

        if (!answer)
            answer = SendText(cpu, state, kCrLf, 2);

        if (answer)
            return answer;

        if (LastPart(request, node))
            break;
    }

    uint32_t answer = SendText(cpu, state, request + rBoundary, 0x14);

    if (!answer)
        answer = SendText(cpu, state, kBoundaryEnd, 4);

    return answer;
}

// Sends a string URL-encoded, through `scratch`.
uint32_t SendEncoded(CpuContext* cpu, uint32_t state, uint32_t text, uint32_t scratch) {
    for (int32_t c; (c = Char(text)) != 0; ++text) {
        const uint32_t length = EncodeChar(scratch, uint32_t(c));

        if (const uint32_t answer = SendText(cpu, state, scratch, length))
            return answer;
    }

    return 0;
}

// 0x80518D2C: application/x-www-form-urlencoded.
uint32_t SendForm(CpuContext* cpu, uint32_t state) {
    const uint32_t globals = Globals();
    const uint32_t mutex = globals + gMutex, request = CurrentRequest(globals);
    int32_t total = 0;

    for (uint32_t node = Read(request + rPosts); node; node = Read(node + 4)) {
        total += EncodedLength(Read(node + fLabel)) + 1;

        if (!Read(node + fValue)) {
            if (!PostLength(cpu, mutex, request, Read(node + fLabel), &total, 2))
                return 3;
        } else {
            total += EncodedLength(Read(node + fValue));
        }

        if (LastPart(request, node))
            break;

        total += 1;
    }

    GuestFrame frame(cpu, 0x40);
    const uint32_t scratch = frame[0x10];
    const uint32_t count = Decimal(scratch, uint32_t(total));
    const std::initializer_list<std::pair<uint32_t, uint32_t>> header = {
        {kFormType, 0x31}, {kContentLengthField, 0x10}, {scratch, count}, {kCrLf, 2}, {kCrLf, 2},
    };

    for (const auto& [text, length] : header) {
        if (const uint32_t answer = SendText(cpu, state, text, length))
            return answer;
    }

    for (uint32_t node = Read(request + rPosts); node; node = Read(node + 4)) {
        if (const uint32_t answer = SendEncoded(cpu, state, Read(node + fLabel), scratch))
            return answer;

        if (const uint32_t answer = SendText(cpu, state, kEquals, 1))
            return answer;

        if (!Read(node + fValue)) {
            if (const uint32_t answer = PostSend(cpu, mutex, request, globals + gWorker + wSendBuffer, Read(node + fLabel), Read(globals + gSocket), state + sFill, 2))
                return answer;
        } else if (const uint32_t answer = SendEncoded(cpu, state, Read(node + fValue), scratch)) {
            return answer;
        }

        if (LastPart(request, node))
            break;

        if (const uint32_t answer = SendText(cpu, state, kAmpersand, 1))
            return answer;
    }

    return 0;
}

// 0x80518FF0: the request is over: closes the socket unless kept alive,
// records the error, frees the request and completes its connection.
void Finish(CpuContext* cpu, uint32_t state) {
    const uint32_t globals = Globals();
    const uint32_t current = globals + gCurrent, mutex = globals + gMutex;
    const uint32_t request = CurrentRequest(globals);
    const uint32_t response = Read(request + rResponse);
    const uint32_t connection = Connections(cpu, mutex, request, kByRequest);

    if (Read(request + rCancel)) {
        Write(state + sError, 8);
        Write(state + sKeepAlive, 0);
    }

    if (!Read(state + sKeepAlive) && ReadInt(globals + gSocket) >= 0) {
        if (CloseSocket(cpu, mutex, request, Read(globals + gSocket)) < 0)
            Write(state + sError, 10);

        Write(globals + gSocket, 0xFFFFFFFF);
    }

    if (!Read(state + sError)) {
        Write(response + pDone, 1);
    } else {
        Write(response + pDone, 0);
        Write(globals + gError, Read(state + sError));

        if (Read(response + pBuffer) == state + sDiscard) {
            Write(response + pBuffer, 0);
            Write(response + pBufferSize, 0);
        }
    }

    if (connection)
        Write(connection + cError, Read(state + sError));

    Lock(cpu, mutex);
    Free(cpu, Read(current));
    Write(current, 0);
    Unlock(cpu, mutex);
    FreeRequest(cpu, mutex, request);

    if (connection && Read(response + pDone))
        Write(connection + cState, 5);

    Complete(cpu, mutex, connection);

    if (connection)
        Wakeup(cpu, connection);
}

// 0x80519170: the next queued request; waits for one when none is.
bool Next(CpuContext* cpu, uint32_t state) {
    const uint32_t globals = Globals();
    const uint32_t mutex = globals + gMutex;
    Lock(cpu, mutex);
    const uint32_t node = PopHead(globals + gQueue);

    if (!node) {
        Write(state + sId, 0xFFFFFFFF);
    } else {
        Write(state + sId, Read(node + qId));
        Write(globals + gCurrent, node);
    }

    Unlock(cpu, mutex);
    const int32_t id = ReadInt(state + sId);

    if (id < 0)
        WorkerWait(cpu, globals + gWorker);

    return id >= 0;
}

// 0x80519228: the server's (or proxy's) address and port; a different one
// ends the kept connection.
bool Resolve(CpuContext* cpu, uint32_t state) {
    const uint32_t request = CurrentRequest(Globals());
    const bool proxy = Read(request + rProxy) != 0;
    const uint32_t host = proxy ? request + rProxyAddress : Read(request + rHost);
    const uint32_t length = StrLen(host);
    bool same = length != 0;

    for (uint32_t i = 0; same && i <= length; ++i)
        same = Memory::Read8(host + i) == Memory::Read8(state + sHost + i);

    if (!same) {
        const uint32_t address = ResolveHost(cpu, host);
        Write(state + sAddress, address);

        if (!address) {
            Write(state + sError, proxy ? 0xC : 4);
            return false;
        }
    } else {
        Write(state + sAddress, Read(state + sLastAddress));
    }

    Clear(cpu, state + sHost, 0x100);
    Copy(cpu, state + sHost, host, StrLen(host));
    Write(state + sPort, Read(request + (proxy ? rProxyPort : rPort)));

    if (Read(state + sAddress) != Read(state + sLastAddress) || Read(state + sPort) != Read(state + sLastPort) || Read(request + rHttps) == 1)
        Write(state + sKeepAlive, 0);

    Write(state + sLastAddress, Read(state + sAddress));
    Write(state + sLastPort, Read(state + sPort));
    return true;
}

// 0x8051937C: the socket: the kept one while it lives, else a new one,
// connected.
bool Open(CpuContext* cpu, uint32_t state) {
    const uint32_t globals = Globals();
    const uint32_t mutex = globals + gMutex, request = CurrentRequest(globals);
    bool dead = false;

    if (Read(state + sKeepAlive) == 1 && SocketAlive(cpu, ReadInt(globals + gSocket)) == -1) {
        dead = true;
        Write(state + sKeepAlive, 0);
    }

    if (Read(state + sKeepAlive)) {
        Lock(cpu, mutex);
        Write(Read(globals + gCurrent) + qSocket, Read(globals + gSocket));
        Unlock(cpu, mutex);
        return true;
    }

    if (ReadInt(globals + gSocket) >= 0 && CloseSocket(cpu, mutex, request, Read(globals + gSocket)) < 0 && !dead) {
        Write(globals + gSocket, 0xFFFFFFFF);
        Write(state + sError, 10);
        return false;
    }

    const int32_t socket = OpenSocket(cpu, request);
    Write(globals + gSocket, uint32_t(socket));

    if (socket < 0) {
        Write(state + sError, 3);
        return false;
    }

    Lock(cpu, mutex);
    Write(Read(globals + gCurrent) + qSocket, Read(globals + gSocket));
    Unlock(cpu, mutex);

    if (Read(request + rCancel))
        return false;

    if (Connect(cpu, globals, mutex, request, Read(globals + gSocket), Read(state + sAddress), Read(state + sPort)) < 0) {
        if (Read(request + rProxy))
            Write(state + sError, 0xD);
        else
            Write(state + sError, Read(globals + gSslError) ? 0xE : 5);

        return false;
    }

    return true;
}

// 0x80519538: https through a proxy: CONNECT, its answer, then SSL. 0 to
// go on, 1 on an error, 2 to retry.
int32_t Tunnel(CpuContext* cpu, uint32_t state) {
    const uint32_t globals = Globals();
    const uint32_t mutex = globals + gMutex, request = CurrentRequest(globals);
    Write(state + sError, 10);
    Write(state + sFill, 0);

    if (!Read(request + rHttps) || !Read(request + rProxy))
        return 0;

    if (const uint32_t answer = ProxyConnect(cpu, state))
        return int32_t(answer);

    if (!ProxyAnswer(cpu, state))
        return 1;

    const int32_t result = SslConnect(cpu, globals, mutex, request, Read(globals + gSocket));

    if (!result)
        return 0;

    if (Read(globals + gSslError))
        Write(state + sError, result == -1004 ? 0x10 : result == -1005 ? 0x11 : 0xE);

    return 1;
}

// 0x8051968C: the request line, Host, the authorizations, the header
// fields and the post data. 0, 1 on an error, 2 to retry, 3 on a post data
// callback error.
uint32_t SendRequest(CpuContext* cpu, uint32_t state) {
    const uint32_t globals = Globals();
    const uint32_t mutex = globals + gMutex, request = CurrentRequest(globals);
    const uint32_t connection = Connections(cpu, mutex, request, kByRequest);
    const uint32_t url = Read(request + rUrl);
    const int32_t urlLength = int32_t(StrLen(url));
    Write(state + sError, 10);
    uint32_t answer = 0;

    if (connection)
        Write(connection + cState, 2);

    Write(state + sFill, 0);
    const int32_t method = ReadInt(request + rMethod);

    if (method == 1)
        answer = SendText(cpu, state, kPost, 5);
    else if (method == 0)
        answer = SendText(cpu, state, kGet, 4);
    else if (method == 2)
        answer = SendText(cpu, state, kHead, 5);

    if (answer)
        return answer;

    if (!Read(request + rProxy) || Read(request + rHttps)) {
        const int32_t path = ReadInt(request + rPathStart);
        answer = path < urlLength ? SendText(cpu, state, url + uint32_t(path), uint32_t(urlLength - path)) : SendText(cpu, state, kSlash, 1);
    } else {
        answer = SendText(cpu, state, url, uint32_t(urlLength));
    }

    if (!answer)
        answer = SendText(cpu, state, kRequestEnd, 0xB);

    const uint32_t skip = Read(request + rHttps) ? 8 : 7;

    if (!answer)
        answer = SendText(cpu, state, kHostField, 6);

    if (!answer)
        answer = SendText(cpu, state, url + skip, Read(request + rHostEnd) - skip);

    if (!answer)
        answer = SendText(cpu, state, kCrLf, 2);

    if (!answer && Read(request + rProxy) && !Read(request + rHttps))
        answer = SendProxyAuth(cpu, state);

    if (answer)
        return answer;

    const uint32_t current = CurrentRequest(Globals());
    answer = SendAuth(cpu, state, kAuthField, 0x15, current + rAuth, current + rAuthLength);

    if (!answer)
        answer = SendFields(cpu, state);

    if (answer)
        return answer;

    if (ReadInt(request + rMethod) == 1) {
        if (!Read(request + rRawPost)) {
            bool multipart = false;

            if (!Read(request + rPostType)) {
                for (uint32_t node = Read(request + rPosts); node; node = Read(node + 4)) {
                    if (Read(node + fBinary)) {
                        multipart = true;
                        break;
                    }

                    if (LastPart(request, node))
                        break;
                }
            } else {
                multipart = Read(request + rPostType) == 2;
            }

            answer = multipart ? SendMultipart(cpu, state) : SendForm(cpu, state);
        } else {
            answer = SendRawPost(cpu, state);
        }

        if (answer) {
            if (answer == 3)
                Write(state + sError, 3);

            return answer;
        }
    } else if ((answer = SendText(cpu, state, kCrLf, 2)) != 0) {
        return answer;
    }

    return Flush(cpu, globals, state, request);
}

// 0x80519B28: the header, a byte at a time up to an empty line, into the
// response (0x400 bytes, then 0x200-byte chunks).
bool ReceiveHeader(CpuContext* cpu, uint32_t state) {
    const uint32_t globals = Globals();
    const uint32_t mutex = globals + gMutex, request = CurrentRequest(globals);
    const uint32_t response = Read(request + rResponse);
    const uint32_t connection = Connections(cpu, mutex, request, kByRequest);
    uint8_t ring[4] = {};

    if (connection)
        Write(connection + cState, 3);

    Write(response + pHeaderLength, 0);
    Clear(cpu, state + sStatusLine, 0xE);
    uint32_t chunk = Read(response + pChunks);
    Write(state + sHeaderLength, 0);

    for (;;) {
        if (Read(request + rCancel))
            return false;

        const uint32_t at = Read(state + sHeaderLength);
        uint32_t link = chunk, to;

        if (int32_t(at) < 0x400) {
            to = response + pInline + at;
        } else {
            if ((at & 0x1FF) == 0) {
                link = Alloc(cpu, 0x204, 4);
                Write(chunk ? chunk : response + pChunks, link);

                if (!link) {
                    Write(state + sError, 1);
                    return false;
                }

                Write(link, 0);
            }

            to = link + (at & 0x1FF) + 4;
        }

        const int32_t n = Recv(cpu, mutex, request, Read(globals + gSocket), to, 1, 0);
        ring[Read(state + sHeaderLength) & 3] = Memory::Read8(to);

        if (n < 1) {
            Write(state + sError, 10);
            return false;
        }

        Write(state + sHeaderLength, Read(state + sHeaderLength) + uint32_t(n));
        chunk = link;

        if (HeaderEnd(ring, Read(state + sHeaderLength)))
            break;
    }

    Write(response + pHeaderLength, Read(state + sHeaderLength));

    if (!Read(response + pHeaderLength)) {
        Write(state + sError, 7);
        return false;
    }

    return true;
}

// 0x80519D14: the status line and the fields the body depends on:
// Content-Length, Connection (keep-alive) and Transfer-Encoding (chunked).
bool ParseHeader(CpuContext* cpu, uint32_t state) {
    const uint32_t globals = Globals();
    const uint32_t request = CurrentRequest(globals);
    const uint32_t response = Read(request + rResponse);
    const uint32_t scratch = globals + gWorker + wSendBuffer;
    const auto bad = [&] {
        Write(state + sError, 7);
        return false;
    };

    if (!CopyHeader(cpu, response, state + sStatusLine, 0, 0xE))
        return bad();

    if (CaseCompare(state + sStatusLine, kStatusStart, 5) != 0 || Char(state + sStatusLine + 8) != ' ')
        return bad();

    const int32_t status = ParseDecimal(state + sStatusLine + 9, 3);
    Write(response + pStatus, uint32_t(status));

    if (status < 0)
        return bad();

    int32_t colon = 0;

    if (FindLine(response, 0xC, ReadInt(response + pHeaderLength), &colon, nullptr) < 0)
        return bad();

    int32_t at = 0;
    const int32_t length = FindField(response, kContentLength, &at);
    Write(state + sContentLength, uint32_t(length));

    if (length == 0) {
        Write(state + sError, 0);
        return false;
    }

    if (length > 0x100)
        return bad();

    if (length < 1) {
        Write(response + pContentLength, 0xFFFFFFFF);
    } else {
        if (!CopyHeader(cpu, response, scratch, at, uint32_t(length)))
            return bad();

        const int32_t value = ParseDecimal(scratch, ReadInt(state + sContentLength));
        Write(state + sContentLength, uint32_t(value));

        if (value < 0)
            return bad();

        Write(response + pContentLength, uint32_t(value));
    }

    if (!Read(request + rHttps)) {
        const int32_t field = FindField(response, kConnection, &at);

        if (field == 0) {
            Write(state + sError, 7);
            Write(state + sKeepAlive, 0);
            return false;
        }

        Write(state + sKeepAlive, CaseCompare(state + sStatusLine, kHttp11, 8) == 0 ? 1 : 0);

        if (field < 0x101 && field > 0) {
            if (MatchToken(response, at, at + field, kKeepAlive, 0) == 0)
                Write(state + sKeepAlive, 1);

            if (MatchToken(response, at, at + field, kClose, 0) == 0)
                Write(state + sKeepAlive, 0);
        }
    } else {
        Write(state + sKeepAlive, 0);
    }

    const int32_t encoding = FindField(response, kTransferEncoding, &at);
    Write(state + sChunked, uint32_t(encoding));

    if (encoding == 0)
        return bad();

    uint32_t chunked = 0;

    if (encoding < 0x101 && encoding > 0)
        chunked = MatchToken(response, at, at + encoding, kChunked, ';') == 0;

    Write(state + sChunked, chunked);
    Write(state + sError, 0);
    Write(response + pHeaderDone, 1);
    return true;
}

// Reads one byte at a time into a 2-byte ring until the last two were CR
// LF; false when a read fails first.
bool SkipLine(CpuContext* cpu, uint32_t mutex, uint32_t request, uint32_t socket, uint32_t ring) {
    Memory::Write8(ring, 0);
    Memory::Write8(ring + 1, 0);
    int32_t n = 0, skipped = 0;

    for (uint32_t k = 0;; ++k) {
        if (Memory::Read8(ring + (k & 1)) == '\r' && Memory::Read8(ring + ((k - 1) & 1)) == '\n') {
            n = skipped;
            break;
        }

        n = Recv(cpu, mutex, request, socket, ring + (k & 1), 1, 0);

        if (n < 1)
            break;

        skipped += n;
    }

    return n >= 1;
}

// The caller's buffer is full: the rest goes to the worker's discard area
// (error 6, the buffer too small).
void Discard(CpuContext* cpu, uint32_t mutex, uint32_t state, uint32_t response, bool check) {
    if ((!check || ReadInt(state + sError) != 6) && !HasRoom(cpu, mutex, response)) {
        Write(state + sError, 6);
        Write(response + pBuffer, state + sDiscard);
        Write(response + pBufferSize, 0x200);
    }
}

// 0x8051A044: the body: chunked, by Content-Length or up to the close;
// none for HEAD, 1xx, 204 and 304.
bool ReceiveBody(CpuContext* cpu, uint32_t state) {
    const uint32_t globals = Globals();
    const uint32_t mutex = globals + gMutex, request = CurrentRequest(globals);
    const uint32_t response = Read(request + rResponse);
    const uint32_t connection = Connections(cpu, mutex, request, kByRequest);
    const uint32_t scratch = globals + gWorker + wSendBuffer, socket = Read(globals + gSocket);
    const int32_t status = ReadInt(response + pStatus);

    if (ReadInt(request + rMethod) == 2 || status == 204 || status == 304 || (status > 99 && status < 200))
        return true;

    SetProgress(connection, 0);

    if (connection)
        Write(connection + cState, 4);

    const auto received = [&](uint32_t n) {
        Write(response + pReceived, Read(response + pReceived) + n);
        Write(response + pTotal, Read(response + pTotal) + n);
    };

    if (ReadInt(state + sContentLength) < 0) {
        Write(state + sError, 10);

        if (Read(state + sChunked)) {
            GuestFrame frame(cpu, 0x30);
            const uint32_t ring = frame[0x10];
            int32_t size = -1;

            for (;;) {
                // The chunk's size line (hex, extensions after ';' skipped).
                int8_t pair[2] = {};
                Write(state + sHeaderLength, 0);

                while (ReadInt(state + sHeaderLength) < 0x100) {
                    if (Recv(cpu, mutex, request, socket, scratch + Read(state + sHeaderLength), 1, 0) < 0)
                        return false;

                    uint32_t at = Read(state + sHeaderLength);
                    const int8_t c = Char(scratch + at);
                    pair[at & 1] = c;

                    if (c != ';' && (c != '\n' || pair[(at - 1) & 1] != '\r')) {
                        Write(state + sHeaderLength, at + 1);
                        continue;
                    }

                    if (c == '\n')
                        --at;
                    else if (!SkipLine(cpu, mutex, request, socket, ring))
                        return false;

                    if (at == 0)
                        return false;

                    size = ParseHex(scratch, int32_t(at));

                    if (size < 0)
                        return false;

                    break;
                }

                if (Read(state + sHeaderLength) == 0x100) {
                    Write(state + sError, 7);
                    return false;
                }

                if (size < 1) {
                    SkipLine(cpu, mutex, request, socket, ring);
                    Write(state + sError, 0);
                    break;
                }

                SetProgress(connection, uint32_t(size));

                while (size > 0) {
                    Discard(cpu, mutex, state, response, true);
                    const bool discard = ReadInt(state + sError) == 6;
                    const int32_t n = RecvAtMost(cpu, mutex, request, socket, discard ? 0 : Read(response + pReceived), size, 0);

                    if (n < 1)
                        return false;

                    size -= n;
                    received(uint32_t(n));

                    if (size == 0 && Recv(cpu, mutex, request, socket, scratch, 2, 0) < 1)
                        return false;
                }
            }
        } else {
            for (;;) {
                Discard(cpu, mutex, state, response, false);
                const bool discard = ReadInt(state + sError) == 6;
                const int32_t n = RecvRest(cpu, mutex, request, socket, discard ? 0 : Read(response + pReceived), 0);

                if (n < 0)
                    return false;

                if (n == 0)
                    break;

                received(uint32_t(n));
            }

            if (ReadInt(state + sError) != 6)
                Write(state + sError, 0);
        }
    } else {
        SetProgress(connection, Read(state + sContentLength));

        while (ReadInt(state + sContentLength) > 0) {
            Discard(cpu, mutex, state, response, true);
            const bool discard = ReadInt(state + sError) == 6;
            const int32_t n = RecvAtMost(cpu, mutex, request, socket, discard ? 0 : Read(response + pReceived), ReadInt(state + sContentLength), 0);

            if (n < 0)
                return false;

            if (n == 0)
                break;

            if (ReadInt(state + sError) != 6)
                received(uint32_t(n));

            Write(state + sContentLength, Read(state + sContentLength) - uint32_t(n));
        }

        if (ReadInt(state + sError) != 6) {
            if (ReadInt(state + sContentLength) == 0)
                Write(state + sError, 0);
            else
                Write(state + sError, AtLeast(response, Read(response + pReceived)) ? 6 : 10);
        }
    }

    const uint32_t done = Connections(cpu, mutex, response, kByResponse);

    if (!Read(state + sError) && done)
        BufferCallback(cpu, mutex, done, 3);

    return true;
}

// 0x8051A634: the worker: takes requests off the queue and works each to
// its end; a send that took nothing retries the request.
void Worker(CpuContext* cpu) {
    const uint32_t globals = Globals();
    GuestFrame frame(cpu, kStateSize + 0x20);
    const uint32_t state = frame[0x10];
    Write(state + sId, 0xFFFFFFFF);
    Clear(cpu, state + sHost, 0x100);
    Clear(cpu, state + sDiscard, 0x200);
    Write(state + sAddress, 0xFFFFFFFF);
    Write(state + sLastAddress, 0xFFFFFFFF);
    Write(state + sFill, 0);
    Write(state + sKeepAlive, 0);
    Write(state + sChunked, 0);
    Write(state + sResume, 0);
    Write(state + sContentLength, 0);
    Write(state + sError, 0);
    bool resume = false;

    while (!Read(globals + gQuit)) {
        if (!resume) {
            if (!Next(cpu, state))
                continue;

            if (Read(CurrentRequest(globals) + rCancel) || !Resolve(cpu, state)) {
                Finish(cpu, state);
                continue;
            }
        }

        resume = false;

        if (!Open(cpu, state)) {
            Finish(cpu, state);
            continue;
        }

        const int32_t tunnel = Tunnel(cpu, state);

        if (tunnel == 1) {
            Finish(cpu, state);
            continue;
        }

        if (tunnel == 2) {
            resume = true;
            continue;
        }

        const uint32_t sent = SendRequest(cpu, state);

        if (sent == 2) {
            resume = true;
            continue;
        }

        if (sent == 1 || sent == 3 || Read(CurrentRequest(globals) + rCancel) || !ReceiveHeader(cpu, state) || !ParseHeader(cpu, state)) {
            Finish(cpu, state);
            continue;
        }

        ReceiveBody(cpu, state);
        Finish(cpu, state);
    }
}

// 0x8051A838: a connection with a request for `url`; listed.
uint32_t CreateConnection(CpuContext* cpu, uint32_t url, int32_t method, uint32_t buffer, uint32_t size, uint32_t callback, uint32_t param) {
    const uint32_t globals = Globals();
    const uint32_t mutex = globals + gMutex;
    const uint32_t connection = Alloc(cpu, 0x8060, 0x20);

    if (!connection) {
        Write(globals + gError, 1);
        return 0;
    }

    Clear(cpu, connection, 0x8060);
    const uint32_t request = CreateRequest(cpu, globals, url, method, buffer, size, param, 0, 0);
    Write(connection + cRequest, request);

    if (!request) {
        Free(cpu, connection);
        return 0;
    }

    Write(connection + cResponse, Read(request + rResponse));
    Write(connection + cState, 0);
    Write(connection + cCallback, callback);
    Write(connection + cPostData, 0);
    Write(connection + cPostLength, 0);
    Write(connection + cId, 0xFFFFFFFF);
    Connections(cpu, mutex, connection, kPush);
    Write(connection + cError, 0xF);
    Write(connection + cSslError, 0);
    Write(connection + cBusy, 1);
    SetProgress(connection, 0);
    Write(connection + cUserCallback, 0);
    Write(connection + cBufferAt, 0);
    Write(connection + cBufferCount, 0);
    Clear(cpu, connection + cBuffer, 0x8000);
    return connection;
}

// 0x8051A980: queues the connection's request.
int32_t StartConnection(CpuContext* cpu, uint32_t connection) {
    const uint32_t globals = Globals();
    const uint32_t found = Connections(cpu, globals + gMutex, connection, kByConnection);

    if (!found || !Read(found + cRequest))
        return -1;

    const int32_t id = Queue(cpu, globals, Read(found + cRequest));
    Write(found + cId, uint32_t(id));

    if (id >= 0)
        Write(found, 1);

    return 0;
}

// 0x8051AA0C: the body's buffer and size; answers the received length.
int32_t Body(CpuContext* cpu, uint32_t connection, uint32_t buffer, uint32_t size) {
    const uint32_t mutex = Globals() + gMutex;
    const uint32_t found = Connections(cpu, mutex, connection, kByConnection);
    const uint32_t response = found ? ResponseOf(cpu, mutex, found) : 0;

    if (!response)
        return -1;

    Write(buffer, Read(response + pBuffer));
    Write(size, Read(response + pBufferSize));
    return ReadInt(response + pReceived);
}

// 0x8051AAA8: the caller's parameter.
uint32_t UserParam(CpuContext* cpu, uint32_t connection) {
    const uint32_t mutex = Globals() + gMutex;
    const uint32_t found = Connections(cpu, mutex, connection, kByConnection);
    const uint32_t response = found ? ResponseOf(cpu, mutex, found) : 0;
    return response ? Read(response + pParam) : 0;
}

// 0x8051AB1C.
int32_t ErrorOf(CpuContext* cpu, uint32_t connection) {
    const uint32_t found = Connections(cpu, Globals() + gMutex, connection, kByConnection);
    return found ? ReadInt(found + cError) : -1;
}

// 0x8051AC70: the connections' callback: streamed post data (1) from the
// request's callback, the body buffer (2) from the response's grow
// callback, the end (4) to the caller's callback.
int32_t Dispatch(CpuContext* cpu, uint32_t connection, int32_t kind, uint32_t args) {
    const uint32_t mutex = Globals() + gMutex;
    const uint32_t found = Connections(cpu, mutex, connection, kByConnection);
    GuestCall call{cpu};

    if (kind == 1) {
        const uint32_t request = found ? RequestOf(cpu, mutex, found) : 0;
        const uint32_t callback = request ? Read(request + rPostCallback) : 0;

        if (!callback)
            return -1;

        const uint32_t param = UserParam(cpu, found);
        return int32_t(call(callback, {Read(args), args + 4, args + 8, Read(args + 12), param}));
    }

    if (kind == 2) {
        const uint32_t response = found ? ResponseOf(cpu, mutex, found) : 0;
        const uint32_t grow = response ? Read(response + pGrow) : 0;

        if (grow) {
            GuestFrame frame(cpu, 0x20);
            const uint32_t previous = frame[0x10];
            Write(previous, Read(args));
            const uint32_t expected = Progress(found), param = UserParam(cpu, found);
            const uint32_t buffer = call(grow, {previous, args + 4, expected, kAllocAddress, kFreeAddress, param});
            Write(args, buffer);

            if (buffer && Read(previous))
                Write(args + 8, 0);
        }

        return 0;
    }

    if (kind == 4) {
        const uint32_t callback = found ? Read(found + cUserCallback) : 0;
        const uint32_t response = callback ? ResponseOf(cpu, mutex, found) : 0;

        if (response) {
            const uint32_t error = uint32_t(ErrorOf(cpu, found)), param = UserParam(cpu, found);
            call(callback, {error, response, param});
        }
    }

    return 0;
}

// 0x8051AE54: a request with the caller's callback and buffer callbacks;
// answers the request (its handle).
uint32_t CreateRequestEx(CpuContext* cpu, uint32_t url, int32_t method, uint32_t buffer, uint32_t size, uint32_t callback, uint32_t param, uint32_t grow, uint32_t release) {
    const uint32_t connection = CreateConnection(cpu, url, method, buffer, size, kDispatchAddress, param);

    if (!connection)
        return 0;

    const uint32_t globals = Globals();
    const uint32_t mutex = globals + gMutex;
    const uint32_t found = Connections(cpu, mutex, connection, kByConnection);
    const uint32_t request = RequestOf(cpu, mutex, found);

    if (!request)
        return 0;

    if (!Read(request + rResponse)) {
        DiscardRequest(cpu, globals, request);
        Connections(cpu, mutex, found, kRemove);
        Free(cpu, found);
        return 0;
    }

    Write(found + cUserCallback, callback);
    Write(Read(request + rResponse) + pGrow, grow);
    Write(Read(request + rResponse) + pRelease, release);
    return request;
}

// Registers the library's version string once.
void RegisterVersion(CpuContext* cpu, uint32_t done, uint32_t version) {
    if (!Read(done)) {
        GuestCall{cpu}.Call<OSRegisterVersion>({Read(version)});
        Write(done, 1);
    }
}

// 0x8051AFA8 / 0x8051B06C: a header field or post part, before sending.
int32_t AddBeforeStart(CpuContext* cpu, uint32_t handle, uint32_t label, uint32_t value, bool post) {
    const uint32_t globals = Globals();
    const uint32_t request = RequestOrSelf(cpu, globals + gMutex, handle);

    if (!request)
        return -1;

    if (Read(request + rStarted)) {
        Print(cpu, kMsgAfterStart, post ? kAddPostDataAsciiName : kAddHeaderFieldName);
        return -1;
    }

    const bool added = post ? AddPost(cpu, request, globals, label, value) : AddHeader(cpu, request, globals, label, value);
    return added ? 0 : -1;
}

// 0x8051B374: the proxy (at most 256 characters) with optional basic
// authentication (user and password at most 32 each).
int32_t SetProxy(CpuContext* cpu, uint32_t handle, uint32_t address, uint32_t port, uint32_t user, uint32_t password) {
    const uint32_t request = RequestOrSelf(cpu, Globals() + gMutex, handle);

    if (!request || !address)
        return -1;

    const uint32_t length = StrLen(address);

    if (int32_t(length) > 0x100) {
        Print(cpu, kMsgProxyTooLong);
        return -1;
    }

    Copy(cpu, request + rProxyAddress, address, length);
    Write(request + rProxyPort, port);

    if (user && password) {
        const uint32_t userLength = StrLen(user), passwordLength = StrLen(password);

        if (int32_t(userLength) > 0x20) {
            Print(cpu, kMsgUserTooLong);
            return -1;
        }

        if (int32_t(passwordLength) > 0x20) {
            Print(cpu, kMsgPasswordTooLong);
            return -1;
        }

        GuestFrame frame(cpu, 0x70);
        const uint32_t text = frame[0x8];
        Clear(cpu, text, 0x41);
        Copy(cpu, text, user, userLength);
        Copy(cpu, text + userLength, kUserSeparator, 1);
        Copy(cpu, text + userLength + 1, password, passwordLength);
        Write(request + rProxyAuthLength, Base64(request + rProxyAuth, text));
    }

    Write(request + rProxy, 1);
    return 0;
}

// 0x8051B4E4: the interface's proxy (its https one for https).
int32_t SetProxyDefault(CpuContext* cpu, uint32_t handle) {
    const uint32_t globals = Globals();
    const uint32_t request = RequestOrSelf(cpu, globals + gMutex, handle);
    const uint32_t proxy = globals + (Read(request + rHttps) ? gHttpsProxy : gHttpProxy);

    if (Memory::Read8(proxy + xEnabled) != 1)
        return -1;

    uint32_t user = 0, password = 0;

    if (Memory::Read8(proxy + xAuth) == 1) {
        user = proxy + xUser;
        password = proxy + xPassword;
    }

    const uint32_t port = Memory::Read16(proxy + xPort);
    Report(cpu, kMsgUsingProxy, proxy + xServer, port, user ? user : kNoAuth, password ? password : kNoAuth);
    const int32_t result = SetProxy(cpu, request, proxy + xServer, port, user, password);

    if (result >= 0)
        return 0;

    Report(cpu, kMsgSetProxyFailed, uint32_t(result));
    Panic(cpu, kFileNhttp, 0x3DC, kSetProxyName);
    return -1;
}

uint32_t Arg(CpuContext* cpu, int n) {
    return cpu->gpr[n];
}

int32_t IntArg(CpuContext* cpu, int n) {
    return int32_t(cpu->gpr[n]);
}

} // namespace

extern "C" void MH3_NHTTPResetGlobals(CpuContext* cpu) {
    ResetGlobals(Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x80514C38, MH3_NHTTPResetGlobals, "NHTTPi_InitGlobals");

extern "C" void MH3_NHTTPAlloc(CpuContext* cpu) {
    cpu->gpr[3] = Alloc(cpu, Arg(cpu, 3), Arg(cpu, 4));
}
REGISTER_NATIVE_FUNCTION_AS(0x80514C68, MH3_NHTTPAlloc, "NHTTPi_Alloc");

extern "C" void MH3_NHTTPFree(CpuContext* cpu) {
    Free(cpu, Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x80514CC4, MH3_NHTTPFree, "NHTTPi_Free");

extern "C" void MH3_NHTTPSetError(CpuContext* cpu) {
    Write(Arg(cpu, 3) + gError, Arg(cpu, 4));
}
REGISTER_NATIVE_FUNCTION_AS(0x80514D08, MH3_NHTTPSetError, "NHTTPi_SetError");

extern "C" void MH3_NHTTPSetSslError(CpuContext* cpu) {
    Write(Arg(cpu, 3) + gSslError, Arg(cpu, 4));
}
REGISTER_NATIVE_FUNCTION_AS(0x80514D10, MH3_NHTTPSetSslError, "NHTTPi_SetSSLError");

extern "C" void MH3_NHTTPSslErrorOf(CpuContext* cpu) {
    cpu->gpr[3] = Read(Arg(cpu, 3) + gSslError);
}
REGISTER_NATIVE_FUNCTION_AS(0x80514D18, MH3_NHTTPSslErrorOf, "NHTTPi_GetSSLError");

extern "C" void MH3_NHTTPStartupWith(CpuContext* cpu) {
    cpu->gpr[3] = Startup(cpu, Arg(cpu, 3), Arg(cpu, 4), Arg(cpu, 5), Arg(cpu, 6)) ? 1 : 0;
}
REGISTER_NATIVE_FUNCTION_AS(0x80514D20, MH3_NHTTPStartupWith, "NHTTPi_Startup");

extern "C" void MH3_NHTTPCleanupWith(CpuContext* cpu) {
    Cleanup(cpu, Arg(cpu, 3), Arg(cpu, 4));
}
REGISTER_NATIVE_FUNCTION_AS(0x80514EB0, MH3_NHTTPCleanupWith, "NHTTPi_CleanupAsync");

extern "C" void MH3_NHTTPErrorOf(CpuContext* cpu) {
    cpu->gpr[3] = Read(Arg(cpu, 3) + gError);
}
REGISTER_NATIVE_FUNCTION_AS(0x80514FA0, MH3_NHTTPErrorOf, "NHTTPi_GetError");

extern "C" void MH3_NHTTPAddField(CpuContext* cpu) {
    cpu->gpr[3] = AddField(cpu, Arg(cpu, 3), Arg(cpu, 4), Arg(cpu, 5), Arg(cpu, 6)) ? 1 : 0;
}
REGISTER_NATIVE_FUNCTION_AS(0x80514FA8, MH3_NHTTPAddField, "NHTTPi_SetList");

extern "C" void MH3_NHTTPPopHead(CpuContext* cpu) {
    cpu->gpr[3] = PopHead(Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x805150E4, MH3_NHTTPPopHead, "NHTTPi_PopList");

extern "C" void MH3_NHTTPAddHeader(CpuContext* cpu) {
    cpu->gpr[3] = AddHeader(cpu, Arg(cpu, 3), Arg(cpu, 4), Arg(cpu, 5), Arg(cpu, 6)) ? 1 : 0;
}
REGISTER_NATIVE_FUNCTION_AS(0x8051512C, MH3_NHTTPAddHeader, "NHTTPi_AddHeaderField");

extern "C" void MH3_NHTTPAddPost(CpuContext* cpu) {
    cpu->gpr[3] = AddPost(cpu, Arg(cpu, 3), Arg(cpu, 4), Arg(cpu, 5), Arg(cpu, 6)) ? 1 : 0;
}
REGISTER_NATIVE_FUNCTION_AS(0x8051514C, MH3_NHTTPAddPost, "NHTTPi_AddPostDataAscii");

extern "C" void MH3_NHTTPQueueInit(CpuContext* cpu) {
    Write(Arg(cpu, 3), 0);
    Write(Arg(cpu, 3) + 4, 0);
}
REGISTER_NATIVE_FUNCTION_AS(0x805152D8, MH3_NHTTPQueueInit, "NHTTPi_InitRequestQueue");

extern "C" void MH3_NHTTPQueueAdd(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(QueueAdd(cpu, Arg(cpu, 3), Arg(cpu, 4)));
}
REGISTER_NATIVE_FUNCTION_AS(0x805152E8, MH3_NHTTPQueueAdd, "NHTTPi_AddRequestQueue");

extern "C" void MH3_NHTTPQueueRemove(CpuContext* cpu) {
    cpu->gpr[3] = QueueRemove(cpu, Arg(cpu, 3), Arg(cpu, 4), IntArg(cpu, 5)) ? 1 : 0;
}
REGISTER_NATIVE_FUNCTION_AS(0x805153B4, MH3_NHTTPQueueRemove, "NHTTPi_CancelRequestQueue");

extern "C" void MH3_NHTTPQueueClear(CpuContext* cpu) {
    QueueClear(cpu, Arg(cpu, 3), Arg(cpu, 4));
}
REGISTER_NATIVE_FUNCTION_AS(0x805154D0, MH3_NHTTPQueueClear, "NHTTPi_AllCancelRequestQueue");

extern "C" void MH3_NHTTPQueuePop(CpuContext* cpu) {
    cpu->gpr[3] = PopHead(Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x80515524, MH3_NHTTPQueuePop, "NHTTPi_GetRequestQueue");

extern "C" void MH3_NHTTPMutexReset(CpuContext* cpu) {
    Write(Arg(cpu, 3) + 0x18, 0);
}
REGISTER_NATIVE_FUNCTION_AS(0x80515528, MH3_NHTTPMutexReset, "NHTTPi_InitMutex");

extern "C" void MH3_NHTTPMutexSetup(CpuContext* cpu) {
    MutexSetup(cpu, Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x80515534, MH3_NHTTPMutexSetup, "NHTTPi_SetupMutex");

extern "C" void MH3_NHTTPMutexEnd(CpuContext*) {
}
REGISTER_NATIVE_FUNCTION_AS(0x80515574, MH3_NHTTPMutexEnd, "NHTTPi_EndMutex");

extern "C" void MH3_NHTTPLock(CpuContext* cpu) {
    Lock(cpu, Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x80515578, MH3_NHTTPLock, "NHTTPi_LockMutex");

extern "C" void MH3_NHTTPUnlock(CpuContext* cpu) {
    Unlock(cpu, Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x8051557C, MH3_NHTTPUnlock, "NHTTPi_UnlockMutex");

extern "C" void MH3_NHTTPWorkerStart(CpuContext* cpu) {
    WorkerStart(cpu, Arg(cpu, 3), Arg(cpu, 4), Arg(cpu, 5));
    cpu->gpr[3] = 1;
}
REGISTER_NATIVE_FUNCTION_AS(0x80515580, MH3_NHTTPWorkerStart, "NHTTPi_CreateThread");

extern "C" void MH3_NHTTPWorkerStop(CpuContext* cpu) {
    WorkerStop(cpu, Arg(cpu, 3), Arg(cpu, 4));
}
REGISTER_NATIVE_FUNCTION_AS(0x80515614, MH3_NHTTPWorkerStop, "NHTTPi_DestroyThread");

extern "C" void MH3_NHTTPWorkerWait(CpuContext* cpu) {
    WorkerWait(cpu, Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x8051565C, MH3_NHTTPWorkerWait, "NHTTPi_IdleThread");

extern "C" void MH3_NHTTPWorkerWake(CpuContext* cpu) {
    WorkerWake(cpu, Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x80515684, MH3_NHTTPWorkerWake, "NHTTPi_WakeupThread");

extern "C" void MH3_NHTTPCheckThread(CpuContext* cpu) {
    CheckThread(cpu, Arg(cpu, 3), Arg(cpu, 4));
}
REGISTER_NATIVE_FUNCTION_AS(0x80515690, MH3_NHTTPCheckThread, "NHTTPi_CheckCurrentThread");

extern "C" void MH3_NHTTPWorkerEntry(CpuContext* cpu) {
    Worker(cpu);
    cpu->gpr[3] = 0;
}
REGISTER_NATIVE_FUNCTION_AS(0x80515728, MH3_NHTTPWorkerEntry, "NHTTPi_ThreadEntry");

extern "C" void MH3_NHTTPFindLine(CpuContext* cpu) {
    const uint32_t colonOut = Arg(cpu, 6), eolOut = Arg(cpu, 7);
    int32_t colon = 0, eol = eolOut ? ReadInt(eolOut) : 0;
    const int32_t answer = FindLine(Arg(cpu, 3), IntArg(cpu, 4), IntArg(cpu, 5), colonOut ? &colon : nullptr, eolOut ? &eol : nullptr);

    if (colonOut)
        Write(colonOut, uint32_t(colon));

    if (eolOut)
        Write(eolOut, uint32_t(eol));

    cpu->gpr[3] = uint32_t(answer);
}
REGISTER_NATIVE_FUNCTION_AS(0x8051574C, MH3_NHTTPFindLine, "NHTTPi_FindLine");

extern "C" void MH3_NHTTPSkipSpaces(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(SkipSpaces(Arg(cpu, 3), IntArg(cpu, 4), IntArg(cpu, 5)));
}
REGISTER_NATIVE_FUNCTION_AS(0x80515944, MH3_NHTTPSkipSpaces, "NHTTPi_SkipSpace");

extern "C" void MH3_NHTTPMatchToken(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(MatchToken(Arg(cpu, 3), IntArg(cpu, 4), IntArg(cpu, 5), Arg(cpu, 6), int8_t(Arg(cpu, 7))));
}
REGISTER_NATIVE_FUNCTION_AS(0x80515A3C, MH3_NHTTPMatchToken, "NHTTPi_CompareToken");

extern "C" void MH3_NHTTPCopyHeader(CpuContext* cpu) {
    cpu->gpr[3] = CopyHeader(cpu, Arg(cpu, 3), Arg(cpu, 4), IntArg(cpu, 5), Arg(cpu, 6)) ? 1 : 0;
}
REGISTER_NATIVE_FUNCTION_AS(0x80515C2C, MH3_NHTTPCopyHeader, "NHTTPi_LoadFromHdrRecvBuf");

extern "C" void MH3_NHTTPAtLeast(CpuContext* cpu) {
    cpu->gpr[3] = AtLeast(Arg(cpu, 3), Arg(cpu, 4));
}
REGISTER_NATIVE_FUNCTION_AS(0x80515D70, MH3_NHTTPAtLeast, "NHTTPi_IsBodyBufFull");

extern "C" void MH3_NHTTPRecvRest(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(RecvRest(cpu, Arg(cpu, 3), Arg(cpu, 4), Arg(cpu, 5), Arg(cpu, 6), Arg(cpu, 7)));
}
REGISTER_NATIVE_FUNCTION_AS(0x80515D8C, MH3_NHTTPRecvRest, "NHTTPi_RecvBuf");

extern "C" void MH3_NHTTPRecvAtMost(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(RecvAtMost(cpu, Arg(cpu, 3), Arg(cpu, 4), Arg(cpu, 5), Arg(cpu, 6), IntArg(cpu, 7), Arg(cpu, 8)));
}
REGISTER_NATIVE_FUNCTION_AS(0x80515DAC, MH3_NHTTPRecvAtMost, "NHTTPi_RecvBufN");

extern "C" void MH3_NHTTPCurrentReset(CpuContext* cpu) {
    Write(Arg(cpu, 3), 0);
}
REGISTER_NATIVE_FUNCTION_AS(0x80515DE8, MH3_NHTTPCurrentReset, "NHTTPi_InitRequestInfo");

extern "C" void MH3_NHTTPCreateRequestWith(CpuContext* cpu) {
    cpu->gpr[3] = CreateRequest(cpu, Arg(cpu, 3), Arg(cpu, 4), IntArg(cpu, 5), Arg(cpu, 6), Arg(cpu, 7), Arg(cpu, 8), Arg(cpu, 9), Arg(cpu, 10));
}
REGISTER_NATIVE_FUNCTION_AS(0x80515DF4, MH3_NHTTPCreateRequestWith, "NHTTPi_CreateRequest");

extern "C" void MH3_NHTTPDiscardRequest(CpuContext* cpu) {
    DiscardRequest(cpu, Arg(cpu, 3), Arg(cpu, 4));
}
REGISTER_NATIVE_FUNCTION_AS(0x8051641C, MH3_NHTTPDiscardRequest, "NHTTPi_DeleteRequest");

extern "C" void MH3_NHTTPFreeRequest(CpuContext* cpu) {
    FreeRequest(cpu, Arg(cpu, 3), Arg(cpu, 4));
    cpu->gpr[3] = 1;
}
REGISTER_NATIVE_FUNCTION_AS(0x80516520, MH3_NHTTPFreeRequest, "NHTTPi_DestroyRequest");

extern "C" void MH3_NHTTPQueue(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(Queue(cpu, Arg(cpu, 3), Arg(cpu, 4)));
}
REGISTER_NATIVE_FUNCTION_AS(0x805165F8, MH3_NHTTPQueue, "NHTTPi_SendRequestAsync");

extern "C" void MH3_NHTTPCancelWith(CpuContext* cpu) {
    cpu->gpr[3] = Cancel(cpu, Arg(cpu, 3), IntArg(cpu, 4)) ? 1 : 0;
}
REGISTER_NATIVE_FUNCTION_AS(0x805166B8, MH3_NHTTPCancelWith, "NHTTPi_CancelRequestAsync");

extern "C" void MH3_NHTTPCancelAll(CpuContext* cpu) {
    CancelAll(cpu, Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x8051677C, MH3_NHTTPCancelAll, "NHTTPi_AllCancelRequestAsync");

extern "C" void MH3_NHTTPFreeResponse(CpuContext* cpu) {
    FreeResponse(cpu, Arg(cpu, 3), Arg(cpu, 4));
}
REGISTER_NATIVE_FUNCTION_AS(0x80516824, MH3_NHTTPFreeResponse, "NHTTPi_DestroyResponse");

extern "C" void MH3_NHTTPFindField(CpuContext* cpu) {
    const uint32_t startOut = Arg(cpu, 5);
    int32_t start = ReadInt(startOut);
    const int32_t answer = FindField(Arg(cpu, 3), Arg(cpu, 4), &start);
    Write(startOut, uint32_t(start));

    cpu->gpr[3] = uint32_t(answer);
}
REGISTER_NATIVE_FUNCTION_AS(0x805168F0, MH3_NHTTPFindField, "NHTTPi_FindHeaderValue");

extern "C" void MH3_NHTTPSocketAlive(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(SocketAlive(cpu, IntArg(cpu, 3)));
}
REGISTER_NATIVE_FUNCTION_AS(0x80516A24, MH3_NHTTPSocketAlive, "NHTTPi_CheckSocket");

extern "C" void MH3_NHTTPOpenSocket(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(OpenSocket(cpu, Arg(cpu, 3)));
}
REGISTER_NATIVE_FUNCTION_AS(0x80516A7C, MH3_NHTTPOpenSocket, "NHTTPi_SocSocket");

extern "C" void MH3_NHTTPCloseSocket(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(CloseSocket(cpu, Arg(cpu, 3), Arg(cpu, 4), Arg(cpu, 5)));
}
REGISTER_NATIVE_FUNCTION_AS(0x80516B50, MH3_NHTTPCloseSocket, "NHTTPi_SocClose");

extern "C" void MH3_NHTTPConnect(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(Connect(cpu, Arg(cpu, 3), Arg(cpu, 4), Arg(cpu, 5), Arg(cpu, 6), Arg(cpu, 7), Arg(cpu, 8)));
}
REGISTER_NATIVE_FUNCTION_AS(0x80516BBC, MH3_NHTTPConnect, "NHTTPi_SocConnect");

extern "C" void MH3_NHTTPSslConnect(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(SslConnect(cpu, Arg(cpu, 3), Arg(cpu, 4), Arg(cpu, 5), Arg(cpu, 6)));
}
REGISTER_NATIVE_FUNCTION_AS(0x80516C7C, MH3_NHTTPSslConnect, "NHTTPi_SocSSLConnect");

extern "C" void MH3_NHTTPBufferedRecv(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(BufferedRecv(cpu, Arg(cpu, 3), Arg(cpu, 4), Arg(cpu, 5), IntArg(cpu, 6), Arg(cpu, 7)));
}
REGISTER_NATIVE_FUNCTION_AS(0x80516E2C, MH3_NHTTPBufferedRecv, "NHTTPi_SocRecvBuffered");

extern "C" void MH3_NHTTPRecv(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(Recv(cpu, Arg(cpu, 3), Arg(cpu, 4), Arg(cpu, 5), Arg(cpu, 6), IntArg(cpu, 7), Arg(cpu, 8)));
}
REGISTER_NATIVE_FUNCTION_AS(0x80516F38, MH3_NHTTPRecv, "NHTTPi_SocRecv");

extern "C" void MH3_NHTTPAlignedSend(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(AlignedSend(cpu, Arg(cpu, 3), Arg(cpu, 4), Arg(cpu, 5), Arg(cpu, 6)));
}
REGISTER_NATIVE_FUNCTION_AS(0x80517014, MH3_NHTTPAlignedSend, "NHTTPi_SocSendAligned");

extern "C" void MH3_NHTTPSend(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(Send(cpu, Arg(cpu, 3), Arg(cpu, 4), Arg(cpu, 5), Arg(cpu, 6), Arg(cpu, 7)));
}
REGISTER_NATIVE_FUNCTION_AS(0x805171AC, MH3_NHTTPSend, "NHTTPi_SocSend");

extern "C" void MH3_NHTTPShutdownSocket(CpuContext* cpu) {
    ShutdownSocket(cpu, Arg(cpu, 3), IntArg(cpu, 5));
}
REGISTER_NATIVE_FUNCTION_AS(0x80517258, MH3_NHTTPShutdownSocket, "NHTTPi_SocCancel");

extern "C" void MH3_NHTTPResolveHost(CpuContext* cpu) {
    cpu->gpr[3] = ResolveHost(cpu, Arg(cpu, 4));
}
REGISTER_NATIVE_FUNCTION_AS(0x805172AC, MH3_NHTTPResolveHost, "NHTTPi_Resolve");

extern "C" void MH3_NHTTPMemcpy(CpuContext* cpu) {
    InvokeIndirectCpu(Memcpy, cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x80517320, MH3_NHTTPMemcpy, "NHTTPi_memcpy");

extern "C" void MH3_NHTTPStrlen(CpuContext* cpu) {
    cpu->gpr[3] = StrLen(Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x80517324, MH3_NHTTPStrlen, "NHTTPi_strlen");

extern "C" void MH3_NHTTPStrcmp(CpuContext* cpu) {
    uint32_t a = Arg(cpu, 3), b = Arg(cpu, 4);
    int32_t diff = 0;

    for (;; ++a, ++b) {
        const uint8_t ca = Memory::Read8(a), cb = Memory::Read8(b);
        diff = int32_t(ca) - int32_t(cb);

        if (diff || !ca)
            break;
    }

    cpu->gpr[3] = uint32_t(diff);
}
REGISTER_NATIVE_FUNCTION_AS(0x80517328, MH3_NHTTPStrcmp, "NHTTPi_strcmp");

extern "C" void MH3_NHTTPMemclear(CpuContext* cpu) {
    Clear(cpu, Arg(cpu, 3), Arg(cpu, 4));
}
REGISTER_NATIVE_FUNCTION_AS(0x8051732C, MH3_NHTTPMemclear, "NHTTPi_memclr");

extern "C" void MH3_NHTTPCaseCompare(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(CaseCompare(Arg(cpu, 3), Arg(cpu, 4), IntArg(cpu, 5)));
}
REGISTER_NATIVE_FUNCTION_AS(0x80517338, MH3_NHTTPCaseCompare, "NHTTPi_strnicmp");

extern "C" void MH3_NHTTPEncodedLength(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(EncodedLength(Arg(cpu, 3)));
}
REGISTER_NATIVE_FUNCTION_AS(0x80517404, MH3_NHTTPEncodedLength, "NHTTPi_encodeUrlLength");

extern "C" void MH3_NHTTPEncodedLengthN(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(EncodedLength(Arg(cpu, 3), IntArg(cpu, 4)));
}
REGISTER_NATIVE_FUNCTION_AS(0x80517474, MH3_NHTTPEncodedLengthN, "NHTTPi_encodeUrlLengthN");

extern "C" void MH3_NHTTPEncodeChar(CpuContext* cpu) {
    cpu->gpr[3] = EncodeChar(Arg(cpu, 3), Arg(cpu, 4));
}
REGISTER_NATIVE_FUNCTION_AS(0x805174E8, MH3_NHTTPEncodeChar, "NHTTPi_encodeUrlChar");

extern "C" void MH3_NHTTPParseHex(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(ParseHex(Arg(cpu, 3), IntArg(cpu, 4)));
}
REGISTER_NATIVE_FUNCTION_AS(0x80517588, MH3_NHTTPParseHex, "NHTTPi_strToHex");

extern "C" void MH3_NHTTPParseDecimal(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(ParseDecimal(Arg(cpu, 3), IntArg(cpu, 4)));
}
REGISTER_NATIVE_FUNCTION_AS(0x805176AC, MH3_NHTTPParseDecimal, "NHTTPi_strToInt");

extern "C" void MH3_NHTTPDecimal(CpuContext* cpu) {
    cpu->gpr[3] = Decimal(Arg(cpu, 3), Arg(cpu, 4));
}
REGISTER_NATIVE_FUNCTION_AS(0x8051774C, MH3_NHTTPDecimal, "NHTTPi_intToStr");

extern "C" void MH3_NHTTPTokenCompare(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(TokenCompare(Arg(cpu, 3), Arg(cpu, 4)));
}
REGISTER_NATIVE_FUNCTION_AS(0x805178C8, MH3_NHTTPTokenCompare, "NHTTPi_compareTokenN");

extern "C" void MH3_NHTTPParseDigits(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(ParseDigits(Arg(cpu, 3), IntArg(cpu, 4)));
}
REGISTER_NATIVE_FUNCTION_AS(0x8051797C, MH3_NHTTPParseDigits, "NHTTPi_strToPort");

extern "C" void MH3_NHTTPContains(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(Contains(Arg(cpu, 3), IntArg(cpu, 4), Arg(cpu, 5), IntArg(cpu, 6)));
}
REGISTER_NATIVE_FUNCTION_AS(0x80517A0C, MH3_NHTTPContains, "NHTTPi_memfind");

extern "C" void MH3_NHTTPBase64(CpuContext* cpu) {
    cpu->gpr[3] = Base64(Arg(cpu, 3), Arg(cpu, 4));
}
REGISTER_NATIVE_FUNCTION_AS(0x80517AB8, MH3_NHTTPBase64, "NHTTPi_Base64Encode");

extern "C" void MH3_NHTTPWorkerQueueReset(CpuContext* cpu) {
    Write(Arg(cpu, 3) + wQueueReady, 0);
}
REGISTER_NATIVE_FUNCTION_AS(0x80517C94, MH3_NHTTPWorkerQueueReset, "NHTTPi_InitThreadInfo");

extern "C" void MH3_NHTTPWorkerQueueSet(CpuContext* cpu) {
    Write(Arg(cpu, 3) + wQueueReady, 1);
}
REGISTER_NATIVE_FUNCTION_AS(0x80517CA0, MH3_NHTTPWorkerQueueSet, "NHTTPi_SetThreadQueueReady");

extern "C" void MH3_NHTTPWorkerQueueReady(CpuContext* cpu) {
    cpu->gpr[3] = Read(Arg(cpu, 3) + wQueueReady);
}
REGISTER_NATIVE_FUNCTION_AS(0x80517CAC, MH3_NHTTPWorkerQueueReady, "NHTTPi_IsThreadQueueReady");

extern "C" void MH3_NHTTPHeaderEnd(CpuContext* cpu) {
    const uint32_t at = Arg(cpu, 3);
    const uint8_t ring[4] = {Memory::Read8(at), Memory::Read8(at + 1), Memory::Read8(at + 2), Memory::Read8(at + 3)};
    cpu->gpr[3] = HeaderEnd(ring, Arg(cpu, 4)) ? 1 : 0;
}
REGISTER_NATIVE_FUNCTION_AS(0x80517CB4, MH3_NHTTPHeaderEnd, "NHTTPi_CheckHeaderEnd");

extern "C" void MH3_NHTTPBufferedSend(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(BufferedSend(cpu, Arg(cpu, 3), Arg(cpu, 4), Arg(cpu, 5), Arg(cpu, 6), Arg(cpu, 7), IntArg(cpu, 8)));
}
REGISTER_NATIVE_FUNCTION_AS(0x80517D7C, MH3_NHTTPBufferedSend, "NHTTPi_SendToBuffer");

extern "C" void MH3_NHTTPPostLength(CpuContext* cpu) {
    const uint32_t totalAt = Arg(cpu, 6);
    int32_t total = ReadInt(totalAt);
    const bool answer = PostLength(cpu, Arg(cpu, 3), Arg(cpu, 4), Arg(cpu, 5), &total, IntArg(cpu, 7));
    Write(totalAt, uint32_t(total));
    cpu->gpr[3] = answer ? 1 : 0;
}
REGISTER_NATIVE_FUNCTION_AS(0x80517E5C, MH3_NHTTPPostLength, "NHTTPi_GetPostContentLength");

extern "C" void MH3_NHTTPPostSend(CpuContext* cpu) {
    cpu->gpr[3] = PostSend(cpu, Arg(cpu, 3), Arg(cpu, 4), Arg(cpu, 5), Arg(cpu, 6), Arg(cpu, 7), Arg(cpu, 8), IntArg(cpu, 9));
}
REGISTER_NATIVE_FUNCTION_AS(0x80517F60, MH3_NHTTPPostSend, "NHTTPi_SendPostDataStream");

extern "C" void MH3_NHTTPHasRoom(CpuContext* cpu) {
    cpu->gpr[3] = HasRoom(cpu, Arg(cpu, 3), Arg(cpu, 4)) ? 1 : 0;
}
REGISTER_NATIVE_FUNCTION_AS(0x805180F0, MH3_NHTTPHasRoom, "NHTTPi_CheckBodyBuf");

extern "C" void MH3_NHTTPSendText(CpuContext* cpu) {
    cpu->gpr[3] = SendText(cpu, Arg(cpu, 3), Arg(cpu, 4), Arg(cpu, 5));
}
REGISTER_NATIVE_FUNCTION_AS(0x805181BC, MH3_NHTTPSendText, "NHTTPi_SendBuffer");

extern "C" void MH3_NHTTPProxyConnect(CpuContext* cpu) {
    cpu->gpr[3] = ProxyConnect(cpu, Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x80518268, MH3_NHTTPProxyConnect, "NHTTPi_SendProxyConnect");

extern "C" void MH3_NHTTPProxyAnswer(CpuContext* cpu) {
    cpu->gpr[3] = ProxyAnswer(cpu, Arg(cpu, 3)) ? 1 : 0;
}
REGISTER_NATIVE_FUNCTION_AS(0x80518510, MH3_NHTTPProxyAnswer, "NHTTPi_RecvProxyConnect");

extern "C" void MH3_NHTTPSendFields(CpuContext* cpu) {
    cpu->gpr[3] = SendFields(cpu, Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x80518708, MH3_NHTTPSendFields, "NHTTPi_SendHeaderList");

extern "C" void MH3_NHTTPSendRawPost(CpuContext* cpu) {
    cpu->gpr[3] = SendRawPost(cpu, Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x80518800, MH3_NHTTPSendRawPost, "NHTTPi_SendPostDataRaw");

extern "C" void MH3_NHTTPSendMultipart(CpuContext* cpu) {
    cpu->gpr[3] = SendMultipart(cpu, Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x80518988, MH3_NHTTPSendMultipart, "NHTTPi_SendPostDataBinary");

extern "C" void MH3_NHTTPSendForm(CpuContext* cpu) {
    cpu->gpr[3] = SendForm(cpu, Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x80518D2C, MH3_NHTTPSendForm, "NHTTPi_SendPostDataAscii");

extern "C" void MH3_NHTTPFinish(CpuContext* cpu) {
    Finish(cpu, Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x80518FF0, MH3_NHTTPFinish, "NHTTPi_ThreadReqEnd");

extern "C" void MH3_NHTTPNext(CpuContext* cpu) {
    cpu->gpr[3] = Next(cpu, Arg(cpu, 3)) ? 1 : 0;
}
REGISTER_NATIVE_FUNCTION_AS(0x80519170, MH3_NHTTPNext, "NHTTPi_ThreadGetNextRequest");

extern "C" void MH3_NHTTPResolve(CpuContext* cpu) {
    cpu->gpr[3] = Resolve(cpu, Arg(cpu, 3)) ? 1 : 0;
}
REGISTER_NATIVE_FUNCTION_AS(0x80519228, MH3_NHTTPResolve, "NHTTPi_ThreadHostAddrProc");

extern "C" void MH3_NHTTPOpen(CpuContext* cpu) {
    cpu->gpr[3] = Open(cpu, Arg(cpu, 3)) ? 1 : 0;
}
REGISTER_NATIVE_FUNCTION_AS(0x8051937C, MH3_NHTTPOpen, "NHTTPi_ThreadConnectProc");

extern "C" void MH3_NHTTPTunnel(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(Tunnel(cpu, Arg(cpu, 3)));
}
REGISTER_NATIVE_FUNCTION_AS(0x80519538, MH3_NHTTPTunnel, "NHTTPi_ThreadProxyProc");

extern "C" void MH3_NHTTPSendRequest(CpuContext* cpu) {
    cpu->gpr[3] = SendRequest(cpu, Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x8051968C, MH3_NHTTPSendRequest, "NHTTPi_ThreadSendProc");

extern "C" void MH3_NHTTPReceiveHeader(CpuContext* cpu) {
    cpu->gpr[3] = ReceiveHeader(cpu, Arg(cpu, 3)) ? 1 : 0;
}
REGISTER_NATIVE_FUNCTION_AS(0x80519B28, MH3_NHTTPReceiveHeader, "NHTTPi_ThreadRecvHeaderProc");

extern "C" void MH3_NHTTPParseHeader(CpuContext* cpu) {
    cpu->gpr[3] = ParseHeader(cpu, Arg(cpu, 3)) ? 1 : 0;
}
REGISTER_NATIVE_FUNCTION_AS(0x80519D14, MH3_NHTTPParseHeader, "NHTTPi_ThreadParseHeaderProc");

extern "C" void MH3_NHTTPReceiveBody(CpuContext* cpu) {
    cpu->gpr[3] = ReceiveBody(cpu, Arg(cpu, 3)) ? 1 : 0;
}
REGISTER_NATIVE_FUNCTION_AS(0x8051A044, MH3_NHTTPReceiveBody, "NHTTPi_ThreadRecvBodyProc");

extern "C" void MH3_NHTTPWorker(CpuContext* cpu) {
    Worker(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x8051A634, MH3_NHTTPWorker, "NHTTPi_ThreadExecute");

extern "C" void MH3_NHTTPCreateConnection(CpuContext* cpu) {
    cpu->gpr[3] = CreateConnection(cpu, Arg(cpu, 3), IntArg(cpu, 4), Arg(cpu, 5), Arg(cpu, 6), Arg(cpu, 7), Arg(cpu, 8));
}
REGISTER_NATIVE_FUNCTION_AS(0x8051A838, MH3_NHTTPCreateConnection, "NHTTPi_CreateConnection");

extern "C" void MH3_NHTTPStartConnection(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(StartConnection(cpu, Arg(cpu, 3)));
}
REGISTER_NATIVE_FUNCTION_AS(0x8051A980, MH3_NHTTPStartConnection, "NHTTPi_StartConnection");

extern "C" void MH3_NHTTPBody(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(Body(cpu, Arg(cpu, 3), Arg(cpu, 4), Arg(cpu, 5)));
}
REGISTER_NATIVE_FUNCTION_AS(0x8051AA0C, MH3_NHTTPBody, "NHTTPi_GetBodyBuffer");

extern "C" void MH3_NHTTPUserParam(CpuContext* cpu) {
    cpu->gpr[3] = UserParam(cpu, Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x8051AAA8, MH3_NHTTPUserParam, "NHTTPi_GetUserParam");

extern "C" void MH3_NHTTPConnectionError(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(ErrorOf(cpu, Arg(cpu, 3)));
}
REGISTER_NATIVE_FUNCTION_AS(0x8051AB1C, MH3_NHTTPConnectionError, "NHTTPi_GetConnectionError");

extern "C" void MH3_NHTTPStartup(CpuContext* cpu) {
    const uint32_t alloc = Arg(cpu, 3), free = Arg(cpu, 4), priority = Arg(cpu, 5);
    const uint32_t globals = Globals();
    RegisterVersion(cpu, kStartupVersionDone, kStartupVersion);
    cpu->gpr[3] = Startup(cpu, globals, alloc, free, priority) ? 0 : 0xFFFFFFFF;
}
REGISTER_NATIVE_FUNCTION_AS(0x8051AB68, MH3_NHTTPStartup, "NHTTPStartup");

extern "C" void MH3_NHTTPCleanupAsync(CpuContext* cpu) {
    Cleanup(cpu, Globals(), Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x8051ABF4, MH3_NHTTPCleanupAsync, "NHTTPCleanupAsync");

extern "C" void MH3_NHTTPGetError(CpuContext* cpu) {
    cpu->gpr[3] = Read(Globals() + gError);
}
REGISTER_NATIVE_FUNCTION_AS(0x8051AC28, MH3_NHTTPGetError, "NHTTPGetError");

extern "C" void MH3_NHTTPGetSSLError(CpuContext* cpu) {
    cpu->gpr[3] = Read(Globals() + gSslError);
}
REGISTER_NATIVE_FUNCTION_AS(0x8051AC4C, MH3_NHTTPGetSSLError, "NHTTPGetSSLError");

extern "C" void MH3_NHTTPDispatch(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(Dispatch(cpu, Arg(cpu, 3), IntArg(cpu, 4), Arg(cpu, 5)));
}
REGISTER_NATIVE_FUNCTION_AS(0x8051AC70, MH3_NHTTPDispatch, "NHTTPi_ConnectionCallback");

extern "C" void MH3_NHTTPCreateRequestEx(CpuContext* cpu) {
    cpu->gpr[3] = CreateRequestEx(cpu, Arg(cpu, 3), IntArg(cpu, 4), Arg(cpu, 5), Arg(cpu, 6), Arg(cpu, 7), Arg(cpu, 8), Arg(cpu, 9), Arg(cpu, 10));
}
REGISTER_NATIVE_FUNCTION_AS(0x8051AE54, MH3_NHTTPCreateRequestEx, "NHTTPi_CreateRequestEx");

extern "C" void MH3_NHTTPCreateRequest(CpuContext* cpu) {
    const uint32_t url = Arg(cpu, 3), buffer = Arg(cpu, 5), size = Arg(cpu, 6), callback = Arg(cpu, 7), param = Arg(cpu, 8);
    const int32_t method = IntArg(cpu, 4);
    RegisterVersion(cpu, kCreateVersionDone, kCreateVersion);
    cpu->gpr[3] = CreateRequestEx(cpu, url, method, buffer, size, callback, param, 0, 0);
}
REGISTER_NATIVE_FUNCTION_AS(0x8051AF24, MH3_NHTTPCreateRequest, "NHTTPCreateRequest");

extern "C" void MH3_NHTTPAddHeaderField(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(AddBeforeStart(cpu, Arg(cpu, 3), Arg(cpu, 4), Arg(cpu, 5), false));
}
REGISTER_NATIVE_FUNCTION_AS(0x8051AFA8, MH3_NHTTPAddHeaderField, "NHTTPAddHeaderField");

extern "C" void MH3_NHTTPAddPostDataAscii(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(AddBeforeStart(cpu, Arg(cpu, 3), Arg(cpu, 4), Arg(cpu, 5), true));
}
REGISTER_NATIVE_FUNCTION_AS(0x8051B06C, MH3_NHTTPAddPostDataAscii, "NHTTPAddPostDataAscii");

extern "C" void MH3_NHTTPSendRequestAsync(CpuContext* cpu) {
    const uint32_t connection = Connections(cpu, Globals() + gMutex, Arg(cpu, 3), kByRequest);
    cpu->gpr[3] = !connection || StartConnection(cpu, connection) != 0 ? 0xFFFFFFFF : Read(connection + cId);
}
REGISTER_NATIVE_FUNCTION_AS(0x8051B130, MH3_NHTTPSendRequestAsync, "NHTTPSendRequestAsync");

extern "C" void MH3_NHTTPCancelRequestAsync(CpuContext* cpu) {
    cpu->gpr[3] = Cancel(cpu, Globals(), IntArg(cpu, 3)) ? 0 : 0xFFFFFFFF;
}
REGISTER_NATIVE_FUNCTION_AS(0x8051B18C, MH3_NHTTPCancelRequestAsync, "NHTTPCancelRequestAsync");

extern "C" void MH3_NHTTPDestroyResponse(CpuContext* cpu) {
    const uint32_t mutex = Globals() + gMutex;
    const uint32_t connection = Connections(cpu, mutex, Arg(cpu, 3), kByResponse);

    if (connection) {
        if (ResponseOf(cpu, mutex, connection))
            FreeResponse(cpu, mutex, Read(connection + cResponse));

        Connections(cpu, mutex, connection, kRemove);
        Free(cpu, connection);
    }
}
REGISTER_NATIVE_FUNCTION_AS(0x8051B1CC, MH3_NHTTPDestroyResponse, "NHTTPDestroyResponse");

extern "C" void MH3_NHTTPGetBodyAll(CpuContext* cpu) {
    const uint32_t bufferOut = Arg(cpu, 4);
    const uint32_t connection = Connections(cpu, Globals() + gMutex, Arg(cpu, 3), kByResponse);
    int32_t answer = -1;

    if (connection) {
        GuestFrame frame(cpu, 0x20);
        Write(frame[0x10], 0);
        answer = Body(cpu, connection, bufferOut, frame[0x10]);
    }

    cpu->gpr[3] = uint32_t(answer);
}
REGISTER_NATIVE_FUNCTION_AS(0x8051B250, MH3_NHTTPGetBodyAll, "NHTTPGetBodyAll");

extern "C" void MH3_NHTTPGetResultCode(CpuContext* cpu) {
    const uint32_t response = ResponseOrSelf(cpu, Globals() + gMutex, Arg(cpu, 3));
    cpu->gpr[3] = response && Read(response + pHeaderDone) ? Read(response + pStatus) : 0xFFFFFFFF;
}
REGISTER_NATIVE_FUNCTION_AS(0x8051B2B8, MH3_NHTTPGetResultCode, "NHTTPGetResultCode");

extern "C" void MH3_NHTTPSetVerifyOption(CpuContext* cpu) {
    const uint32_t option = Arg(cpu, 4);
    const uint32_t request = RequestOrSelf(cpu, Globals() + gMutex, Arg(cpu, 3));

    if (request)
        Write(request + rVerify, option);

    cpu->gpr[3] = request ? 0 : 0xFFFFFFFF;
}
REGISTER_NATIVE_FUNCTION_AS(0x8051B318, MH3_NHTTPSetVerifyOption, "NHTTPSetVerifyOption");

extern "C" void MH3_NHTTPSetProxy(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(SetProxy(cpu, Arg(cpu, 3), Arg(cpu, 4), Arg(cpu, 5), Arg(cpu, 6), Arg(cpu, 7)));
}
REGISTER_NATIVE_FUNCTION_AS(0x8051B374, MH3_NHTTPSetProxy, "NHTTPSetProxy");

extern "C" void MH3_NHTTPSetProxyDefault(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(SetProxyDefault(cpu, Arg(cpu, 3)));
}
REGISTER_NATIVE_FUNCTION_AS(0x8051B4E4, MH3_NHTTPSetProxyDefault, "NHTTPSetProxyDefault");

extern "C" void MH3_NHTTPSetRootCADefault(CpuContext* cpu) {
    const uint32_t request = RequestOrSelf(cpu, Globals() + gMutex, Arg(cpu, 3));

    if (request) {
        Write(request + rRootCaId, 0);
        Write(request + rRootCa, 0);
        Write(request + rRootCaLength, 0);
    }

    cpu->gpr[3] = request ? 0 : 0xFFFFFFFF;
}
REGISTER_NATIVE_FUNCTION_AS(0x8051B60C, MH3_NHTTPSetRootCADefault, "NHTTPSetRootCADefault");

extern "C" void MH3_NHTTPSetClientCertDefault(CpuContext* cpu) {
    const uint32_t request = RequestOrSelf(cpu, Globals() + gMutex, Arg(cpu, 3));

    if (request) {
        Write(request + rClientCertId, 0);
        Write(request + rBuiltinClientCert, 1);
        Write(request + rClientCert, 0);
        Write(request + rClientCertLength, 0);
        Write(request + rClientKey, 0);
        Write(request + rClientKeyLength, 0);
    }

    cpu->gpr[3] = request ? 0 : 0xFFFFFFFF;
}
REGISTER_NATIVE_FUNCTION_AS(0x8051B66C, MH3_NHTTPSetClientCertDefault, "NHTTPSetClientCertDefault");

extern "C" void MH3_NHTTPWakeup(CpuContext* cpu) {
    Wakeup(cpu, Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x8051B6DC, MH3_NHTTPWakeup, "NHTTPi_NotifyCompletion");

extern "C" void MH3_NHTTPConnections(CpuContext* cpu) {
    cpu->gpr[3] = Connections(cpu, Arg(cpu, 3), Arg(cpu, 4), IntArg(cpu, 5));
}
REGISTER_NATIVE_FUNCTION_AS(0x8051B758, MH3_NHTTPConnections, "NHTTPi_ConnectionList");

extern "C" void MH3_NHTTPPushConnection(CpuContext* cpu) {
    cpu->gpr[3] = Connections(cpu, Arg(cpu, 3), Arg(cpu, 4), kPush) ? 0 : 0xFFFFFFFF;
}
REGISTER_NATIVE_FUNCTION_AS(0x8051B868, MH3_NHTTPPushConnection, "NHTTPi_AddConnection");

extern "C" void MH3_NHTTPRemoveConnection(CpuContext* cpu) {
    cpu->gpr[3] = Connections(cpu, Arg(cpu, 3), Arg(cpu, 4), kRemove) ? 0 : 0xFFFFFFFF;
}
REGISTER_NATIVE_FUNCTION_AS(0x8051B898, MH3_NHTTPRemoveConnection, "NHTTPi_RemoveConnection");

extern "C" void MH3_NHTTPRequestOf(CpuContext* cpu) {
    cpu->gpr[3] = RequestOf(cpu, Arg(cpu, 3), Arg(cpu, 4));
}
REGISTER_NATIVE_FUNCTION_AS(0x8051B8C8, MH3_NHTTPRequestOf, "NHTTPi_ConnectionRequest");

extern "C" void MH3_NHTTPResponseOf(CpuContext* cpu) {
    cpu->gpr[3] = ResponseOf(cpu, Arg(cpu, 3), Arg(cpu, 4));
}
REGISTER_NATIVE_FUNCTION_AS(0x8051B900, MH3_NHTTPResponseOf, "NHTTPi_ConnectionResponse");

extern "C" void MH3_NHTTPByRequest(CpuContext* cpu) {
    cpu->gpr[3] = Connections(cpu, Arg(cpu, 3), Arg(cpu, 4), kByRequest);
}
REGISTER_NATIVE_FUNCTION_AS(0x8051B938, MH3_NHTTPByRequest, "NHTTPi_FindConnectionByRequest");

extern "C" void MH3_NHTTPByResponse(CpuContext* cpu) {
    cpu->gpr[3] = Connections(cpu, Arg(cpu, 3), Arg(cpu, 4), kByResponse);
}
REGISTER_NATIVE_FUNCTION_AS(0x8051B940, MH3_NHTTPByResponse, "NHTTPi_FindConnectionByResponse");

extern "C" void MH3_NHTTPByConnection(CpuContext* cpu) {
    cpu->gpr[3] = Connections(cpu, Arg(cpu, 3), Arg(cpu, 4), kByConnection);
}
REGISTER_NATIVE_FUNCTION_AS(0x8051B948, MH3_NHTTPByConnection, "NHTTPi_FindConnection");

extern "C" void MH3_NHTTPRequestOrSelf(CpuContext* cpu) {
    cpu->gpr[3] = RequestOrSelf(cpu, Arg(cpu, 3), Arg(cpu, 4));
}
REGISTER_NATIVE_FUNCTION_AS(0x8051B950, MH3_NHTTPRequestOrSelf, "NHTTPi_HandleRequest");

extern "C" void MH3_NHTTPResponseOrSelf(CpuContext* cpu) {
    cpu->gpr[3] = ResponseOrSelf(cpu, Arg(cpu, 3), Arg(cpu, 4));
}
REGISTER_NATIVE_FUNCTION_AS(0x8051B994, MH3_NHTTPResponseOrSelf, "NHTTPi_HandleResponse");

extern "C" void MH3_NHTTPConnectionCount(CpuContext* cpu) {
    cpu->gpr[3] = ConnectionCount();
}
REGISTER_NATIVE_FUNCTION_AS(0x8051B9D8, MH3_NHTTPConnectionCount, "NHTTPi_ConnectionCount");

extern "C" void MH3_NHTTPPostCallback(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(PostCallback(cpu, Arg(cpu, 3), Arg(cpu, 4), Arg(cpu, 5), Arg(cpu, 6)));
}
REGISTER_NATIVE_FUNCTION_AS(0x8051B9F8, MH3_NHTTPPostCallback, "NHTTPi_PostDataCallback");

extern "C" void MH3_NHTTPGrowCallback(CpuContext* cpu) {
    BufferCallback(cpu, Arg(cpu, 3), Arg(cpu, 4), 2);
}
REGISTER_NATIVE_FUNCTION_AS(0x8051BAE0, MH3_NHTTPGrowCallback, "NHTTPi_BodyBufCallback");

extern "C" void MH3_NHTTPBodyDoneCallback(CpuContext* cpu) {
    BufferCallback(cpu, Arg(cpu, 3), Arg(cpu, 4), 3);
}
REGISTER_NATIVE_FUNCTION_AS(0x8051BBF8, MH3_NHTTPBodyDoneCallback, "NHTTPi_BodyDoneCallback");

extern "C" void MH3_NHTTPComplete(CpuContext* cpu) {
    Complete(cpu, Arg(cpu, 3), Arg(cpu, 4));
}
REGISTER_NATIVE_FUNCTION_AS(0x8051BD10, MH3_NHTTPComplete, "NHTTPi_CompleteCallback");

extern "C" void MH3_NHTTPGlobals(CpuContext* cpu) {
    cpu->gpr[3] = Globals();
}
REGISTER_NATIVE_FUNCTION_AS(0x8051BD68, MH3_NHTTPGlobals, "NHTTPi_GetSystemInfoP");

extern "C" void MH3_NHTTPGlobalsSelf(CpuContext*) {
}
REGISTER_NATIVE_FUNCTION_AS(0x8051BDD0, MH3_NHTTPGlobalsSelf, "NHTTPi_GetBgnEndInfoP");

extern "C" void MH3_NHTTPQueueOf(CpuContext* cpu) {
    cpu->gpr[3] += gQueue;
}
REGISTER_NATIVE_FUNCTION_AS(0x8051BDD4, MH3_NHTTPQueueOf, "NHTTPi_GetReqInfoP");

extern "C" void MH3_NHTTPCurrentOf(CpuContext* cpu) {
    cpu->gpr[3] += gCurrent;
}
REGISTER_NATIVE_FUNCTION_AS(0x8051BDDC, MH3_NHTTPCurrentOf, "NHTTPi_GetReqP");

extern "C" void MH3_NHTTPWorkerOf(CpuContext* cpu) {
    cpu->gpr[3] += gWorker;
}
REGISTER_NATIVE_FUNCTION_AS(0x8051BDE4, MH3_NHTTPWorkerOf, "NHTTPi_GetThreadInfoP");

extern "C" void MH3_NHTTPMutexOf(CpuContext* cpu) {
    cpu->gpr[3] += gMutex;
}
REGISTER_NATIVE_FUNCTION_AS(0x8051BDEC, MH3_NHTTPMutexOf, "NHTTPi_GetMutexInfoP");

extern "C" void MH3_NHTTPSetProgress(CpuContext* cpu) {
    SetProgress(Arg(cpu, 3), Arg(cpu, 4));
}
REGISTER_NATIVE_FUNCTION_AS(0x8051BDF4, MH3_NHTTPSetProgress, "NHTTPi_SetContentLength");

extern "C" void MH3_NHTTPProgress(CpuContext* cpu) {
    cpu->gpr[3] = Progress(Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x8051BE04, MH3_NHTTPProgress, "NHTTPi_GetContentLength");

extern "C" void MH3_NHTTPResetConnections(CpuContext* cpu) {
    ResetConnections(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x8051BE1C, MH3_NHTTPResetConnections, "NHTTPi_InitConnectionList");
