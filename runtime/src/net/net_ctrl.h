// The net_ctrl layer (RMHP08): the online screens' state machine and the
// commands it gives the network managers. The addresses and work fields that
// the ports (net_ctrl.cpp) and the online autopilot (online_auto.cpp) share.
#pragma once

#include "memory.h"

#include <cstdint>

namespace NetCtrl {

enum : uint32_t {
    kWorkPointer = 0x807972A8, // net_ctrl_wk (r13 - 0x4038)
    kManagers = 0x80797190,    // r13 - 0x4150: +0 the circle manager, +0xC the layer manager
};

// net_ctrl_wk.
enum : uint32_t {
    kState = 0x17,        // u8: the screens' state (0x19..0x1E: in a gate or city)
    kSub = 0x18,          // u8: its sub-state
    kNotice = 0x19,       // u8: a message over the screen's list
    kPage = 0x1C,         // u32: a list's page or column
    kCursor = 0x20,       // u32: its row
    kScroll = 0x24,       // u32: the Capcom ID list's scroll
    kMembers = 0x40,      // u8[4]: the circle's member slots in use
    kLeader = 0x44,       // u32: this player posted the circle
    kSlot = 0x6A,         // s8: this player's member slot
    kResults = 0x78,      // u32[]: each command's result, cleared at its start
    kSessionError = 0x140, // u32[3]: the session's last error (code, phase, detail; commands 0xC, 0x1C, 0x23)
    kCircles = 0x168,     // u32: the circle list (0x2B4 bytes a circle)
    kCircle = 0x170,      // s32: the joined circle's index in the list
    kIdCount = 0x178,     // u32: the Capcom IDs on the ID screen
    kServerCount = 0x660, // u32
    kServers = 0x668,     // 0x40 bytes a server
    kCategoryServers = 0xA100, // u32[4]: the servers in each category
    kGateName = 0xC0C4,   // char[]: the entered gate's name
    kCityName = 0xC104,   // char[]: the entered city's name
    kDelay = 0xC48C,      // s32: frames before a list takes input
    kBusy = 0x829C,       // u32: a command runs
    kCommand = 0x82A0,    // u32: which
    kDone = 0x82A4,       // u32: the function that ends it
    kDoneArgs = 0x82A8,   // u32[5]
};

// A circle in the list: its members (0x40 bytes each).
enum : uint32_t {
    kCircleBytes = 0x2B4,
    kCircleMembers = 0x90, // u32 in use, then u8
    kMemberBytes = 0x40,
};

// A server's category (1..4; 0 none).
constexpr uint32_t kServerCategory = 0x3C;

// The commands (net_ctrl_wk +kCommand).
enum : uint32_t {
    kCommandPost = 4,
    kCommandJoin = 6,
    kCommandDepart = 0x18,
    kCommandReady = 0x1A,
};

// The circle manager's methods (vtable offsets).
enum : uint32_t {
    kCircleIn = 0x54,       // (index in the list): ReqCircleIn
    kCircleCount = 0x70,    // (): the circles in the list
    kMatchOptionSet = 0xC0, // (ready): ReqCircleMatchOptionSet, its standby flag
    kMatchStart = 0xC4,     // (): ReqCircleMatchStart
};

// The quest circle commands. Each returns 1 when it started, 0 while another
// runs or offline.
enum : uint32_t {
    Post = 0x8042A43C,    // (u16 quest, s8 members at +2, char password[5] at +3, remarks at +8)
    Join = 0x8042AD0C,    // (index in the circle list)
    Ready = 0x8042AE7C,   // ()
    Unready = 0x8042AF5C, // (): not the leader
    Depart = 0x8042B2FC,  // (): the leader
};

inline uint32_t Work() {
    return Memory::Read32(kWorkPointer);
}

inline uint32_t Manager(uint32_t slot) {
    const uint32_t managers = Memory::Read32(kManagers);
    return managers ? Memory::Read32(managers + slot) : 0;
}

inline uint32_t CircleManager() {
    return Manager(0);
}

inline uint32_t LayerManager() {
    return Manager(0xC);
}

} // namespace NetCtrl
