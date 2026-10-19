// The field HUD's draw, RMHP08: 0x802E55C0, a 2D layer entry (frame_draw.cpp)
// that takes about 2 ms/frame on the devkit in the Alatreon fight, decompiled
// with Ghidra. Outside an area (get_now_areano 0xFF) it draws nothing.
// Otherwise, with the HUD work at 0x806C0148 (its first word the field HUD
// state): no depth test, then the HUD's parts in a fixed order; three of them
// only while 0x802E550C(state) is 1, one only while the player's +0xEF is
// set, and, unless 0x802A0368(state) says otherwise, one of four alternatives
// chosen by the state's +0x5BC, +0x5BE and +0x5BD. Calls the original makes
// without arguments get none here either (r3 keeps the previous result).
// MH3_PROFILE=split times each part (the MH3 hud split line);
// MH3_PORT_CHECK=FieldHudDraw checks the port.
#include "abi_bridge.h"
#include "frame_split.h"
#include "native_graphics.h"
#include "guest_call.h"
#include "memory.h"
#include "port_check.h"
#ifdef MH3_DEVELOPMENT
#include <cstdio>
#include <string>
#endif

namespace {

constexpr uint32_t kHud = 0x806C0148; // HUD work; its first word is the state
enum : uint32_t {
    kZeroed = 0xCD,      // u8, cleared before the parts draw
    kMeterArgument = 0x16F,
    kItemArgument = 0x16B,
};

enum : uint32_t {
    kPlayerShown = 0xEF, // player work, u8
    kVariantA = 0x5BC,   // state, u8: the four alternatives
    kVariantB = 0x5BE,
    kVariantC = 0x5BD,
};

enum : uint32_t {
    GetMoveWorkAddress = 0x800CFAF4, // (index): the player work
    GetNowAreaNumber = 0x802AFCE8,
    SetZMode = 0x800E46EC, // (compare, function, update)
    PartsShown = 0x802E550C, // (state): 1 when the gauges show
    Overlaid = 0x802A0368,   // (state): nonzero skips the alternatives
    MenuOpen = 0x800CF2E4,
};

// The timed parts, in the order they draw.
enum Part : unsigned {
    Prepare,  // 0x802E5718(hud)
    Gauges,   // 0x802DFF20
    Clock,    // 0x802EC764
    PartA,    // 0x802E7228
    PartB,    // 0x802E8004
    PartC,    // 0x802ED4E4
    Map,      // 0x802EE890(hud)
    Player,   // 0x802EF160(hud)
    AltA,     // 0x802E5EF4
    AltB,     // 0x802E64D0
    AltC,     // 0x802E6B10
    AltD,     // 0x802E8EC8, 0x802EE6C0, 0x802EDD48
    Items,    // 0x802EA3A0(hud)
    Late,     // 0x802E6F18
    Last,     // 0x80383B48
    Menu,     // 0x802EF488
    Finish,   // 0x802EF794
    Parts
};

constexpr const char* kPartNames[Parts] = {"802E5718", "802DFF20", "802EC764", "802E7228", "802E8004", "802ED4E4", "802EE890", "802EF160", "802E5EF4",
                                           "802E64D0", "802E6B10", "802E8EC8+", "802EA3A0", "802E6F18", "80383B48", "802EF488", "802EF794"};
#ifdef MH3_DEVELOPMENT
double partMs[Parts];
#endif

// Where each part sits on a target wider than 16:9 (MH3_ASPECT=window): 0
// the middle, 1 the left edge (the clock and the gauges, drawn from x -0.92 to
// 0.38 of the 16:9 layer), 2 the right edge (the map, the item box and its
// alternatives, from 0.45 to 0.96, and the quest menu).
constexpr uint32_t kAnchors[Parts] = {0, 1, 1, 1, 1, 1, 2, 0, 2, 2, 2, 2, 2, 0, 0, 2, 0};

uint32_t Timed(GuestCall& call, Part part, uint32_t function, std::initializer_list<uint32_t> arguments = {}) {
#ifdef MH3_DEVELOPMENT
    const bool timed = mh3SplitDetail;
    const auto start = timed ? MH3SplitClock::now() : MH3SplitClock::time_point{};
#endif
    BeginNativeHudPart(kAnchors[part]);
    const uint32_t result = arguments.size() ? call(function, arguments) : call(function);
    EndNativeHudPart();
#ifdef MH3_DEVELOPMENT
    if (timed)
        partMs[part] += std::chrono::duration<double, std::milli>(MH3SplitClock::now() - start).count();
#endif
    return result;
}

void FieldHudDraw(CpuContext* cpu) {
    GuestFrame frame(cpu, 0x20);
    GuestCall call{cpu};
    const uint32_t player = call(GetMoveWorkAddress, {0});
    const uint32_t state = Memory::Read32(kHud);

    if (uint8_t(call(GetNowAreaNumber)) == 0xFF)
        return;

    call(SetZMode, {0, 0, 0});
    Memory::Write8(kHud + kZeroed, 0);
    Timed(call, Prepare, 0x802E5718, {kHud});
    Timed(call, Gauges, 0x802DFF20);
    Timed(call, Clock, 0x802EC764);

    if (call(PartsShown, {state}) == 1) {
        Timed(call, PartA, 0x802E7228);
        Timed(call, PartB, 0x802E8004);
        Timed(call, PartC, 0x802ED4E4);
    }

    Timed(call, Map, 0x802EE890, {kHud});

    if (Memory::Read8(player + kPlayerShown) != 0)
        Timed(call, Player, 0x802EF160, {kHud});

    if (call(Overlaid, {state}) == 0) {
        if (Memory::Read8(state + kVariantA) != 0) {
            Timed(call, AltA, 0x802E5EF4);
        } else if (Memory::Read8(state + kVariantB) != 0) {
            Timed(call, AltB, 0x802E64D0);
        } else if (Memory::Read8(state + kVariantC) != 0) {
            Timed(call, AltC, 0x802E6B10);
        } else {
            Timed(call, AltD, 0x802E8EC8);

            if (call(PartsShown, {state}) == 1) {
                Timed(call, AltD, 0x802EE6C0, {state, kHud + kMeterArgument});
                Timed(call, AltD, 0x802EDD48, {state, kHud + kItemArgument});
            }
        }
    }

    Timed(call, Items, 0x802EA3A0, {kHud});
    Timed(call, Late, 0x802E6F18);
    Timed(call, Last, 0x80383B48);

    if (call(MenuOpen) != 0)
        Timed(call, Menu, 0x802EF488);

    Timed(call, Finish, 0x802EF794);
}

} // namespace

extern "C" void func_802E55C0(CpuContext* cpu);
extern "C" void MH3_FieldHudDraw(CpuContext* cpu) {
    PortCheck::Call(cpu, "FieldHudDraw", func_802E55C0, FieldHudDraw);
}
REGISTER_NATIVE_FUNCTION_AS(0x802E55C0, MH3_FieldHudDraw, "FieldHudDraw");

#ifdef MH3_DEVELOPMENT
void MH3PrintHudSplit(unsigned frames) {
    std::string line = "MH3 hud split (ms/frame):";
    char part[48];

    for (unsigned i = 0; i < Parts; ++i) {
        std::snprintf(part, sizeof part, " %s %.2f", kPartNames[i], frames ? partMs[i] / frames : 0.0);
        line += part;
        partMs[i] = 0;
    }

    std::printf("%s\n", line.c_str());
}
#endif
