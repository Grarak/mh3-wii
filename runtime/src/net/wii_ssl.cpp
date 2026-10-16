// The SSL library over /dev/net/ssl (RMHP08 0x8051BE7C..0x8051CBD0;
// decompiled with Ghidra, the call arguments checked in the translated C++),
// ported natively. Each call opens the device, sends one ioctlv whose
// vectors are 32-byte buffers (the result first, then the context, data or
// certificate), closes it and answers the result word. Reads and writes are
// split so the device sees 32-byte aligned buffers: an unaligned head and
// tail go through a bounce buffer. NHTTP (the account server's HTTPS) is the
// one caller. The guest's memcpy and memset stay guest calls.
#include "abi_bridge.h"
#include "guest_call.h"
#include "memory.h"

#include <initializer_list>
#include <utility>

namespace {

enum : uint32_t {
    Memcpy = 0x80004000,
    Memset = 0x80004350,
    IOSOpen = 0x804BC500,
    IOSClose = 0x804BC810,
    IOSIoctlv = 0x804BD310,
    OSRegisterVersion = 0x804CBA00,
    OSInitMutex = 0x804D2560,
    OSLockMutex = 0x804D25A0,
    OSUnlockMutex = 0x804D2680,
};

enum : uint32_t {
    kDevice = 0x80631890,      // "/dev/net/ssl"
    kVersionDone = 0x80797D48,
    kVersion = 0x807968E8,
    kCertsReady = 0x80797D4C,
    kCertMutex = 0x80765DC0,
    kClientCert = 0x80765DE0,  // 0x1000 each
    kClientKey = 0x80766DE0,
    kRootCa = 0x80767DE0,
};

enum : uint32_t {
    kNew = 1,
    kConnect = 2,
    kHandshake = 3,
    kRead = 4,
    kWrite = 5,
    kShutdown = 6,
    kSetClientCert = 7,
    kSetRootCa = 10,
    kSetBuiltinRootCa = 13,
    kSetBuiltinClientCert = 14,
};

constexpr int32_t kError = -1;
constexpr uint32_t kMaxRead = 0x8000;

// A frame with 32-byte aligned slots for the vectors and their buffers.
struct Slots {
    GuestFrame frame;
    uint32_t base;

    explicit Slots(CpuContext* cpu, uint32_t bytes) : frame(cpu, bytes + 0x40) {
        base = (frame[0x10] + 0x1F) & ~0x1Fu;
    }

    uint32_t operator[](uint32_t offset) const {
        return base + offset;
    }
};

// One ioctlv: `in` vectors read, `out` written, each (address, length); the
// first is the 32-byte result word, set to -1 first.
int32_t Ioctlv(CpuContext* cpu, uint32_t fd, uint32_t command, uint32_t in, uint32_t out, uint32_t vectors, std::initializer_list<std::pair<uint32_t, uint32_t>> buffers) {
    uint32_t at = vectors;

    for (const auto& [address, length] : buffers) {
        Memory::Write32(at, address);
        Memory::Write32(at + 4, length);
        at += 8;
    }

    Memory::Write32(buffers.begin()->first, 0xFFFFFFFF);
    GuestCall{cpu}.Call<IOSIoctlv>({fd, command, in, out, vectors});
    return int32_t(Memory::Read32(buffers.begin()->first));
}

// A call with words in 32-byte buffers after the result: (result, words...).
int32_t WordCall(CpuContext* cpu, uint32_t command, uint32_t in, uint32_t out, std::initializer_list<uint32_t> words) {
    GuestCall call{cpu};
    const int32_t fd = int32_t(call.Call<IOSOpen>({kDevice, 0}));

    if (fd < 0)
        return kError;

    Slots slot(cpu, 0xC0);
    const uint32_t vectors = slot[0], result = slot[0x20];
    uint32_t buffer = slot[0x40];
    std::pair<uint32_t, uint32_t> buffers[4] = {{result, 0x20}};
    size_t n = 1;

    for (const uint32_t word : words) {
        Memory::Write32(buffer, word);
        buffers[n++] = {buffer, 0x20};
        buffer += 0x20;
    }

    uint32_t at = vectors;

    for (size_t i = 0; i < n; ++i) {
        Memory::Write32(at, buffers[i].first);
        Memory::Write32(at + 4, buffers[i].second);
        at += 8;
    }

    Memory::Write32(result, 0xFFFFFFFF);
    call.Call<IOSIoctlv>({uint32_t(fd), command, in, out, vectors});
    call.Call<IOSClose>({uint32_t(fd)});
    return int32_t(Memory::Read32(result));
}

// 0x8051BE7C: SSLNew(verify option, host name): a context for the host (at
// most 256 characters; none gives -1 and, as the original, leaves the device
// open).
int32_t New(CpuContext* cpu, uint32_t option, uint32_t host) {
    GuestCall call{cpu};
    const int32_t fd = int32_t(call.Call<IOSOpen>({kDevice, 0}));

    if (!Memory::Read32(kVersionDone)) {
        call.Call<OSRegisterVersion>({Memory::Read32(kVersion)});
        Memory::Write32(kVersionDone, 1);
    }

    if (fd < 0)
        return kError;

    uint32_t length = 0;

    while (length < 0x100 && Memory::Read8(host + length))
        ++length;

    if (!length)
        return kError;

    Slots slot(cpu, 0x180);
    const uint32_t vectors = slot[0], result = slot[0x20], value = slot[0x40], name = slot[0x60];
    call.Call<Memset>({name, 0, 0x100});
    call.Call<Memcpy>({name, host, length});
    Memory::Write32(value, option);
    const int32_t answer = Ioctlv(cpu, uint32_t(fd), kNew, 1, 2, vectors, {{result, 0x20}, {value, 0x20}, {name, 0x100}});
    call.Call<IOSClose>({uint32_t(fd)});
    return answer;
}

// 0x8051C148: SSLRead(context, buffer, length), at most 32 KiB: the bytes
// read, or the device's error when none were.
int32_t Read(CpuContext* cpu, uint32_t context, uint32_t buffer, uint32_t length) {
    GuestCall call{cpu};
    const int32_t fd = int32_t(call.Call<IOSOpen>({kDevice, 0}));

    if (fd < 0)
        return kError;

    if (length > kMaxRead)
        length = kMaxRead;

    Slots slot(cpu, 0x80);
    const uint32_t vectors = slot[0], result = slot[0x20], ctx = slot[0x40], bounce = slot[0x60];
    uint32_t head = (buffer & 0x1F) ? 0x20 - (buffer & 0x1F) : 0;
    call.Call<Memset>({bounce, 0, 0x20});
    int32_t total = 0, answer = kError;
    Memory::Write32(ctx, context);

    auto done = [&](int32_t value) {
        call.Call<IOSClose>({uint32_t(fd)});
        return value;
    };

    if (head) {
        if (length < head)
            head = length;

        answer = Ioctlv(cpu, uint32_t(fd), kRead, 2, 1, vectors, {{result, 0x20}, {bounce, head}, {ctx, 0x20}});

        if (answer < 1)
            return done(answer);

        call.Call<Memcpy>({buffer, bounce, uint32_t(answer)});

        if (uint32_t(answer) < head)
            return done(answer);

        buffer += uint32_t(answer);
        length -= uint32_t(answer);
        total = answer;
    }

    if (const uint32_t middle = length & ~0x1Fu) {
        answer = Ioctlv(cpu, uint32_t(fd), kRead, 2, 1, vectors, {{result, 0x20}, {buffer, middle}, {ctx, 0x20}});

        if (answer < 1)
            return done(total > 0 ? total : answer);

        total += answer;

        if (uint32_t(answer) < middle)
            return done(total);

        buffer += uint32_t(answer);
        length -= uint32_t(answer);
    }

    if (length & 0x1F) {
        call.Call<Memset>({bounce, 0, 0x20});
        answer = Ioctlv(cpu, uint32_t(fd), kRead, 2, 1, vectors, {{result, 0x20}, {bounce, length & 0x1F}, {ctx, 0x20}});

        if (answer < 1)
            return done(total > 0 ? total : answer);

        total += answer;
        call.Call<Memcpy>({buffer, bounce, uint32_t(answer)});
    }

    return done(total > 0 ? total : answer);
}

// 0x8051C418: SSLWrite(context, buffer, length): the bytes written, or the
// device's error when none were.
int32_t Write(CpuContext* cpu, uint32_t context, uint32_t buffer, uint32_t length) {
    GuestCall call{cpu};
    const int32_t fd = int32_t(call.Call<IOSOpen>({kDevice, 0}));

    if (fd < 0)
        return kError;

    Slots slot(cpu, 0x80);
    const uint32_t vectors = slot[0], result = slot[0x20], ctx = slot[0x40], bounce = slot[0x60];
    uint32_t head = (buffer & 0x1F) ? 0x20 - (buffer & 0x1F) : 0;
    call.Call<Memset>({bounce, 0, 0x20});
    int32_t total = 0, answer = kError;
    Memory::Write32(ctx, context);

    auto done = [&](int32_t value) {
        call.Call<IOSClose>({uint32_t(fd)});
        return value;
    };

    if (head) {
        if (length < head)
            head = length;

        call.Call<Memcpy>({bounce, buffer, head});
        answer = Ioctlv(cpu, uint32_t(fd), kWrite, 1, 2, vectors, {{result, 0x20}, {ctx, 0x20}, {bounce, head}});

        if (answer < 1 || uint32_t(answer) < head)
            return done(answer);

        buffer += uint32_t(answer);
        length -= uint32_t(answer);
        total = answer;
    }

    if (const uint32_t middle = length & ~0x1Fu) {
        answer = Ioctlv(cpu, uint32_t(fd), kWrite, 1, 2, vectors, {{result, 0x20}, {ctx, 0x20}, {buffer, middle}});

        if (answer < 1)
            return done(total > 0 ? total : answer);

        total += answer;

        if (uint32_t(answer) < middle)
            return done(total);

        buffer += uint32_t(answer);
        length -= uint32_t(answer);
    }

    if (const uint32_t tail = length & 0x1F) {
        call.Call<Memset>({bounce, 0, 0x20});
        call.Call<Memcpy>({bounce, buffer, tail});
        answer = Ioctlv(cpu, uint32_t(fd), kWrite, 1, 2, vectors, {{result, 0x20}, {ctx, 0x20}, {bounce, tail}});

        if (answer < 1)
            return done(total > 0 ? total : answer);

        total += answer;
    }

    return done(total > 0 ? total : answer);
}

// The certificate buffers and their mutex, once.
void CertsSetup(CpuContext* cpu) {
    const uint32_t msr = cpu->msr;
    cpu->msr &= ~0x8000u;

    if (!Memory::Read32(kCertsReady)) {
        GuestCall call{cpu};
        call.Call<OSInitMutex>({kCertMutex});
        call.Call<Memset>({kClientCert, 0, 0x1000});
        call.Call<Memset>({kClientKey, 0, 0x1000});
        call.Call<Memset>({kRootCa, 0, 0x1000});
        Memory::Write32(kCertsReady, 1);
    }

    cpu->msr = (msr & 0x8000u) ? cpu->msr | 0x8000u : cpu->msr & ~0x8000u;
}

// 0x8051C784: SSLSetClientCert(context, certificate, its length, key, its
// length).
int32_t SetClientCert(CpuContext* cpu, uint32_t context, uint32_t cert, uint32_t certLength, uint32_t key, uint32_t keyLength) {
    GuestCall call{cpu};
    const int32_t fd = int32_t(call.Call<IOSOpen>({kDevice, 0}));

    if (fd < 0)
        return kError;

    CertsSetup(cpu);
    call.Call<OSLockMutex>({kCertMutex});
    call.Call<Memcpy>({kClientCert, cert, certLength});
    call.Call<Memcpy>({kClientKey, key, keyLength});
    Slots slot(cpu, 0x80);
    const uint32_t vectors = slot[0], result = slot[0x20], ctx = slot[0x40];
    Memory::Write32(ctx, context);
    const int32_t answer = Ioctlv(cpu, uint32_t(fd), kSetClientCert, 1, 3, vectors, {{result, 0x20}, {ctx, 0x20}, {kClientCert, certLength}, {kClientKey, keyLength}});
    call.Call<OSUnlockMutex>({kCertMutex});
    call.Call<IOSClose>({uint32_t(fd)});
    return answer;
}

// 0x8051C8F0: SSLSetRootCA(context, certificate, its length).
int32_t SetRootCa(CpuContext* cpu, uint32_t context, uint32_t ca, uint32_t length) {
    GuestCall call{cpu};
    const int32_t fd = int32_t(call.Call<IOSOpen>({kDevice, 0}));

    if (fd < 0)
        return kError;

    CertsSetup(cpu);
    call.Call<OSLockMutex>({kCertMutex});
    call.Call<Memcpy>({kRootCa, ca, length});
    Slots slot(cpu, 0x80);
    const uint32_t vectors = slot[0], result = slot[0x20], ctx = slot[0x40];
    Memory::Write32(ctx, context);
    const int32_t answer = Ioctlv(cpu, uint32_t(fd), kSetRootCa, 1, 2, vectors, {{result, 0x20}, {ctx, 0x20}, {kRootCa, length}});
    call.Call<OSUnlockMutex>({kCertMutex});
    call.Call<IOSClose>({uint32_t(fd)});
    return answer;
}

uint32_t Arg(CpuContext* cpu, int n) {
    return cpu->gpr[n];
}

} // namespace

extern "C" void MH3_SSLNew(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(New(cpu, Arg(cpu, 3), Arg(cpu, 4)));
}
REGISTER_NATIVE_FUNCTION_AS(0x8051BE7C, MH3_SSLNew, "SSLNew");

extern "C" void MH3_SSLConnect(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(WordCall(cpu, kConnect, 1, 2, {Arg(cpu, 3), Arg(cpu, 4)}));
}
REGISTER_NATIVE_FUNCTION_AS(0x8051BFD4, MH3_SSLConnect, "SSLConnect");

extern "C" void MH3_SSLDoHandshake(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(WordCall(cpu, kHandshake, 1, 1, {Arg(cpu, 3)}));
}
REGISTER_NATIVE_FUNCTION_AS(0x8051C09C, MH3_SSLDoHandshake, "SSLDoHandshake");

extern "C" void MH3_SSLRead(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(Read(cpu, Arg(cpu, 3), Arg(cpu, 4), Arg(cpu, 5)));
}
REGISTER_NATIVE_FUNCTION_AS(0x8051C148, MH3_SSLRead, "SSLRead");

extern "C" void MH3_SSLWrite(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(Write(cpu, Arg(cpu, 3), Arg(cpu, 4), Arg(cpu, 5)));
}
REGISTER_NATIVE_FUNCTION_AS(0x8051C418, MH3_SSLWrite, "SSLWrite");

extern "C" void MH3_SSLShutdown(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(WordCall(cpu, kShutdown, 1, 1, {Arg(cpu, 3)}));
}
REGISTER_NATIVE_FUNCTION_AS(0x8051C6D8, MH3_SSLShutdown, "SSLShutdown");

extern "C" void MH3_SSLSetClientCert(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(SetClientCert(cpu, Arg(cpu, 3), Arg(cpu, 4), Arg(cpu, 5), Arg(cpu, 6), Arg(cpu, 7)));
}
REGISTER_NATIVE_FUNCTION_AS(0x8051C784, MH3_SSLSetClientCert, "SSLSetClientCert");

extern "C" void MH3_SSLSetRootCA(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(SetRootCa(cpu, Arg(cpu, 3), Arg(cpu, 4), Arg(cpu, 5)));
}
REGISTER_NATIVE_FUNCTION_AS(0x8051C8F0, MH3_SSLSetRootCA, "SSLSetRootCA");

extern "C" void MH3_SSLSetBuiltinRootCA(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(WordCall(cpu, kSetBuiltinRootCa, 1, 2, {Arg(cpu, 3), Arg(cpu, 4)}));
}
REGISTER_NATIVE_FUNCTION_AS(0x8051CA38, MH3_SSLSetBuiltinRootCA, "SSLSetBuiltinRootCA");

extern "C" void MH3_SSLSetBuiltinClientCert(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(WordCall(cpu, kSetBuiltinClientCert, 1, 2, {Arg(cpu, 3), Arg(cpu, 4)}));
}
REGISTER_NATIVE_FUNCTION_AS(0x8051CB00, MH3_SSLSetBuiltinClientCert, "SSLSetBuiltinClientCert");

// The library's mutex thunks (branches to OSInitMutex, OSLockMutex,
// OSUnlockMutex).
extern "C" void MH3_SSLInitMutex(CpuContext* cpu) {
    InvokeIndirectCpu(OSInitMutex, cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x8051CBC8, MH3_SSLInitMutex, "SSLiInitMutex");

extern "C" void MH3_SSLLockMutex(CpuContext* cpu) {
    InvokeIndirectCpu(OSLockMutex, cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x8051CBCC, MH3_SSLLockMutex, "SSLiLockMutex");

extern "C" void MH3_SSLUnlockMutex(CpuContext* cpu) {
    InvokeIndirectCpu(OSUnlockMutex, cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x8051CBD0, MH3_SSLUnlockMutex, "SSLiUnlockMutex");
