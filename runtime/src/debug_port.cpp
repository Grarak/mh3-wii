// The debug port (development builds, docs/debug-port.md). A listener thread
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
//   pokef ADDR VALUE...       floats
//   wait TICKS                answer TICKS ticks of game time later (30 a s)
//   steps N                   answer N logic steps later
//   shot PATH                 dump the next frame to PATH (BMP)
//   call ADDR [INT...] [f FLOAT...]  call a guest function (ints hex, in r3..,
//                             floats in f1..); answers r3 and f1
//   cheat NAME[=VALUE]        turn on an integrated cheat
//   trace [ITEM on|off]       the MH3_TRACE items and which are on, or turn
//                             one on or off
//   movie skip                end the open movie (as at its last frame)
//   press BUTTONS [TICKS]     hold Classic buttons (a+b, ..., or hex bits) for
//                             TICKS ticks (3), then give the controller back
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
        return "ok help state peek peekh peekb peekf peeks poke pokeh pokeb pokef wait steps press shot call cheat trace movie online (docs/debug-port.md)";

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
            return "error: press BUTTONS [TICKS]";

        in >> ticks;
        uint16_t buttons = 0;
        std::istringstream parts(arg);

        for (std::string part; std::getline(parts, part, '+');) {
            char* end = nullptr;
            const unsigned long bits = std::strtoul(part.c_str(), &end, 16);

            if (const auto name = names.find(part); name != names.end())
                buttons |= name->second;
            else if (!part.empty() && !*end && bits <= 0xFFFF)
                buttons |= uint16_t(bits);
            else
                return "error: no button " + part;
        }

        SetControllerTestSample(ClassicControllerSample{true, buttons});
        s.busy = true;
        s.release = true;
        s.until = GameStep::Elapsed() + ticks;
        s.steps = 0;
        return "";
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
