// The online session's close, RMHP08 0x803D6FFC (a network manager method,
// reached through its vtable; ported from the translated C++), and the poll
// its wait uses, 0x8041C7F4.
//
// The close cancels the server connection (0x807971A4) and waits for the
// connection's worker thread to leave it before deleting it. The original
// spins on 0x8041C7F4 for that: on the Wii the worker's I/O interrupt
// preempts the spin, but guest threads here switch only at OS calls, so the
// worker never ran again and the game hung when a connection error was
// dismissed (the devkit, 2026-10-08). The port waits in
// GuestThreads::BlockUntil, which runs the other guest threads and the
// devices meanwhile.
//
// MH3_PORT_CHECK=ConnectionBusy checks the poll; the close has no check (the
// original it would run first is the spin). MH3_TRACE=net prints each close's
// wait.
#include "abi_bridge.h"
#include "guest_call.h"
#include "guest_threads.h"
#include "memory.h"
#include "port_check.h"

#ifdef MH3_DEVELOPMENT
#include "dev_options.h"

#include <chrono>
#include <cstdio>
#endif

namespace {
// Globals (r13 small data).
enum : uint32_t {
    kSession = 0x80797170,    // r13 - 0x4170: deleted at close unless +0x60D4
    kNetSystem = 0x80797180,  // r13 - 0x4160: what +0x660 is released to
    kConnection = 0x807971A4, // r13 - 0x413C: the server connection
};

// The network manager.
enum : uint32_t {
    kHeld = 0x660,       // u32: released to kNetSystem at close
    kConnected = 0x6E75, // u8: the connection is up, cancel it at close
};

enum : uint32_t {
    kSessionKept = 0x60D4, // u32, kSession's: keep it at close
};

// The connection.
enum : uint32_t {
    kRunning = 0x120,     // u8: its worker thread is using it
    kRunningSeen = 0x121, // u8
    kCloseAsked = 0x122,  // u8: set by 0x8041C870
    kCancelAsked = 0x123, // u8: set by 0x8041C948
    kAskSeen = 0x126,     // u8
};

enum : uint32_t {
    Begin = 0x803DE9AC,
    SessionClose = 0x803FD35C,
    Release = 0x80418BCC,
    CancelConnection = 0x8041C948,
    CloseConnection = 0x8041C870,
    End = 0x803D4D84,
    CloseMessage = 0x805FB2C8, // kNetSystem's vtable +16 argument
};

// Delete through the vtable's deleting destructor (+8, 1).
constexpr uint32_t kDelete = 8;
constexpr uint32_t kLog = 16;

// CR bit 6 (cr1 eq): a variadic call's FPR arguments are in registers (the
// original clears it: there are none).
constexpr uint32_t kCrFloatArgs = 1u << (31 - 6);

// 0x8041C7F4: whether the connection's worker thread still has it. A cancel
// or close request counts until the worker takes it.
uint32_t ConnectionBusy(uint32_t connection) {
    if (Memory::Read8(connection + kCancelAsked) || Memory::Read8(connection + kCloseAsked)) {
        Memory::Write8(connection + kAskSeen, 1);
        return 1;
    }

    const uint8_t running = Memory::Read8(connection + kRunning);

    if (running)
        Memory::Write8(connection + kRunningSeen, 1);

    return running;
}

void ConnectionBusyPort(CpuContext* cpu) {
    cpu->gpr[3] = ConnectionBusy(cpu->gpr[3]);
}

void OnlineClose(CpuContext* cpu) {
    const uint32_t manager = cpu->gpr[3];
    uint32_t result[2];

    {
        GuestFrame frame(cpu, 0x10);
        GuestCall call{cpu};
        call(Begin, {manager});

        // The global is read again after each call, as the original does.
        if (Memory::Read32(kSession)) {
            call(SessionClose, {Memory::Read32(kSession)});

            if (!Memory::Read32(Memory::Read32(kSession) + kSessionKept))
                if (const uint32_t session = Memory::Read32(kSession))
                    call.Virtual(session, kDelete, {1});
        }

        if (const uint32_t held = Memory::Read32(manager + kHeld)) {
            call(Release, {Memory::Read32(kNetSystem), held});
            Memory::Write32(manager + kHeld, 0);
        }

        if (Memory::Read32(kConnection)) {
            if (Memory::Read8(manager + kConnected)) {
                cpu->cr &= ~kCrFloatArgs;
                call.Virtual(Memory::Read32(kNetSystem), kLog, {CloseMessage});
                Memory::Write8(manager + kConnected, 0);
                call(CancelConnection, {Memory::Read32(kConnection)});
                call(CloseConnection, {Memory::Read32(kConnection)});
            }

#ifdef MH3_DEVELOPMENT
            const auto start = std::chrono::steady_clock::now();
#endif

            GuestThreads::BlockUntil(cpu, [] {
                return ConnectionBusy(Memory::Read32(kConnection)) == 0;
            });

#ifdef MH3_DEVELOPMENT
            static const TraceFlag trace("net");

            if (trace)
                std::fprintf(stderr, "MH3 net: online close waited %.1f ms for the connection's worker\n", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
#endif

            if (const uint32_t connection = Memory::Read32(kConnection))
                call.Virtual(connection, kDelete, {1});
        }

        result[0] = call(End, {manager});
        result[1] = cpu->gpr[4];
    }

    cpu->gpr[3] = result[0];
    cpu->gpr[4] = result[1];
}

} // namespace

extern "C" void func_8041C7F4(CpuContext* cpu);
extern "C" void MH3_ConnectionBusy(CpuContext* cpu) {
    PortCheck::Call(cpu, "ConnectionBusy", func_8041C7F4, ConnectionBusyPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8041C7F4, MH3_ConnectionBusy, "ConnectionBusy");

extern "C" void MH3_OnlineClose(CpuContext* cpu) {
    OnlineClose(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x803D6FFC, MH3_OnlineClose, "OnlineClose");
