// The native launcher (docs/vita-launcher.md). It runs on the Vita and, in
// development builds, on Linux hosts (MH3 --launcher), which have no system
// dialogs: messages draw as a panel there and the quest name keeps its value.
#include "vita_launcher.h"
#include "fatal.h"
#include "launcher_font.h"
#include "launcher_screen.h"
#include "launcher_cheats.h"
#include "player_health.h"
#include "village_boot.h"
#include "field_jump.h"
#include "disc_image.h"
#include "fs_status.h"
#include "widescreen.h"
#include <aurora/aurora.h>
#include <aurora/event.h>
#include <aurora/thread_role.hpp>
#ifdef __vita__
#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/message_dialog.h>
#include <psp2/ime_dialog.h>
#include <psp2/vshbridge.h>
#else
#include "controller_host.h"
#include "../native/mh3_classic_map.h"
#include <chrono>
#endif
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#ifdef MH3_DEVELOPMENT
#include <fstream>
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
// Development log lines (the TTY); the devkit harness reads them.
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
// MH3_LAUNCHER_INPUT=file: scripted launcher input for devkit tests. Each line
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
    // (SDL's Vita driver): a PS TV controller, or the devkit harness's CTRLP pad.
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
    // its buttons by position on the Vita: B is Cross, A Circle, X Triangle,
    // Y Square, Plus Start.
    const auto pad = ReadLiveController();
    static constexpr std::pair<uint16_t, uint32_t> map[] = {
        {MH3_CL_UP, Button::Up},       {MH3_CL_DOWN, Button::Down},   {MH3_CL_LEFT, Button::Left},    {MH3_CL_RIGHT, Button::Right},
        {MH3_CL_B, Button::Cross},     {MH3_CL_A, Button::Circle},    {MH3_CL_X, Button::Triangle},   {MH3_CL_Y, Button::Square},
        {MH3_CL_PLUS, Button::Start},  {MH3_CL_MINUS, Button::Select}, {MH3_CL_L, Button::L2},        {MH3_CL_R, Button::R2}};
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

// The system keyboard (SceImeDialog, part of SceCommonDialog like the message).
Status EditName(std::string& name) {
    std::array<SceWChar16, 44> initial{}, output{};

    for (size_t i = 0; i < std::min(name.size(), initial.size() - 1); ++i)
        initial[i] = uint8_t(name[i]);

    const SceWChar16 title[] = {'Q', 'u', 'e', 's', 't', ' ', 'n', 'a', 'm', 'e', 0};
    SceImeDialogParam param{};
    sceImeDialogParamInit(&param);
    param.type = SCE_IME_TYPE_BASIC_LATIN;
    param.dialogMode = SCE_IME_DIALOG_DIALOG_MODE_WITH_CANCEL;
    param.title = title;
    param.maxTextLength = initial.size() - 1;
    param.initialText = initial.data();
    param.inputTextBuffer = output.data();

    if (const int error = sceImeDialogInit(&param); error < 0)
        return Status::Error("Cannot open the quest-name keyboard (error " + std::to_string(error) + ")");

    Log("quest-name keyboard open");
    RunDialog(sceImeDialogGetStatus, sceImeDialogAbort);
    SceImeDialogResult result{};
    sceImeDialogGetResult(&result);
    sceImeDialogTerm();

    if (result.button != SCE_IME_DIALOG_BUTTON_ENTER) {
        Log("quest-name keyboard closed without a name");
        return {};
    }

    std::string value;

    for (auto c : output) {
        if (!c)
            break;

        if (c < 32 || c > 126)
            return Status::Error("Use printable Latin characters for quest names");

        value += char(c);
    }

    // Leading and trailing spaces would make names that look the same.
    value.erase(0, value.find_first_not_of(' '));
    value.erase(value.find_last_not_of(' ') + 1);

    if (!value.empty())
        name = std::move(value);

    Log("quest name: %s", name.c_str());
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
    messageFont->Text(frame, 180, top + height - 42, "X OK", ink, 600);
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

        if (held & ~previous & (Button::Cross | Button::Circle | Button::Start))
            break;

        previous = held;
    }
}

// No system keyboard on a development host: the name stays as it is.
Status EditName(const std::string& name) {
    Log("quest-name keyboard: the Vita's system keyboard; the host keeps \"%s\"", name.c_str());
    return {};
}
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

constexpr size_t VisibleRows = 7;
void Draw(Font& font, const std::string& title, const std::vector<std::string>& rows, size_t selected, const std::string& detail, const std::string& footer) {
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

    const size_t first = selected >= VisibleRows ? selected - (VisibleRows - 1) : 0;

    for (size_t row = first; row < std::min(first + VisibleRows, rows.size()); ++row) {
        const float y = 110 + (row - first) * 43;

        if (row == selected) {
            Rect(rects, 54, y - 2, 848, 36, ink);
            Rect(rects, 54, y - 2, 6, 36, gold);
        }

        font.Text(rects, 72, y + 4, rows[row], row == selected ? gold : ink, 810);
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

    font.Text(rects, 45, 513, footer, light, 890);
    PresentScreen(&rects);
    Wait(8000);
}

// The disc image stays in base; its metadata and the converted-asset cache live
// in base/disc. A development install of a prepared disc directly in base is used as is.
Status EnsureDisc(Font& font, const std::filesystem::path& base) {
    auto output = base;

    if (!DiscReady(output))
        output = base / "disc";

    std::filesystem::path iso;
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

    if (!DiscReady(output) || (!iso.empty() && output != base)) {
        if (iso.empty())
            return Status::Error("Game data is missing. Copy your European Monster Hunter Tri (RMHP08) disc image (ISO, RVZ or WIA) to " + base.string() +
                                 ", then start Monster Hunter Tri again.");
        std::mutex mutex;
        std::string status = "Checking game data", error;
        uint64_t done = 0, total = 1;
        std::atomic<bool> finished = false;
        std::jthread worker([&] {
            aurora::pin_current_thread(aurora::ThreadRole::Worker);
            const Progress progress = [&](const std::string& s, uint64_t d, uint64_t t) {
                std::lock_guard guard(mutex);
                status = s;
                done = d;
                total = std::max<uint64_t>(t, 1);
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
            uint64_t completed, maximum;
            {
                std::lock_guard guard(mutex);
                label = status;
                completed = done;
                maximum = total;
            }

            const unsigned percent = std::min<uint64_t>(100, completed * 100 / maximum);
            auto& rects = shown;
            Frame(rects);
            Rect(rects, 42, 46, 876, 48, ink);
            font.Text(rects, 60, 58, "MONSTER HUNTER TRI  /  Preparing your game", gold, 840);
            font.Text(rects, 150, 203, iso.filename().string(), ink, 660);
            font.Text(rects, 150, 243, label, ink, 660);
            Rect(rects, 150, 288, 660, 25, ink);
            Rect(rects, 153, 291, 654 * percent / 100.f, 19, gold);
            font.Text(rects, 150, 327, std::to_string(percent) + "%", ink, 660);
            font.Text(rects, 45, 513, "Keep the application open", light, 890);
            PresentScreen(&rects);
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
    Templates,
    Editor,
    Cheats
};

const char* PageName(Page page) {
    constexpr const char* names[]{"Main", "Quests", "Saves", "Quest list", "Custom quests", "Templates", "Editor", "Cheats"};
    return names[unsigned(page)];
}

// A new custom quest's name (at most 43 characters).
std::string DefaultName(const QuestCatalog::Quest& source) {
    return "Custom " + source.name.substr(0, 36);
}

const char* OnOff(bool on) {
    return on ? "ON" : "OFF";
}

// The menus, until the game starts (started) or the user exits. A failure
// ends them; an action's failure only shows its message. After the hand-off
// the launcher no longer draws: nothing fails from there on.
Status Menu(std::unique_ptr<Font>& font, bool& started) {
    StartScreen();

    // The empty page: messages before the menu (kubridge, game data) draw over it.
    Frame(shown);
#ifdef __vita__
    SceCommonDialogConfigParam config{};
    sceCommonDialogConfigParamInit(&config);
    sceCommonDialogSetConfigParam(&config);
    const uint32_t search[2]{};

    if (_vshKernelSearchModuleByName("kubridge", search) < 0) {
        Message("Kubridge is not installed. Install kubridge.skprx under *KERNEL in your taiHEN config.txt, then restart your PS Vita before launching Monster Hunter Tri.");
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
    const char* userDirectory = std::getenv("MH3_USER_DIR");
    const auto user = std::filesystem::path(userDirectory ? userDirectory : "saves");
    Page page = Page::Main;
    size_t selected = 0;
    bool creating = false, noDamage = false, noStamina = false;

    // The game reads it once, at RenderInit, after the hand-off.
    bool widescreen = WidescreenSetting().value_or(true);
    SaveSlot save{};
    std::vector<SaveSlot> saves;
    std::vector<QuestCatalog::Quest> quests;
    std::vector<CustomQuest> custom;
    CustomQuest edit;

    // The editor returns to the list it came from.
    Page editorParent = Page::Main;
    QuestCatalog::Set set = QuestCatalog::Set::Village;
    std::vector<QuestCatalog::Quest> editorQuests;
    std::vector<uint16_t> encounters, maps;
    const auto monsters = QuestCatalog::MonsterNames();
    const auto monsterName = [&](uint16_t id) { return id < monsters.size() && !monsters[id].empty() ? monsters[id] : "monster " + std::to_string(id); };
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

    const auto findEditorQuest = [&](uint16_t id) -> const QuestCatalog::Quest* {
        const auto found = std::find_if(editorQuests.begin(), editorQuests.end(), [&](const auto& q) { return q.id == id; });
        return found == editorQuests.end() ? nullptr : &*found;
    };

    // The editor's template quest `id`.
    const auto editorQuest = [&](uint16_t id, const QuestCatalog::Quest*& quest) {
        quest = findEditorQuest(id);
        return quest ? Status{} : Status::Error("Custom quest template is unavailable");
    };

    const auto refreshEditor = [&] {
        editorQuests = catalog(edit.set);
        std::vector<uint16_t> options, templates;
        MH3_RETURN_IF_ERROR(CompositionOptions(edit, options));
        encounters = std::move(options);
        MH3_RETURN_IF_ERROR(CompositionMaps(edit, templates));
        maps = std::move(templates);
        return Status{};
    };

    Input input;
    const auto enter = [&](Page next) {
        page = next;
        selected = 0;
        Log("page %s", PageName(next));
    };

    const auto saveName = [&] { return "File " + std::to_string(save.file + 1) + " / Character " + std::to_string(save.character + 1); };
    const auto start = [&](const std::string& what) {
        started = true;
        StartGame(*font, what);
    };

    Log("menu ready");

    while (Pump()) {
        std::string title, detail, footer = "X Select   O Back   D-pad Move";
        std::vector<std::string> rows;

        switch (page) {
        case Page::Main: {
            title = "Main menu";
            rows = {"Start game", "Quests", "Create custom quest", "Cheats", std::string("Widescreen: ") + (widescreen ? "ON (16:9)" : "OFF (4:3)")};
            const std::string cheats = noDamage || noStamina ? std::string(" Active: ") + (noDamage ? "No damage" : "") + (noDamage && noStamina ? ", " : "") +
                                                                   (noStamina ? "No action stamina drain" : "") + "."
                                                             : "";
            const std::string details[] = {"Continue through the game's title and character selection." + cheats,
                                           "Start a quest directly with an existing hunter." + cheats,
                                           "Make a hunt or capture quest from one of the game's quests.",
                                           "Gameplay toggles for this launch." + cheats,
                                           widescreen ? "The game's own 16:9 mode on the whole display." : "4:3, with bars at the sides of the display."};
            detail = details[std::min<size_t>(selected, 4)];
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
        case Page::Templates:
            title = "Choose a quest template";
            rows = {"Offline quests", "Online quests", "Arena"};
            detail = "The template supplies the map, monster spawns and rewards.";
            break;
        case Page::Saves:
            title = "Choose your hunter";

            for (const auto& s : saves)
                rows.push_back("File " + std::to_string(s.file + 1) + " / Character " + std::to_string(s.character + 1));

            detail = "Characters of valid saves.";
            break;
        case Page::Quests:
            title = creating ? "Choose a hunt/capture template" : "Quest list";

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
            const auto* baseFound = findEditorQuest(edit.base);
            const auto* encounterFound = findEditorQuest(edit.encounter ? edit.encounter : edit.base);

            if (!baseFound || !encounterFound)
                return Status::Error("Custom quest template is unavailable");

            const auto& baseQuest = *baseFound;
            const auto& encounter = *encounterFound;
            const auto& objective = encounter.objectives.front();
            title = edit.storageKey.empty() ? "Create custom quest" : "Edit custom quest";
            rows = {"Name: " + edit.name,
                    "Map: " + QuestCatalog::MapName(baseQuest.map) + (encounters.empty() ? " (fixed)" : ""),
                    "Monster: " + monsterName(objective.target) + (encounters.empty() ? " (fixed)" : ""),
                    "Objective: " + std::string(edit.capture ? "Capture" : "Hunt"),
                    "Target count: " + std::to_string(edit.count) + " of " + std::to_string(objective.count),
                    "Time: " + (edit.minutes ? std::to_string(edit.minutes) + " minutes" : "as the template"),
                    "Stars: " + std::to_string(edit.stars),
                    "Save quest"};
            switch (std::min<size_t>(selected, 7)) {
            case 0:
#ifdef __vita__
                detail = "X opens the keyboard.";
#else
                detail = "The keyboard is the Vita's; the host keeps the name.";
#endif
                footer = "X Keyboard   O Back   D-pad Move";
                break;
            case 7:
                detail = edit.storageKey.empty() ? "Saves a new custom quest." : "Saves the changes to this custom quest.";
                footer = "X Save   O Back   D-pad Move";
                break;
            default:
                detail = encounters.empty() ? "This template's map and monsters are scripted and stay as they are." : "Placement and rewards from " + encounter.name + ".";
                footer = "Left / Right Change   O Back   D-pad Move";
            }

            break;
        }
        case Page::Cheats:
            title = "Cheats";
            rows = {std::string("No damage: ") + OnOff(noDamage), std::string("No action stamina drain: ") + OnOff(noStamina)};
            detail = selected == 0 ? "The hunter takes no damage. For this launch." : "Running, guarding and other continuous actions cost no stamina. For this launch.";
            footer = "X Toggle   O Back   D-pad Move";
            break;
        }

        if (rows.empty())
            rows = {"No quests available"};

        selected = std::min(selected, rows.size() - 1);
        Draw(*font, title, rows, selected, detail, footer);
        const uint32_t buttons = input.Read();

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
                enter(creating ? Page::Templates : Page::Categories);
                break;
            case Page::Custom:
                enter(Page::Categories);
                selected = 3;
                break;
            case Page::Editor:
                enter(editorParent);

                if (editorParent == Page::Custom)
                    MH3_RETURN_IF_ERROR(LoadCustomQuests(user, custom));

                break;
            case Page::Saves:
            case Page::Categories:
                enter(Page::Main);
                selected = 1;
                break;
            case Page::Templates:
                enter(Page::Main);
                selected = 2;
                break;
            case Page::Cheats:
                enter(Page::Main);
                selected = 3;
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

            if (page == Page::Editor && (direction || accept)) {
                if (selected == 0 && accept)
                    MH3_RETURN_IF_ERROR(EditName(edit.name));

                const auto cycle = [&](size_t index, size_t count) {
                    return (index + count + (direction ? direction : 1)) % count;
                };

                // A name nobody typed follows the quest the placement comes from.
                const QuestCatalog::Quest* quest = nullptr;
                MH3_RETURN_IF_ERROR(editorQuest(edit.encounter ? edit.encounter : edit.base, quest));
                const bool automaticName = edit.name == DefaultName(*quest);

                if (selected == 1 && !encounters.empty() && !maps.empty()) {
                    MH3_RETURN_IF_ERROR(editorQuest(edit.base, quest));
                    const auto map = quest->map;
                    bool missing = false;

                    // Stops at a missing template, as the search stopped at its failure.
                    const auto found = std::find_if(maps.begin(), maps.end(), [&](auto id) {
                        const auto* candidate = findEditorQuest(id);
                        missing = !candidate;
                        return missing || candidate->map == map;
                    });

                    if (missing)
                        return Status::Error("Custom quest template is unavailable");

                    edit.base = maps[cycle(found - maps.begin(), maps.size())];
                    edit.encounter = 0;
                    MH3_RETURN_IF_ERROR(editorQuest(edit.base, quest));
                    edit.count = quest->objectives.front().count;
                    edit.capture = false;
                    std::vector<uint16_t> options;
                    MH3_RETURN_IF_ERROR(CompositionOptions(edit, options));
                    encounters = std::move(options);
                }

                if (selected == 2 && !encounters.empty()) {
                    const auto found = std::find(encounters.begin(), encounters.end(), edit.encounter ? edit.encounter : edit.base);
                    edit.encounter = encounters[cycle(found == encounters.end() ? 0 : found - encounters.begin(), encounters.size())];
                    MH3_RETURN_IF_ERROR(editorQuest(edit.encounter, quest));
                    edit.count = quest->objectives.front().count;

                    if (edit.capture) {
                        bool supported = false;
                        MH3_RETURN_IF_ERROR(SupportsCapture(edit, supported));

                        if (!supported)
                            edit.capture = false;
                    }
                }

                if (automaticName) {
                    MH3_RETURN_IF_ERROR(editorQuest(edit.encounter ? edit.encounter : edit.base, quest));
                    edit.name = DefaultName(*quest);
                }

                if (selected == 3) {
                    if (!edit.capture) {
                        bool supported = false;
                        MH3_RETURN_IF_ERROR(SupportsCapture(edit, supported));

                        if (!supported)
                            return Status::Error("No quest captures this monster with this placement, so this quest stays a hunt.");
                    }

                    edit.capture = !edit.capture;
                }

                if (selected == 4) {
                    MH3_RETURN_IF_ERROR(editorQuest(edit.encounter ? edit.encounter : edit.base, quest));
                    edit.count = std::clamp<int>(int(edit.count) + (direction ? direction : 1), 1, quest->objectives.front().count);
                }

                if (selected == 5)
                    edit.minutes = std::clamp<int>(int(edit.minutes) + (direction ? direction : 1), 0, 50);

                if (selected == 6)
                    edit.stars = std::clamp<int>(int(edit.stars) + (direction ? direction : 1), 1, 7);

                if (selected == 7 && accept) {
                    MH3_RETURN_IF_ERROR(SaveCustomQuest(user, edit));
                    Log("custom quest saved: %s (template %u, encounter %u, %u stars)", edit.name.c_str(), unsigned(edit.base), unsigned(edit.encounter), unsigned(edit.stars));
                    Message("Custom quest saved. Play it from Quests > Custom quests.");

                    if (editorParent == Page::Custom) {
                        std::vector<CustomQuest> saved;
                        MH3_RETURN_IF_ERROR(LoadCustomQuests(user, saved));
                        custom = std::move(saved);
                        enter(Page::Custom);
                    } else
                        enter(Page::Main);
                }

                return {};
            }

            if (page == Page::Custom && !custom.empty() && (buttons & Button::Triangle)) {
                edit = custom[selected];
                MH3_RETURN_IF_ERROR(refreshEditor());
                editorParent = Page::Custom;
                enter(Page::Editor);
                return {};
            }

            if (!accept)
                return {};

            switch (page) {
            case Page::Main:
                if (selected == 0) {
                    start("title screen");
                    return {};
                }

                if (selected == 1) {
                    creating = false;
                    saves = ProbeSaves(user);

                    if (saves.empty()) {
                        Message("No hunter save was found. Quests need an existing character: choose Start game and create one through the game's own screens, "
                                "or copy your Monster Hunter Tri save.");
                        break;
                    }

                    save = saves.front();
                    enter(saves.size() > 1 ? Page::Saves : Page::Categories);
                } else if (selected == 2) {
                    creating = true;
                    enter(Page::Templates);
                } else if (selected == 3)
                    enter(Page::Cheats);
                else {
                    widescreen = !widescreen;
                    SetWidescreenSetting(widescreen);
                    Log("widescreen %s", widescreen ? "on" : "off");
                }

                break;
            case Page::Saves:
                save = saves[selected];
                enter(Page::Categories);
                break;
            case Page::Categories:
            case Page::Templates:
                if (selected == 3) {
                    std::vector<CustomQuest> saved;
                    MH3_RETURN_IF_ERROR(LoadCustomQuests(user, saved));
                    custom = std::move(saved);
                    enter(Page::Custom);
                    break;
                }

                set = QuestCatalog::Set(selected);
                quests = catalog(set);

                if (creating)
                    quests.erase(std::remove_if(quests.begin(), quests.end(), [](const auto& q) { return q.objectives.empty() || (q.objectives[0].type != 1 && q.objectives[0].type != 0x81); }),
                                 quests.end());
                enter(Page::Quests);
                break;
            case Page::Quests:
                if (quests.empty())
                    break;

                if (creating) {
                    const auto& q = quests[selected];
                    edit = {DefaultName(q), set, q.id, q.objectives[0].count, q.objectives[0].type == 0x81, q.stars};
                    MH3_RETURN_IF_ERROR(refreshEditor());
                    editorParent = Page::Quests;
                    enter(Page::Editor);
                } else {
                    const auto& q = quests[selected];
                    StartVillageBoot(save.file, save.character);
                    RequestQuestJump(q.id, set, -1);
                    start("quest " + std::to_string(q.id) + " " + q.name + ", " + saveName());
                    return {};
                }

                break;
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
                if (selected == 0) {
                    noDamage = !noDamage;
                    PlayerHealth::SetNoDamage(noDamage);
                } else {
                    noStamina = !noStamina;
                    LauncherCheats::SetNoStaminaDrain(noStamina);
                }

                Log("cheats: no damage %s, no action stamina drain %s", noDamage ? "on" : "off", noStamina ? "on" : "off");
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
