// The native launcher. It runs on the Vita and, in
// development builds, on Linux hosts (MH3 --launcher), which have no system
// dialogs: messages draw as a panel there and the quest name keeps its value.
#include "vita_launcher.h"
#include "fatal.h"
#include "launcher_font.h"
#include "launcher_screen.h"
#include "launcher_cheats.h"
#include "village_boot.h"
#include "field_jump.h"
#include "disc_image.h"
#include "asset_cache.h"
#include "fs_status.h"
#include "widescreen.h"
#include "upscaling.h"
#include "frame_limit.h"
#include "msaa.h"
#include "ssaa.h"
#include "language.h"
#include "../src/net/online.h"
#include <aurora/aurora.h>
#include <aurora/event.h>
#include <aurora/thread_role.hpp>
#ifdef __vita__
#include <psp2/ctrl.h>
#include <psp2/io/devctl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/message_dialog.h>
#include <psp2/ime_dialog.h>
#include <psp2/vshbridge.h>
#else
#include "controller_host.h"
#include "../native/mh3_classic_map.h"
#include <SDL3/SDL.h>
#include <chrono>
#include <cmath>
#endif
#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#endif
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#ifdef MH3_DEVELOPMENT
#include <sstream>
#endif

namespace VitaLauncher {
namespace {
constexpr uint32_t ink = 0xff28302c, paper = 0xffc8d7d8, gold = 0xff73b9de, light = 0xffe1ebed, faded = 0xff707a78;

// The menu's buttons, with the values of the Vita's SCE_CTRL bits.
namespace Button {
constexpr uint32_t Select = 0x1, Start = 0x8, Up = 0x10, Right = 0x20, Down = 0x40, Left = 0x80, L2 = 0x100, R2 = 0x200, L1 = 0x400, R1 = 0x800, Triangle = 0x1000,
                   Circle = 0x2000, Cross = 0x4000, Square = 0x8000;
// sceCtrlPeekBufferPositive2 reports the handheld's L/R as L1/R1; a PS TV
// controller has both pairs.
constexpr uint32_t L = L1 | L2, R = R1 | R2, Directions = Up | Down | Left | Right;
} // namespace Button
#ifdef __vita__
static_assert(Button::Select == SCE_CTRL_SELECT && Button::Start == SCE_CTRL_START && Button::Up == SCE_CTRL_UP && Button::Right == SCE_CTRL_RIGHT &&
              Button::Down == SCE_CTRL_DOWN && Button::Left == SCE_CTRL_LEFT && Button::L2 == SCE_CTRL_LTRIGGER && Button::R2 == SCE_CTRL_RTRIGGER &&
              Button::L1 == SCE_CTRL_L1 && Button::R1 == SCE_CTRL_R1 && Button::Triangle == SCE_CTRL_TRIANGLE && Button::Circle == SCE_CTRL_CIRCLE &&
              Button::Cross == SCE_CTRL_CROSS && Button::Square == SCE_CTRL_SQUARE);
uint64_t Microseconds() {
    return sceKernelGetProcessTimeWide();
}

void Wait(unsigned microseconds) {
    sceKernelDelayThread(microseconds);
}
#else
uint64_t Microseconds() {
    return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

void Wait(unsigned microseconds) {
    std::this_thread::sleep_for(std::chrono::microseconds(microseconds));
}
#endif
// Development log lines (the TTY).
[[gnu::format(printf, 1, 2)]] void Log([[maybe_unused]] const char* format, ...) {
#ifdef MH3_DEVELOPMENT
    va_list arguments;
    va_start(arguments, format);
    std::fputs("MH3 launcher: ", stderr);
    std::vfprintf(stderr, format, arguments);
    std::fputc('\n', stderr);
    va_end(arguments);
#endif
}
#ifdef MH3_DEVELOPMENT
// MH3_LAUNCHER_INPUT=file: scripted launcher input for tests. Each line
// "FRAME BUTTONS" holds BUTTONS from launcher input frame FRAME on: names
// joined by '+' (up down left right cross circle triangle square start select
// l r), "none", or SCE_CTRL bits as a number. Frames count the menu's input
// reads. While a script runs, the dialogs close after 2.5 seconds; after its
// last line the controller takes over.
class Script {
  public:
    Script() {
        const char* path = std::getenv("MH3_LAUNCHER_INPUT");

        if (!path)
            return;

        std::ifstream input(path);

        if (!input)
            MH3_THROW(std::runtime_error(std::string("Cannot read MH3_LAUNCHER_INPUT ") + path));

        for (std::string line; std::getline(input, line);) {
            std::istringstream fields(line.substr(0, line.find('#')));
            uint64_t frame;
            std::string names;

            if (!(fields >> frame))
                continue;

            if (!(fields >> names) || (!entries.empty() && frame < entries.back().first))
                MH3_THROW(std::runtime_error("Invalid MH3_LAUNCHER_INPUT line: " + line));

            entries.emplace_back(frame, Buttons(names));
        }

        Log("input script %s, %u entries", path, unsigned(entries.size()));
    }

    bool Running() const { return !finished && !entries.empty(); }
    // The held buttons of this frame, or nothing once the script has ended.
    std::optional<uint32_t> Held(uint64_t frame) {
        if (finished || entries.empty())
            return std::nullopt;
        while (next < entries.size() && entries[next].first <= frame)
            held = entries[next++].second;

        if (next == entries.size()) {
            finished = true;
            Log("input script finished at frame %llu", (unsigned long long)frame);
        }

        return held;
    }

  private:
    static uint32_t Buttons(const std::string& text) {
        if (text == "none")
            return 0;

        if (std::isdigit(uint8_t(text[0])))
            return uint32_t(std::stoul(text, nullptr, 0));

        static const std::pair<const char*, uint32_t> named[] = {
            {"up", Button::Up},       {"down", Button::Down},     {"left", Button::Left},         {"right", Button::Right},
            {"cross", Button::Cross}, {"circle", Button::Circle}, {"triangle", Button::Triangle}, {"square", Button::Square},
            {"start", Button::Start}, {"select", Button::Select}, {"l", Button::L2},              {"r", Button::R2}};
        uint32_t value = 0;

        for (size_t at = 0; at <= text.size();) {
            const size_t end = std::min(text.find('+', at), text.size());
            const auto name = text.substr(at, end - at);
            const auto found = std::find_if(std::begin(named), std::end(named), [&](const auto& n) { return name == n.first; });

            if (found == std::end(named))
                MH3_THROW(std::runtime_error("Unknown MH3_LAUNCHER_INPUT button " + name));

            value |= found->second;
            at = end + 1;
        }

        return value;
    }

    std::vector<std::pair<uint64_t, uint32_t>> entries;
    size_t next = 0;
    uint32_t held = 0;
    bool finished = false;
};

Script& InputScript() {
    static Script script;
    return script;
}

bool ScriptRunning() {
    return InputScript().Running();
}
#else
bool ScriptRunning() {
    return false;
}
#endif
bool Pump() {
    for (const auto* e = aurora_update(); e && e->type != AURORA_NONE; ++e)
        if (e->type == AURORA_EXIT)
            return false;
    return true;
}

void Rect(std::vector<Rectangle>& list, float x, float y, float w, float h, uint32_t color) {
    list.push_back({x, y, w, h, color});
}

// The controls as menu buttons; the left stick adds directions.
uint32_t LiveButtons() {
#ifdef __vita__
    // Port 1 first, then the handheld's port 0, as the game's input reads them
    // (SDL's Vita driver): a PS TV controller.
    SceCtrlData sample{};

    if (sceCtrlPeekBufferPositive2(1, &sample, 1) < 0 && sceCtrlPeekBufferPositive2(0, &sample, 1) < 0)
        return 0;

    uint32_t held = sample.buttons;

    if (sample.ly < 80)
        held |= Button::Up;

    if (sample.ly > 176)
        held |= Button::Down;

    if (sample.lx < 80)
        held |= Button::Left;

    if (sample.lx > 176)
        held |= Button::Right;

    return held;
#else
    // The host's controls as the game's Classic Controller (docs/vita-controls.md),
    // its buttons by what they do in the game's menus: A (Enter, F) selects as
    // Cross, B (Space, Backspace) goes back as Circle, X and Y (E) edit as
    // Triangle. Plus (Esc) is left out: Esc confirming would surprise.
    const auto pad = ReadLiveController();
    static constexpr std::pair<uint16_t, uint32_t> map[] = {
        {MH3_CL_UP, Button::Up},      {MH3_CL_DOWN, Button::Down},    {MH3_CL_LEFT, Button::Left},  {MH3_CL_RIGHT, Button::Right},
        {MH3_CL_A, Button::Cross},    {MH3_CL_B, Button::Circle},     {MH3_CL_X, Button::Triangle}, {MH3_CL_Y, Button::Triangle},
        {MH3_CL_MINUS, Button::Select}, {MH3_CL_L, Button::L2},       {MH3_CL_R, Button::R2}};
    uint32_t held = 0;

    for (const auto& [classic, button] : map)
        if (pad.buttons & classic)
            held |= button;
    if (pad.leftY > 200)
        held |= Button::Up;

    if (pad.leftY < -200)
        held |= Button::Down;

    if (pad.leftX < -200)
        held |= Button::Left;

    if (pad.leftX > 200)
        held |= Button::Right;

    return held;
#endif
}
#ifndef __vita__
// The mouse while the menus run (hosts): an SDL event watch collects its
// moves, clicks and wheel on the launcher's screen, and a click no longer
// captures it for the camera (SetMouseLook).
struct MouseEvents {
    bool moved = false, clicked = false, back = false;

    // The last position on the screen (ScreenWidth x ScreenHeight), -1 off
    // the picture.
    float x = -1, y = -1;

    // Wheel notches, up positive.
    float wheel = 0;
};

std::mutex mouseLock;
MouseEvents mouseEvents;

// A window point in SDL's coordinates on the launcher's screen.
bool OnScreen(SDL_WindowID id, float x, float y, float& screenX, float& screenY) {
    SDL_Window* window = SDL_GetWindowFromID(id);
    const float density = window ? SDL_GetWindowPixelDensity(window) : 1.f;
    float pictureX, pictureY;

    if (!PointOnPicture(x * density, y * density, pictureX, pictureY))
        return false;

    screenX = pictureX * ScreenWidth;
    screenY = pictureY * ScreenHeight;
    return true;
}

bool SDLCALL WatchMouse(void*, SDL_Event* event) {
    std::lock_guard lock(mouseLock);
    auto& mouse = mouseEvents;

    if (event->type == SDL_EVENT_MOUSE_MOTION) {
        if (!OnScreen(event->motion.windowID, event->motion.x, event->motion.y, mouse.x, mouse.y))
            mouse.x = mouse.y = -1;

        mouse.moved = true;
    } else if (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN && event->button.button == SDL_BUTTON_LEFT) {
        if (OnScreen(event->button.windowID, event->button.x, event->button.y, mouse.x, mouse.y))
            mouse.clicked = true;
    } else if (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN && event->button.button == SDL_BUTTON_RIGHT)
        mouse.back = true;
    else if (event->type == SDL_EVENT_MOUSE_WHEEL)
        mouse.wheel += event->wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -event->wheel.y : event->wheel.y;

    return true;
}

// The events since the last call; whole wheel notches only (a touchpad's
// fractions add up).
MouseEvents TakeMouse() {
    std::lock_guard lock(mouseLock);
    MouseEvents taken = mouseEvents;
    taken.wheel = std::trunc(mouseEvents.wheel);
    mouseEvents.wheel -= taken.wheel;
    mouseEvents.moved = mouseEvents.clicked = mouseEvents.back = false;
    return taken;
}

// The menus' hold on the mouse, from the screen's start to the hand-off.
struct MouseScope {
    MouseScope() {
        TakeMouse();
        SDL_AddEventWatch(WatchMouse, nullptr);
        SetMouseLook(false);
    }

    ~MouseScope() {
        SDL_RemoveEventWatch(WatchMouse, nullptr);
        SetMouseLook(true);
    }
};
#endif

// The last launcher frame; dialogs draw over it.
std::vector<Rectangle> shown;

// For the messages of development hosts, which draw them themselves.
[[maybe_unused]] Font* messageFont = nullptr;
#ifdef __vita__
// System dialogs draw into the display buffer the render thread presents:
// Aurora's GXM present calls sceCommonDialogUpdate while one is active. The
// render thread owns that flag, and a dialog must not end while a present
// could still update it, so both changes run on the render thread.
template <class Status, class Close>
void RunDialog(Status status, Close close) {
    RunOnRenderThread([] { aurora_set_common_dialog_active(true); }, &shown);
    const uint64_t opened = Microseconds();
    bool closing = false;

    while (status() != SCE_COMMON_DIALOG_STATUS_FINISHED && Pump()) {
        if (!closing && ScriptRunning() && Microseconds() - opened > 2500000) {
            Log("dialog closed by the input script");
            close();
            closing = true;
        }

        PresentScreen(&shown);
        Wait(8000);
    }

    RunOnRenderThread([] { aurora_set_common_dialog_active(false); }, &shown);
}

void Message(const std::string& text) {
    SceMsgDialogUserMessageParam user{};
    user.buttonType = SCE_MSG_DIALOG_BUTTON_TYPE_OK;
    user.msg = reinterpret_cast<const SceChar8*>(text.c_str());
    SceMsgDialogParam param{};
    sceMsgDialogParamInit(&param);
    param.mode = SCE_MSG_DIALOG_MODE_USER_MSG;
    param.userMsgParam = &user;
    Log("message: %s", text.c_str());

    if (sceMsgDialogInit(&param) < 0) {
        std::fprintf(stderr, "MH3 launcher: %s\n", text.c_str());
        return;
    }

    RunDialog(sceMsgDialogGetStatus, sceMsgDialogClose);
    sceMsgDialogTerm();
}

// The system keyboard (SceImeDialog, part of SceCommonDialog like the
// message): a name of up to `length` characters, `what` ("quest name") in
// its title and log lines.
Status TypeName(std::string& name, const std::string& what, size_t length) {
    std::array<SceWChar16, 44> initial{}, output{}, title{};
    length = std::min(length, initial.size() - 1);

    for (size_t i = 0; i < std::min(name.size(), length); ++i)
        initial[i] = uint8_t(name[i]);

    for (size_t i = 0; i < std::min(what.size(), title.size() - 1); ++i)
        title[i] = uint8_t(i == 0 ? std::toupper(static_cast<unsigned char>(what[i])) : what[i]);

    std::string key = what;
    std::replace(key.begin(), key.end(), ' ', '-');

    SceImeDialogParam param{};
    sceImeDialogParamInit(&param);
    param.type = SCE_IME_TYPE_BASIC_LATIN;
    param.dialogMode = SCE_IME_DIALOG_DIALOG_MODE_WITH_CANCEL;
    param.title = title.data();
    param.maxTextLength = length;
    param.initialText = initial.data();
    param.inputTextBuffer = output.data();

    if (const int error = sceImeDialogInit(&param); error < 0)
        return Status::Error("Cannot open the " + key + " keyboard (error " + std::to_string(error) + ")");

    Log("%s keyboard open", key.c_str());
    RunDialog(sceImeDialogGetStatus, sceImeDialogAbort);
    SceImeDialogResult result{};
    sceImeDialogGetResult(&result);
    sceImeDialogTerm();

    if (result.button != SCE_IME_DIALOG_BUTTON_ENTER) {
        Log("%s keyboard closed without a name", key.c_str());
        return {};
    }

    std::string value;

    for (auto c : output) {
        if (!c)
            break;

        if (c < 32 || c > 126)
            return Status::Error("Use printable Latin characters for " + what + "s");

        value += char(c);
    }

    // Leading and trailing spaces would make names that look the same.
    value.erase(0, value.find_first_not_of(' '));
    value.erase(value.find_last_not_of(' ') + 1);

    if (!value.empty())
        name = std::move(value);

    Log("%s: %s", what.c_str(), name.c_str());
    return {};
}
#else
// A panel over the last menu frame; Cross, Circle or Start closes it.
void Message(const std::string& text) {
    Log("message: %s", text.c_str());

    if (!messageFont) {
        std::fprintf(stderr, "MH3 launcher: %s\n", text.c_str());
        return;
    }

    auto frame = shown;
    const auto lines = messageFont->Wrap(text, 600);
    const float height = 90 + 26 * lines.size(), top = std::max(40.f, (ScreenHeight - height) / 2);
    Rect(frame, 140, top - 6, 680, height + 12, gold);
    Rect(frame, 146, top, 668, height, paper);

    for (size_t i = 0; i < lines.size(); ++i)
        messageFont->Text(frame, 180, top + 24 + 26 * i, lines[i], ink, 600);

    Rect(frame, 180, top + height - 52, 600, 2, ink);
    messageFont->Text(frame, 180, top + height - 42, "Enter OK", ink, 600);
    const uint64_t opened = Microseconds();

    // Buttons held when it opens do not close it.
    uint32_t previous = LiveButtons();

    while (Pump()) {
        PresentScreen(&frame);
        Wait(8000);

        if (ScriptRunning()) {
            if (Microseconds() - opened > 2500000) {
                Log("dialog closed by the input script");
                break;
            }

            continue;
        }

        const uint32_t held = LiveButtons();

        // Or either mouse button.
        if (const auto mouse = TakeMouse(); mouse.clicked || mouse.back)
            break;

        if (held & ~previous & (Button::Cross | Button::Circle | Button::Start))
            break;

        previous = held;
    }
}

#ifdef __EMSCRIPTEN__
// The browser's text prompt, on the page's thread (which it holds until the
// prompt closes): a name of up to `length` printable Latin characters.
Status TypeName(std::string& name, const std::string& what, size_t length) {
    const std::string title = "The " + what + " (at most " + std::to_string(length) + " characters)";
    std::array<char, 256> typed{};
    const int entered = MAIN_THREAD_EM_ASM_INT(
        {
            const text = prompt(UTF8ToString($0), UTF8ToString($1));

            if (text === null)
                return 0;

            stringToUTF8(text, $2, $3);
            return 1;
        },
        title.c_str(), name.c_str(), typed.data(), typed.size());

    std::string key = what;
    std::replace(key.begin(), key.end(), ' ', '-');

    if (!entered) {
        Log("%s prompt closed without a name", key.c_str());
        return {};
    }

    std::string value = typed.data();

    for (char c : value)
        if (c < 32 || c > 126)
            return Status::Error("Use printable Latin characters for " + what + "s");

    // Leading and trailing spaces would make names that look the same.
    value.erase(0, value.find_first_not_of(' '));
    value.erase(value.find_last_not_of(' ') + 1);

    if (value.size() > length)
        return Status::Error("Use at most " + std::to_string(length) + " characters for " + what + "s");

    if (!value.empty())
        name = std::move(value);

    Log("%s: %s", what.c_str(), name.c_str());
    return {};
}
#else
// No system keyboard on a development host: the name stays as it is.
Status TypeName(const std::string& name, std::string what, size_t) {
    std::replace(what.begin(), what.end(), ' ', '-');
    Log("%s keyboard: the Vita's system keyboard; the host keeps \"%s\"", what.c_str(), name.c_str());
    return {};
}
#endif
#endif
#ifdef __EMSCRIPTEN__
// The web's quest creator: a form over the page (runtime/web/quest-form.js)
// with every option of a quest from scratch, for the mouse and the keyboard.
// It opens with the quest and the choices as JSON and answers with the
// quest's settings as "key=value" lines.
namespace QuestForm {
std::string Quoted(std::string_view text) {
    std::string quoted = "\"";

    for (const char c : text) {
        if (c == '"' || c == '\\')
            quoted += '\\';

        if (static_cast<unsigned char>(c) >= 32)
            quoted += c;
    }

    return quoted + "\"";
}

// The maps with their monsters, the monsters with their names, which stay
// alone and which can be captured, and each map's intruders (from the
// disc's quests: read once).
Status Choices(const std::function<std::string(uint16_t)>& monsterName, std::string& json) {
    static std::string cached;

    if (!cached.empty()) {
        json = cached;
        return {};
    }

    std::string maps, monsters, intruders;
    std::vector<uint8_t> all;

    for (const uint8_t map : ScratchMaps()) {
        std::string list;

        for (const uint8_t monster : ScratchMonsters(map)) {
            list += (list.empty() ? "" : ",") + std::to_string(monster);

            if (std::find(all.begin(), all.end(), monster) == all.end())
                all.push_back(monster);
        }

        std::vector<IntruderChoice> choices;
        MH3_RETURN_IF_ERROR(ScratchIntruders(map, choices));
        std::string found;

        for (const auto& choice : choices)
            found += std::string(found.empty() ? "" : ",") + "{\"chance\":" + std::to_string(choice.intruder.chance) + ",\"table\":" + std::to_string(choice.intruder.table) +
                     ",\"when\":" + std::to_string(choice.intruder.when) + ",\"group\":" + std::to_string(choice.intruder.group) + ",\"quest\":" + Quoted(choice.quest) + "}";

        maps += std::string(maps.empty() ? "" : ",") + "{\"id\":" + std::to_string(map) + ",\"name\":" + Quoted(QuestCatalog::MapName(map)) +
                ",\"capture\":" + (map >= 1 && map <= 5 ? "true" : "false") + ",\"monsters\":[" + list + "],\"intruders\":[" + found + "]}";
    }

    for (const uint8_t monster : all) {
        CustomQuest probe;
        probe.map = 1;
        probe.monsters = {monster, 0, 0};
        bool capture = false;
        MH3_RETURN_IF_ERROR(ScratchCapture(probe, capture));
        monsters += std::string(monsters.empty() ? "" : ",") + "{\"id\":" + std::to_string(monster) + ",\"name\":" + Quoted(monsterName(monster)) +
                    ",\"alone\":" + (ScratchAlone(monster) ? "true" : "false") + ",\"capture\":" + (capture ? "true" : "false") + "}";
    }

    cached = "\"maps\":[" + maps + "],\"monsters\":[" + monsters + "]";
    json = cached;
    return {};
}

std::string Quest(const CustomQuest& q) {
    const auto list = [](const std::array<uint8_t, 3>& values) { return std::to_string(values[0]) + "," + std::to_string(values[1]) + "," + std::to_string(values[2]); };
    return "{\"name\":" + Quoted(q.name) + ",\"map\":" + std::to_string(q.map) + ",\"monsters\":[" + list(q.monsters) + "],\"sizes\":[" + list(q.sizes) +
           "],\"rank\":" + std::to_string(q.rank) + ",\"stars\":" + std::to_string(q.stars) + ",\"minutes\":" + std::to_string(q.minutes) +
           ",\"capture\":" + (q.capture ? "true" : "false") + ",\"arrival\":" + std::to_string(unsigned(q.arrival)) + ",\"intruder\":{\"chance\":" +
           std::to_string(q.intruder.chance) + ",\"table\":" + std::to_string(q.intruder.table) + ",\"when\":" + std::to_string(q.intruder.when) +
           ",\"group\":" + std::to_string(q.intruder.group) + "},\"patch\":" + Quoted(q.patch) + "}";
}

void Open(const std::string& title, const std::string& choices, const CustomQuest& q) {
    const std::string json = "{\"title\":" + Quoted(title) + "," + choices + ",\"quest\":" + Quest(q) + "}";
    MAIN_THREAD_EM_ASM({ mh3QuestForm.open(UTF8ToString($0)); }, json.c_str());
}

// Open: nullopt; closed without saving: an empty string; saved: its lines.
std::optional<std::string> Poll() {
    const int length = MAIN_THREAD_EM_ASM_INT({ return mh3QuestForm.poll(); });

    if (length < 0)
        return std::nullopt;

    std::string lines(size_t(length) + 1, '\0');
    MAIN_THREAD_EM_ASM({ stringToUTF8(mh3QuestForm.result(), $0, $1); }, lines.data(), lines.size());
    lines.resize(size_t(length));
    return lines;
}

// The form stays open with the reason.
void Fail(const std::string& message) {
    MAIN_THREAD_EM_ASM({ mh3QuestForm.fail(UTF8ToString($0)); }, message.c_str());
}

void Close() {
    MAIN_THREAD_EM_ASM({ mh3QuestForm.close(); });
}

// The form's lines over q: name (empty: none typed), map, monsters, sizes,
// rank, stars, minutes, capture, arrival, intruder, patch.
Status Read(std::string_view lines, CustomQuest& q) {
    const auto numbers = [](std::string_view text, std::vector<unsigned>& values) {
        values.clear();

        for (size_t at = 0; at <= text.size();) {
            const size_t end = std::min(text.find(',', at), text.size());
            const std::string part(text.substr(at, end - at));

            if (part.empty() || part.size() > 3 || !std::all_of(part.begin(), part.end(), [](char c) { return c >= '0' && c <= '9'; }))
                return false;

            values.push_back(unsigned(std::stoul(part)));
            at = end + 1;
        }

        return std::all_of(values.begin(), values.end(), [](unsigned v) { return v <= 255; });
    };

    const Status invalid = Status::Error("The quest creator sent settings this build cannot read");
    std::vector<unsigned> values;

    while (!lines.empty()) {
        const size_t end = std::min(lines.find('\n'), lines.size());
        const std::string_view line = lines.substr(0, end);
        lines.remove_prefix(std::min(end + 1, lines.size()));
        const size_t equals = line.find('=');

        if (equals == std::string_view::npos)
            return invalid;

        const std::string_view key = line.substr(0, equals), value = line.substr(equals + 1);

        if (key == "name") {
            q.name = std::string(value);
            continue;
        }

        if (key == "patch") {
            q.patch = std::string(value);
            continue;
        }

        if (!numbers(value, values))
            return invalid;

        const size_t want = key == "monsters" || key == "sizes" ? 3 : key == "intruder" ? 4 : 1;

        if (values.size() != want)
            return invalid;

        if (key == "map")
            q.map = uint8_t(values[0]);
        else if (key == "monsters")
            q.monsters = {uint8_t(values[0]), uint8_t(values[1]), uint8_t(values[2])};
        else if (key == "sizes")
            q.sizes = {uint8_t(values[0]), uint8_t(values[1]), uint8_t(values[2])};
        else if (key == "rank")
            q.rank = uint8_t(values[0]);
        else if (key == "stars")
            q.stars = uint8_t(values[0]);
        else if (key == "minutes")
            q.minutes = uint16_t(values[0]);
        else if (key == "capture")
            q.capture = values[0] != 0;
        else if (key == "arrival")
            q.arrival = Arrival(values[0]);
        else if (key == "intruder")
            q.intruder = {uint8_t(values[0]), uint8_t(values[1]), uint8_t(values[2]), uint8_t(values[3])};
        else
            return invalid;
    }

    return {};
}
} // namespace QuestForm
#endif
class Input {
  public:
    uint32_t Read() {
        const uint64_t index = frame++;
        uint32_t held;
#ifdef MH3_DEVELOPMENT
        if (const auto scripted = InputScript().Held(index))
            held = *scripted;
        else
#endif
            held = LiveButtons();
        const uint64_t now = Microseconds();
        uint32_t edges = held & ~previous;
        const uint32_t directions = held & Button::Directions;

        if (directions) {
            if (directions != lastDirection) {
                nextRepeat = now + 350000;
                lastDirection = directions;
            } else if (now >= nextRepeat) {
                edges |= directions;
                nextRepeat = now + 90000;
            }
        } else
            lastDirection = 0;

        previous = held;

        if (edges)
            Log("input frame %llu buttons 0x%x", (unsigned long long)index, unsigned(edges));

        return edges;
    }

  private:
    uint32_t previous = 0, lastDirection = 0;
    uint64_t nextRepeat = 0, frame = 0;
};

// The parchment page and its copper border.
void Frame(std::vector<Rectangle>& rects) {
    rects.clear();
    Rect(rects, 0, 0, 960, 544, ink);
    Rect(rects, 30, 30, 900, 475, paper);

    // Authored geometric borders echo MH3's framed parchment quest menus.
    // Game fonts/art remain external; the launcher uses installed PVF.
    for (int x = 34; x < 926; x += 24) {
        Rect(rects, x, 34, 12, 4, gold);
        Rect(rects, x, 497, 12, 4, gold);
    }
}

// The rows' layout: the first visible row's top, the space between rows,
// the rows' left edge and width.
constexpr size_t VisibleRows = 7;
constexpr float RowTop = 110, RowPitch = 43, RowLeft = 54, RowWidth = 848;
#ifndef __vita__
// The selected row's value arrows (a row that takes Left and Right): the
// left one's left edge, where the right one starts.
constexpr float ArrowsLeft = 826, ArrowsMiddle = 864;

struct FooterPart {
    std::string text;
    uint32_t button; // 0: not a button
};

// A footer's parts (three spaces apart, each a button and what it does),
// with the host's keys for the Vita's buttons (LiveButtons).
std::vector<FooterPart> FooterParts(std::string_view footer) {
    static constexpr struct {
        std::string_view vita, host;
        uint32_t button;
    } keys[] = {{"X ", "Enter ", Button::Cross}, {"O ", "Space ", Button::Circle}, {"Triangle ", "E ", Button::Triangle}, {"D-pad ", "Arrows ", 0}, {"L / R ", "Q / Shift ", 0}};
    std::vector<FooterPart> parts;

    for (size_t at = 0; at < footer.size();) {
        const size_t end = std::min(footer.find("   ", at), footer.size());
        std::string_view part = footer.substr(at, end - at);
        FooterPart host{std::string(part), 0};

        for (const auto& key : keys)
            if (part.starts_with(key.vita)) {
                host = {std::string(key.host) + std::string(part.substr(key.vita.size())), key.button};
                break;
            }

        parts.push_back(std::move(host));
        at = end + 3;
    }

    return parts;
}

// The footer's buttons as drawn: where each starts and ends.
struct FooterSpan {
    float left, right;
    uint32_t button;
};

std::vector<FooterSpan> footerSpans;

// The visible row at a point of the screen, or `count` for none.
size_t RowAt(float x, float y, size_t first, size_t count) {
    const float top = RowTop - 2;

    if (x < RowLeft || x > RowLeft + RowWidth || y < top)
        return count;

    const size_t slot = size_t((y - top) / RowPitch);

    if (slot >= VisibleRows || y - top - slot * RowPitch > 36 || first + slot >= count)
        return count;

    return first + slot;
}

// The footer button at a point of the screen, or 0.
uint32_t FooterButtonAt(float x, float y) {
    if (y < 505 || y > 540)
        return 0;

    for (const auto& span : footerSpans)
        if (x >= span.left && x <= span.right)
            return span.button;

    return 0;
}
#endif
// `first` is the first visible row: on the Vita the selection is the last
// visible row once the list scrolls; hosts scroll only to keep it in view,
// so the row under the mouse stays put. `values`: the selected row takes
// Left and Right (hosts draw arrows on it for the mouse).
void Draw(Font& font, const std::string& title, const std::vector<std::string>& rows, size_t selected, size_t& first, const std::string& detail, const std::string& footer,
          [[maybe_unused]] bool values) {
    auto& rects = shown;
    Frame(rects);
    Rect(rects, 42, 46, 876, 48, ink);
    std::string position;

    if (rows.size() > VisibleRows)
        position = std::to_string(selected + 1) + " / " + std::to_string(rows.size());

    const float positionWidth = position.empty() ? 0 : font.Width(position) + 24;
    font.Text(rects, 60, 58, "MONSTER HUNTER TRI  /  " + title, gold, 840 - positionWidth);

    if (!position.empty())
        font.Text(rects, 900 - font.Width(position), 58, position, light, positionWidth);

#ifdef __vita__
    first = selected >= VisibleRows ? selected - (VisibleRows - 1) : 0;
#else
    first = std::min(std::clamp(first, selected >= VisibleRows ? selected - (VisibleRows - 1) : 0, selected), rows.size() > VisibleRows ? rows.size() - VisibleRows : 0);
#endif

    for (size_t row = first; row < std::min(first + VisibleRows, rows.size()); ++row) {
        const float y = RowTop + (row - first) * RowPitch;
        float width = 810;

        if (row == selected) {
            Rect(rects, RowLeft, y - 2, RowWidth, 36, ink);
            Rect(rects, RowLeft, y - 2, 6, 36, gold);
#ifndef __vita__
            if (values) {
                font.Text(rects, ArrowsLeft + 10, y + 4, "<", gold, 20);
                font.Text(rects, ArrowsMiddle + 10, y + 4, ">", gold, 20);
                width = ArrowsLeft - 72 - 10;
            }
#endif
        }

        font.Text(rects, 72, y + 4, rows[row], row == selected ? gold : ink, width);
    }

    // More rows above or below the visible ones.
    if (first)
        Rect(rects, 466, 100, 28, 3, faded);

    if (first + VisibleRows < rows.size())
        Rect(rects, 466, 411, 28, 3, faded);

    Rect(rects, 54, 421, 848, 2, ink);
    const auto lines = font.Wrap(detail, 820);

    for (size_t i = 0; i < std::min<size_t>(lines.size(), 2); ++i)
        font.Text(rects, 64, 430 + 28 * i, lines[i], ink, 820);

#ifdef __vita__
    font.Text(rects, 45, 513, footer, light, 890);
#else
    // Each part on its own, its button clickable (FooterButtonAt).
    footerSpans.clear();
    float x = 45;

    for (const auto& part : FooterParts(footer)) {
        const float width = font.Width(part.text);
        font.Text(rects, x, 513, part.text, light, std::max(0.f, 935 - x));

        if (part.button)
            footerSpans.push_back({x, x + width, part.button});

        x += width + font.Width("   ");
    }
#endif
    PresentScreen(&rects);
    Wait(8000);
}

// The disc image stays in base; its metadata and the converted-asset cache live
// in base/disc. A development install of a prepared disc directly in base is used as is.
Status EnsureDisc(Font& font, const std::filesystem::path& base) {
    auto output = base;
    std::filesystem::path iso;

    // The check reads the image's file table (one validation of the
    // metadata against the image); steps report no fraction, so the screen
    // shows the step, with dots while it runs.
    unsigned frames = 0;
    const auto screen = [&](const std::string& label) {
        auto& rects = shown;
        Frame(rects);
        Rect(rects, 42, 46, 876, 48, ink);
        font.Text(rects, 60, 58, "MONSTER HUNTER TRI  /  Preparing your game", gold, 840);
        font.Text(rects, 150, 203, iso.filename().string(), ink, 660);
        font.Text(rects, 150, 243, label + std::string(frames++ / 20 % 4, '.'), ink, 660);
        font.Text(rects, 45, 513, "Keep the application open", light, 890);
        PresentScreen(&rects);
    };

    std::error_code listing;
    std::filesystem::directory_iterator entries(base, listing);

    if (listing)
        return FsStatus::Failure("directory iterator cannot open directory", listing, base);

    for (; entries != std::filesystem::directory_iterator(); entries.increment(listing)) {
        const auto& entry = *entries;
        std::error_code type;
        const bool regular = entry.is_regular_file(type);

        if (type)
            return FsStatus::Failure("status", type, entry.path());

        if (regular && (entry.path().extension() == ".iso" || entry.path().extension() == ".rvz" || entry.path().extension() == ".wia")) {
            if (!iso.empty())
                return Status::Error("There is more than one disc image in " + base.string() + ". Leave only your European Monster Hunter Tri (RMHP08) ISO, RVZ or WIA there.");

            iso = entry.path();
        }
    }

    if (listing)
        return Status::Error("filesystem error: directory iterator cannot advance: " + listing.message());

    if (!DiscReady(output))
        output = base / "disc";

    if (output != base) {
        screen("Checking game data");
        Pump();
    }

    // A development install prepared in base is used as is; disc/ when it
    // was prepared from this image.
    if (output != base && (!DiscReady(output) || (!iso.empty() && !DiscPreparedFrom(output, iso)))) {
        if (iso.empty())
            return Status::Error("Game data is missing. Copy your European Monster Hunter Tri (RMHP08) disc image (ISO, RVZ or WIA) to " + base.string() +
                                 ", then start Monster Hunter Tri again.");
        std::mutex mutex;
        std::string status = "Checking game data", error;
        std::atomic<bool> finished = false;
        std::jthread worker([&] {
            aurora::pin_current_thread(aurora::ThreadRole::Worker);
            const Progress progress = [&](const std::string& s, uint64_t, uint64_t) {
                std::lock_guard guard(mutex);
                status = s;
            };

            const Status processed = ProcessDisc(iso, output, progress);

            if (!processed.ok()) {
                std::lock_guard guard(mutex);
                error = processed.message();
            }

            finished.store(true, std::memory_order_release);
        });

        while (!finished.load(std::memory_order_acquire)) {
            std::string label;
            {
                std::lock_guard guard(mutex);
                label = status;
            }

            screen(label);
            Pump();
            Wait(12000);
        }

        worker.join();

        if (!error.empty())
            return Status::Error("The disc image could not be read: " + error);

        if (!DiscReady(output))
            return Status::Error("Prepared game data did not pass validation");
    }

    setenv("MH3_DISC_DIR", output.string().c_str(), 1);
    Log("game data in %s", output.string().c_str());

    // The game's native assets open at the hand-off (StartGame); textures and
    // models convert as the game loads them (disc/cache/).
    return {};
}

// Hands the renderer over to the game. The last launcher frame stays on the
// display while the game boots; the game's native graphics startup restarts
// the render thread at its own size.
void StartGame(Font& font, const std::string& what) {
    Frame(shown);
    const std::string text = "Starting Monster Hunter Tri";
    font.Text(shown, (960 - font.Width(text)) / 2, 250, text, ink, 840);

    // Packets are newest-wins: the render thread presents this one before it stops.
    PresentScreen(&shown);
    Log("starting the game (%s) after %llu launcher frames", what.c_str(), (unsigned long long)ScreenFrames());
    HandOffScreen();
}

enum class Page {
    Main,
    Categories,
    Saves,
    Quests,
    Custom,
    Editor,
    Cheats,
    CheatList,
    Online
};

const char* PageName(Page page) {
    constexpr const char* names[]{"Main", "Quests", "Saves", "Quest list", "Custom quests", "Editor", "Cheats", "Cheat list", "Online"};
    return names[unsigned(page)];
}

const char* RankName(uint8_t rank) {
    return rank == 0 ? "Village" : rank == 1 ? "Low rank" : "High rank";
}

const char* OnOff(bool on) {
    return on ? "ON" : "OFF";
}

// The main menu's rows. The web build connects nowhere: it leaves out the
// City and the online settings (the online quests, played offline, stay).
// Its WebGPU renderer supersamples instead of multisampling.
enum class Item { Start, Village, City, Quests, Create, Cheats, Widescreen, Upscaling, FrameLimit, Msaa, Ssaa, Language, Online };

constexpr Item MainItems[]{Item::Start,
                           Item::Village,
#ifndef __EMSCRIPTEN__
                           Item::City,
#endif
                           Item::Quests,
                           Item::Create,
                           Item::Cheats,
                           Item::Widescreen,
                           Item::Upscaling,
                           Item::FrameLimit,
#ifdef __EMSCRIPTEN__
                           Item::Ssaa,
#else
                           Item::Msaa,
#endif
                           Item::Language,
#ifndef __EMSCRIPTEN__
                           Item::Online
#endif
};

size_t MainRow(Item item) {
    return size_t(std::find(std::begin(MainItems), std::end(MainItems), item) - std::begin(MainItems));
}

// The Framelimit row's choices, in order: the Vita's VBlank modes and off;
// the web adds caps at common PC rates and a typed one (the last).
constexpr FrameLimit CustomFrameCap{FrameLimitMode::Cap, 0};
#ifdef __EMSCRIPTEN__
constexpr FrameLimit FrameLimits[]{{FrameLimitMode::VSync},     {FrameLimitMode::HalfVSync}, {FrameLimitMode::Cap, 30},  {FrameLimitMode::Cap, 60},
                                   {FrameLimitMode::Cap, 120},  {FrameLimitMode::Cap, 144},  {FrameLimitMode::Cap, 165}, {FrameLimitMode::Cap, 240},
                                   {FrameLimitMode::Off},       CustomFrameCap};
#else
constexpr FrameLimit FrameLimits[]{{FrameLimitMode::VSync}, {FrameLimitMode::HalfVSync}, {FrameLimitMode::Off}};
#endif

// A limit's choice: the typed cap's for a cap among none of them (the first
// where there is none).
size_t FrameLimitChoice(FrameLimit limit) {
    const auto found = std::find(std::begin(FrameLimits), std::end(FrameLimits), limit);

    if (found != std::end(FrameLimits))
        return size_t(found - std::begin(FrameLimits));

    return limit.mode == FrameLimitMode::Cap && std::end(FrameLimits)[-1] == CustomFrameCap ? std::size(FrameLimits) - 1 : 0;
}

// The cheats of a group, in table order.
std::vector<size_t> GroupCheats(std::string_view group) {
    std::vector<size_t> list;
    const auto cheats = LauncherCheats::List();

    for (size_t i = 0; i < cheats.size(); ++i)
        if (group == cheats[i].group)
            list.push_back(i);
    return list;
}

// " Active: ..." for the main menu: the cheats' names, or how many.
std::string ActiveCheats() {
    const size_t count = LauncherCheats::EnabledCount();

    if (count == 0)
        return "";

    if (count > 2)
        return " Active: " + std::to_string(count) + " cheats.";

    std::string names;
    const auto cheats = LauncherCheats::List();

    for (size_t i = 0; i < cheats.size(); ++i)
        if (LauncherCheats::Enabled(i))
            names += (names.empty() ? "" : ", ") + std::string(cheats[i].name);
    return " Active: " + names + ".";
}

// The menu's settings, kept as "key=value" lines in settings.txt beside the
// disc data (ux0:data/mh3-wii): read before the menu shows, written each time
// one changes. Unknown keys and values that do not parse are left out.
const char* const SettingsFile = "settings.txt";

void LoadSettings(const std::filesystem::path& base) {
    std::ifstream file(base / SettingsFile);

    for (std::string line; std::getline(file, line);) {
        const size_t equals = line.find('=');

        if (equals == std::string::npos)
            continue;

        const std::string key = line.substr(0, equals), value = line.substr(equals + 1);

        if (key == "online_server") {
            if (CheckOnlineServer(value).empty())
                SetOnlineServerSetting(value);

            continue;
        }

        if (key == "language") {
            for (const auto& language : GameLanguages())
                if (value == language.code)
                    SetLanguageSetting(language.system);

            continue;
        }

        // vsync, half, off or cap:N (60 and 30 from before the caps).
        if (key == "framelimit") {
            if (const auto limit = ParseFrameLimit(value))
                SetFrameLimitSetting(*limit);

            continue;
        }

        // Saved as off, 2x or 4x (on from before the 2x mode: 4x).
        if (key == "msaa") {
            SetMsaaSetting(value == "2x" ? 2 : value == "4x" || value == "on" ? 4 : 1);
            continue;
        }

        if (key == "ssaa") {
            SetSsaaSetting(value == "2x" ? 2 : value == "4x" ? 4 : 1);
            continue;
        }

        if (value != "on" && value != "off")
            continue;

        const bool on = value == "on";

        if (key == "widescreen")
            SetWidescreenSetting(on);
        else if (key == "upscaling")
            SetUpscalingSetting(on);
        else if (key == "online_skip_tls")
            SetSkipTlsSetting(on);
    }
}

// Through a temporary file: a write cut short keeps the previous settings.
Status SaveSettings(const std::filesystem::path& base) {
    const auto path = base / SettingsFile, temporary = base / (std::string(SettingsFile) + ".tmp");
    const char* language = "en";

    for (const auto& known : GameLanguages())
        if (LanguageSetting() == known.system)
            language = known.code;

    {
        std::ofstream file(temporary, std::ios::trunc);
        const auto onOff = [](bool on) { return on ? "on" : "off"; };
        file << "widescreen=" << onOff(WidescreenSetting().value_or(true)) << "\nupscaling=" << onOff(UpscalingSetting()) << "\nframelimit=" << FrameLimitText(FrameLimitSetting())
#ifdef __EMSCRIPTEN__
             << "\nssaa=" << (SsaaSetting() == 1 ? "off" : SsaaSetting() == 2 ? "2x" : "4x")
#else
             << "\nmsaa=" << (MsaaSetting() == 1 ? "off" : MsaaSetting() == 2 ? "2x" : "4x")
#endif
             << "\nlanguage=" << language << "\nonline_server=" << OnlineServerSetting() << "\nonline_skip_tls=" << onOff(SkipTlsSetting()) << "\n";
        file.close();

        if (!file)
            return Status::Error("Cannot write the settings to " + temporary.string());
    }

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    return FsStatus::Rename(temporary, path);
}

// The menus, until the game starts (started) or the user exits. A failure
// ends them; an action's failure only shows its message. After the hand-off
// the launcher no longer draws: nothing fails from there on.
Status Menu(std::unique_ptr<Font>& font, bool& started) {
    StartScreen();
#ifndef __vita__
    MouseScope mouseScope;
#endif

    // The empty page: messages before the menu (kubridge, game data) draw over it.
    Frame(shown);
#ifdef __vita__
    SceCommonDialogConfigParam config{};
    sceCommonDialogConfigParamInit(&config);
    sceCommonDialogSetConfigParam(&config);
    const uint32_t search[2]{};

    if (_vshKernelSearchModuleByName("kubridge", search) < 0) {
        Message("Kubridge is not installed. Install kubridge.skprx version 0.3.1 or newer (https://github.com/bythos14/kubridge/releases) under *KERNEL in your taiHEN config.txt, then restart your PS Vita before launching Monster Hunter Tri.");
        return {};
    }

    // Core 3 (the draw thread, the disc read-ahead) needs CapUnlocker; the
    // module check is enough (azahar's citra_vita_main.cpp does the same).
    if (_vshKernelSearchModuleByName("CapUnlocker", search) < 0) {
        Message("CapUnlocker is not installed. Install CapUnlocker.skprx (https://github.com/GrapheneCt/CapUnlocker) under *KERNEL in your taiHEN config.txt, then restart your PS Vita before launching Monster Hunter Tri.");
        return {};
    }

    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    sceCtrlSetSamplingModeExt(SCE_CTRL_MODE_ANALOG);
#endif
#ifdef MH3_DEVELOPMENT
    InputScript(); // a script that cannot run stops the process
#endif
    MH3_RETURN_IF_ERROR(Font::Open(font));
    messageFont = font.get();
    const char* discDirectory = std::getenv("MH3_DISC_DIR");

    if (!discDirectory)
        return Status::Error("MH3_DISC_DIR is not set");

    const auto base = std::filesystem::path(discDirectory);
    MH3_RETURN_IF_ERROR(FsStatus::CreateDirectories(base));
    MH3_RETURN_IF_ERROR(EnsureDisc(*font, base));

    // The asset cache (converted packs and sound stream copies) wants
    // AssetCache::StorageWanted of the storage, counting what it holds.
    if (const std::string warning = AssetCache::StorageWarning(base); !warning.empty())
        Message(warning);

    LoadSettings(base);
    const char* userDirectory = std::getenv("MH3_USER_DIR");
    const auto user = std::filesystem::path(userDirectory ? userDirectory : "saves");
    Page page = Page::Main;
    size_t selected = 0, first = 0;

    // Where the hunter selection leads: Quests, or a start in the village or the City.
    enum class Then { Quests, Village, City } then = Then::Quests;
    size_t cheatGroup = 0;
    std::vector<size_t> groupCheats;

    // The game reads it once, at RenderInit, after the hand-off.
    bool widescreen = WidescreenSetting().value_or(true);

    // The game's graphics startup applies them, after the hand-off.
    bool upscaling = UpscalingSetting();
    FrameLimit frameLimit = FrameLimitSetting();
    uint32_t msaa = MsaaSetting(), ssaa = SsaaSetting();

    // The web's typed cap, while another limit is chosen.
    uint32_t customFps = FrameLimitChoice(frameLimit) == std::size(FrameLimits) - 1 && frameLimit.mode == FrameLimitMode::Cap ? frameLimit.fps : 90;
    std::string onlineServer = OnlineServerSetting();
    bool skipTls = SkipTlsSetting();

    // The game reads it at startup; English unless the settings name another.
    const auto languages = GameLanguages();
    size_t language = 0;

    for (size_t i = 0; i < languages.size(); ++i)
        if (LanguageSetting() == languages[i].system)
            language = i;

    SetLanguageSetting(languages[language].system);
    SaveSlot save{};
    std::vector<SaveSlot> saves;
    std::vector<QuestCatalog::Quest> quests;
    std::vector<CustomQuest> custom;
    CustomQuest edit;

    // The editor returns to the list it came from.
    Page editorParent = Page::Main;
    QuestCatalog::Set set = QuestCatalog::Set::Village;
    const auto monsters = QuestCatalog::MonsterNames();
    const auto monsterName = [&](uint16_t id) { return id < monsters.size() && !monsters[id].empty() ? monsters[id] : "monster " + std::to_string(id); };

    // A quest from scratch's goal ("Hunt Rathalos, Rathian and Barroth";
    // "Hunt Tiny Uragaan" at 80% or below, "Giant" at 120% or above), also
    // its name until one is typed (at most 43 characters).
    const auto goal = [&](const CustomQuest& q) {
        const size_t count = q.monsters[2] ? 3 : q.monsters[1] ? 2 : 1;
        std::string text = q.capture ? "Capture " : "Hunt ";

        for (size_t i = 0; i < count; ++i) {
            const uint8_t size = q.sizes[i];
            text += (i == 0 ? "" : i + 1 == count ? " and " : ", ") + std::string(size <= 80 ? "Tiny " : size >= 120 ? "Giant " : "") + monsterName(q.monsters[i]);
        }

        return text;
    };

    // "120% size, " for one size of all, "80/100% sizes, " for several.
    const auto sizes = [](const CustomQuest& q) {
        const size_t count = q.monsters[2] ? 3 : q.monsters[1] ? 2 : 1;

        if (std::all_of(q.sizes.begin(), q.sizes.begin() + count, [&](uint8_t size) { return size == q.sizes[0]; }))
            return q.sizes[0] == 100 ? std::string() : std::to_string(q.sizes[0]) + "% size, ";

        std::string text;

        for (size_t i = 0; i < count; ++i)
            text += (i ? "/" : "") + std::to_string(q.sizes[i]);

        return text + "% sizes, ";
    };

    const auto automaticName = [&](const CustomQuest& q) { return goal(q).substr(0, 43); };
    std::array<std::optional<std::vector<QuestCatalog::Quest>>, 3> catalogs;
    const auto catalog = [&](QuestCatalog::Set s) -> const std::vector<QuestCatalog::Quest>& {
        auto& loaded = catalogs[unsigned(s)];

        if (!loaded)
            loaded = QuestCatalog::Load(s);

        return *loaded;
    };

    const auto findQuest = [&](QuestCatalog::Set s, uint16_t id) -> const QuestCatalog::Quest* {
        const auto& list = catalog(s);
        const auto found = std::find_if(list.begin(), list.end(), [&](const auto& q) { return q.id == id; });
        return found == list.end() ? nullptr : &*found;
    };

    Input input;
    const auto enter = [&](Page next) {
        page = next;
        selected = first = 0;
        Log("page %s", PageName(next));
    };

    const auto saveName = [&] { return "File " + std::to_string(save.file + 1) + " / Character " + std::to_string(save.character + 1); };
    const auto start = [&](const std::string& what) {
        started = true;
        StartGame(*font, what);
    };

    // The Village and City shortcuts, with the chosen hunter.
    const auto startMode = [&] {
        const bool city = then == Then::City;
        StartVillageBoot(save.file, save.character, city);
        start(std::string(city ? "City" : "village") + ", " + saveName());
    };

#ifdef __EMSCRIPTEN__
    // The web's creator (QuestForm): every option in a form over the page.
    // The launcher shows what is open behind it; a quest the game cannot take
    // keeps the form open with the reason. An empty name follows the goal.
    const auto editInForm = [&]() -> Status {
        std::string choices;
        MH3_RETURN_IF_ERROR(QuestForm::Choices(monsterName, choices));
        const std::string title = edit.storageKey.empty() ? "Create custom quest" : "Edit custom quest";
        QuestForm::Open(title, choices, edit);
        Log("quest form open");
        const std::vector<std::string> rows{"The quest creator is open over the page"};

        while (Pump()) {
            Draw(*font, title, rows, 0, first, "Save the quest there, or close it with Cancel or Esc to come back here.", "", false);
            const auto lines = QuestForm::Poll();

            if (!lines)
                continue;

            if (lines->empty()) {
                Log("quest form closed");
                return {};
            }

            CustomQuest candidate = edit;

            if (const Status read = QuestForm::Read(*lines, candidate); !read.ok()) {
                QuestForm::Fail(read.message());
                continue;
            }

            SetScratchCarrier(candidate);

            if (candidate.name.empty())
                candidate.name = automaticName(candidate);

            if (const Status saved = SaveCustomQuest(user, candidate); !saved.ok()) {
                Log("quest form: %s", saved.message().c_str());
                QuestForm::Fail(saved.message());
                continue;
            }

            QuestForm::Close();
            edit = candidate;
            Log("custom quest saved: %s (map %u, monsters %u %u %u, arrival %u, intruder %u%%)", edit.name.c_str(), unsigned(edit.map), unsigned(edit.monsters[0]),
                unsigned(edit.monsters[1]), unsigned(edit.monsters[2]), unsigned(edit.arrival), unsigned(edit.intruder.chance));
            Message("Custom quest saved. Play it from Quests > Custom quests.");
            return {};
        }

        return {};
    };
#endif

    Log("menu ready");

    while (Pump()) {
#ifdef __EMSCRIPTEN__
        if (page == Page::Editor) {
            if (const Status edited = editInForm(); !edited.ok())
                Message(edited.message());

            if (editorParent == Page::Custom) {
                MH3_RETURN_IF_ERROR(LoadCustomQuests(user, custom));
                enter(Page::Custom);
            } else {
                enter(Page::Main);
                selected = MainRow(Item::Create);
            }

            continue;
        }
#endif
        std::string title, detail, footer = "X Select   O Back   D-pad Move";
        std::vector<std::string> rows;

        // The selected row takes Left and Right.
        bool values = false;

        switch (page) {
        case Page::Main: {
            title = "Main menu";
            const std::string cheats = ActiveCheats();
            const auto samples = [](uint32_t n) { return n == 1 ? "OFF" : n == 2 ? "2x" : "4x"; };

            // An item's row and what it does.
            const auto describe = [&](Item item) -> std::pair<std::string, std::string> {
                switch (item) {
                case Item::Start:
                    return {"Start game", "Continue through the game's title and character selection." + cheats};
                case Item::Village:
                    return {"Village", "Straight into Moga Village with an existing hunter, past the title." + cheats};
                case Item::City:
                    return {"City (online)", "Straight to the City's network screen with an existing hunter: online play on " + onlineServer + "." +
                                                 (LauncherCheats::EnabledCount() ? " Online play is blocked while cheats are on." : "") + cheats};
                case Item::Quests:
                    return {"Quests", "Start a quest directly with an existing hunter." + cheats};
                case Item::Create:
#ifdef __EMSCRIPTEN__
                    return {"Create custom quest", "Make a quest of your own in a form: a map, up to three large monsters and their sizes, how they arrive, an intruder."};
#else
                    return {"Create custom quest", "Make a hunt or capture quest of your own: a map and one or two large monsters."};
#endif
                case Item::Cheats:
                    return {"Cheats", "Gameplay toggles for this launch." + cheats};
                case Item::Widescreen:
                    return {std::string("Widescreen: ") + (widescreen ? "ON (16:9)" : "OFF (4:3)"),
                            widescreen ? "The game's own 16:9 mode on the whole display." : "4:3, with bars at the sides of the display."};
                case Item::Upscaling:
#ifdef __vita__
                    return {std::string("Upscaling: ") + (upscaling ? "ON (544 lines)" : "OFF (448 lines)"),
#else
                    return {std::string("Upscaling: ") + (upscaling ? "ON" : "OFF (448 lines)"),
#endif
                            upscaling ? "Renders at the display's full resolution." : "Renders the Wii's 448 lines and scales them to the display: less GPU work."};
                case Item::FrameLimit: {
                    const std::string fps = std::to_string(frameLimit.fps) + " FPS";
#ifdef __EMSCRIPTEN__
                    // The browser shows frames at the display's refresh rate.
                    switch (frameLimit.mode) {
                    case FrameLimitMode::VSync:
                        return {"Framelimit: VSync", "A frame for each refresh of the display (60, 120, 144 Hz...): every frame is shown, motion is even."};
                    case FrameLimitMode::HalfVSync:
                        return {"Framelimit: Half VSync", "A frame for every second refresh of the display: steadier motion where the game cannot keep up with it."};
                    case FrameLimitMode::Cap:
                        if (FrameLimits[FrameLimitChoice(frameLimit)] == CustomFrameCap)
                            return {"Framelimit: Custom (" + fps + ")", "At most this many frames a second (Enter types it); the display shows the newest at each refresh."};

                        return {"Framelimit: " + fps, "At most " + fps + "; the display shows the newest frame at each refresh, so off its rate motion can judder."};
                    case FrameLimitMode::Off:
                        return {"Framelimit: Unlimited", "As many frames as the game makes; the display shows the newest at each refresh, and motion can judder."};
                    }
#else
                    switch (frameLimit.mode) {
                    case FrameLimitMode::VSync:
                        return {"Framelimit: 60 FPS", "60 fps: the game waits for each frame to be drawn before the next, and every frame is shown."};
                    case FrameLimitMode::HalfVSync:
                        return {"Framelimit: 30 FPS", "30 fps: every frame stays on screen for two refreshes: steadier motion where the game cannot hold 60."};
                    case FrameLimitMode::Cap:
                        return {"Framelimit: " + fps, "At most " + fps + " (MH3_FRAME_LIMIT); frames the display outruns are dropped."};
                    case FrameLimitMode::Off:
                        return {"Framelimit: OFF", "The game runs ahead of drawing; frames it outruns are dropped and motion can judder."};
                    }
#endif
                    return {};
                }
                case Item::Msaa:
                    return {std::string("MSAA: ") + samples(msaa),
                            msaa == 1   ? "No antialiasing. MSAA smooths edges but can reduce performance: about 5% fewer fps at 2x and 15% at 4x in big fights."
                            : msaa == 2 ? "2x multisample antialiasing: smoother edges. Can reduce performance: about 5% fewer fps in big fights (4x: about 15%)."
                                        : "4x multisample antialiasing: the smoothest edges. Can reduce performance: about 15% fewer fps in big fights (2x: about 5%)."};
                case Item::Ssaa:
                    return {std::string("SSAA: ") + samples(ssaa),
                            ssaa == 1   ? "No antialiasing. SSAA renders more pixels and filters them down: smoother edges and textures, at 2x or 4x the GPU work."
                            : ssaa == 2 ? "2x supersampling: 1.41 times the width and height, filtered down. About twice the GPU work."
                                        : "4x supersampling: twice the width and height, filtered down: the smoothest picture. About four times the GPU work."};
                case Item::Language:
                    return {std::string("Language: ") + languages[language].name, "The game's text, menus and keyboard: English, French, German, Italian or Spanish."};
                case Item::Online:
                    return {"Online settings: " + onlineServer + (skipTls ? " (no TLS)" : ""), "The server the game's City mode connects to."};
                }

                return {};
            };

            for (const Item item : MainItems)
                rows.push_back(describe(item).first);

            const Item item = MainItems[std::min(selected, std::size(MainItems) - 1)];
            detail = describe(item).second;
            values = item == Item::FrameLimit || item == Item::Msaa || item == Item::Ssaa || item == Item::Language;
            footer = "X Select   D-pad Move";
            break;
        }
        case Page::Categories: {
            title = "Quests";
            rows = {"Offline quests", "Online quests", "Arena", "Custom quests"};
            const char* details[] = {"Moga Village quests.", "Online quests, played offline.", "Arena quests.", "Your custom quests."};
            detail = std::string(details[std::min<size_t>(selected, 3)]) + "  Hunter: " + saveName() + ".";
            break;
        }
        case Page::Saves:
            title = "Choose your hunter";

            for (const auto& s : saves)
                rows.push_back("File " + std::to_string(s.file + 1) + " / Character " + std::to_string(s.character + 1));

            detail = "Characters of valid saves.";
            break;
        case Page::Quests:
            title = "Quest list";

            for (const auto& q : quests)
                rows.push_back(std::to_string(q.stars) + "*  " + q.name);

            if (!quests.empty()) {
                const auto& q = quests[std::min(selected, quests.size() - 1)];
                detail = QuestCatalog::MapName(q.map) + ": " + q.goal;
            }

            footer = "X Select   O Back   D-pad Move   L / R Page";
            break;
        case Page::Custom:
            title = "Custom quests";

            for (const auto& q : custom)
                rows.push_back(q.name);

            if (custom.empty()) {
                detail = "No custom quests yet. Make one with Create custom quest in the main menu.";
                footer = "O Back";
            } else {
                const auto& q = custom[std::min(selected, custom.size() - 1)];

                if (q.map) {
                    detail = QuestCatalog::MapName(q.map) + ": " + goal(q) + ", " + RankName(q.rank) + ", " + sizes(q) +
                             std::to_string(q.stars) + "*" +
                             (q.minutes ? ", " + std::to_string(q.minutes) + " minutes" : "") + ".";
                    footer = "X Launch   Triangle Edit   O Back   D-pad Move";
                    break;
                }

                const auto* base = findQuest(q.set, q.base);
                const auto* encounter = findQuest(q.set, q.encounter ? q.encounter : q.base);

                if (base && encounter && !encounter->objectives.empty())
                    detail = QuestCatalog::MapName(base->map) + ": " + (q.capture ? "Capture " : "Hunt ") + std::to_string(q.count) + " " +
                             monsterName(encounter->objectives.front().target) + ", " + std::to_string(q.stars) + "*" +
                             (q.minutes ? ", " + std::to_string(q.minutes) + " minutes" : "") + ".";
                else
                    detail = "Its template is not on this disc.";

                footer = "X Launch   Triangle Edit   O Back   D-pad Move";
            }

            break;
        case Page::Editor: {
            title = edit.storageKey.empty() ? "Create custom quest" : "Edit custom quest";
            rows = {"Name: " + edit.name,
                    "Map: " + QuestCatalog::MapName(edit.map),
                    "Monster: " + monsterName(edit.monsters[0]),
                    "Second monster: " + (edit.monsters[1] ? monsterName(edit.monsters[1]) : std::string("None")),
                    "Third monster: " + (edit.monsters[2] ? monsterName(edit.monsters[2]) : std::string("None")),
                    "Objective: " + std::string(edit.capture ? "Capture" : "Hunt"),
                    "Rank: " + std::string(RankName(edit.rank)),
                    "Size: " + std::to_string(edit.sizes[0]) + "%",
                    "Time: " + (edit.minutes ? std::to_string(edit.minutes) + " minutes" : "as the map's quest"),
                    "Stars: " + std::to_string(edit.stars),
                    "Save quest"};
            const char* details[] = {
#ifdef __vita__
                "X opens the keyboard.",
#elif defined(__EMSCRIPTEN__)
                "Enter opens the browser's text prompt.",
#else
                "The keyboard is the Vita's; the host keeps the name.",
#endif
                "Any field map, the arenas or the Sacred Pinnacle. Jhen Mohran's Great Desert and Ceadeus's lair take only their own monster.",
                "Any large monster but Ceadeus and Jhen Mohran, who need their own maps. The Water Arena takes the swimmers.",
                "A second large monster at the same time, or none. Ceadeus, Alatreon and Jhen Mohran are always alone.",
                "A third large monster at the same time (after a second), or none.",
                "Capture needs monsters the game lets you capture, on a field map.",
                "The monsters' strength and rewards come from the game's quests of this rank, nearest the stars.",
                "Every monster's size, 50-200% (the game's quests: about 90-120%; event quests go further).",
                "1 to 50 minutes.",
                "The quest's stars; at a rank they also pick the monsters' strength.",
                edit.storageKey.empty() ? "Saves a new custom quest." : "Saves the changes to this custom quest."};
            const size_t row = std::min<size_t>(selected, 10);
            detail = details[row];
            values = row >= 1 && row <= 9;
            footer = row == 0 ? "X Keyboard   O Back   D-pad Move" : row == 10 ? "X Save   O Back   D-pad Move" : "Left / Right Change   O Back   D-pad Move";
            break;
        }
        case Page::Cheats: {
            title = "Cheats";
            const auto groups = LauncherCheats::Groups();

            for (const char* group : groups) {
                const auto list = GroupCheats(group);
                const size_t on = std::count_if(list.begin(), list.end(), [](size_t i) { return LauncherCheats::Enabled(i); });
                rows.push_back(std::string(group) + "  (" + std::to_string(list.size()) + " codes" + (on ? ", " + std::to_string(on) + " on" : "") + ")");
            }

            if (selected == 0)
                detail = "The launcher's own toggles. For this launch.";
            else
                detail = "Codes from the " + std::string(groups[std::min(selected, groups.size() - 1)]) + " Gecko list, ported to this disc. For this launch.";

            break;
        }
        case Page::Online:
            title = "Online";
            rows = {"Server: " + onlineServer, std::string("Skip TLS handshake: ") + OnOff(skipTls)};

            if (selected == 0) {
                detail = std::string("A domain such as ") + kDefaultOnlineServer + " (the community server) or a local MH3SP's IP address. Add :port for an account server off port 80.";
                footer = "X Keyboard   Triangle Default   O Back   D-pad Move";
            } else {
                detail = skipTls ? "TLS sessions run unencrypted: for a server whose OPN has UseSSL = OFF. The community server needs TLS."
                                 : "TLS as on the Wii, verified against the MH3SP certificate authority.";
                footer = "X Toggle   O Back   D-pad Move";
            }

            break;
        case Page::CheatList: {
            const auto cheats = LauncherCheats::List();
            title = std::string("Cheats: ") + LauncherCheats::Groups()[cheatGroup];

            for (size_t i : groupCheats) {
                const std::string value = LauncherCheats::ValueLabel(i);
                rows.push_back(std::string(cheats[i].name) + ": " + OnOff(LauncherCheats::Enabled(i)) + (value.empty() ? "" : "  [" + value + "]"));
            }

            const size_t cheat = groupCheats[std::min(selected, groupCheats.size() - 1)];
            detail = cheats[cheat].detail;
            values = LauncherCheats::Choices(cheat) || LauncherCheats::TextLength(cheat);
            footer = LauncherCheats::Choices(cheat) ? "X Toggle   Left / Right Value   O Back   L / R Page"
                     : LauncherCheats::TextLength(cheat) ? "X Toggle   Left / Right Type the name   O Back   L / R Page"
                                                         : "X Toggle   O Back   D-pad Move   L / R Page";
            break;
        }
        }

        if (rows.empty())
            rows = {"No quests available"};

        selected = std::min(selected, rows.size() - 1);
        Draw(*font, title, rows, selected, first, detail, footer, values);
        uint32_t buttons = input.Read();
#ifndef __vita__
        // The mouse: the row under it is selected as it moves, a click
        // accepts it (on the selected row's arrows: changes its value) or
        // presses a footer button, the right button goes back and the wheel
        // moves through the rows.
        const auto mouse = TakeMouse();
        const size_t pointed = RowAt(mouse.x, mouse.y, first, rows.size());

        if (mouse.clicked && pointed == selected && values && mouse.x >= ArrowsLeft)
            buttons |= mouse.x < ArrowsMiddle ? Button::Left : Button::Right;
        else if (mouse.clicked && pointed < rows.size()) {
            selected = pointed;
            buttons |= Button::Cross;
        } else if (mouse.clicked)
            buttons |= FooterButtonAt(mouse.x, mouse.y);
        else if (mouse.moved && pointed < rows.size())
            selected = pointed;

        if (mouse.back)
            buttons |= Button::Circle;

        if (mouse.wheel)
            selected = size_t(std::clamp<long>(long(selected) - long(mouse.wheel), 0, long(rows.size()) - 1));
#endif

        if (buttons & Button::Up)
            selected = (selected + rows.size() - 1) % rows.size();

        if (buttons & Button::Down)
            selected = (selected + 1) % rows.size();

        if (buttons & Button::L)
            selected = selected > VisibleRows ? selected - VisibleRows : 0;

        if (buttons & Button::R)
            selected = std::min(selected + VisibleRows, rows.size() - 1);

        if (buttons & Button::Circle) {
            switch (page) {
            case Page::Quests:
                enter(Page::Categories);
                break;
            case Page::Custom:
                enter(Page::Categories);
                selected = 3;
                break;
            case Page::Editor:
                enter(editorParent);

                if (editorParent == Page::Custom)
                    MH3_RETURN_IF_ERROR(LoadCustomQuests(user, custom));
                else
                    selected = MainRow(Item::Create);

                break;
            case Page::Saves:
            case Page::Categories:
                enter(Page::Main);
                selected = MainRow(then == Then::Village ? Item::Village : then == Then::City ? Item::City : Item::Quests);
                break;
            case Page::Cheats:
                enter(Page::Main);
                selected = MainRow(Item::Cheats);
                break;
            case Page::CheatList:
                enter(Page::Cheats);
                selected = cheatGroup;
                break;
            case Page::Online:
                enter(Page::Main);
                selected = MainRow(Item::Online);
                break;
            case Page::Main:
                break;
            }

            continue;
        }

        // An action that fails shows why; the menu stays.
        const auto act = [&]() -> Status {
            const bool accept = buttons & (Button::Cross | Button::Start);
            const int direction = (buttons & Button::Left) ? -1 : (buttons & Button::Right) ? 1 : 0;
            const auto onMain = [&](Item item) { return page == Page::Main && MainItems[selected] == item; };

            if (page == Page::Editor && (direction || accept)) {
                if (selected == 0 && accept)
                    MH3_RETURN_IF_ERROR(TypeName(edit.name, "quest name", 43));

                const int step = direction ? direction : 1;

                // The next of `list` after `value` (the first when it is not there).
                const auto next = [&](const std::vector<uint8_t>& list, uint8_t value) {
                    const auto found = std::find(list.begin(), list.end(), value);
                    const size_t index = found == list.end() ? 0 : size_t(found - list.begin());
                    return list[(index + list.size() + step) % list.size()];
                };

                // A name nobody typed follows the quest's goal.
                const bool automatic = edit.name == automaticName(edit);
                const CustomQuest before = edit;

                if (selected == 1) {
                    edit.map = next(ScratchMaps(), edit.map);
                    const auto allowed = ScratchMonsters(edit.map);
                    const auto has = [&](uint8_t m) { return std::find(allowed.begin(), allowed.end(), m) != allowed.end(); };

                    if (!has(edit.monsters[0]))
                        edit.monsters[0] = allowed.front();

                    for (size_t i = 1; i < edit.monsters.size(); ++i)
                        if (edit.monsters[i] && !has(edit.monsters[i]))
                            edit.monsters[i] = 0;

                    if (!edit.monsters[1])
                        std::swap(edit.monsters[1], edit.monsters[2]);
                }

                if (selected == 2)
                    edit.monsters[0] = next(ScratchMonsters(edit.map), edit.monsters[0]);

                // A third monster only after a second, none beside the
                // bosses.
                if ((selected == 3 || (selected == 4 && edit.monsters[1])) && !ScratchAlone(edit.monsters[0])) {
                    auto choices = ScratchMonsters(edit.map);
                    std::erase_if(choices, ScratchAlone);
                    choices.insert(choices.begin(), 0);
                    auto& monster = edit.monsters[selected - 2];
                    monster = next(choices, monster);

                    if (!edit.monsters[1])
                        edit.monsters[2] = 0;
                }

                if (ScratchAlone(edit.monsters[0]))
                    edit.monsters[1] = edit.monsters[2] = 0;

                if (selected == 5)
                    edit.capture = !edit.capture;

                if (selected == 6)
                    edit.rank = uint8_t((edit.rank + 3 + step) % 3);

                // Every monster's (the first's shown).
                if (selected == 7)
                    edit.sizes.fill(uint8_t(std::clamp<int>(int(edit.sizes[0]) + step * 5, 50, 200)));

                if (selected == 8)
                    edit.minutes = std::clamp<int>(int(edit.minutes ? edit.minutes : 50) + step, 1, 50);

                if (selected == 9)
                    edit.stars = std::clamp<int>(int(edit.stars) + step, 1, 7);

                SetScratchCarrier(edit);

                // Captures only where the game can capture the monsters.
                if (edit.capture) {
                    bool supported = false;
                    MH3_RETURN_IF_ERROR(ScratchCapture(edit, supported));

                    if (!supported) {
                        const bool asked = selected == 5;
                        edit.capture = false;

                        if (asked) {
                            edit = before;
                            return Status::Error("The game cannot capture these monsters here, so this quest stays a hunt.");
                        }
                    }
                }

                if (automatic)
                    edit.name = automaticName(edit);

                if (selected == 10 && accept) {
                    MH3_RETURN_IF_ERROR(SaveCustomQuest(user, edit));
                    Log("custom quest saved: %s (map %u, monsters %u %u %u, rank %u, size %u, %u stars)", edit.name.c_str(), unsigned(edit.map), unsigned(edit.monsters[0]),
                        unsigned(edit.monsters[1]), unsigned(edit.monsters[2]), unsigned(edit.rank), unsigned(edit.sizes[0]), unsigned(edit.stars));
                    Message("Custom quest saved. Play it from Quests > Custom quests.");

                    if (editorParent == Page::Custom) {
                        std::vector<CustomQuest> saved;
                        MH3_RETURN_IF_ERROR(LoadCustomQuests(user, saved));
                        custom = std::move(saved);
                        enter(Page::Custom);
                    } else {
                        enter(Page::Main);
                        selected = MainRow(Item::Create);
                    }
                }

                return {};
            }

            if (page == Page::CheatList && direction) {
                const size_t cheat = groupCheats[selected];
                const size_t choices = LauncherCheats::Choices(cheat);

                if (choices) {
                    LauncherCheats::Select(cheat, (LauncherCheats::Selected(cheat) + choices + direction) % choices);
                    Log("cheat %s: %s", LauncherCheats::List()[cheat].name, LauncherCheats::ValueLabel(cheat).c_str());
                } else if (const size_t length = LauncherCheats::TextLength(cheat)) {
                    std::string name = LauncherCheats::Text(cheat);
                    MH3_RETURN_IF_ERROR(TypeName(name, "hunter name", length));
                    LauncherCheats::SetText(cheat, name);
                    Log("cheat %s: %s", LauncherCheats::List()[cheat].name, LauncherCheats::ValueLabel(cheat).c_str());
                }

                return {};
            }

            if (page == Page::Custom && !custom.empty() && (buttons & Button::Triangle)) {
                // A template quest opens as one from scratch; saving keeps it so.
                CustomQuest quest = custom[selected];
                MH3_RETURN_IF_ERROR(ScratchFromTemplate(quest));
                edit = quest;
                editorParent = Page::Custom;
                enter(Page::Editor);
                return {};
            }

            if (onMain(Item::FrameLimit) && (accept || direction)) {
                const size_t count = std::size(FrameLimits), index = FrameLimitChoice(frameLimit);
                FrameLimit next = FrameLimits[(index + count + (direction ? direction : 1)) % count];

                // Enter on the typed cap types another.
                if (FrameLimits[index] == CustomFrameCap && !direction) {
                    std::string typed = std::to_string(customFps);
                    MH3_RETURN_IF_ERROR(TypeName(typed, "frame limit in frames a second", 4));
                    const auto limit = ParseFrameLimit("cap:" + typed);

                    if (!limit)
                        return Status::Error("The frame limit is a number of frames a second from " + std::to_string(MinFrameCap) + " to " + std::to_string(MaxFrameCap));

                    customFps = limit->fps;
                    next = CustomFrameCap;
                }

                frameLimit = next == CustomFrameCap ? FrameLimit{FrameLimitMode::Cap, customFps} : next;
                SetFrameLimitSetting(frameLimit);
                Log("framelimit %s", FrameLimitText(frameLimit).c_str());
                return SaveSettings(base);
            }

            if ((onMain(Item::Msaa) || onMain(Item::Ssaa)) && (accept || direction)) {
                constexpr uint32_t modes[] = {1, 2, 4};
                uint32_t& samples = onMain(Item::Msaa) ? msaa : ssaa;
                const int index = samples == 1 ? 0 : samples == 2 ? 1 : 2;
                samples = modes[(index + 3 + (direction ? direction : 1)) % 3];

                if (onMain(Item::Msaa))
                    SetMsaaSetting(samples);
                else
                    SetSsaaSetting(samples);

                Log("%s %ux", onMain(Item::Msaa) ? "msaa" : "ssaa", unsigned(samples));
                return SaveSettings(base);
            }

            if (onMain(Item::Language) && (accept || direction)) {
                language = (language + languages.size() + (direction ? direction : 1)) % languages.size();
                SetLanguageSetting(languages[language].system);
                Log("language %s", languages[language].code);
                return SaveSettings(base);
            }

            if (page == Page::Online && selected == 0 && (accept || (buttons & Button::Triangle))) {
                std::string server = accept ? onlineServer : kDefaultOnlineServer;

                if (accept)
                    MH3_RETURN_IF_ERROR(TypeName(server, "server", 43));

                if (const std::string error = CheckOnlineServer(server); !error.empty())
                    return Status::Error(error);

                onlineServer = server;
                SetOnlineServerSetting(server);
                Log("online server %s", server.c_str());
                return SaveSettings(base);
            }

            if (!accept)
                return {};

            switch (page) {
            case Page::Main: {
                const Item item = MainItems[selected];

                if (item == Item::Start) {
                    start("title screen");
                    return {};
                }

                if (item == Item::Village || item == Item::City || item == Item::Quests) {
                    then = item == Item::Village ? Then::Village : item == Item::City ? Then::City : Then::Quests;
                    saves = ProbeSaves(user);

                    if (saves.empty()) {
#ifdef __EMSCRIPTEN__
                        Message("No hunter save was found. Village and Quests need an existing character: choose Start game and create one through the game's own "
                                "screens, or reload the page and import your Monster Hunter Tri save there.");
#else
                        Message("No hunter save was found. Village, City and Quests need an existing character: choose Start game and create one through the game's own "
                                "screens, or copy your Monster Hunter Tri save.");
#endif
                        break;
                    }

                    save = saves.front();

                    if (saves.size() > 1)
                        enter(Page::Saves);
                    else if (then == Then::Quests)
                        enter(Page::Categories);
                    else {
                        startMode();
                        return {};
                    }
                } else if (item == Item::Create) {
                    edit = {};
                    edit.map = 1;
                    edit.monsters = {ScratchMonsters(1).front(), 0, 0};
                    edit.minutes = 50;
                    edit.stars = 4;
                    edit.name = automaticName(edit);
                    SetScratchCarrier(edit);
                    editorParent = Page::Main;
                    enter(Page::Editor);
                } else if (item == Item::Cheats)
                    enter(Page::Cheats);
                else if (item == Item::Widescreen) {
                    widescreen = !widescreen;
                    SetWidescreenSetting(widescreen);
                    Log("widescreen %s", widescreen ? "on" : "off");
                } else if (item == Item::Upscaling) {
                    upscaling = !upscaling;
                    SetUpscalingSetting(upscaling);
                    Log("upscaling %s", upscaling ? "on" : "off");
                } else if (item == Item::Online)
                    enter(Page::Online);

                if (item == Item::Widescreen || item == Item::Upscaling)
                    return SaveSettings(base);

                break;
            }
            case Page::Saves:
                save = saves[selected];

                if (then != Then::Quests) {
                    startMode();
                    return {};
                }

                enter(Page::Categories);
                break;
            case Page::Categories:
                if (selected == 3) {
                    std::vector<CustomQuest> saved;
                    MH3_RETURN_IF_ERROR(LoadCustomQuests(user, saved));
                    custom = std::move(saved);
                    enter(Page::Custom);
                    break;
                }

                set = QuestCatalog::Set(selected);
                quests = catalog(set);
                enter(Page::Quests);
                break;
            case Page::Quests:
                if (quests.empty())
                    break;

                {
                    const auto& q = quests[selected];
                    StartVillageBoot(save.file, save.character);
                    RequestQuestJump(q.id, set, -1);
                    start("quest " + std::to_string(q.id) + " " + q.name + ", " + saveName());
                    return {};
                }
            case Page::Custom:
                if (!custom.empty()) {
                    const auto& q = custom[selected];
                    StartVillageBoot(save.file, save.character);

                    if (Status activated = ActivateCustomQuest(q); !activated.ok()) {
                        FinishVillageBoot();
                        return activated;
                    }

                    RequestQuestJump(q.base, q.set, -1);
                    start("custom quest " + q.name + " on template " + std::to_string(q.base) + ", " + saveName());
                    return {};
                }

                break;
            case Page::Cheats:
                cheatGroup = selected;
                groupCheats = GroupCheats(LauncherCheats::Groups()[cheatGroup]);
                enter(Page::CheatList);
                break;
            case Page::CheatList: {
                const size_t cheat = groupCheats[selected];
                LauncherCheats::Enable(cheat, !LauncherCheats::Enabled(cheat));
                const std::string value = LauncherCheats::ValueLabel(cheat);
                Log("cheat %s %s%s", LauncherCheats::List()[cheat].name, LauncherCheats::Enabled(cheat) ? "on" : "off", value.empty() ? "" : (" (" + value + ")").c_str());
                break;
            }
            case Page::Online:
                if (selected == 1) {
                    skipTls = !skipTls;
                    SetSkipTlsSetting(skipTls);
                    Log("skip TLS handshake %s", skipTls ? "on" : "off");
                    return SaveSettings(base);
                }

                break;
            case Page::Editor:
                break;
            }

            return {};
        };

        if (const Status acted = act(); !acted.ok())
            Message(acted.message());
        else if (started)
            return {};
    }

    return {};
}
} // namespace

bool Run() {
    // Declared out here: a development host's error message draws with it.
    std::unique_ptr<Font> font;
    struct Forget {
        ~Forget() { messageFont = nullptr; }
    } forget;

    bool started = false;

    if (const Status status = Menu(font, started); !status.ok()) {
        Message(status.message());
        return false;
    }

    return started;
}
} // namespace VitaLauncher
