// Monster control for debugging, RMHP08 (docs/monster-control.md).
//
// A monster's action is a pair of bytes in its work: the mode (+0x1E5) and
// the action within it (+0x1E6); the action's routine state is +5..+7. From
// the disassembly (Ghidra):
//
//   0x80128624 (work, mode, sub, how): starts an action. It lets the
//              species' remap handler (index 4 below) change the pair, stores
//              it (0x80128604: +5..+7 cleared, +0x1E5, +0x1E6, a random
//              word at +0x1EC) and runs the species' start handler (index 5,
//              from 0x801283A4). `how` 0 is the AI's (online, it is also
//              sent to the other players), 1 a spawn's.
//   0x80126520 (work, index): the species' handler `index` from its enemy
//              data's table (get_enemy_data(work) +0x10): 0 set up, 4 remap,
//              5 start, 6 step (0x80138BC4 calls it each step while the
//              monster is active), 8 after the step, 0xC its spawn place.
//   0x8013C4BC (work, pair*): the AI's pick; {0, 0xFF} picks the waiting
//              action that fits its state (0.0, 0.1, 0.2, ...).
//   0x80127FAC (work): an action's end, back to a waiting action (how 2),
//              where the AI picks the next.
//
// MH3_TRACE=actions prints each action start (monster_control.cpp), and once
// per kind its handlers. MH3_MONSTER_CONTROL=FILE reads commands from FILE,
// one a line, each run once when it appears (append lines while the game
// runs; it is read about every third of a second):
//
//   target N              the monster work index the commands act on (0)
//   act MODE SUB          start that action now
//   every TICKS MODE SUB  start it every TICKS ticks of game time (30 a
//                         second); `every off` stops
//   ai off|on             the AI picks no action (it waits between commands)
//   freeze on|off         hold the monster's step (it stands still)
//   face DEGREES          set its yaw
//   goto X Y Z            place it
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
constexpr uint32_t kAiPickReturn = 0x8013C5C8; // 0x8013C4BC (work, pair*): the AI's pick, its SetAction call

// The action starts this file makes (the trace marks them).
bool g_commanded = false;

struct Control {
    std::string path;
    off_t size = 0;   // the file's size when last read
    size_t lines = 0; // commands run
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
    } else if (word == "list")
        List();
    else {
        std::fprintf(stderr, "MH3 monster control: unknown command '%s'\n", line.c_str());
        return;
    }

    std::fprintf(stderr, "MH3 monster control: %s\n", line.c_str());
}

// The commands appended since the last read: whole lines only (a line
// without its newline waits for it); a shorter file was written anew.
void Read(GuestCall& game, Control& c) {
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
            Run(game, c, text.substr(at, end - at));
            c.lines = line + 1;
        }
}

// With `ai off`, the target's own picks: the actions 0x8013C4BC (the AI's
// pick, which asks again each step) starts, in any mode.
bool Blocked(const CpuContext* ctx) {
    const Control& c = State();
    return !c.ai && !g_commanded && ctx->lr == kAiPickReturn && ctx->gpr[3] == Monster(c.target);
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
        Read(game, c);

    if (!c.repeat)
        return;

    const uint32_t ticks = GameStep::Ticks();

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

} // namespace MonsterControl

extern "C" void func_80128624(CpuContext* ctx);

extern "C" void MH3_EnemySetAction(CpuContext* ctx) {
#ifdef MH3_DEVELOPMENT
    if (Blocked(ctx)) [[unlikely]]
        return;

    static const bool traced = TraceOptions().Has("actions");

    if (traced) [[unlikely]] {
        const uint32_t work = ctx->gpr[3], mode = ctx->gpr[4] & 0xFF, sub = ctx->gpr[5] & 0xFF, how = ctx->gpr[6] & 0xFF, lr = ctx->lr;
        const uint32_t fromMode = Memory::Read8(work + kMode), fromSub = Memory::Read8(work + kSub), kind = Memory::Read8(work + kKind);
        const bool commanded = g_commanded;
        const int32_t yaw = int32_t(Memory::Read32(work + kYaw)); // as the last action left it
        static std::set<uint32_t> kinds;

        if (kinds.insert(kind).second) {
            GuestCall game{ctx};
            const uint32_t data = game.Call<EnemyData>({work});
            const uint32_t table = data ? Memory::Read32(data + 0x10) : 0;
            std::fprintf(stderr, "MH3 monster handlers: kind %u variant %u data %08x table %08x", kind, Memory::Read8(work + kVariant), data, table);

            for (uint32_t i = 0; table && i <= 0x16; ++i)
                std::fprintf(stderr, " %u:%08x", i, i == 7 ? 0u : Memory::Read32(table + 4 * i));

            std::fprintf(stderr, "\n");
            ctx->gpr[3] = work, ctx->gpr[4] = mode, ctx->gpr[5] = sub, ctx->gpr[6] = how, ctx->lr = lr;
        }

        func_80128624(ctx);
        std::fprintf(stderr, "MH3 monster action: work %08x kind %u %u.%u -> %u.%u (asked %u.%u, how %u, lr %08x%s) tick %u time %.2f scale %.3f yaw %d\n", work, kind, fromMode,
                     fromSub, Memory::Read8(work + kMode), Memory::Read8(work + kSub), mode, sub, how, lr, commanded ? ", control" : "", Memory::Read32(kStepCounter),
                     GameStep::Elapsed(), GameStep::Scale(), yaw);
        return;
    }
#endif
    func_80128624(ctx);
}

REGISTER_NATIVE_FUNCTION_AS(0x80128624, MH3_EnemySetAction, "EnemySetAction");
