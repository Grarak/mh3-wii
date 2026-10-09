// The online autopilot (development builds; release builds keep its ports,
// which then only trace): MH3_ONLINE_AUTO takes a --city
// launch through the online screens and into a quest without input, and the
// debug port's `online` command runs the quest circle commands by hand
// (net_ctrl.cpp).
//
//   MH3_ONLINE_AUTO=id=0,server=0,gate=0,city=0,post=10057,depart=2
//   MH3_ONLINE_AUTO=id=0,server=0,gate=0,city=0,join=0
//
// id: the Capcom ID (its place on the ID screen); server: the server's place
// in the server list (all categories, in the list's order); gate, city: their
// places in their lists (an empty city is created, one with hunters joined).
// post=QUEST (members=N, 4 by default): post the quest and, once `depart`
// members are in the circle and the others stand by, ready: the leader's
// ready departs. join=N: join the circle list's circle N, then ready. In the
// circle both accept its quest as the quest counter does (the quest the depart
// loads; `online accept` by hand).
// Movies are skipped meanwhile.
//
// The screens' state machine (0x8044E4FC; net_ctrl_wk +0x17 state, +0x18
// sub-state) reads the pad's pressed keys itself. At each choice the
// autopilot puts the screen's cursor on the item and that one call of the
// state machine sees A pressed (Psw +0x2C4), so the item's own confirm runs:
// the terms (0x98, agreed), a notice (0x9A), the Capcom ID (7), the server's
// category and the server (0xC), the gate (0x16). In the gate it opens the
// gate counter's city list (0x8042E15C, as talking to it does) and confirms
// the same way through the town menus' keys (lobby_w +0x84): the city, then
// a new city's settings (Create) or a city's hunters (Enter) and its yes.
//
// MH3_TRACE=net prints each screen state change, the online field task's
// modes, the quest session's phases and events and, while in a circle, its
// members once a second.
#include "online_auto.h"
#include "net_ctrl.h"

#include "abi_bridge.h"
#include "../cheats.h"
#include "dev_options.h"
#include "game_step.h"
#include "guest_call.h"
#include "memory.h"
#include "movie_draw.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>

namespace {

using namespace NetCtrl;

constexpr uint32_t kPressed = 0x8065BA94; // u16: Psw channel 0's pressed game keys (+0x2C4)
constexpr uint16_t kKeyA = 0x10;

enum : uint32_t {
    OperatorNew = 0x800404DC,
    TalkStart = 0x80208B60,  // (PLW): as the gate counter's talk begins
    OpenCities = 0x8042E15C, // (PLW, counter kind): the town's city list
    SendAction = 0x80376AF4, // (PLW, 0): the hunter's action to the others
    AcceptQuest = 0x80321894, // (PLW, quest, how): the quest counter's accept; how 1 pays the fee, 2 not
};

// How AcceptQuest takes the quest: the poster pays its fee (the counter after
// the post, 0x8032092C), a member does not (the join menu, 0x80394264).
enum : uint32_t {
    kAcceptPosted = 1,
    kAcceptJoined = 2,
};

constexpr uint32_t kCirclePost = 0x190; // a circle's posted block: u16 quest, s8 members

// The town menus.
enum : uint32_t {
    kLobby = 0x806ACFC4,        // u8 lobby_w: the open menu (0 none)
    kLobbyPressed = 0x806AD048, // u16: their pressed keys
    kCityMenu = 0x806AD070,     // u32: the city list's work
};

constexpr uint32_t kGateCounter = 0xB; // OpenCities' kind for the gate

// The city list's work.
enum : uint32_t {
    kMenuState = 0,      // u8: 2 the list, 3 a city's hunters, 4 enter it?, 5 a new city's settings
    kMenuSub = 1,        // u8
    kMenuMessage = 2,    // u8: a message is open
    kMenuRow = 0xA,      // s16
    kMenuPage = 0xE,     // s16
    kInfoItem = 0x1143,  // u8: the hunters screen's item (the last: Enter)
    kInfoItems = 0x1145, // u8
    kSettingsRow = 0x130C,  // s16: the settings' row (the last: Create)
    kSettingsRows = 0x130E, // s16
};

// The screens' states (net_ctrl_wk +kState).
enum : uint8_t {
    kStateIds = 7,       // the Capcom ID screen
    kStateServers = 0xC, // sub 0 the categories, 1 a category's servers
    kStateGates = 0x16,  // sub 1 the list
    kStateInGate = 0x19, // to 0x1E: in a gate or a city (0x1D in a circle)
    kStateLast = 0x1E,
    kStateTerms = 0x98,  // the terms of use: sub 0 and 3 ask
    kStateNotice = 0x9A,
};

// More of net_ctrl_wk.
enum : uint32_t {
    kChosenCategory = 0xA11C, // u32
    kGateCount = 0xA140,     // u32
};

// The message window.
enum : uint32_t {
    kAgree = 0x125, // u8: the cursor is on Agree
    kAsked = 0x126, // u8: a second question is open
    kWait = 0x128,  // s16: frames before it takes input
};

// The list window's cursors (s16).
enum : uint32_t {
    kCategoryRow = 0xA,
    kServerRow = 0x49C,
    kServerPage = 0x4A0,
    kGateRow = 0x604,
    kGatePage = 0x608,
};

constexpr uint32_t kRowsPerPage = 8;
constexpr uint32_t kIdRows = 3;
constexpr double kSettleTicks = 90; // in a city, its entry done, before the circle commands
constexpr uint32_t kGateFrames = 180; // in the gate before its city list opens

// net_ctrl_wk: the city entry's phases (in the in-city states).
enum : uint32_t {
    kWhere = 0x64,       // u32: 2 in a city
    kListAsked = 0x82C4, // u8: the circle list was asked for
    kUserData = 0x82C5,  // u8: 2 the hunter's data exchanged
    kNotices = 0x82C7,   // u8: 2
};

constexpr uint32_t kPostBlockBytes = 0x40;

struct Plan {
    bool on = false;
    uint32_t id = 0, server = 0, gate = 0;

    // city -1: stay in the gate (Loc Lac's streets) instead of a city.
    int32_t city = 0, post = -1, join = -1;
    uint32_t members = 4, depart = 0;
};

enum class Step {
    Screens,
    Settle,
    Circle,
    Depart,
    Done
};

Plan plan;
Step step = Step::Screens;
double settled = 0;
uint32_t inGate = 0, frame = 0, postBlock = 0;

[[gnu::format(printf, 1, 2)]] void Say(const char* format, ...) {
    va_list args;
    va_start(args, format);
    std::fprintf(stderr, "MH3 online auto: ");
    std::vfprintf(stderr, format, args);
    std::fputc('\n', stderr);
    va_end(args);
}

Plan Parse(const std::string& spec) {
    Plan p;
    std::istringstream in(spec);
    std::string item;

    while (std::getline(in, item, ',')) {
        const auto equals = item.find('=');
        const std::string key = item.substr(0, equals);
        const long value = equals == std::string::npos ? 0 : std::strtol(item.c_str() + equals + 1, nullptr, 0);

        if (key == "id")
            p.id = value;
        else if (key == "server")
            p.server = value;
        else if (key == "gate")
            p.gate = value;
        else if (key == "city")
            p.city = value;
        else if (key == "post")
            p.post = value;
        else if (key == "members")
            p.members = value;
        else if (key == "depart")
            p.depart = value;
        else if (key == "join")
            p.join = value;
        else if (!key.empty())
            Say("ignoring unknown item '%s'", key.c_str());
    }

    p.on = true;
    return p;
}

int16_t Wait(uint32_t window) {
    return int16_t(Memory::Read16(window + kWait));
}

// The keys the screens see pressed in this call, after the cursor is put on
// the plan's item; 0 when the screen takes no choice now.
uint16_t Choose(uint32_t work) {
    const uint8_t state = Memory::Read8(work + kState), sub = Memory::Read8(work + kSub);
    const bool waiting = int32_t(Memory::Read32(work + kDelay)) > 0;
    const uint32_t list = Memory::Read32(work + kList);

    switch (state) {
    case kStateTerms: {
        const uint32_t dialog = Memory::Read32(work + kDialog);

        if ((sub != 0 && sub != 3) || Wait(dialog) > 0)
            return 0;

        Memory::Write8(dialog + kAgree, 1);
        return kKeyA;
    }

    case kStateNotice: {
        const uint32_t dialog = Memory::Read32(work + kDialog);
        return sub == 0 && Memory::Read8(dialog + kAsked) != 1 && Wait(dialog) <= 0 ? kKeyA : 0;
    }

    case kStateIds: {
        if (sub == 0x65)
            return kKeyA;

        if (sub != 0 || waiting)
            return 0;

        if (plan.id >= Memory::Read32(work + kIdCount)) {
            Say("no Capcom ID %u (the screen lists %u)", plan.id, Memory::Read32(work + kIdCount));
            plan.on = false;
            return 0;
        }

        const uint32_t row = std::min(plan.id, kIdRows - 1);
        Memory::Write32(work + kPage, 1);
        Memory::Write32(work + kCursor, row);
        Memory::Write32(work + kScroll, plan.id - row);
        return kKeyA;
    }

    case kStateServers: {
        if (Memory::Read8(work + kNotice) || plan.server >= Memory::Read32(work + kServerCount)) {
            if (plan.server >= Memory::Read32(work + kServerCount)) {
                Say("no server %u (the list has %u)", plan.server, Memory::Read32(work + kServerCount));
                plan.on = false;
            }

            return 0;
        }

        auto category = [work](uint32_t server) { return Memory::Read32(work + kServers + server * 0x40 + kServerCategory) - 1; };
        const uint32_t wanted = category(plan.server);

        if (sub == 0 && !waiting) {
            uint32_t row = 0;

            for (uint32_t c = 0; c < wanted; ++c)
                row += Memory::Read32(work + kCategoryServers + 4 * c) != 0;

            Memory::Write32(work + kPage, row);
            Memory::Write16(list + kCategoryRow, row);
            return kKeyA;
        }

        if (sub == 1 && Memory::Read32(work + kChosenCategory) == wanted) {
            uint32_t index = 0;

            for (uint32_t server = 0; server < plan.server; ++server)
                index += category(server) == wanted;

            Memory::Write32(work + kCursor, index);
            Memory::Write16(list + kServerRow, index % kRowsPerPage);
            Memory::Write16(list + kServerPage, index / kRowsPerPage);
            return kKeyA;
        }

        return 0;
    }

    case kStateGates: {
        if (sub != 1 || waiting)
            return 0;

        if (plan.gate >= Memory::Read32(work + kGateCount)) {
            Say("no gate %u (the list has %u)", plan.gate, Memory::Read32(work + kGateCount));
            plan.on = false;
            return 0;
        }

        Memory::Write32(work + kPage, plan.gate / kRowsPerPage);
        Memory::Write32(work + kCursor, plan.gate % kRowsPerPage);
        Memory::Write16(list + kGateRow, plan.gate % kRowsPerPage);
        Memory::Write16(list + kGatePage, plan.gate / kRowsPerPage);
        return kKeyA;
    }
    }

    return 0;
}

// As Choose, for the city list (0x8042E26C, while lobby_w is its menu).
uint16_t ChooseCity(uint32_t menu) {
    const uint8_t state = Memory::Read8(menu + kMenuState), sub = Memory::Read8(menu + kMenuSub);

    if (state == 2 && sub == 2) {
        Memory::Write16(menu + kMenuRow, plan.city % kRowsPerPage);
        Memory::Write16(menu + kMenuPage, plan.city / kRowsPerPage);
        return kKeyA;
    }

    if (state == 3 && sub == 1 && !Memory::Read8(menu + kMenuMessage)) {
        Memory::Write8(menu + kInfoItem, Memory::Read8(menu + kInfoItems) - 1);
        return kKeyA;
    }

    if (state == 4 && sub == 0)
        return kKeyA;

    if (state == 5 && sub == 1) {
        Memory::Write16(menu + kSettingsRow, Memory::Read16(menu + kSettingsRows) - 1);
        return kKeyA;
    }

    return 0;
}

uint32_t CircleAddress(uint32_t work) {
    const uint32_t circles = Memory::Read32(work + kCircles);
    const int32_t index = int32_t(Memory::Read32(work + kCircle));
    return circles && index >= 0 ? circles + uint32_t(index) * kCircleBytes : 0;
}

// The circle's members in use and how many of them stand by.
std::pair<uint32_t, uint32_t> Members(uint32_t work) {
    const uint32_t circle = CircleAddress(work);
    uint32_t present = 0, standing = 0;

    for (uint32_t slot = 0; circle && slot < 4; ++slot) {
        const uint32_t member = circle + kCircleMembers + slot * kMemberBytes;

        if (Memory::Read32(member)) {
            ++present;
            standing += Memory::Read8(member + 4) != 0;
        }
    }

    return {present, standing};
}

std::string Describe(uint32_t work) {
    char text[256];
    const auto [present, standing] = Members(work);
    std::snprintf(text, sizeof(text), "state %02x sub %02x where %u entry %u%u%u busy %u command %u leader %u slot %d circle %d members %u standing %u gate '%.24s' city '%.24s'",
                  Memory::Read8(work + kState), Memory::Read8(work + kSub), Memory::Read32(work + kWhere), Memory::Read8(work + kUserData), Memory::Read8(work + kNotices),
                  Memory::Read8(work + kListAsked), Memory::Read32(work + kBusy), Memory::Read32(work + kCommand), Memory::Read32(work + kLeader),
                  int8_t(Memory::Read8(work + kSlot)), int32_t(Memory::Read32(work + kCircle)), present, standing,
                  reinterpret_cast<const char*>(Memory::GetPointer(work + kGateName, 24)), reinterpret_cast<const char*>(Memory::GetPointer(work + kCityName, 24)));
    return text;
}

// The joined circle's quest (0x8042A700: its posted block).
uint32_t CircleQuest(uint32_t work) {
    return Memory::Read16(Memory::Read32(work + kCircles) + Memory::Read32(work + kCircle) * kCircleBytes + kCirclePost);
}

uint32_t PostQuest(CpuContext* cpu, uint32_t quest, uint32_t members) {
    GuestCall call{cpu};

    // The board's block, kept: the circle manager may hold on to it.
    if (!postBlock)
        postBlock = call(OperatorNew, {kPostBlockBytes});

    if (!postBlock)
        return 0;

    for (uint32_t i = 0; i < kPostBlockBytes; i += 4)
        Memory::Write32(postBlock + i, 0);

    Memory::Write16(postBlock, quest);
    Memory::Write8(postBlock + 2, members);
    return call(Post, {postBlock});
}

// The city list once in the gate, the circle commands once in a city.
void RunCircle(CpuContext* cpu, uint32_t work, bool movie) {
    const uint8_t state = Memory::Read8(work + kState);
    const bool inside = state >= kStateInGate && state <= kStateLast;
    const bool idle = !Memory::Read32(work + kBusy);
    GuestCall call{cpu};

    switch (step) {
    case Step::Screens:
        if (inside && Memory::Read8(work + kCityName)) {
            Say("in city '%s'", reinterpret_cast<const char*>(Memory::GetPointer(work + kCityName, 24)));
            step = Step::Settle;
            settled = -1;
            break;
        }

        // In the gate: its counter's city list, as talking to it.
        inGate = inside && plan.city >= 0 && Memory::Read8(work + kGateName) && !Memory::Read8(kLobby) && !movie ? inGate + 1 : 0;

        if (inGate == kGateFrames) {
            if (const uint32_t plw = Cheats::LocalPlayer()) {
                Say("open the gate's city list for city %d", plan.city);
                call(TalkStart, {plw});
                call(OpenCities, {plw, kGateCounter});
                call(SendAction, {plw, 0});
            }

            inGate = 0;
        }

        break;

    case Step::Settle:
        // The city's entry runs its own exchanges (the hunter's data, the
        // circle list); a circle command beside them failed it.
        if (!idle || Memory::Read32(work + kWhere) != 2 || Memory::Read8(work + kUserData) != 2 || Memory::Read8(work + kNotices) != 2 || !Memory::Read8(work + kListAsked)) {
            settled = -1;
            break;
        }

        if (settled < 0)
            settled = GameStep::Elapsed();

        if (GameStep::Elapsed() - settled < kSettleTicks)
            break;

        if (plan.post >= 0) {
            const uint32_t started = PostQuest(cpu, plan.post, plan.members);
            Say("post quest %d for %u: %s", plan.post, plan.members, started ? "started" : "refused");
            step = started ? Step::Circle : Step::Done;
        } else if (plan.join >= 0) {
            const uint32_t circles = call.Virtual(CircleManager(), kCircleCount);

            if (uint32_t(plan.join) < circles) {
                const uint32_t started = call(Join, {uint32_t(plan.join)});
                Say("join circle %d of %u: %s", plan.join, circles, started ? "started" : "refused");
                step = started ? Step::Circle : Step::Done;
            }
        } else {
            step = Step::Done;
        }

        break;

    case Step::Circle:
        if (!idle)
            break;

        if (int8_t(Memory::Read8(work + kSlot)) < 0) {
            Say("the circle refused us: %s", Describe(work).c_str());
            step = Step::Done;
            break;
        }

        // In the circle the quest counter accepts its quest: the quest the
        // depart loads (lb_param_w) and the hunter's quest flags. Without it
        // the quest's work has no reward and QuestStep fails it at once.
        if (const uint32_t plw = Cheats::LocalPlayer()) {
            const bool leader = Memory::Read32(work + kLeader);
            const uint32_t quest = leader ? uint32_t(plan.post) : CircleQuest(work);
            call(AcceptQuest, {plw, quest, leader ? kAcceptPosted : kAcceptJoined});
            Say("accept quest %u", quest);
        }

        // The leader's ready departs (net_ctrl.cpp): it waits for the others.
        if (Memory::Read32(work + kLeader)) {
            step = plan.depart ? Step::Depart : Step::Done;
            break;
        }

        Say("in the circle, ready: %s", call(Ready) ? "started" : "refused");
        step = Step::Done;
        break;

    case Step::Depart: {
        const auto [present, standing] = Members(work);

        if (idle && present >= plan.depart && standing + 1 >= present) {
            Say("%u members, the others stand by, depart: %s", present, call(Ready) ? "started" : "refused");
            step = Step::Done;
        }

        break;
    }

    case Step::Done:
        break;
    }
}

void Trace(uint32_t work) {
    static const TraceFlag trace("net");
    static uint16_t last = 0xFFFF;

    if (!trace)
        return;

    const uint16_t now = uint16_t(Memory::Read8(work + kState) << 8 | Memory::Read8(work + kSub));

    static uint32_t phases = 0;
    const uint32_t entry = Memory::Read32(work + kWhere) << 24 | Memory::Read8(work + kUserData) << 16 | Memory::Read8(work + kNotices) << 8 | Memory::Read8(work + kListAsked);

    if (now != last || entry != phases || (Memory::Read8(work + kState) == 0x1D && frame % 120 == 0))
        Say("%s", Describe(work).c_str());

    last = now;
    phases = entry;

    // The error a quest's session ended with (MH3GetSessionErrorCode 0x8042BE2C).
    static uint32_t error[3] = {};

    for (uint32_t i = 0; i < 3; ++i) {
        if (Memory::Read32(work + kSessionError + 4 * i) == error[i])
            continue;

        for (uint32_t j = 0; j < 3; ++j)
            error[j] = Memory::Read32(work + kSessionError + 4 * j);

        Say("session error %08X phase %d detail %08X", error[0], int32_t(error[1]), error[2]);
        break;
    }

    // The online field task's move work (get_move_work_adrs(0), FieldTaskOnline
    // 0x8028BF80): mode (+0), sub (+1, +2), the quest's end reason (+0xFA), its
    // end flag (+0x111) and the return to the city (+0x22DA).
    static uint32_t field = 0;
    const uint32_t works = Memory::Read32(0x8065AB04);
    const uint32_t move = works ? Memory::Read32(works + 0x10) : 0;
    const uint32_t fields = move ? (Memory::Read8(move) << 24 | Memory::Read8(move + 1) << 16 | Memory::Read8(move + 2) << 8 | Memory::Read8(move + 0xFA)) : 0;

    if (fields != field)
        Say("field %08X mode %u sub %u.%u end %u ended %u return %u", move, fields >> 24, fields >> 16 & 0xFF, fields >> 8 & 0xFF, fields & 0xFF,
            move ? Memory::Read8(move + 0x111) : 0, move ? Memory::Read8(move + 0x22DA) : 0);

    field = fields;
}

} // namespace

namespace OnlineAuto {

std::string Command(CpuContext* cpu, std::istringstream& in) {
    const uint32_t work = Work();
    std::string word;

    if (!work)
        return "error: not online";

    if (!(in >> word) || word == "state")
        return "ok " + Describe(work);

    GuestFrame frame(cpu, 0x10);
    GuestCall call{cpu};
    uint32_t started = 0;

    if (word == "post") {
        uint32_t quest = 0, members = 4;

        if (!(in >> quest))
            return "error: online post QUEST [MEMBERS]";

        in >> members;
        started = PostQuest(cpu, quest, members);
    } else if (word == "join") {
        uint32_t index = 0;
        in >> index;
        started = call(Join, {index});
    } else if (word == "ready") {
        started = call(Ready);
    } else if (word == "unready") {
        started = call(Unready);
    } else if (word == "depart") {
        started = call(Depart);
    } else if (word == "accept") {
        const uint32_t plw = Cheats::LocalPlayer();

        if (!plw)
            return "error: no hunter";

        const bool leader = Memory::Read32(work + kLeader);
        uint32_t quest = leader ? 0 : CircleQuest(work);
        in >> quest;
        call(AcceptQuest, {plw, quest, leader ? kAcceptPosted : kAcceptJoined});
        return "ok " + std::to_string(quest);
    } else if (word == "circles") {
        return "ok " + std::to_string(call.Virtual(CircleManager(), kCircleCount));
    } else if (word == "auto") {
        std::string spec;
        in >> spec;
        plan = Parse(spec);
        step = Step::Screens;
        return "ok";
    } else {
        return "error: online [state|circles|post QUEST [MEMBERS]|join N|accept [QUEST]|ready|unready|depart|auto SPEC]";
    }

    return started ? "ok started" : "error: refused (" + Describe(work) + ")";
}

} // namespace OnlineAuto

extern "C" void func_8044E4FC(CpuContext* cpu);
extern "C" void MH3_OnlineScreens(CpuContext* cpu) {
#ifdef MH3_DEVELOPMENT
    static const bool configured = [] {
        if (const char* spec = std::getenv("MH3_ONLINE_AUTO")) {
            plan = Parse(spec);
            Say("plan id %u server %u gate %u post %d members %u depart %u join %d", plan.id, plan.server, plan.gate, plan.post, plan.members, plan.depart, plan.join);
        }

        return true;
    }();
    (void)configured;
#endif
    const uint32_t work = Work();
    uint16_t keys = 0;
    ++frame;

    if (work) {
        Trace(work);

        if (plan.on) {
            GuestFrame guard(cpu, 0x10);
            keys = Choose(work);
#ifdef MH3_DEVELOPMENT
            const bool movie = SkipMovie();
#else
            const bool movie = false;
#endif
            RunCircle(cpu, work, movie);
        }
    }

    if (!keys) {
        func_8044E4FC(cpu);
        return;
    }

    const uint16_t pressed = Memory::Read16(kPressed);
    Memory::Write16(kPressed, pressed | keys);
    func_8044E4FC(cpu);
    Memory::Write16(kPressed, pressed);
}
REGISTER_NATIVE_FUNCTION_AS(0x8044E4FC, MH3_OnlineScreens, "OnlineScreens");

extern "C" void func_8042E26C(CpuContext* cpu);
extern "C" void MH3_CityList(CpuContext* cpu) {
    const uint32_t menu = Memory::Read32(kCityMenu);
    const uint16_t keys = plan.on && menu ? ChooseCity(menu) : 0;

    if (!keys) {
        func_8042E26C(cpu);
        return;
    }

    const uint16_t pressed = Memory::Read16(kLobbyPressed);
    Memory::Write16(kLobbyPressed, pressed | keys);
    func_8042E26C(cpu);
    Memory::Write16(kLobbyPressed, pressed);
}
REGISTER_NATIVE_FUNCTION_AS(0x8042E26C, MH3_CityList, "CityList");

// A quest session's start (0x803D99B0) and end (0x803DA0CC) state machines:
// (session, phase*) -> done. MH3_TRACE=net prints each phase change, with the
// session's started flag (+0x3C1).
namespace {

// A session's members (0x48 bytes each from +0x538: in use, ..., state at
// +4), the local one (+0x536) and the order (+0x664 count, +0x668 indices).
std::string SessionMembers(uint32_t session) {
    char text[160];
    int length = std::snprintf(text, sizeof(text), "self %d order", int8_t(Memory::Read8(session + 0x536)));
    const uint32_t count = std::min<uint32_t>(Memory::Read32(session + 0x664), 4);

    for (uint32_t i = 0; i < count; ++i)
        length += std::snprintf(text + length, sizeof(text) - length, " %d", int8_t(Memory::Read8(session + 0x668 + i)));

    for (uint32_t i = 0; i < 4; ++i) {
        const uint32_t member = session + 0x538 + i * 0x48;
        length += std::snprintf(text + length, sizeof(text) - length, " [%u: use %u state %u]", i, Memory::Read8(member), Memory::Read8(member + 4));
    }

    return text;
}

void TracePhases(CpuContext* cpu, const char* name, void (*original)(CpuContext*)) {
    static const TraceFlag trace("net");
    const uint32_t session = cpu->gpr[3], phase = cpu->gpr[4], lr = cpu->lr;

    if (!trace) {
        original(cpu);
        return;
    }

    const int32_t before = int32_t(Memory::Read32(phase));
    original(cpu);
    const int32_t after = int32_t(Memory::Read32(phase));

    if (after != before || (cpu->gpr[3] & 0xFF))
        Say("session %08X %s phase %d -> %d (at %08X, lr %08X), started %u, done %u, %s", session, name, before, after, phase, lr, Memory::Read8(session + 0x3C1),
            cpu->gpr[3] & 0xFF, SessionMembers(session).c_str());
}

} // namespace

extern "C" void func_803D99B0(CpuContext* cpu);
extern "C" void MH3_SessionStart(CpuContext* cpu) {
    TracePhases(cpu, "start", func_803D99B0);
}
REGISTER_NATIVE_FUNCTION_AS(0x803D99B0, MH3_SessionStart, "SessionStart");

extern "C" void func_803DA0CC(CpuContext* cpu);
extern "C" void MH3_SessionEnd(CpuContext* cpu) {
    TracePhases(cpu, "end", func_803DA0CC);
}
REGISTER_NATIVE_FUNCTION_AS(0x803DA0CC, MH3_SessionEnd, "SessionEnd");

// The quest session's event handler 0x803DC5AC (session, event, ...; the
// event's block in r8): MH3_TRACE=net prints each event's first 4 and every
// 0x805F (a member's status: its kind at +8 and value at +0x18 of the block's
// data, kind 3 value 3 = in the session).
extern "C" void func_803DC5AC(CpuContext* cpu);
extern "C" void MH3_SessionEvent(CpuContext* cpu) {
    static const TraceFlag trace("net");

    if (trace) {
        static uint32_t counts[0x100] = {};
        const uint32_t event = cpu->gpr[4], block = cpu->gpr[8];
        uint32_t& count = counts[event & 0xFF];

        if (event == 0x805F && block && Memory::Read8(block)) {
            const uint32_t data = Memory::Read32(block + 0xC), session = cpu->gpr[3];
            Say("session event 805F: flag %u kind %u type %u value %u, before %s", Memory::Read8(data), Memory::Read8(data + 8), Memory::Read8(data + 0x10),
                Memory::Read32(data + 0x18), SessionMembers(session).c_str());
            func_803DC5AC(cpu);
            Say("session event 805F: after %s", SessionMembers(session).c_str());
            ++count;
            return;
        } else if (count < 4) {
            Say("session event %04X (%08X %08X %08X)", event, cpu->gpr[5], cpu->gpr[6], cpu->gpr[7]);
        }

        ++count;
    }

    func_803DC5AC(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x803DC5AC, MH3_SessionEvent, "SessionEvent");
