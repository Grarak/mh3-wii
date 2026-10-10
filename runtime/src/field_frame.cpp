// The field's frame update, RMHP08: 0x8028CCE0, called by the field task's
// main state (0x8028CE14), decompiled with Ghidra. Unless the field work's
// +0x112 is set, it moves the field's parts in a fixed order (player control,
// camera shake, camera, lights and the rest), then runs 0x8028CDA0 and
// 0x8029A3BC. 0x80046F70 returning 1 skips the moves, and the second call;
// with +0x113 set, 0x803B890C returning 1 ends the update after the moves.
// MH3_PROFILE=split times each call (the MH3 field split line);
// MH3_PORT_CHECK=FieldFrameUpdate checks the port against the original.
#include "abi_bridge.h"
#include "frame_split.h"
#include "guest_call.h"
#include "memory.h"
#include "port_check.h"
#ifdef MH3_DEVELOPMENT
#include <cstdio>
#include <string>
#endif

namespace {

enum : uint32_t {
    kSkip = 0x112,    // u8: no update
    kMayStop = 0x113, // u8: 0x803B890C may end the update after the moves
};

enum : uint32_t {
    Paused = 0x80046F70, // 1: the moves are skipped
    StopAfterMoves = 0x803B890C,
    AfterMoves = 0x8028CDA0,
    Last = 0x8029A3BC,
};

// A call without an argument keeps the caller's r3, as the original does.
constexpr int32_t kNoArgument = -1;
struct Move {
    uint32_t function;
    int32_t argument;
    const char* name;
};

constexpr Move kMoves[] = {
    {0x803C3FD0, kNoArgument, "EventDemoDriver"},
    {0x80299F3C, kNoArgument, "80299F3C"},
    {0x80267D64, kNoArgument, "player_control_move"},
    {0x80143C78, kNoArgument, "80143C78"},
    {0x802C2964, kNoArgument, "802C2964"},
    {0x803857C0, kNoArgument, "803857C0"},
    {0x802AAE40, 1, "802AAE40(1)"},
    {0x80297E98, kNoArgument, "80297E98"},
    {0x80268128, kNoArgument, "80268128"},
    {0x80143DDC, kNoArgument, "80143DDC"},
    {0x802D853C, kNoArgument, "802D853C"},
    {0x802AC2D8, kNoArgument, "yure_move"},
    {0x802ADBFC, kNoArgument, "802ADBFC"},
    {0x802B5938, kNoArgument, "CameraMove"},
    {0x802C1D94, kNoArgument, "light_move"},
    {0x802AAE40, 0, "802AAE40(0)"},
    {0x800F8E08, kNoArgument, "800F8E08"},
    {0x803BA604, kNoArgument, "803BA604"},
    {0x8035ABB4, kNoArgument, "8035ABB4"},
};

constexpr unsigned kMoveCount = sizeof(kMoves) / sizeof(kMoves[0]);
#ifdef MH3_DEVELOPMENT
// Per call: the moves, then 0x8028CDA0 and 0x8029A3BC.
double splitMs[kMoveCount + 2];
#endif

uint32_t Timed(GuestCall& call, unsigned index, uint32_t function, int32_t argument) {
#ifdef MH3_DEVELOPMENT
    const bool timed = mh3SplitDetail;
    const auto start = timed ? MH3SplitClock::now() : MH3SplitClock::time_point{};
#endif
    const uint32_t result = argument == kNoArgument ? call(function) : call(function, {uint32_t(argument)});
#ifdef MH3_DEVELOPMENT
    if (timed)
        splitMs[index] += std::chrono::duration<double, std::milli>(MH3SplitClock::now() - start).count();
#endif
    return result;
}

void FieldFrameUpdate(CpuContext* cpu) {
    const uint32_t work = cpu->gpr[3];

    if (Memory::Read8(work + kSkip) != 0)
        return;

    GuestFrame frame(cpu, 0x10);
    GuestCall call{cpu};

    if (call(Paused) != 1) {
        for (unsigned i = 0; i < kMoveCount; ++i)
            Timed(call, i, kMoves[i].function, kMoves[i].argument);

        if (Memory::Read8(work + kMayStop) == 1 && int32_t(call(StopAfterMoves)) == 1)
            return;
    }

    Timed(call, kMoveCount, AfterMoves, kNoArgument);

    if (call(Paused) != 1)
        Timed(call, kMoveCount + 1, Last, kNoArgument);
}

} // namespace

extern "C" void func_8028CCE0(CpuContext* cpu);
extern "C" void MH3_FieldFrameUpdate(CpuContext* cpu) {
    PortCheck::Call(cpu, "FieldFrameUpdate", func_8028CCE0, FieldFrameUpdate);
}
REGISTER_NATIVE_FUNCTION_AS(0x8028CCE0, MH3_FieldFrameUpdate, "FieldFrameUpdate");

#ifdef MH3_DEVELOPMENT
void MH3PrintFieldSplit(unsigned frames) {
    std::string line = "MH3 field split (ms/frame):";
    char part[64];

    for (unsigned i = 0; i < kMoveCount + 2; ++i) {
        std::snprintf(part, sizeof part, " %s %.2f", i < kMoveCount ? kMoves[i].name : i == kMoveCount ? "8028CDA0" : "8029A3BC", frames ? splitMs[i] / frames : 0.0);
        line += part;
        splitMs[i] = 0;
    }

    std::printf("%s\n", line.c_str());
}
#endif
