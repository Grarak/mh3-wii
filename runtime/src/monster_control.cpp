// Monster control for debugging, RMHP08 (docs/monster-control.md).
//
// A monster's action is a pair of bytes in its work, mode (+0x1E5) and sub
// (+0x1E6), started by EnemySetAction (0x80128624). The game's side is
// ported in enemy_actions.cpp, which documents it; it asks Blocked and calls
// NoteAction here. MH3_TRACE=actions prints each action start, and once per
// kind its handlers. MH3_MONSTER_CONTROL=FILE reads commands from FILE,
// one a line, each run once in order as it appears (append lines while the
// game runs; it is read about every third of a second):
//
//   target N              the monster work index the commands act on (0)
//   kind K                target the first monster of kind K (+3)
//   await K               hold the next commands until a monster of kind K
//                         is in play, then target it
//   act MODE SUB          start that action now
//   every TICKS MODE SUB  start it every TICKS ticks of game time (30 a
//                         second); `every off` stops
//   ai off|on             the AI picks no action (it waits between commands)
//   freeze on|off         hold the monster's step (it stands still)
//   face DEGREES          set its yaw
//   goto X Y Z            place it
//   hunter save|back      remember the local hunter's place, put it back
//   hunter X Y Z          place the local hunter
//   wait TICKS            run the next commands TICKS ticks of game time later
//   list                  print each monster's kind, action and position
#include "monster_control.h"

#include "abi_bridge.h"
#include "guest_call.h"
#include "memory.h"
#include "ppc_runtime.h"

#ifdef MH3_DEVELOPMENT
#include "cheats.h"
#include "dev_options.h"
#include "game_step.h"

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <string>
#include <sys/stat.h>
#endif

#include <cstdint>

namespace {

enum : uint32_t {
    kInUse = 0x000,     // u16: 0x0101 while in use
    kKind = 0x003,      // u8: the enemy kind
    kVariant = 0x00A,   // u8
    kState = 0x005,     // u8[3]: the action's routine state
    kPosition = 0x188,  // VEC3
    kYaw = 0x1C0,       // s32: 65536 a turn
    kMode = 0x1E5,      // u8: the action's mode
    kSub = 0x1E6,       // u8: the action within the mode
};

enum : uint32_t {
    SetAction = 0x80128624,   // (work, mode, sub, how)
    EnemyData = 0x80126098,   // get_enemy_data (work)
    SetPosition = 0x801460BC, // (work; f1-f3)
    SetAngles = 0x80146170,   // (work; f1-f3 degrees)
};

#ifdef MH3_DEVELOPMENT
constexpr uint32_t kStepCounter = 0x8065AA6C; // ticks of game time
constexpr uint32_t kHunterPosition = 0x3C;    // PLW f32[3] (the teleporter cheat's)

// The action starts this file makes (the trace marks them).
bool g_commanded = false;

struct Control {
    std::string path;
    off_t size = 0;   // the file's size when last read
    size_t lines = 0; // commands read
    std::deque<std::string> queue; // read, not run yet
    uint32_t wait = 0; // ticks before the next command runs
    float hunter[3] = {}; // `hunter save`
    int32_t awaitKind = -1; // `await`: the kind the commands wait for
    bool saved = false;
    uint32_t poll = 0;
    uint32_t target = 0;
    uint32_t frozen = 0; // the frozen monster's work, 0 for none
    bool ai = true;      // the target picks its own actions
    bool repeat = false;
    uint8_t mode = 0, sub = 0;
    uint32_t every = 0, left = 0;
};

Control& State() {
    static Control control = [] {
        Control c;

        if (const char* path = std::getenv("MH3_MONSTER_CONTROL"); path && *path)
            c.path = path;

        return c;
    }();
    return control;
}

float F(uint32_t a) {
    return Memory::ReadFloat32(a);
}

uint32_t Monster(uint32_t index) {
    const uint32_t work = Cheats::MoveWork(Cheats::Kind::Monster, index);
    return work && Memory::Read16(work + kInUse) == 0x0101 ? work : 0;
}

// The index of the first monster of `kind` in play, or -1.
int32_t OfKind(uint32_t kind) {
    for (uint32_t i = 0, n = Cheats::MoveWorkCount(Cheats::Kind::Monster); i < n; ++i)
        if (const uint32_t m = Monster(i); m && Memory::Read8(m + kKind) == kind)
            return int32_t(i);

    return -1;
}

void Start(GuestCall& game, uint32_t work, uint8_t mode, uint8_t sub) {
    g_commanded = true;
    game.Call<SetAction>({work, mode, sub, 0});
    g_commanded = false;
}

void List() {
    for (uint32_t i = 0, n = Cheats::MoveWorkCount(Cheats::Kind::Monster); i < n; ++i)
        if (const uint32_t m = Monster(i))
            std::fprintf(stderr, "MH3 monster control: [%u] work %08x kind %u variant %u action %u.%u state %u.%u.%u at %.1f %.1f %.1f yaw %.1f\n", i, m, Memory::Read8(m + kKind),
                         Memory::Read8(m + kVariant), Memory::Read8(m + kMode), Memory::Read8(m + kSub), Memory::Read8(m + kState), Memory::Read8(m + kState + 1),
                         Memory::Read8(m + kState + 2), F(m + kPosition), F(m + kPosition + 4), F(m + kPosition + 8),
                         double(int32_t(Memory::Read32(m + kYaw))) * 360.0 / 65536.0);
}

void Run(GuestCall& game, Control& c, const std::string& line) {
    std::istringstream in(line);
    std::string word;

    if (!(in >> word) || word[0] == '#')
        return;

    const uint32_t work = Monster(c.target);
    const auto need = [&] {
        if (!work)
            std::fprintf(stderr, "MH3 monster control: no monster %u for '%s'\n", c.target, line.c_str());
        return work != 0;
    };

    unsigned a = 0, b = 0, t = 0;
    std::string arg;
    float x = 0, y = 0, z = 0;

    if (word == "target" && in >> a)
        c.target = a;
    else if (word == "kind" && in >> a) {
        if (const int32_t i = OfKind(a); i >= 0)
            c.target = uint32_t(i);
    } else if (word == "await" && in >> a)
        c.awaitKind = int32_t(a);
    else if (word == "act" && in >> a >> b) {
        if (need())
            Start(game, work, uint8_t(a), uint8_t(b));
    } else if (word == "every" && in >> arg) {
        if (arg == "off")
            c.repeat = false;
        else if (in >> a >> b && (t = unsigned(std::strtoul(arg.c_str(), nullptr, 0))) > 0) {
            c.repeat = true;
            c.every = t;
            c.left = 0;
            c.mode = uint8_t(a);
            c.sub = uint8_t(b);
        }
    } else if (word == "ai" && in >> arg)
        c.ai = arg != "off";
    else if (word == "freeze" && in >> arg)
        c.frozen = arg == "on" && need() ? work : 0;
    else if (word == "face" && in >> x) {
        if (need())
            game.Call<SetAngles>({work}, {0.0, double(x), 0.0});
    } else if (word == "goto" && in >> x >> y >> z) {
        if (need())
            game.Call<SetPosition>({work}, {double(x), double(y), double(z)});
    } else if (word == "hunter" && in >> arg) {
        const uint32_t plw = Cheats::LocalPlayer();

        if (!plw) {
            std::fprintf(stderr, "MH3 monster control: no hunter for '%s'\n", line.c_str());
            return;
        }

        if (arg == "save") {
            for (uint32_t k = 0; k < 3; ++k)
                c.hunter[k] = F(plw + kHunterPosition + 4 * k);

            c.saved = true;
        } else if (arg == "back" && c.saved) {
            for (uint32_t k = 0; k < 3; ++k)
                Memory::WriteFloat32(plw + kHunterPosition + 4 * k, c.hunter[k]);
        } else {
            std::istringstream place(line.substr(line.find(arg)));

            if (place >> x >> y >> z) {
                Memory::WriteFloat32(plw + kHunterPosition, x);
                Memory::WriteFloat32(plw + kHunterPosition + 4, y);
                Memory::WriteFloat32(plw + kHunterPosition + 8, z);
            }
        }
    } else if (word == "wait" && in >> a)
        c.wait = a;
    else if (word == "list")
        List();
    else {
        std::fprintf(stderr, "MH3 monster control: unknown command '%s'\n", line.c_str());
        return;
    }

    std::fprintf(stderr, "MH3 monster control: %s\n", line.c_str());
}

// The commands appended since the last read, queued: whole lines only (a
// line without its newline waits for it); a shorter file was written anew.
void Read(Control& c) {
    struct stat info {};

    if (::stat(c.path.c_str(), &info) != 0 || info.st_size == c.size)
        return;

    if (info.st_size < c.size)
        c.lines = 0;

    c.size = info.st_size;
    std::ifstream file(c.path, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    size_t line = 0;

    for (size_t at = 0, end; (end = text.find('\n', at)) != std::string::npos; at = end + 1, ++line)
        if (line >= c.lines) {
            c.queue.push_back(text.substr(at, end - at));
            c.lines = line + 1;
        }
}

#endif

} // namespace

namespace MonsterControl {

void Step([[maybe_unused]] CpuContext* ctx) {
#ifdef MH3_DEVELOPMENT
    Control& c = State();

    if (c.path.empty())
        return;

    GuestCall game{ctx};

    if (c.poll++ % 10 == 0)
        Read(c);

    const uint32_t ticks = GameStep::Ticks();
    c.wait = c.wait > ticks ? c.wait - ticks : 0;

    if (c.awaitKind >= 0) {
        const int32_t i = OfKind(uint32_t(c.awaitKind));

        if (i < 0)
            return;

        c.target = uint32_t(i);
        c.awaitKind = -1;
    }

    while (!c.queue.empty() && c.wait == 0 && c.awaitKind < 0) {
        const std::string line = std::move(c.queue.front());
        c.queue.pop_front();
        Run(game, c, line);
    }

    if (!c.repeat)
        return;

    if (c.left > ticks) {
        c.left -= ticks;
        return;
    }

    if (const uint32_t work = Monster(c.target)) {
        Start(game, work, c.mode, c.sub);
        c.left = c.every;
    }
#endif
}

bool Frozen([[maybe_unused]] uint32_t work) {
#ifdef MH3_DEVELOPMENT
    return work != 0 && State().frozen == work;
#else
    return false;
#endif
}

#ifdef MH3_DEVELOPMENT
// With `ai off`, the target's own picks: the actions the AI's pick
// (0x8013C4BC, which asks again each step) starts, in any mode.
bool Blocked(uint32_t work, uint32_t caller) {
    const Control& c = State();
    return !c.ai && !g_commanded && caller == kAiPickReturn && work == Monster(c.target);
}

ActionStart Begin(uint32_t work, uint8_t mode, uint8_t sub, uint8_t how, uint32_t caller) {
    return {work, caller, Memory::Read8(work + kMode), Memory::Read8(work + kSub), mode, sub, how, int32_t(Memory::Read32(work + kYaw)), F(work + kPosition),
            F(work + kPosition + 4), F(work + kPosition + 8)};
}

// MH3_TRACE=actions: the action started, the one before, the pair asked for
// (the remap handler can change it), how and the caller, the yaw and place
// the last action left; once per kind its handler table.
void NoteAction(CpuContext* ctx, const ActionStart& a) {
    static const bool traced = TraceOptions().Has("actions");

    if (!traced)
        return;

    const uint32_t kind = Memory::Read8(a.work + kKind);
    static std::set<uint32_t> kinds;

    if (kinds.insert(kind).second) {
        GuestCall game{ctx};
        const uint32_t data = game.Call<EnemyData>({a.work});
        const uint32_t table = data ? Memory::Read32(data + 0x10) : 0;
        std::fprintf(stderr, "MH3 monster handlers: kind %u variant %u data %08x table %08x", kind, Memory::Read8(a.work + kVariant), data, table);

        for (uint32_t i = 0; table && i <= 0x16; ++i)
            std::fprintf(stderr, " %u:%08x", i, i == 7 ? 0u : Memory::Read32(table + 4 * i));

        std::fprintf(stderr, "\n");
    }

    std::fprintf(stderr, "MH3 monster action: work %08x kind %u %u.%u -> %u.%u (asked %u.%u, how %u, lr %08x%s) tick %u time %.2f scale %.3f yaw %d at %.1f %.1f %.1f\n", a.work, kind,
                 a.fromMode, a.fromSub, Memory::Read8(a.work + kMode), Memory::Read8(a.work + kSub), a.mode, a.sub, a.how, a.caller, g_commanded ? ", control" : "",
                 Memory::Read32(kStepCounter), GameStep::Elapsed(), GameStep::Scale(), a.yaw, a.x, a.y, a.z);
}
#endif

} // namespace MonsterControl
