// net_ctrl's quest circle commands (RMHP08; ported from the translated C++):
// join a posted quest 0x8042AD0C, ready 0x8042AE7C and its cancel 0x8042AF5C,
// depart 0x8042B2FC. The quest board and the circle's menu call them, the
// online autopilot and the debug port's `online` command too
// (online_auto.cpp).
//
// Each starts one circle manager task (0x80CE08C0, through the managers'
// holder) and marks net_ctrl's command slot busy with the command and the
// function that ends it (the join's 0x8042AC48 enters the circle, the
// ready's 0x8042AE30 its waiting state). One command runs at a time: a call
// while one runs, or offline, does nothing and returns 0.
//
// The ready sends ReqCircleMatchOptionSet with its standby flag. A member's
// ready stands it by; the leader's ready departs: its result (0x80428F74)
// makes the screens close the circle (command 0xF, 0x8042B050) and that
// one's result departs (0x8042B2FC, ReqCircleMatchStart), which kicks the
// members that do not stand by. The leader's ready clears those results
// (0xF, the depart's) and 0x1C's.
//
// MH3_PORT_CHECK=NetCtrlJoin,NetCtrlReady,NetCtrlUnready,NetCtrlDepart.
#include "net_ctrl.h"

#include "abi_bridge.h"
#include "guest_call.h"
#include "port_check.h"

namespace {

using namespace NetCtrl;

enum : uint32_t {
    JoinDone = 0x8042AC48,
    ReadyDone = 0x8042AE30,
    DepartDone = 0x8042B2DC,
};

constexpr uint32_t kDoneArgCount = 5;

// 0x8042A1A4.
void ClearResult(uint32_t work, uint32_t command) {
    Memory::Write32(work + kResults + 4 * command, 0);
}

// Takes the command slot for `command`, ended by `done`; false while it is
// taken.
bool Begin(uint32_t work, uint32_t command, uint32_t done) {
    if (Memory::Read32(work + kBusy))
        return false;

    Memory::Write32(work + kBusy, 1);
    Memory::Write32(work + kCommand, command);
    Memory::Write32(work + kDone, done);

    for (uint32_t i = 0; i < kDoneArgCount; ++i)
        Memory::Write32(work + kDoneArgs + 4 * i, 0);

    return true;
}

uint32_t JoinCircle(CpuContext* cpu, uint32_t index) {
    const uint32_t work = Work();

    if (!CircleManager() || !Begin(work, kCommandJoin, JoinDone))
        return 0;

    for (uint32_t slot = 0; slot < 4; ++slot)
        Memory::Write8(work + kMembers + slot, 0);

    Memory::Write32(work + kCircle, index);
    ClearResult(work, kCommandJoin);
    GuestCall{cpu}.Virtual(CircleManager(), kCircleIn, {index});
    return 1;
}

uint32_t SetReady(CpuContext* cpu, bool ready) {
    const uint32_t work = Work();

    // Only a member cancels: the leader's ready stands until it departs.
    if (!CircleManager() || (!ready && Memory::Read32(work + kLeader)) || !Begin(work, kCommandReady, ReadyDone))
        return 0;

    ClearResult(work, kCommandReady);

    if (ready && Memory::Read32(work + kLeader)) {
        ClearResult(work, kCommandDepart);
        ClearResult(work, 0xF);
        ClearResult(work, 0x1C);
    }

    GuestCall{cpu}.Virtual(CircleManager(), kMatchOptionSet, {ready ? 1u : 0u});
    return 1;
}

uint32_t DepartQuest(CpuContext* cpu) {
    const uint32_t work = Work();

    if (!CircleManager() || Memory::Read32(work + kLeader) != 1 || !Begin(work, kCommandDepart, DepartDone))
        return 0;

    ClearResult(work, kCommandDepart);
    GuestCall{cpu}.Virtual(CircleManager(), kMatchStart);
    return 1;
}

void JoinPort(CpuContext* cpu) {
    uint32_t started;
    {
        GuestFrame frame(cpu, 0x10);
        started = JoinCircle(cpu, frame.Arg(3));
    }

    cpu->gpr[3] = started;
}

void ReadyPort(CpuContext* cpu) {
    uint32_t started;
    {
        GuestFrame frame(cpu, 0x10);
        started = SetReady(cpu, true);
    }

    cpu->gpr[3] = started;
}

void UnreadyPort(CpuContext* cpu) {
    uint32_t started;
    {
        GuestFrame frame(cpu, 0x10);
        started = SetReady(cpu, false);
    }

    cpu->gpr[3] = started;
}

void DepartPort(CpuContext* cpu) {
    uint32_t started;
    {
        GuestFrame frame(cpu, 0x10);
        started = DepartQuest(cpu);
    }

    cpu->gpr[3] = started;
}

} // namespace

extern "C" void func_8042AD0C(CpuContext* cpu);
extern "C" void MH3_NetCtrlJoin(CpuContext* cpu) {
    PortCheck::Call(cpu, "NetCtrlJoin", func_8042AD0C, JoinPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8042AD0C, MH3_NetCtrlJoin, "NetCtrlJoin");

extern "C" void func_8042AE7C(CpuContext* cpu);
extern "C" void MH3_NetCtrlReady(CpuContext* cpu) {
    PortCheck::Call(cpu, "NetCtrlReady", func_8042AE7C, ReadyPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8042AE7C, MH3_NetCtrlReady, "NetCtrlReady");

extern "C" void func_8042AF5C(CpuContext* cpu);
extern "C" void MH3_NetCtrlUnready(CpuContext* cpu) {
    PortCheck::Call(cpu, "NetCtrlUnready", func_8042AF5C, UnreadyPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8042AF5C, MH3_NetCtrlUnready, "NetCtrlUnready");

extern "C" void func_8042B2FC(CpuContext* cpu);
extern "C" void MH3_NetCtrlDepart(CpuContext* cpu) {
    PortCheck::Call(cpu, "NetCtrlDepart", func_8042B2FC, DepartPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x8042B2FC, MH3_NetCtrlDepart, "NetCtrlDepart");
