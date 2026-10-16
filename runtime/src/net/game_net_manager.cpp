// The game's session manager (Capcom's net library, RMHP08
// 0x803D4968..0x803D6D64; decompiled with Ghidra, the call arguments taken
// from the translated C++), ported natively: NetworkSessionManager, which
// owns the session and runs its requests (joining, leaving, hosting, ...).
// Each of 21 request kinds has a slot; a started request is one of two
// request blocks holding a pointer to a member function of the manager's
// class, its arguments and a timeout, stepped once per update until it is
// done. A request starts only when the others allow it: kind 2 waits for
// all others to end and holds every other kind back while it waits.
#include "abi_bridge.h"
#include "guest_call.h"
#include "memory.h"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <initializer_list>

namespace {

enum : uint32_t {
    OperatorNew = 0x800404DC,
    OperatorDelete = 0x80040564,
    ConstructArray = 0x80457198,
    DestroyArray = 0x80457290,
    IdConstruct = 0x803F881C,
    IdDestroy = 0x803CCE5C,
    MutexDestroy = 0x803CA39C,
    MutexConstruct = 0x803CA3E0,
    MutexUnlock = 0x803CF6B8,
    MutexLock = 0x803CF6C0,
    SessionSetFlag = 0x803CF6D0,
    ConnectionEvent = 0x803DD7B4,
    SessionEvent = 0x803DC5AC,
};

constexpr uint32_t kPlatformPtr = 0x80797180;
constexpr uint32_t kCrFloatArgs = 1u << (31 - 6);
constexpr uint32_t kManagerVtable = 0x805FB0C8;
constexpr uint32_t kRequestSerial = 0x80797160; // the next request's serial
constexpr uint32_t kNullMemberFunction = 0x80572BF0; // 3 words

// The manager: vtable, two words its owner sets, the session, the request
// slots and whether each runs, two options, then the request blocks.
enum : uint32_t {
    mOwner = 0x4,
    mOwnerParam = 0x8,
    mSession = 0xC,
    mSlots = 0x10,   // 21 x request
    mRunning = 0x64, // 21 x u8
    mOption79 = 0x79, // u8
    mOption7A = 0x7A, // u8
    mBlocks = 0x7C,  // 2 x 0xA4
};

constexpr uint32_t kKinds = 21;
constexpr uint32_t kBlock = 0xA4;

// A request block.
enum : uint32_t {
    rTimeout = 0x4C,  // f32 seconds, 0: none
    rStarted = 0x50,  // f32
    rResult = 0x54,   // 3 words, set by its step
    rSerial = 0x70,
    rCancelled = 0x74, // u8
    rMutex = 0x78,
    rCount = 0x28,
    rArgs = 0x2C,     // up to 8
    rOwner = 0x94,
    rMember = 0x98,   // pointer to member function (3 words)
};

// Manager virtuals.
enum : uint32_t {
    vClose = 0x14,
    vMapPeer = 0x1C0, // (manager, peer): the session's peer number of a player, or -1
};

// Session virtuals (NetworkSessionStable).
enum : uint32_t {
    sCloseAll = 0x10,
    sUpdate = 0x14,
    sPutData = 0x38,
    sDeliver = 0x44,
    sPeerRtt = 0x8C,
    sThroughput = 0x90,
};

uint32_t Read(uint32_t address) {
    return Memory::Read32(address);
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

// 0x803D4AB4 (and 0x803D4BC0, its branch): a request block back to free.
void RequestReset(uint32_t request) {
    Write(request, 0);
    SetF(request + 0x4C, 0.0f);
    SetF(request + rStarted, 0.0f);
    Write(request + rSerial, 0);
    Write(request + 0x24, 0);
    Memory::Write8(request + rCancelled, 0);
    Write(request + rOwner, 0);

    for (uint32_t i = 0; i < 12; i += 4)
        Write(request + rMember + i, Read(kNullMemberFunction + i));

    Write(request + rCount, 0);

    for (uint32_t at = 0x54; at <= 0x6C; at += 4)
        Write(request + at, 0);

    for (uint32_t at = 0x4; at <= 0x20; at += 4)
        Write(request + at, 0);

    for (uint32_t at = rArgs; at <= 0x48; at += 4)
        Write(request + at, 0);
}

bool RequestActive(uint32_t request) {
    return Read(request + rOwner) != 0;
}

// 0x803D4BC4 / 0x803D4B5C.
uint32_t RequestConstruct(CpuContext* cpu, uint32_t request) {
    GuestCall{cpu}.Call<MutexConstruct>({request + rMutex});
    RequestReset(request);
    return request;
}

uint32_t RequestDestroy(CpuContext* cpu, uint32_t request, int16_t how) {
    if (request) {
        RequestReset(request);
        GuestCall{cpu}.Call<MutexDestroy>({request + rMutex, 0xFFFFFFFF});

        if (how > 0)
            GuestCall{cpu}.Call<OperatorDelete>({request});
    }

    return request;
}

// 0x803D4C88 (virtual 0x10): no session, no request.
void ManagerReset(uint32_t manager) {
    Write(manager + mSession, 0);

    for (uint32_t kind = 0; kind < kKinds; ++kind) {
        Write(manager + mSlots + kind * 4, 0);
        Memory::Write8(manager + mRunning + kind, 0);
    }

    for (uint32_t block = 0; block < 2; ++block)
        RequestReset(manager + mBlocks + block * kBlock);
}

// 0x803D4968.
uint32_t ManagerConstruct(CpuContext* cpu, uint32_t manager) {
    GuestCall game{cpu};
    Write(manager, kManagerVtable);
    game.Call<ConstructArray>({manager + mBlocks, 0x803D4BC4, 0x803D4B5C, kBlock, 2});
    Write(manager + mOwner, 0);
    Write(manager + mOwnerParam, 0);
    ManagerReset(manager);
    Memory::Write8(manager + mOption79, 1);
    Memory::Write8(manager + mOption7A, 1);
    return manager;
}

// 0x803D6508: frees a slot's request (one still running is reported).
void ReleaseRequest(CpuContext* cpu, uint32_t slot) {
    if (const uint32_t request = Read(slot)) {
        if (RequestActive(request))
            Log(cpu, 0x14, {0x805FB088});

        RequestReset(request);
    }

    Write(slot, 0);
}

// 0x803D4D84 (virtual 0x14): the session closed and deleted, every request
// freed.
void ManagerClose(CpuContext* cpu, uint32_t manager) {
    GuestCall game{cpu};

    if (Read(manager + mSession)) {
        game.Virtual(Read(manager + mSession), sCloseAll);

        if (const uint32_t session = Read(manager + mSession)) {
            game.Virtual(session, 0x8, {1});
            Write(manager + mSession, 0);
        }
    }

    for (uint32_t kind = 0; kind < kKinds; ++kind)
        ReleaseRequest(cpu, manager + mSlots + kind * 4);

    for (uint32_t block = 0; block < 2; ++block)
        RequestReset(manager + mBlocks + block * kBlock);
}

// 0x803D4C00.
uint32_t ManagerDestroy(CpuContext* cpu, uint32_t manager, int16_t how) {
    if (manager) {
        GuestCall game{cpu};
        Write(manager, kManagerVtable);
        ManagerClose(cpu, manager);
        game.Call<DestroyArray>({manager + mBlocks, 0x803D4B5C, kBlock, 2});

        if (how > 0)
            game.Call<OperatorDelete>({manager});
    }

    return manager;
}

// 0x803D50E8: a request's step: its member function with the request; done
// when it returns nonzero.
void RequestStep(CpuContext* cpu, uint32_t request) {
    const uint32_t owner = Read(request + rOwner);

    if (!owner)
        return;

    const uint32_t self = owner + Read(request + rMember);
    const int32_t slot = int32_t(Read(request + rMember + 4));
    uint32_t fn = Read(request + rMember + 8);

    if (slot >= 0)
        fn = Read(Read(self + fn) + uint32_t(slot));

    if (GuestCall{cpu}(fn, {self, request}))
        RequestReset(request);
}

bool Running(uint32_t manager, uint32_t kind) {
    return Memory::Read8(manager + mRunning + kind) != 0;
}

// Whether a waiting request of `kind` may start now.
bool MayStart(CpuContext* cpu, uint32_t manager, uint32_t kind) {
    if (kind != 2 && Read(manager + mSlots + 2 * 4))
        return false;

    switch (kind) {
    case 1:
        return !Running(manager, 2);

    case 2: {
        uint32_t first = 0;

        while (first < kKinds && !Running(manager, first))
            ++first;

        if (first != kKinds) {
            Log(cpu, 0x14, {0x805FAF48, first});
            return false;
        }

        return true;
    }

    case 3:
    case 6:
        return !Running(manager, 11) && !Running(manager, 12) && !Running(manager, 2) && !Running(manager, 1);

    case 10:
        return !Running(manager, 11);

    case 12:
        if (Running(manager, 11))
            return false;

        [[fallthrough]];

    case 11:
        return !Running(manager, 2) && !Running(manager, 1) && !Running(manager, 3) && !Running(manager, 6) && !Running(manager, 10);

    default:
        return true;
    }
}

// 0x803D4E48 (virtual 0x18): the session's update, then the requests: a
// waiting one starts when allowed, a running one steps and its slot frees
// when it is done.
void ManagerUpdate(CpuContext* cpu, uint32_t manager) {
    if (const uint32_t session = Read(manager + mSession))
        GuestCall{cpu}.Virtual(session, sUpdate);

    for (uint32_t kind = 0; kind < kKinds; ++kind) {
        const uint32_t slot = manager + mSlots + kind * 4;
        uint32_t request = Read(slot);

        if (request && !Running(manager, kind)) {
            if (!MayStart(cpu, manager, kind))
                continue;

            Memory::Write8(manager + mRunning + kind, 1);
        }

        request = Read(slot);

        if (request && Running(manager, kind)) {
            RequestStep(cpu, request);

            if (!RequestActive(request)) {
                ReleaseRequest(cpu, slot);
                Memory::Write8(manager + mRunning + kind, 0);
            }
        }
    }
}

// 0x803D6494: a free request block, or 0.
uint32_t FreeBlock(uint32_t manager) {
    for (uint32_t block = 0; block < 2; ++block) {
        const uint32_t request = manager + mBlocks + block * kBlock;

        if (!RequestActive(request))
            return request;
    }

    return 0;
}

// 0x803D51B4: a request block takes its member function and arguments.
void RequestStart(CpuContext* cpu, uint32_t request, uint32_t owner, uint32_t member, uint32_t count, std::initializer_list<uint32_t> args) {
    RequestReset(request);
    SetF(request + rStarted, float(Now(cpu)));
    Write(request + rSerial, Read(kRequestSerial));
    Write(kRequestSerial, Read(kRequestSerial) + 1);
    Write(request + rOwner, owner);

    for (uint32_t i = 0; i < 12; i += 4)
        Write(request + rMember + i, Read(member + i));

    Write(request + rCount, std::min<uint32_t>(count, 8));
    uint32_t at = request + rArgs;

    for (uint32_t value : args) {
        if (at >= request + rArgs + Read(request + rCount) * 4)
            break;

        Write(at, value);
        at += 4;
    }
}

// The request kinds: their slot and member function (0x805FAF8C ..).
struct Kind {
    uint32_t start;
    uint32_t slot;
    uint32_t member;
    uint32_t count;
};

constexpr Kind kKindTable[] = {
    {0x803D513C, 0x14, 0x805FAF8C, 0},
    {0x803D52F0, 0x18, 0x805FAF98, 0},
    {0x803D537C, 0x1C, 0x805FAFA4, 2},
    {0x803D5414, 0x20, 0x805FAFB0, 1},
    {0x803D549C, 0x24, 0x805FAFBC, 1},
    {0x803D5524, 0x28, 0x805FAFC8, 1},
    {0x803D55AC, 0x28, 0x805FAFD4, 1},
    {0x803D5634, 0x2C, 0x805FAFE0, 0},
    {0x803D56AC, 0x50, 0x805FAFEC, 1},
    {0x803D5734, 0x54, 0x805FAFF8, 1},
    {0x803D57BC, 0x60, 0x805FB004, 0},
    {0x803D5834, 0x30, 0x805FB010, 1},
    {0x803D58BC, 0x34, 0x805FB01C, 0},
    {0x803D5934, 0x5C, 0x805FB028, 0},
    {0x803D59AC, 0x38, 0x805FB034, 0},
    {0x803D5A24, 0x3C, 0x805FB040, 0},
    {0x803D5A9C, 0x4C, 0x805FB04C, 3},
    {0x803D5B44, 0x40, 0x805FB058, 0},
    {0x803D5BBC, 0x44, 0x805FB064, 2},
    {0x803D5C54, 0x48, 0x805FB070, 2},
    {0x803D5CEC, 0x58, 0x805FB07C, 1},
};

// A request starter: when its slot is empty and a block is free.
void StartKind(CpuContext* cpu, const Kind& kind) {
    const uint32_t manager = Arg(cpu, 3);
    const uint32_t a = Arg(cpu, 4), b = Arg(cpu, 5), c = Arg(cpu, 6);

    if (Read(manager + kind.slot))
        return;

    const uint32_t request = FreeBlock(manager);

    if (!request)
        return;

    Write(manager + kind.slot, request);
    RequestStart(cpu, request, manager, kind.member, kind.count, {a, b, c});
}

// Data to players through the session (0x803D5F5C, 0x803D5FB0, 0x803D6000):
// one target (all or the others), a channel and a limit.
void PutTo(CpuContext* cpu, uint32_t manager, uint32_t data, uint32_t length, uint32_t channel, int8_t target, uint32_t limit) {
    GuestFrame frame(cpu, 0x20);
    const uint32_t targets = frame[0x8];
    Memory::Write8(targets, uint8_t(target));

    if (const uint32_t session = Read(manager + mSession))
        GuestCall{cpu}.Virtual(session, sPutData, {data, length, channel, 1, targets, limit & 0xFF});
}

// 0x803D6054 / 0x803D6124: data to the listed players (mapped to the
// session's peers; unknown ones left out).
void PutToPlayers(CpuContext* cpu, uint32_t manager, uint32_t data, uint32_t length, int32_t count, uint32_t players, uint32_t channel, uint32_t limit) {
    if (count < 1)
        return;

    GuestCall game{cpu};
    GuestFrame frame(cpu, 0x60);
    const uint32_t targets = frame[0x10];
    uint32_t mapped = 0;

    for (int32_t i = 0; i < count; ++i) {
        const int8_t peer = int8_t(game.Virtual(manager, vMapPeer, {uint32_t(int32_t(int8_t(Memory::Read8(players + uint32_t(i)))))}));

        if (peer >= 0)
            Memory::Write8(targets + mapped++, uint8_t(peer));
    }

    if (const uint32_t session = Read(manager + mSession))
        game.Virtual(session, sPutData, {data, length, channel, mapped, targets, limit & 0xFF});
}

// 0x803D61F8 / 0x803D6290: data to one player.
void PutToPlayer(CpuContext* cpu, uint32_t manager, uint32_t data, uint32_t length, int8_t player, uint32_t channel, uint32_t limit) {
    GuestCall game{cpu};
    GuestFrame frame(cpu, 0x20);
    const uint32_t targets = frame[0x8];
    const int8_t peer = int8_t(game.Virtual(manager, vMapPeer, {uint32_t(int32_t(player))}));
    Memory::Write8(targets, uint8_t(peer));

    if (peer < 0)
        return;

    if (const uint32_t session = Read(manager + mSession))
        game.Virtual(session, sPutData, {data, length, channel, 1, targets, limit});
}

// A session virtual with the caller's registers (r3 the session): the
// original branches to it. Without a session, 0.
void ForwardToSession(CpuContext* cpu, uint32_t slot) {
    const uint32_t session = Read(Arg(cpu, 3) + mSession);
    cpu->gpr[3] = session;

    if (session)
        GuestCall{cpu}(Read(Read(session) + slot));
}

// 0x803D66D0: whether a request's timeout passed.
bool TimedOut(CpuContext* cpu, uint32_t request) {
    GuestCall game{cpu};
    game.Call<MutexLock>({request + rMutex});
    bool late = false;

    if (F(request + rTimeout) != 0.0f) {
        const double now = Now(cpu);
        late = F(request + rTimeout) < float(now - double(F(request + rStarted)));
    }

    game.Call<MutexUnlock>({request + rMutex});
    return late;
}

// 0x803D6754: a request's timeout (f1 seconds) from now.
void SetTimeout(CpuContext* cpu, uint32_t request, double seconds) {
    GuestCall game{cpu};
    game.Call<MutexLock>({request + rMutex});
    SetF(request + rStarted, float(Now(cpu)));
    SetF(request + rTimeout, float(seconds));
    game.Call<MutexUnlock>({request + rMutex});
}

// 0x803D6578: a request's result, when it has one.
bool TakeResult(CpuContext* cpu, uint32_t request, uint32_t out) {
    GuestCall game{cpu};
    game.Call<MutexLock>({request + rMutex});
    const uint32_t result = Read(request + rResult);

    if (result) {
        Write(out, result);
        Write(out + 4, Read(request + rResult + 4));
        Write(out + 8, Read(request + rResult + 8));
    }

    game.Call<MutexUnlock>({request + rMutex});
    return result != 0;
}

// 0x803D65F0.
void SetResult(CpuContext* cpu, uint32_t request, uint32_t a, uint32_t b, uint32_t c) {
    GuestCall game{cpu};
    game.Call<MutexLock>({request + rMutex});
    Write(request + rResult + 4, b);
    Write(request + rResult + 8, c);
    Write(request + rResult, a);
    game.Call<MutexUnlock>({request + rMutex});
}

// 0x803D6658: a request's argument, 0 (with an error) past the last.
uint32_t Argument(CpuContext* cpu, uint32_t request, uint32_t index) {
    const uint32_t count = Read(request + rCount);

    if (index < count)
        return Read(request + rArgs + index * 4);

    Log(cpu, 0x10, {0x805FB290, count, index});
    return 0;
}

// 0x803D6C80 (and 0x803D6C7C, its branch): the derived manager's request
// block back to free (the same fields).
void DerivedRequestReset(uint32_t request) {
    RequestReset(request);
}

constexpr uint32_t kDerivedVtable = 0x805FB8B0;
constexpr uint32_t kBigSingleton = 0x807971A4;

// 0x803D6D28 / 0x803D6C18.
uint32_t DerivedRequestConstruct(CpuContext* cpu, uint32_t request) {
    GuestCall{cpu}.Call<MutexConstruct>({request + rMutex});
    DerivedRequestReset(request);
    return request;
}

uint32_t DerivedRequestDestroy(CpuContext* cpu, uint32_t request, int16_t how) {
    if (request) {
        DerivedRequestReset(request);
        GuestCall{cpu}.Call<MutexDestroy>({request + rMutex, 0xFFFFFFFF});

        if (how > 0)
            GuestCall{cpu}.Call<OperatorDelete>({request});
    }

    return request;
}

// An object with an id at `at` (0x803D6B50 / 0x803D6A9C at 0x108,
// 0x803D6BE4 / 0x803D6B84 at 8, 0x803D68A4 / 0x803D6848 at 0).
uint32_t WithIdConstruct(CpuContext* cpu, uint32_t object, uint32_t at) {
    GuestCall{cpu}.Call<IdConstruct>({object + at});
    return object;
}

uint32_t WithIdDestroy(CpuContext* cpu, uint32_t object, int16_t how, uint32_t at) {
    if (object) {
        GuestCall{cpu}.Call<IdDestroy>({object + at, 0xFFFFFFFF});

        if (how > 0)
            GuestCall{cpu}.Call<OperatorDelete>({object});
    }

    return object;
}

// 0x803D6B04 / 0x803D6A30: a table of 32 player entries (0x31C each).
uint32_t PlayerTableConstruct(CpuContext* cpu, uint32_t table) {
    GuestCall{cpu}.Call<ConstructArray>({table + 4, 0x803D6B50, 0x803D6A9C, 0x31C, 0x20});
    return table;
}

uint32_t PlayerTableDestroy(CpuContext* cpu, uint32_t table, int16_t how) {
    if (table) {
        GuestCall{cpu}.Call<DestroyArray>({table + 4, 0x803D6A9C, 0x31C, 0x20});

        if (how > 0)
            GuestCall{cpu}.Call<OperatorDelete>({table});
    }

    return table;
}

// 0x803D6934: the game's manager: the base, two more request blocks, an
// id, four member entries and the player table; the matchmaking objects are
// created once (0xD640 and 0x44A0 bytes); then it resets.
uint32_t DerivedConstruct(CpuContext* cpu, uint32_t manager) {
    GuestCall game{cpu};
    ManagerConstruct(cpu, manager);
    Write(manager, kDerivedVtable);
    game.Call<ConstructArray>({manager + 0x1C4, 0x803D6D28, 0x803D6C18, kBlock, 2});
    game.Call<IdConstruct>({manager + 0x3CC});
    game.Call<ConstructArray>({manager + 0x538, 0x803D6BE4, 0x803D6B84, 0x48, 4});
    PlayerTableConstruct(cpu, manager + 0xAF0);

    if (!game.Call<0x80376954>()) {
        if (const uint32_t object = game.Call<OperatorNew>({0xD640}))
            game.Call<0x803FCCB0>({object});
    }

    if (!Read(kBigSingleton)) {
        if (const uint32_t object = game.Call<OperatorNew>({0x44A0}))
            game.Call<0x8041CB00>({object});
    }

    Memory::Write8(manager + 0x6E75, 0);
    game.Virtual(manager, 0x10);
    return manager;
}

// 0x803D6D64.
uint32_t DerivedDestroy(CpuContext* cpu, uint32_t manager, int16_t how) {
    if (manager) {
        GuestCall game{cpu};
        Write(manager, kDerivedVtable);
        game.Virtual(manager, vClose);
        PlayerTableDestroy(cpu, manager + 0xAF0, -1);
        game.Call<DestroyArray>({manager + 0x538, 0x803D6B84, 0x48, 4});
        game.Call<IdDestroy>({manager + 0x3CC, 0xFFFFFFFF});
        game.Call<DestroyArray>({manager + 0x1C4, 0x803D6C18, kBlock, 2});
        ManagerDestroy(cpu, manager, 0);

        if (how > 0)
            game.Call<OperatorDelete>({manager});
    }

    return manager;
}

} // namespace

extern "C" void MH3_NetManagerConstruct(CpuContext* cpu) {
    cpu->gpr[3] = ManagerConstruct(cpu, Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x803D4968, MH3_NetManagerConstruct, "NetManagerConstruct");

extern "C" void MH3_NetRequestReset(CpuContext* cpu) {
    RequestReset(Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x803D4AB4, MH3_NetRequestReset, "NetRequestReset");

extern "C" void MH3_NetRequestDestroy(CpuContext* cpu) {
    cpu->gpr[3] = RequestDestroy(cpu, Arg(cpu, 3), int16_t(Arg(cpu, 4)));
}
REGISTER_NATIVE_FUNCTION_AS(0x803D4B5C, MH3_NetRequestDestroy, "NetRequestDestroy");

extern "C" void MH3_NetRequestReset2(CpuContext* cpu) {
    RequestReset(Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x803D4BC0, MH3_NetRequestReset2, "NetRequestReset2");

extern "C" void MH3_NetRequestConstruct(CpuContext* cpu) {
    cpu->gpr[3] = RequestConstruct(cpu, Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x803D4BC4, MH3_NetRequestConstruct, "NetRequestConstruct");

extern "C" void MH3_NetManagerDestroy(CpuContext* cpu) {
    cpu->gpr[3] = ManagerDestroy(cpu, Arg(cpu, 3), int16_t(Arg(cpu, 4)));
}
REGISTER_NATIVE_FUNCTION_AS(0x803D4C00, MH3_NetManagerDestroy, "NetManagerDestroy");

// 0x803D4C7C (virtual 0xC): the owner's callback and parameter, then a reset.
extern "C" void MH3_NetManagerInit(CpuContext* cpu) {
    const uint32_t manager = Arg(cpu, 3);
    Write(manager + mOwner, Arg(cpu, 4));
    Write(manager + mOwnerParam, Arg(cpu, 5));
    ManagerReset(manager);
}
REGISTER_NATIVE_FUNCTION_AS(0x803D4C7C, MH3_NetManagerInit, "NetManagerInit");

extern "C" void MH3_NetManagerReset(CpuContext* cpu) {
    ManagerReset(Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x803D4C88, MH3_NetManagerReset, "NetManagerReset");

extern "C" void MH3_NetManagerClose(CpuContext* cpu) {
    ManagerClose(cpu, Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x803D4D84, MH3_NetManagerClose, "NetManagerClose");

extern "C" void MH3_NetManagerUpdate(CpuContext* cpu) {
    ManagerUpdate(cpu, Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x803D4E48, MH3_NetManagerUpdate, "NetManagerUpdate");

extern "C" void MH3_NetRequestActive(CpuContext* cpu) {
    cpu->gpr[3] = RequestActive(Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x803D50D4, MH3_NetRequestActive, "NetRequestActive");

extern "C" void MH3_NetRequestStep(CpuContext* cpu) {
    RequestStep(cpu, Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x803D50E8, MH3_NetRequestStep, "NetRequestStep");

// 0x803D51B4: variadic (count, then that many words in r7..).
extern "C" void MH3_NetRequestStart(CpuContext* cpu) {
    RequestStart(cpu, Arg(cpu, 3), Arg(cpu, 4), Arg(cpu, 5), Arg(cpu, 6), {Arg(cpu, 7), Arg(cpu, 8), Arg(cpu, 9), Arg(cpu, 10)});
}
REGISTER_NATIVE_FUNCTION_AS(0x803D51B4, MH3_NetRequestStart, "NetRequestStart");

extern "C" void MH3_NetStartRequest0(CpuContext* cpu) {
    StartKind(cpu, kKindTable[0]);
}
REGISTER_NATIVE_FUNCTION_AS(0x803D513C, MH3_NetStartRequest0, "NetStartRequest0");

extern "C" void MH3_NetStartRequest1(CpuContext* cpu) {
    StartKind(cpu, kKindTable[1]);
}
REGISTER_NATIVE_FUNCTION_AS(0x803D52F0, MH3_NetStartRequest1, "NetStartRequest1");

extern "C" void MH3_NetStartRequest2(CpuContext* cpu) {
    StartKind(cpu, kKindTable[2]);
}
REGISTER_NATIVE_FUNCTION_AS(0x803D537C, MH3_NetStartRequest2, "NetStartRequest2");

extern "C" void MH3_NetStartRequest3(CpuContext* cpu) {
    StartKind(cpu, kKindTable[3]);
}
REGISTER_NATIVE_FUNCTION_AS(0x803D5414, MH3_NetStartRequest3, "NetStartRequest3");

extern "C" void MH3_NetStartRequest4(CpuContext* cpu) {
    StartKind(cpu, kKindTable[4]);
}
REGISTER_NATIVE_FUNCTION_AS(0x803D549C, MH3_NetStartRequest4, "NetStartRequest4");

extern "C" void MH3_NetStartRequest5(CpuContext* cpu) {
    StartKind(cpu, kKindTable[5]);
}
REGISTER_NATIVE_FUNCTION_AS(0x803D5524, MH3_NetStartRequest5, "NetStartRequest5");

extern "C" void MH3_NetStartRequest6(CpuContext* cpu) {
    StartKind(cpu, kKindTable[6]);
}
REGISTER_NATIVE_FUNCTION_AS(0x803D55AC, MH3_NetStartRequest6, "NetStartRequest6");

extern "C" void MH3_NetStartRequest7(CpuContext* cpu) {
    StartKind(cpu, kKindTable[7]);
}
REGISTER_NATIVE_FUNCTION_AS(0x803D5634, MH3_NetStartRequest7, "NetStartRequest7");

extern "C" void MH3_NetStartRequest8(CpuContext* cpu) {
    StartKind(cpu, kKindTable[8]);
}
REGISTER_NATIVE_FUNCTION_AS(0x803D56AC, MH3_NetStartRequest8, "NetStartRequest8");

extern "C" void MH3_NetStartRequest9(CpuContext* cpu) {
    StartKind(cpu, kKindTable[9]);
}
REGISTER_NATIVE_FUNCTION_AS(0x803D5734, MH3_NetStartRequest9, "NetStartRequest9");

extern "C" void MH3_NetStartRequest10(CpuContext* cpu) {
    StartKind(cpu, kKindTable[10]);
}
REGISTER_NATIVE_FUNCTION_AS(0x803D57BC, MH3_NetStartRequest10, "NetStartRequest10");

extern "C" void MH3_NetStartRequest11(CpuContext* cpu) {
    StartKind(cpu, kKindTable[11]);
}
REGISTER_NATIVE_FUNCTION_AS(0x803D5834, MH3_NetStartRequest11, "NetStartRequest11");

extern "C" void MH3_NetStartRequest12(CpuContext* cpu) {
    StartKind(cpu, kKindTable[12]);
}
REGISTER_NATIVE_FUNCTION_AS(0x803D58BC, MH3_NetStartRequest12, "NetStartRequest12");

extern "C" void MH3_NetStartRequest13(CpuContext* cpu) {
    StartKind(cpu, kKindTable[13]);
}
REGISTER_NATIVE_FUNCTION_AS(0x803D5934, MH3_NetStartRequest13, "NetStartRequest13");

extern "C" void MH3_NetStartRequest14(CpuContext* cpu) {
    StartKind(cpu, kKindTable[14]);
}
REGISTER_NATIVE_FUNCTION_AS(0x803D59AC, MH3_NetStartRequest14, "NetStartRequest14");

extern "C" void MH3_NetStartRequest15(CpuContext* cpu) {
    StartKind(cpu, kKindTable[15]);
}
REGISTER_NATIVE_FUNCTION_AS(0x803D5A24, MH3_NetStartRequest15, "NetStartRequest15");

extern "C" void MH3_NetStartRequest16(CpuContext* cpu) {
    StartKind(cpu, kKindTable[16]);
}
REGISTER_NATIVE_FUNCTION_AS(0x803D5A9C, MH3_NetStartRequest16, "NetStartRequest16");

extern "C" void MH3_NetStartRequest17(CpuContext* cpu) {
    StartKind(cpu, kKindTable[17]);
}
REGISTER_NATIVE_FUNCTION_AS(0x803D5B44, MH3_NetStartRequest17, "NetStartRequest17");

extern "C" void MH3_NetStartRequest18(CpuContext* cpu) {
    StartKind(cpu, kKindTable[18]);
}
REGISTER_NATIVE_FUNCTION_AS(0x803D5BBC, MH3_NetStartRequest18, "NetStartRequest18");

extern "C" void MH3_NetStartRequest19(CpuContext* cpu) {
    StartKind(cpu, kKindTable[19]);
}
REGISTER_NATIVE_FUNCTION_AS(0x803D5C54, MH3_NetStartRequest19, "NetStartRequest19");

extern "C" void MH3_NetStartRequest20(CpuContext* cpu) {
    StartKind(cpu, kKindTable[20]);
}
REGISTER_NATIVE_FUNCTION_AS(0x803D5CEC, MH3_NetStartRequest20, "NetStartRequest20");

// 0x803D5368 (virtual 0x24): whether there is a session.
extern "C" void MH3_NetManagerHasSession(CpuContext* cpu) {
    cpu->gpr[3] = Read(Arg(cpu, 3) + mSession) != 0;
}
REGISTER_NATIVE_FUNCTION_AS(0x803D5368, MH3_NetManagerHasSession, "NetManagerHasSession");

// 0x803D5DBC / 0x803D5DC8: a request's cancel flag.
extern "C" void MH3_NetRequestCancel(CpuContext* cpu) {
    Memory::Write8(Arg(cpu, 3) + rCancelled, 1);
}
REGISTER_NATIVE_FUNCTION_AS(0x803D5DBC, MH3_NetRequestCancel, "NetRequestCancel");

extern "C" void MH3_NetRequestCancelled(CpuContext* cpu) {
    cpu->gpr[3] = Memory::Read8(Arg(cpu, 3) + rCancelled);
}
REGISTER_NATIVE_FUNCTION_AS(0x803D5DC8, MH3_NetRequestCancelled, "NetRequestCancelled");

// 0x803D5D74 / 0x803D5DD0: cancels the request in slot 0x20 or 0x48.
void CancelSlot(uint32_t manager, uint32_t slot) {
    const uint32_t request = Read(manager + slot);

    if (request && !Memory::Read8(request + rCancelled))
        Memory::Write8(request + rCancelled, 1);
}

extern "C" void MH3_NetManagerCancel20(CpuContext* cpu) {
    CancelSlot(Arg(cpu, 3), 0x20);
}
REGISTER_NATIVE_FUNCTION_AS(0x803D5D74, MH3_NetManagerCancel20, "NetManagerCancel20");

extern "C" void MH3_NetManagerCancel48(CpuContext* cpu) {
    CancelSlot(Arg(cpu, 3), 0x48);
}
REGISTER_NATIVE_FUNCTION_AS(0x803D5DD0, MH3_NetManagerCancel48, "NetManagerCancel48");

extern "C" void MH3_NetManagerSetOption79(CpuContext* cpu) {
    Memory::Write8(Arg(cpu, 3) + mOption79, uint8_t(Arg(cpu, 4)));
}
REGISTER_NATIVE_FUNCTION_AS(0x803D5E18, MH3_NetManagerSetOption79, "NetManagerSetOption79");

extern "C" void MH3_NetManagerSetFlag(CpuContext* cpu) {
    GuestCall{cpu}.Call<SessionSetFlag>({Arg(cpu, 4)});
}
REGISTER_NATIVE_FUNCTION_AS(0x803D5E20, MH3_NetManagerSetFlag, "NetManagerSetFlag");

extern "C" void MH3_NetManagerSetOption7A(CpuContext* cpu) {
    Memory::Write8(Arg(cpu, 3) + mOption7A, uint8_t(Arg(cpu, 4)));
}
REGISTER_NATIVE_FUNCTION_AS(0x803D5E28, MH3_NetManagerSetOption7A, "NetManagerSetOption7A");

// 0x803D5E30 / 0x803D5E9C: a player's throughput and round trip.
extern "C" void MH3_NetManagerThroughput(CpuContext* cpu) {
    const uint32_t manager = Arg(cpu, 3);
    const uint32_t player = uint32_t(int32_t(int8_t(Arg(cpu, 4))));

    if (!Read(manager + mSession)) {
        cpu->gpr[3] = 0;
        return;
    }

    GuestCall game{cpu};
    const int8_t peer = int8_t(game.Virtual(manager, vMapPeer, {player}));
    cpu->gpr[3] = game.Virtual(Read(manager + mSession), sThroughput, {uint32_t(int32_t(peer))});
}
REGISTER_NATIVE_FUNCTION_AS(0x803D5E30, MH3_NetManagerThroughput, "NetManagerThroughput");

extern "C" void MH3_NetManagerPeerRtt(CpuContext* cpu) {
    const uint32_t manager = Arg(cpu, 3);
    const uint32_t player = uint32_t(int32_t(int8_t(Arg(cpu, 4))));

    if (!Read(manager + mSession)) {
        cpu->fpr[1].d = 0.0;
        return;
    }

    GuestCall game{cpu};
    const int8_t peer = int8_t(game.Virtual(manager, vMapPeer, {player}));
    game.Virtual(Read(manager + mSession), sPeerRtt, {uint32_t(int32_t(peer))});
}
REGISTER_NATIVE_FUNCTION_AS(0x803D5E9C, MH3_NetManagerPeerRtt, "NetManagerPeerRtt");

// 0x803D5F08: virtual 0x138 to players 0..3.
extern "C" void MH3_NetManagerToAll(CpuContext* cpu) {
    const uint32_t manager = Arg(cpu, 3), a = Arg(cpu, 4), b = Arg(cpu, 5);
    GuestFrame frame(cpu, 0x10);
    const uint32_t players = frame[0x8];

    for (uint32_t i = 0; i < 4; ++i)
        Memory::Write8(players + i, uint8_t(i));

    GuestCall{cpu}.Virtual(manager, 0x138, {a, b, 4, players});
}
REGISTER_NATIVE_FUNCTION_AS(0x803D5F08, MH3_NetManagerToAll, "NetManagerToAll");

extern "C" void MH3_NetManagerPutAllLow(CpuContext* cpu) {
    PutTo(cpu, Arg(cpu, 3), Arg(cpu, 4), Arg(cpu, 5), 1, -1, Arg(cpu, 6));
}
REGISTER_NATIVE_FUNCTION_AS(0x803D5F5C, MH3_NetManagerPutAllLow, "NetManagerPutAllLow");

extern "C" void MH3_NetManagerPutOthers(CpuContext* cpu) {
    PutTo(cpu, Arg(cpu, 3), Arg(cpu, 4), Arg(cpu, 5), 0, -2, 0xFF);
}
REGISTER_NATIVE_FUNCTION_AS(0x803D5FB0, MH3_NetManagerPutOthers, "NetManagerPutOthers");

extern "C" void MH3_NetManagerPutOthersLow(CpuContext* cpu) {
    PutTo(cpu, Arg(cpu, 3), Arg(cpu, 4), Arg(cpu, 5), 1, -2, Arg(cpu, 6));
}
REGISTER_NATIVE_FUNCTION_AS(0x803D6000, MH3_NetManagerPutOthersLow, "NetManagerPutOthersLow");

extern "C" void MH3_NetManagerPutPlayers(CpuContext* cpu) {
    PutToPlayers(cpu, Arg(cpu, 3), Arg(cpu, 4), Arg(cpu, 5), int32_t(Arg(cpu, 6)), Arg(cpu, 7), 0, 0xFF);
}
REGISTER_NATIVE_FUNCTION_AS(0x803D6054, MH3_NetManagerPutPlayers, "NetManagerPutPlayers");

extern "C" void MH3_NetManagerPutPlayersLow(CpuContext* cpu) {
    PutToPlayers(cpu, Arg(cpu, 3), Arg(cpu, 4), Arg(cpu, 5), int32_t(Arg(cpu, 6)), Arg(cpu, 7), 1, Arg(cpu, 8));
}
REGISTER_NATIVE_FUNCTION_AS(0x803D6124, MH3_NetManagerPutPlayersLow, "NetManagerPutPlayersLow");

extern "C" void MH3_NetManagerPutPlayer(CpuContext* cpu) {
    PutToPlayer(cpu, Arg(cpu, 3), Arg(cpu, 4), Arg(cpu, 5), int8_t(Arg(cpu, 6)), 0, 0xFF);
}
REGISTER_NATIVE_FUNCTION_AS(0x803D61F8, MH3_NetManagerPutPlayer, "NetManagerPutPlayer");

extern "C" void MH3_NetManagerPutPlayerLow(CpuContext* cpu) {
    PutToPlayer(cpu, Arg(cpu, 3), Arg(cpu, 4), Arg(cpu, 5), int8_t(Arg(cpu, 6)), 1, Arg(cpu, 7) & 0xFF);
}
REGISTER_NATIVE_FUNCTION_AS(0x803D6290, MH3_NetManagerPutPlayerLow, "NetManagerPutPlayerLow");

// 0x803D6334: when the manager allows it (virtual 0x28), the session
// delivers its data.
extern "C" void MH3_NetManagerDeliver(CpuContext* cpu) {
    const uint32_t manager = Arg(cpu, 3);

    if (!Read(manager + mSession))
        return;

    GuestCall game{cpu};

    if (game.Virtual(manager, 0x28))
        game.Virtual(Read(manager + mSession), sDeliver);
}
REGISTER_NATIVE_FUNCTION_AS(0x803D6334, MH3_NetManagerDeliver, "NetManagerDeliver");

extern "C" void MH3_NetManagerSendHostId(CpuContext* cpu) {
    ForwardToSession(cpu, 0x30);
}
REGISTER_NATIVE_FUNCTION_AS(0x803D6394, MH3_NetManagerSendHostId, "NetManagerSendHostId");

extern "C" void MH3_NetManagerSendHostGone(CpuContext* cpu) {
    ForwardToSession(cpu, 0x34);
}
REGISTER_NATIVE_FUNCTION_AS(0x803D63B4, MH3_NetManagerSendHostGone, "NetManagerSendHostGone");

extern "C" void MH3_NetManagerSetLoss(CpuContext* cpu) {
    ForwardToSession(cpu, 0x50);
}
REGISTER_NATIVE_FUNCTION_AS(0x803D63D4, MH3_NetManagerSetLoss, "NetManagerSetLoss");

extern "C" void MH3_NetManagerSetHostTimeout(CpuContext* cpu) {
    ForwardToSession(cpu, 0x54);
}
REGISTER_NATIVE_FUNCTION_AS(0x803D63F4, MH3_NetManagerSetHostTimeout, "NetManagerSetHostTimeout");

extern "C" void MH3_NetManagerSetKeepalive(CpuContext* cpu) {
    ForwardToSession(cpu, 0x60);
}
REGISTER_NATIVE_FUNCTION_AS(0x803D6414, MH3_NetManagerSetKeepalive, "NetManagerSetKeepalive");

extern "C" void MH3_NetManagerSetTimeout(CpuContext* cpu) {
    ForwardToSession(cpu, 0x64);
}
REGISTER_NATIVE_FUNCTION_AS(0x803D6434, MH3_NetManagerSetTimeout, "NetManagerSetTimeout");

extern "C" void MH3_NetManagerSetNoReceive(CpuContext* cpu) {
    ForwardToSession(cpu, 0x68);
}
REGISTER_NATIVE_FUNCTION_AS(0x803D6454, MH3_NetManagerSetNoReceive, "NetManagerSetNoReceive");

extern "C" void MH3_NetManagerSetRate(CpuContext* cpu) {
    ForwardToSession(cpu, 0x5C);
}
REGISTER_NATIVE_FUNCTION_AS(0x803D6474, MH3_NetManagerSetRate, "NetManagerSetRate");

extern "C" void MH3_NetManagerFreeBlock(CpuContext* cpu) {
    cpu->gpr[3] = FreeBlock(Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x803D6494, MH3_NetManagerFreeBlock, "NetManagerFreeBlock");

extern "C" void MH3_NetManagerRelease(CpuContext* cpu) {
    ReleaseRequest(cpu, Arg(cpu, 4));
}
REGISTER_NATIVE_FUNCTION_AS(0x803D6508, MH3_NetManagerRelease, "NetManagerRelease");

extern "C" void MH3_NetRequestTakeResult(CpuContext* cpu) {
    cpu->gpr[3] = TakeResult(cpu, Arg(cpu, 3), Arg(cpu, 4));
}
REGISTER_NATIVE_FUNCTION_AS(0x803D6578, MH3_NetRequestTakeResult, "NetRequestTakeResult");

extern "C" void MH3_NetRequestSetResult(CpuContext* cpu) {
    SetResult(cpu, Arg(cpu, 3), Arg(cpu, 4), Arg(cpu, 5), Arg(cpu, 6));
}
REGISTER_NATIVE_FUNCTION_AS(0x803D65F0, MH3_NetRequestSetResult, "NetRequestSetResult");

extern "C" void MH3_NetRequestArgument(CpuContext* cpu) {
    cpu->gpr[3] = Argument(cpu, Arg(cpu, 3), Arg(cpu, 4));
}
REGISTER_NATIVE_FUNCTION_AS(0x803D6658, MH3_NetRequestArgument, "NetRequestArgument");

extern "C" void MH3_NetRequestTimedOut(CpuContext* cpu) {
    cpu->gpr[3] = TimedOut(cpu, Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x803D66D0, MH3_NetRequestTimedOut, "NetRequestTimedOut");

extern "C" void MH3_NetRequestSetTimeout(CpuContext* cpu) {
    SetTimeout(cpu, Arg(cpu, 3), cpu->fpr[1].d);
}
REGISTER_NATIVE_FUNCTION_AS(0x803D6754, MH3_NetRequestSetTimeout, "NetRequestSetTimeout");

// 0x803D67B4: a member entry copied (0x48 bytes).
extern "C" void MH3_NetMemberCopy(CpuContext* cpu) {
    const uint32_t to = Arg(cpu, 3), from = Arg(cpu, 4);

    for (uint32_t i = 0; i < 0x48; i += 4)
        Write(to + i, Read(from + i));
}
REGISTER_NATIVE_FUNCTION_AS(0x803D67B4, MH3_NetMemberCopy, "NetMemberCopy");

extern "C" void MH3_NetIdHolderDestroy(CpuContext* cpu) {
    cpu->gpr[3] = WithIdDestroy(cpu, Arg(cpu, 3), int16_t(Arg(cpu, 4)), 0);
}
REGISTER_NATIVE_FUNCTION_AS(0x803D6848, MH3_NetIdHolderDestroy, "NetIdHolderDestroy");

extern "C" void MH3_NetIdHolderConstruct(CpuContext* cpu) {
    cpu->gpr[3] = WithIdConstruct(cpu, Arg(cpu, 3), 0);
}
REGISTER_NATIVE_FUNCTION_AS(0x803D68A4, MH3_NetIdHolderConstruct, "NetIdHolderConstruct");

// 0x803D68D4 / 0x803D6904: the connection's and the session's callbacks,
// with the manager (their parameter) first.
extern "C" void MH3_NetManagerConnectionEvent(CpuContext* cpu) {
    const uint32_t a = Arg(cpu, 3), b = Arg(cpu, 4), c = Arg(cpu, 5), d = Arg(cpu, 6), e = Arg(cpu, 7), f = Arg(cpu, 8);
    cpu->gpr[3] = GuestCall{cpu}.Call<ConnectionEvent>({f, a, uint32_t(int32_t(int8_t(b))), c, d, e});
}
REGISTER_NATIVE_FUNCTION_AS(0x803D68D4, MH3_NetManagerConnectionEvent, "NetManagerConnectionEvent");

extern "C" void MH3_NetManagerSessionEvent(CpuContext* cpu) {
    const uint32_t a = Arg(cpu, 3), b = Arg(cpu, 4), c = Arg(cpu, 5), d = Arg(cpu, 6), e = Arg(cpu, 7), f = Arg(cpu, 8);
    cpu->gpr[3] = GuestCall{cpu}.Call<SessionEvent>({f, a, b, c, d, e});
}
REGISTER_NATIVE_FUNCTION_AS(0x803D6904, MH3_NetManagerSessionEvent, "NetManagerSessionEvent");

extern "C" void MH3_NetGameManagerConstruct(CpuContext* cpu) {
    cpu->gpr[3] = DerivedConstruct(cpu, Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x803D6934, MH3_NetGameManagerConstruct, "NetGameManagerConstruct");

extern "C" void MH3_NetPlayerTableDestroy(CpuContext* cpu) {
    cpu->gpr[3] = PlayerTableDestroy(cpu, Arg(cpu, 3), int16_t(Arg(cpu, 4)));
}
REGISTER_NATIVE_FUNCTION_AS(0x803D6A30, MH3_NetPlayerTableDestroy, "NetPlayerTableDestroy");

extern "C" void MH3_NetPlayerDestroy(CpuContext* cpu) {
    cpu->gpr[3] = WithIdDestroy(cpu, Arg(cpu, 3), int16_t(Arg(cpu, 4)), 0x108);
}
REGISTER_NATIVE_FUNCTION_AS(0x803D6A9C, MH3_NetPlayerDestroy, "NetPlayerDestroy");

// 0x803D6AFC: the matchmaking object (0x44A0 bytes), once created.
extern "C" void MH3_NetMatchmaking(CpuContext* cpu) {
    cpu->gpr[3] = Read(kBigSingleton);
}
REGISTER_NATIVE_FUNCTION_AS(0x803D6AFC, MH3_NetMatchmaking, "NetMatchmaking");

extern "C" void MH3_NetPlayerTableConstruct(CpuContext* cpu) {
    cpu->gpr[3] = PlayerTableConstruct(cpu, Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x803D6B04, MH3_NetPlayerTableConstruct, "NetPlayerTableConstruct");

extern "C" void MH3_NetPlayerConstruct(CpuContext* cpu) {
    cpu->gpr[3] = WithIdConstruct(cpu, Arg(cpu, 3), 0x108);
}
REGISTER_NATIVE_FUNCTION_AS(0x803D6B50, MH3_NetPlayerConstruct, "NetPlayerConstruct");

extern "C" void MH3_NetMemberDestroy(CpuContext* cpu) {
    cpu->gpr[3] = WithIdDestroy(cpu, Arg(cpu, 3), int16_t(Arg(cpu, 4)), 8);
}
REGISTER_NATIVE_FUNCTION_AS(0x803D6B84, MH3_NetMemberDestroy, "NetMemberDestroy");

extern "C" void MH3_NetMemberConstruct(CpuContext* cpu) {
    cpu->gpr[3] = WithIdConstruct(cpu, Arg(cpu, 3), 8);
}
REGISTER_NATIVE_FUNCTION_AS(0x803D6BE4, MH3_NetMemberConstruct, "NetMemberConstruct");

extern "C" void MH3_NetGameRequestDestroy(CpuContext* cpu) {
    cpu->gpr[3] = DerivedRequestDestroy(cpu, Arg(cpu, 3), int16_t(Arg(cpu, 4)));
}
REGISTER_NATIVE_FUNCTION_AS(0x803D6C18, MH3_NetGameRequestDestroy, "NetGameRequestDestroy");

extern "C" void MH3_NetGameRequestReset2(CpuContext* cpu) {
    DerivedRequestReset(Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x803D6C7C, MH3_NetGameRequestReset2, "NetGameRequestReset2");

extern "C" void MH3_NetGameRequestReset(CpuContext* cpu) {
    DerivedRequestReset(Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x803D6C80, MH3_NetGameRequestReset, "NetGameRequestReset");

extern "C" void MH3_NetGameRequestConstruct(CpuContext* cpu) {
    cpu->gpr[3] = DerivedRequestConstruct(cpu, Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x803D6D28, MH3_NetGameRequestConstruct, "NetGameRequestConstruct");

extern "C" void MH3_NetGameManagerDestroy(CpuContext* cpu) {
    cpu->gpr[3] = DerivedDestroy(cpu, Arg(cpu, 3), int16_t(Arg(cpu, 4)));
}
REGISTER_NATIVE_FUNCTION_AS(0x803D6D64, MH3_NetGameManagerDestroy, "NetGameManagerDestroy");
