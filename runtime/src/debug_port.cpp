// The debug port (development builds). A listener thread
// takes TCP connections on MH3_DEBUG_PORT and collects their lines; the game
// thread runs them in order once a logic step (Step) and hands the answers
// back to the listener, which writes them. A command that takes game time
// (wait, steps) holds the ones after it until it is done.
//
//   help                      the commands
//   state                     step, game time, map, area, the local hunter
//   peek ADDR [N]             N 32-bit words (hex); peekh, peekb: 16-bit, bytes
//   peekf ADDR [N]            N floats
//   peeks ADDR [MAX]          a NUL-terminated string
//   poke ADDR VALUE...        32-bit words (hex); pokeh, pokeb: 16-bit, bytes
//   find HEX                  the addresses (up to 16) in MEM1 and MEM2 where
//                             those bytes start, at a 4-byte boundary
//   pokef ADDR VALUE...       floats
//   wait TICKS                answer TICKS ticks of game time later (30 a s)
//   steps N                   answer N logic steps later
//   shot PATH                 dump the next frame to PATH (BMP)
//   call ADDR [INT...] [f FLOAT...]  call a guest function (ints hex, in r3..,
//                             floats in f1..); answers r3 and f1
//   hide [NAME,NAME...]       draw no model material of those MDL0 names
//                             (none: draw all again; MH3_HIDE_MATERIALS)
//   cheat NAME[=VALUE]        turn on an integrated cheat
//   trace [ITEM on|off]       the MH3_TRACE items and which are on, or turn
//                             one on or off
//   movie skip                end the open movie (as at its last frame)
//   press BUTTONS [TICKS [LX LY [RX RY]]]
//                             hold Classic buttons (a+b, ..., none, or hex
//                             bits) and sticks (-512..511, up and right
//                             positive) for TICKS ticks (3), then give the
//                             controller back
//   hunter                    the local hunter's weapon, stance (3 swimming),
//                             action, its state and timer, motion, charge
//   hunter act GROUP ACTION [FLAGS]
//                             start an action (FieldNextAction) without
//                             input: MH3_TRACE=actions prints the ones the
//                             hunter starts. Group 4 holds the weapon's: the
//                             hammer's charged spin is 4 18 on land, 4 36
//                             swimming (its turns: the charge level + 2)
//   hunter charge LEVEL       set the charge level the charged attacks read
//   online [state|circles|post QUEST [MEMBERS]|join N|accept [QUEST]|ready|unready|depart|auto SPEC]
//                             the online screens' state, or a quest circle
//                             command (online_auto.cpp)
// Development builds on the hosts only (CMakeLists.txt adds this file to
// those).
#include "debug_port.h"

#include "abi_bridge.h"
#include "cheats.h"
#include "controller_host.h"
#include "dev_options.h"
#include "game_step.h"
#include "gx_host.h"
#include "launcher_cheats.h"
#include "memory.h"
#include "movie_draw.h"
#include "net/online_auto.h"
#include "ppc_runtime.h"
#include "runtime/native/adapters/model_scope_bridge.h"
#include "runtime/native/mh3_classic_map.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <netinet/in.h>
#include <poll.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

constexpr uint32_t kMapNo = 0x806BB805, kAreaNo = 0x806BB806; // u8: get_now_mapno, get_now_areano
constexpr uint32_t kHunterPosition = 0x3C;                    // PLW f32[3]

// The PLW's action (player_work.h, field_player_actions.cpp).
enum : uint32_t {
    kHunterWeapon = 2,        // u8: 0-8 (1 sword and shield, 2 hammer)
    kHunterState = 5,         // u8
    kHunterStance = 9,        // u8: 3 swimming
    kHunterGroup = 10,        // u8
    kHunterAction = 12,       // u16
    kHunterTimer = 0x28,      // s32
    kHunterChargeSteps = 0x310, // s16
    kHunterChargeLevel = 0x312, // u8
    kHunterChar = 0x13C,      // MHchar*, its motion a u16 at +0x54
};

constexpr uint32_t kFieldNextAction = 0x80275B28; // (plw, u8 group, u16 action, u16 flags)

struct Line {
    uint32_t client;
    std::string text;
};

// Shared between the listener and the game thread.
std::mutex lock;
std::vector<Line> inbox, outbox;
int wake[2] = {-1, -1}; // the game thread writes a byte when outbox has lines

// The listener: accepts clients, reads their lines, writes their answers.
void Listen(int server) {
    std::map<uint32_t, std::pair<int, std::string>> clients; // id -> fd, unread text
    uint32_t next = 1;

    for (;;) {
        std::vector<pollfd> polls{{server, POLLIN, 0}, {wake[0], POLLIN, 0}};
        std::vector<uint32_t> ids;

        for (const auto& [id, client] : clients) {
            polls.push_back({client.first, POLLIN, 0});
            ids.push_back(id);
        }

        if (poll(polls.data(), polls.size(), -1) < 0) {
            if (errno == EINTR)
                continue;

            std::fprintf(stderr, "MH3 debug port: poll failed: %s\n", std::strerror(errno));
            return;
        }

        if (polls[0].revents & POLLIN) {
            if (const int fd = accept(server, nullptr, nullptr); fd >= 0)
                clients[next++] = {fd, {}};
        }

        if (polls[1].revents & POLLIN) {
            char drain[64];

            while (read(wake[0], drain, sizeof(drain)) > 0) {
            }

            std::vector<Line> answers;
            {
                std::lock_guard guard(lock);
                answers.swap(outbox);
            }

            for (const Line& answer : answers)
                if (const auto it = clients.find(answer.client); it != clients.end()) {
                    const std::string text = answer.text + "\n";

                    for (size_t sent = 0; sent < text.size();) {
                        const ssize_t n = send(it->second.first, text.data() + sent, text.size() - sent, MSG_NOSIGNAL);

                        if (n <= 0)
                            break;

                        sent += size_t(n);
                    }
                }
        }

        for (size_t i = 0; i < ids.size(); ++i) {
            if (!(polls[i + 2].revents & (POLLIN | POLLHUP | POLLERR)))
                continue;

            auto& [fd, text] = clients[ids[i]];
            char buffer[4096];
            const ssize_t n = recv(fd, buffer, sizeof(buffer), 0);

            if (n <= 0) {
                close(fd);
                clients.erase(ids[i]);
                continue;
            }

            text.append(buffer, size_t(n));
            std::vector<Line> lines;

            for (size_t end; (end = text.find('\n')) != std::string::npos; text.erase(0, end + 1)) {
                std::string line = text.substr(0, end);

                if (!line.empty() && line.back() == '\r')
                    line.pop_back();

                lines.push_back({ids[i], std::move(line)});
            }

            std::lock_guard guard(lock);
            inbox.insert(inbox.end(), lines.begin(), lines.end());
        }
    }
}

// MH3_DEBUG_PORT=PORT (127.0.0.1) or ADDR:PORT; false when unset or the
// socket can't be opened.
bool Start() {
    const char* option = std::getenv("MH3_DEBUG_PORT");

    if (!option || !*option)
        return false;

    std::string host = "127.0.0.1", port = option;

    if (const auto colon = port.rfind(':'); colon != std::string::npos) {
        host = port.substr(0, colon);
        port = port.substr(colon + 1);
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(uint16_t(std::atoi(port.c_str())));
    const int server = socket(AF_INET, SOCK_STREAM, 0);
    const int on = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    if (inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1 || bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || listen(server, 4) != 0 ||
        pipe(wake) != 0) {
        std::fprintf(stderr, "MH3 debug port: can't listen on %s:%s: %s\n", host.c_str(), port.c_str(), std::strerror(errno));
        close(server);
        return false;
    }

    fcntl(wake[0], F_SETFL, O_NONBLOCK);
    std::thread(Listen, server).detach();
    std::fprintf(stderr, "MH3 debug port: listening on %s:%s\n", host.c_str(), port.c_str());
    return true;
}

void Answer(uint32_t client, std::string text) {
    {
        std::lock_guard guard(lock);
        outbox.push_back({client, std::move(text)});
    }

    const char byte = 0;
    [[maybe_unused]] const ssize_t n = write(wake[1], &byte, 1);
}

// Guest RAM at its virtual addresses (Memory's reads don't alias the
// physical ones).
bool Ram(uint32_t address, size_t length) {
    return address >= 0x80000000u && Memory::Contains(address, length);
}

// The bytes a peek or poke command's words take.
uint32_t WordSize(const std::string& word) {
    const char last = word.back();
    return last == 'b' || last == 's' ? 1 : last == 'h' ? 2 : 4;
}

bool Hex(const std::string& word, uint32_t& value) {
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(word.c_str(), &end, 16);
    value = uint32_t(parsed);
    return !word.empty() && !*end && parsed <= 0xFFFFFFFFul;
}

// The game thread's side.
struct Session {
    std::deque<Line> queue;

    // The command that takes game time: until (game time) and steps both pass.
    bool busy = false;
    uint32_t client = 0;
    double until = 0;
    uint32_t steps = 0;
    uint64_t step = 0;

    // press: the controller goes back to the host when the command is done.
    bool release = false;
};

std::string Format(const char* format, ...) __attribute__((format(printf, 1, 2)));

std::string Format(const char* format, ...) {
    char text[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    return text;
}

std::string State(const Session& s) {
    std::string text = Format("ok step %llu time %.2f scale %.3f map %u area %u", static_cast<unsigned long long>(s.step), GameStep::Elapsed(), GameStep::Scale(),
                              Memory::Read8(kMapNo), Memory::Read8(kAreaNo));

    if (const uint32_t plw = Cheats::LocalPlayer())
        text += Format(" hunter %08x at %.1f %.1f %.1f", plw, Memory::ReadFloat32(plw + kHunterPosition), Memory::ReadFloat32(plw + kHunterPosition + 4),
                       Memory::ReadFloat32(plw + kHunterPosition + 8));
    else
        text += " hunter none";

    return text;
}

// Runs one command; an empty answer means it is still running (busy).
std::string Run(CpuContext* ctx, Session& s, const Line& line) {
    std::istringstream in(line.text);
    std::string word, arg;

    if (!(in >> word) || word[0] == '#')
        return "ok";

    uint32_t address = 0, value = 0;

    if (word == "help")
        return "ok help state peek peekh peekb peekf peeks poke pokeh pokeb pokef find wait steps press hunter point mouse shot call hide cheat trace movie online";

    if (word == "state")
        return State(s);

    if (word.rfind("peek", 0) == 0) {
        if (!(in >> arg) || !Hex(arg, address))
            return "error: peek ADDR [N]";

        unsigned count = 1;
        in >> count;
        std::string text = "ok";

        if (word == "peeks") {
            text += ' ';

            for (unsigned i = 0; i < (count > 1 ? count : 256); ++i) {
                if (!Ram(address + i, 1))
                    return "error: no guest RAM at " + Format("%08x", address + i);

                const char c = char(Memory::Read8(address + i));

                if (!c)
                    break;

                text += c >= 0x20 && c < 0x7F ? c : '.';
            }

            return text;
        }

        count = count < 256 ? count : 256;

        if (!Ram(address, size_t(count) * WordSize(word)))
            return "error: no guest RAM at " + arg;

        for (unsigned i = 0; i < count; ++i) {
            if (word == "peek")
                text += Format(" %08x", Memory::Read32(address + 4 * i));
            else if (word == "peekh")
                text += Format(" %04x", Memory::Read16(address + 2 * i));
            else if (word == "peekb")
                text += Format(" %02x", Memory::Read8(address + i));
            else if (word == "peekf")
                text += Format(" %g", double(Memory::ReadFloat32(address + 4 * i)));
            else
                return "error: unknown command " + word;
        }

        return text;
    }

    if (word.rfind("poke", 0) == 0) {
        if (!(in >> arg) || !Hex(arg, address))
            return "error: poke ADDR VALUE...";

        unsigned count = 0;

        for (std::string start = arg; in >> arg; ++count) {
            if (!Ram(address + WordSize(word) * count, WordSize(word)))
                return "error: no guest RAM at " + start;

            if (word == "pokef") {
                Memory::WriteFloat32(address + 4 * count, std::strtod(arg.c_str(), nullptr));
                continue;
            }

            if (!Hex(arg, value))
                return "error: bad value " + arg;

            if (word == "poke")
                Memory::Write32(address + 4 * count, value);
            else if (word == "pokeh")
                Memory::Write16(address + 2 * count, uint16_t(value));
            else if (word == "pokeb")
                Memory::Write8(address + count, uint8_t(value));
            else
                return "error: unknown command " + word;
        }

        return Format("ok %u written", count);
    }

    if (word == "find") {
        std::vector<uint8_t> bytes;

        if (!(in >> arg) || arg.size() < 8 || arg.size() % 2)
            return "error: find HEX (at least four bytes)";

        for (size_t i = 0; i < arg.size(); i += 2) {
            char* end = nullptr;
            const std::string pair = arg.substr(i, 2);
            bytes.push_back(uint8_t(std::strtoul(pair.c_str(), &end, 16)));

            if (*end)
                return "error: bad hex " + pair;
        }

        const uint32_t first = uint32_t(bytes[0]) << 24 | uint32_t(bytes[1]) << 16 | uint32_t(bytes[2]) << 8 | bytes[3];
        std::string text = "ok";
        uint32_t found = 0;

        for (const auto [start, end] : {std::pair{0x80000000u, 0x81800000u}, std::pair{0x90000000u, 0x94000000u}}) {
            for (uint32_t a = start; a + bytes.size() <= end && found < 16; a += 4) {
                if (Memory::Read32(a) != first)
                    continue;

                size_t k = 4;

                while (k < bytes.size() && Memory::Read8(a + uint32_t(k)) == bytes[k])
                    ++k;

                if (k == bytes.size()) {
                    text += Format(" %08x", a);
                    ++found;
                }
            }
        }

        return text;
    }

    if (word == "wait" || word == "steps") {
        double amount = 0;
        in >> amount;
        s.busy = true;
        s.until = word == "wait" ? GameStep::Elapsed() + amount : 0;
        s.steps = word == "steps" ? uint32_t(amount) : 0;
        return "";
    }

    if (word == "press") {
        static const std::map<std::string, uint16_t> names{
            {"a", MH3_CL_A}, {"b", MH3_CL_B}, {"x", MH3_CL_X}, {"y", MH3_CL_Y}, {"l", MH3_CL_L}, {"r", MH3_CL_R}, {"zl", MH3_CL_ZL}, {"zr", MH3_CL_ZR},
            {"plus", MH3_CL_PLUS}, {"minus", MH3_CL_MINUS}, {"home", MH3_CL_HOME}, {"up", MH3_CL_UP}, {"down", MH3_CL_DOWN}, {"left", MH3_CL_LEFT},
            {"right", MH3_CL_RIGHT},
        };

        double ticks = 3;

        if (!(in >> arg))
            return "error: press BUTTONS [TICKS [LX LY [RX RY]]]";

        in >> ticks;
        uint16_t buttons = 0;
        std::istringstream parts(arg);

        for (std::string part; std::getline(parts, part, '+');) {
            char* end = nullptr;
            const unsigned long bits = std::strtoul(part.c_str(), &end, 16);

            if (const auto name = names.find(part); name != names.end())
                buttons |= name->second;
            else if (part == "none")
                continue;
            else if (!part.empty() && !*end && bits <= 0xFFFF)
                buttons |= uint16_t(bits);
            else
                return "error: no button " + part;
        }

        int sticks[4] = {};

        for (int& axis : sticks) {
            if (!(in >> axis))
                break;

            if (axis < -512 || axis > 511)
                return "error: stick values are -512..511";
        }

        SetControllerTestSample(ClassicControllerSample{true, buttons, int16_t(sticks[0]), int16_t(sticks[1]), int16_t(sticks[2]), int16_t(sticks[3])});
        s.busy = true;
        s.release = true;
        s.until = GameStep::Elapsed() + ticks;
        s.steps = 0;
        return "";
    }

    if (word == "hunter") {
        const uint32_t plw = Cheats::LocalPlayer();

        if (!plw)
            return "error: no hunter";

        if (!(in >> arg)) {
            const uint32_t chr = Memory::Read32(plw + kHunterChar);
            return Format("ok hunter %08x weapon %u stance %u action %u.%u state %u timer %d motion %u charge %d level %u", plw, Memory::Read8(plw + kHunterWeapon),
                          Memory::Read8(plw + kHunterStance), Memory::Read8(plw + kHunterGroup), Memory::Read16(plw + kHunterAction), Memory::Read8(plw + kHunterState),
                          int32_t(Memory::Read32(plw + kHunterTimer)), chr ? Memory::Read16(chr + 0x54) : 0u, int16_t(Memory::Read16(plw + kHunterChargeSteps)),
                          Memory::Read8(plw + kHunterChargeLevel));
        }

        unsigned group = 0, action = 0, flags = 0;

        if (arg == "act" && in >> group >> action) {
            in >> std::hex >> flags;

            if (group > 0xFF || action > 0xFFFF || flags > 0xFFFF)
                return "error: hunter act GROUP ACTION [FLAGS]";

            ctx->gpr[3] = plw;
            ctx->gpr[4] = group;
            ctx->gpr[5] = action;
            ctx->gpr[6] = flags;
            InvokeIndirectCpu(kFieldNextAction, ctx);
            return Format("ok %u.%u", group, action);
        }

        if (arg == "charge" && in >> action && action <= 0xFF) {
            Memory::Write8(plw + kHunterChargeLevel, uint8_t(action));
            return "ok";
        }

        return "error: hunter [act GROUP ACTION [FLAGS] | charge LEVEL]";
    }

    if (word == "point") {
        if (in >> arg && arg == "off") {
            SetPointerTestSample(std::nullopt);
            return "ok";
        }

        double ticks = 30;
        const float x = float(std::strtod(arg.c_str(), nullptr));
        float y = -1;

        if (!(in >> y) || x < 0 || x > 1 || y < 0 || y > 1)
            return "error: point X Y [TICKS] (0..1 across and down the picture) or point off";

        in >> ticks;
        SetPointerTestSample(std::make_pair(x, y));
        s.busy = true;
        s.until = GameStep::Elapsed() + ticks;
        s.steps = 0;
        return "";
    }

    if (word == "mouse") {
        float dx = 0, dy = 0;
        uint32_t frames = 1;

        if (!(in >> dx >> dy))
            return "error: mouse DX DY [FRAMES] (pixels of captured mouse motion, spread over FRAMES paced frames)";

        in >> frames;
        AddMouseMotion(dx, dy, frames);
        return "ok";
    }

    if (word == "shot") {
        if (!(in >> arg))
            return "error: shot PATH";

        RequestFrameDump(arg);
        return "ok " + arg;
    }

    if (word == "call") {
        if (!(in >> arg) || !Hex(arg, address))
            return "error: call ADDR [INT...] [f FLOAT...]";

        if (!Ram(address, 4) || address & 3)
            return "error: no code at " + arg;

        uint32_t r = 3, f = 1;
        bool floats = false;

        while (in >> arg) {
            if (arg == "f") {
                floats = true;
                continue;
            }

            if (floats && f <= 8)
                ctx->fpr[f++].d = std::strtod(arg.c_str(), nullptr);
            else if (!floats && r <= 10 && Hex(arg, value))
                ctx->gpr[r++] = value;
            else
                return "error: bad argument " + arg;
        }

        InvokeIndirectCpu(address, ctx);
        return Format("ok r3 %08x f1 %g", uint32_t(ctx->gpr[3]), ctx->fpr[1].d);
    }

    if (word == "hide") {
        std::string names;
        in >> names;
        HideNativeMaterials(names.c_str());
        return "ok";
    }

    if (word == "cheat") {
        std::string rest;
        std::getline(in >> std::ws, rest);
        const auto equals = rest.find('=');
        const std::string name = rest.substr(0, equals), choice = equals == std::string::npos ? "" : rest.substr(equals + 1);
        return LauncherCheats::EnableByName(name, choice) ? "ok" : "error: no cheat or value '" + rest + "'";
    }

    if (word == "trace") {
        std::string state;

        if (!(in >> arg)) {
            std::string text = "ok";

            for (const std::string_view item : kTraceItems)
                if (const auto* on = TraceFlag::Switch(item))
                    text += " " + std::string(item) + (on->load() ? "=on" : "=off");

            return text;
        }

        auto* on = TraceFlag::Switch(arg);

        if (!on || !(in >> state) || (state != "on" && state != "off"))
            return "error: trace [ITEM on|off] (`trace` lists the items)";

        on->store(state == "on");
        return "ok";
    }

    if (word == "movie") {
        if (!(in >> arg) || arg != "skip")
            return "error: movie skip";

        return SkipMovie() ? "ok" : "error: no movie is open";
    }

    if (word == "online")
        return OnlineAuto::Command(ctx, in);

    return "error: unknown command " + word;
}

// Whether the running command is done this step.
bool Done(Session& s) {
    if (s.steps)
        --s.steps;

    return !s.steps && GameStep::Elapsed() >= s.until;
}

} // namespace

namespace DebugPort {

void Step(CpuContext* ctx) {
    static const bool active = Start();

    if (!active)
        return;

    static Session s;
    ++s.step;
    {
        std::lock_guard guard(lock);

        for (Line& line : inbox)
            s.queue.push_back(std::move(line));

        inbox.clear();
    }

    if (s.busy) {
        if (!Done(s))
            return;

        s.busy = false;

        if (s.release)
            SetControllerTestSample(std::nullopt);

        s.release = false;
        Answer(s.client, "ok");
    }

    while (!s.queue.empty() && !s.busy) {
        const Line line = std::move(s.queue.front());
        s.queue.pop_front();
        std::string answer;

        try {
            answer = Run(ctx, s, line);
        } catch (const std::exception& e) {
            answer = std::string("error: ") + e.what();
        }

        if (answer.empty())
            s.client = line.client;
        else
            Answer(line.client, std::move(answer));
    }
}

} // namespace DebugPort
