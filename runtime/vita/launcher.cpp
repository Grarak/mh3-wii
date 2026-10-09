#include "vita_launcher.h"
#include "launcher_font.h"
#include "launcher_screen.h"
#include "launcher_cheats.h"
#include "player_health.h"
#include "village_boot.h"
#include "field_jump.h"
#include "disc_image.h"
#include <aurora/aurora.h>
#include <aurora/event.h>
#include <aurora/thread_role.hpp>
#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/message_dialog.h>
#include <psp2/ime_dialog.h>
#include <psp2/vshbridge.h>
#include <psp2/sysmodule.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <thread>
#ifdef MH3_DEVELOPMENT
#include <fstream>
#include <sstream>
#endif

namespace VitaLauncher {
namespace {
constexpr uint32_t ink = 0xff28302c, paper = 0xffc8d7d8, gold = 0xff73b9de, light = 0xffe1ebed;
// Development log lines (runtime.log); the devkit harness reads them.
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
            throw std::runtime_error(std::string("Cannot read MH3_LAUNCHER_INPUT ") + path);
        for (std::string line; std::getline(input, line);) {
            std::istringstream fields(line.substr(0, line.find('#')));
            uint64_t frame;
            std::string names;
            if (!(fields >> frame))
                continue;
            if (!(fields >> names) || (!entries.empty() && frame < entries.back().first))
                throw std::runtime_error("Invalid MH3_LAUNCHER_INPUT line: " + line);
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
            {"up", SCE_CTRL_UP},           {"down", SCE_CTRL_DOWN},     {"left", SCE_CTRL_LEFT},     {"right", SCE_CTRL_RIGHT},
            {"cross", SCE_CTRL_CROSS},     {"circle", SCE_CTRL_CIRCLE}, {"triangle", SCE_CTRL_TRIANGLE}, {"square", SCE_CTRL_SQUARE},
            {"start", SCE_CTRL_START},     {"select", SCE_CTRL_SELECT}, {"l", SCE_CTRL_LTRIGGER},    {"r", SCE_CTRL_RTRIGGER}};
        uint32_t value = 0;
        for (size_t at = 0; at <= text.size();) {
            const size_t end = std::min(text.find('+', at), text.size());
            const auto name = text.substr(at, end - at);
            const auto found = std::find_if(std::begin(named), std::end(named), [&](const auto& n) { return name == n.first; });
            if (found == std::end(named))
                throw std::runtime_error("Unknown MH3_LAUNCHER_INPUT button " + name);
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
// The last launcher frame; dialogs draw over it.
std::vector<Rectangle> shown;
// System dialogs draw into the display buffer the render thread presents:
// Aurora's GXM present calls sceCommonDialogUpdate while one is active. The
// render thread owns that flag, and a dialog must not end while a present
// could still update it, so both changes run on the render thread.
template <class Status, class Close>
void RunDialog(Status status, [[maybe_unused]] Close close) {
    RunOnRenderThread([] { aurora_set_common_dialog_active(true); }, &shown);
    [[maybe_unused]] const uint64_t opened = sceKernelGetProcessTimeWide();
    [[maybe_unused]] bool closing = false;
    while (status() != SCE_COMMON_DIALOG_STATUS_FINISHED && Pump()) {
#ifdef MH3_DEVELOPMENT
        if (!closing && InputScript().Running() && sceKernelGetProcessTimeWide() - opened > 2500000) {
            Log("dialog closed by the input script");
            close();
            closing = true;
        }
#endif
        PresentScreen(&shown);
        sceKernelDelayThread(8000);
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
void EditName(std::string& name) {
    std::array<SceWChar16, 44> initial{}, output{};
    for (size_t i = 0; i < std::min(name.size(), initial.size() - 1); ++i)
        initial[i] = uint8_t(name[i]);
    const SceWChar16 title[] = {'Q', 'u', 'e', 's', 't', ' ', 'n', 'a', 'm', 'e', 0};
    SceImeDialogParam param{};
    sceImeDialogParamInit(&param);
    param.type = SCE_IME_TYPE_BASIC_LATIN;
    param.dialogMode = SCE_IME_DIALOG_DIALOG_MODE_WITH_CANCEL;
    param.title = title;
    param.maxTextLength = 43;
    param.initialText = initial.data();
    param.inputTextBuffer = output.data();
    if (sceSysmoduleLoadModule(SCE_SYSMODULE_IME) < 0 && sceSysmoduleIsLoaded(SCE_SYSMODULE_IME) < 0)
        throw std::runtime_error("Cannot load the quest-name keyboard");
    if (sceImeDialogInit(&param) < 0)
        throw std::runtime_error("Cannot open the quest-name keyboard");
    Log("quest-name keyboard open");
    RunDialog(sceImeDialogGetStatus, sceImeDialogAbort);
    SceImeDialogResult result{};
    sceImeDialogGetResult(&result);
    sceImeDialogTerm();
    if (result.button == SCE_IME_DIALOG_BUTTON_ENTER) {
        std::string value;
        for (auto c : output) {
            if (!c)
                break;
            if (c < 32 || c > 126)
                throw std::runtime_error("Use printable Latin characters for quest names");
            value += char(c);
        }
        if (!value.empty())
            name = std::move(value);
    }
}
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
            held = Live();
        const uint64_t now = sceKernelGetProcessTimeWide();
        uint32_t edges = held & ~previous;
        const uint32_t directions = held & (SCE_CTRL_UP | SCE_CTRL_DOWN | SCE_CTRL_LEFT | SCE_CTRL_RIGHT);
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
    static uint32_t Live() {
        SceCtrlData sample{};
        if (sceCtrlPeekBufferPositive(0, &sample, 1) < 1)
            return 0;
        uint32_t held = sample.buttons;
        if (sample.ly < 80)
            held |= SCE_CTRL_UP;
        if (sample.ly > 176)
            held |= SCE_CTRL_DOWN;
        if (sample.lx < 80)
            held |= SCE_CTRL_LEFT;
        if (sample.lx > 176)
            held |= SCE_CTRL_RIGHT;
        return held;
    }
    uint32_t previous = 0, lastDirection = 0;
    uint64_t nextRepeat = 0, frame = 0;
};
void Draw(Font& font, const std::string& title, const std::vector<std::string>& rows, size_t selected, const std::string& detail,
          const std::string& footer = "X Select   O Back   D-pad / left stick Move") {
    auto& rects = shown;
    rects.clear();
    Rect(rects, 0, 0, 960, 544, ink);
    Rect(rects, 30, 30, 900, 475, paper);
    // Authored geometric borders echo MH3's framed parchment quest menus.
    // Game fonts/art remain external; the launcher uses installed PVF.
    for (int x = 34; x < 926; x += 24) {
        Rect(rects, x, 34, 12, 4, gold);
        Rect(rects, x, 497, 12, 4, gold);
    }
    Rect(rects, 42, 46, 876, 48, ink);
    font.Text(rects, 60, 58, "MONSTER HUNTER TRI  /  " + title, gold, 830);
    const size_t first = selected >= 7 ? selected - 6 : 0;
    for (size_t row = first; row < std::min(first + 7, rows.size()); ++row) {
        const float y = 110 + (row - first) * 43;
        if (row == selected) {
            Rect(rects, 54, y - 2, 848, 36, ink);
            Rect(rects, 54, y - 2, 6, 36, gold);
        }
        font.Text(rects, 72, y + 4, rows[row], row == selected ? gold : ink, 810);
    }
    Rect(rects, 54, 421, 848, 2, ink);
    font.Text(rects, 64, 435, detail, ink, 820);
    font.Text(rects, 45, 517, footer, light, 890);
    PresentScreen(&rects);
    sceKernelDelayThread(8000);
}
// The disc image stays in base; its metadata and the converted-asset cache live
// in base/disc. A development install of a prepared disc directly in base is used as is.
bool EnsureDisc(Font& font, const std::filesystem::path& base) {
    auto output = base;
    if (!DiscReady(output))
        output = base / "disc";
    std::filesystem::path iso;
    for (const auto& entry : std::filesystem::directory_iterator(base))
        if (entry.is_regular_file() && (entry.path().extension() == ".iso" || entry.path().extension() == ".rvz" || entry.path().extension() == ".wia")) {
            if (!iso.empty())
                throw std::runtime_error("Place exactly one RMHP08 disc image in " + base.string());
            iso = entry.path();
        }
    if (!DiscReady(output) || (!iso.empty() && output != base)) {
        if (iso.empty())
            throw std::runtime_error("Game data is missing. Place your European RMHP08 ISO or RVZ in " + base.string() + ".");
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
            try {
                ProcessDisc(iso, output, progress);
            } catch (const std::exception& e) {
                std::lock_guard guard(mutex);
                error = e.what();
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
            rects.clear();
            Rect(rects, 0, 0, 960, 544, ink);
            Rect(rects, 120, 178, 720, 184, paper);
            font.Text(rects, 150, 203, "Preparing your game", ink);
            font.Text(rects, 150, 243, label, ink, 650);
            Rect(rects, 150, 288, 660, 25, ink);
            Rect(rects, 153, 291, 654 * percent / 100.f, 19, gold);
            font.Text(rects, 150, 327, std::to_string(percent) + "%  Keep the application open", ink);
            PresentScreen(&rects);
            Pump();
            sceKernelDelayThread(12000);
        }
        worker.join();
        if (!error.empty())
            throw std::runtime_error(error);
        if (!DiscReady(output))
            throw std::runtime_error("Prepared game data did not pass validation");
    }
    setenv("MH3_DISC_DIR", output.string().c_str(), 1);
    Log("game data in %s", output.string().c_str());
    // The game's native assets open at the hand-off (StartGame); textures and
    // models convert as the game loads them (disc/cache/).
    return true;
}
// Hands the renderer over to the game. The last launcher frame stays on the
// display while the game boots; the game's native graphics startup restarts
// the render thread at its own size.
void StartGame(Font& font, const std::string& what) {
    shown.clear();
    Rect(shown, 0, 0, 960, 544, ink);
    font.Text(shown, 60, 250, "Starting Monster Hunter Tri", gold, 830);
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
} // namespace
bool Run() {
    // After the hand-off the launcher no longer draws, so errors from there on
    // go to the caller instead of a dialog.
    bool handedOff = false;
    try {
        StartScreen();
        SceCommonDialogConfigParam config{};
        sceCommonDialogConfigParamInit(&config);
        sceCommonDialogSetConfigParam(&config);
        const uint32_t search[2]{};
        if (_vshKernelSearchModuleByName("kubridge", search) < 0) {
            Message("Kubridge is not installed. Install kubridge.skprx under *KERNEL in your taiHEN config.txt, then restart your PS Vita before launching Monster Hunter Tri.");
            return false;
        }
        sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
#ifdef MH3_DEVELOPMENT
        InputScript();
#endif
        Font font;
        const auto base = std::filesystem::path(std::getenv("MH3_DISC_DIR"));
        std::filesystem::create_directories(base);
        EnsureDisc(font, base);
        const auto user = std::filesystem::path(std::getenv("MH3_USER_DIR"));
        Page page = Page::Main;
        size_t selected = 0;
        bool creating = false, noDamage = false, noStamina = false;
        SaveSlot save{};
        std::vector<SaveSlot> saves;
        std::vector<QuestCatalog::Quest> quests;
        std::vector<CustomQuest> custom;
        CustomQuest edit;
        QuestCatalog::Set set = QuestCatalog::Set::Village;
        std::vector<QuestCatalog::Quest> editorQuests;
        std::vector<uint16_t> encounters, maps;
        const auto monsters = QuestCatalog::MonsterNames();
        const auto editorQuest = [&](uint16_t id) -> const QuestCatalog::Quest& {
            const auto found = std::find_if(editorQuests.begin(), editorQuests.end(), [&](const auto& q) { return q.id == id; });
            if (found == editorQuests.end())
                throw std::runtime_error("Custom quest template is unavailable");
            return *found;
        };
        const auto refreshEditor = [&] {
            editorQuests = QuestCatalog::Load(edit.set);
            encounters = CompositionOptions(edit);
            maps = CompositionMaps(edit);
        };
        Input input;
        const auto enter = [&](Page next) {
            page = next;
            selected = 0;
            Log("page %s", PageName(next));
        };
        const auto saveName = [&] { return "File " + std::to_string(save.file + 1) + " / character " + std::to_string(save.character + 1); };
        const auto start = [&](const std::string& what) {
            handedOff = true;
            StartGame(font, what);
        };
        Log("menu ready");
        while (Pump()) {
            std::string title, detail;
            std::vector<std::string> rows;
            switch (page) {
            case Page::Main:
                title = "Main menu";
                rows = {"Start game", "Quests", "Create custom quest", "Cheats"};
                detail = "Continue through the game's title and character selection";
                break;
            case Page::Categories:
                title = "Quests";
                rows = {"Offline quests", "Online quests", "Arena", "Custom quests"};
                detail = "Online quests launch locally with the selected character";
                break;
            case Page::Templates:
                title = "Choose a quest template";
                rows = {"Offline quests", "Online quests", "Arena"};
                detail = "The template supplies the map, monster spawns and rewards";
                break;
            case Page::Saves:
                title = "Choose your hunter";
                for (const auto& s : saves)
                    rows.push_back("File " + std::to_string(s.file + 1) + " / Character " + std::to_string(s.character + 1));
                detail = "Only existing characters with valid save checksums appear";
                break;
            case Page::Quests:
                title = creating ? "Choose a hunt/capture template" : "Quest list";
                for (const auto& q : quests)
                    rows.push_back(std::to_string(q.stars) + "*  " + q.name);
                if (!quests.empty())
                    detail = QuestCatalog::MapName(quests[std::min(selected, quests.size() - 1)].map) + " - " + quests[std::min(selected, quests.size() - 1)].goal;
                break;
            case Page::Custom:
                title = "Custom quests";
                for (const auto& q : custom)
                    rows.push_back(q.name);
                detail = custom.empty() ? "Create a custom quest from the main menu" : "X Launch   Triangle Edit";
                break;
            case Page::Editor: {
                const auto& baseQuest = editorQuest(edit.base);
                const auto& encounter = editorQuest(edit.encounter ? edit.encounter : edit.base);
                const auto target = encounter.objectives.front().target;
                const std::string monster = target < monsters.size() ? monsters[target] : "ID " + std::to_string(target);
                title = "Create custom quest";
                rows = {"Name: " + edit.name,
                        "Map: " + QuestCatalog::MapName(baseQuest.map) + (encounters.empty() ? " (scripted)" : ""),
                        "Monster: " + monster,
                        "Objective: " + std::string(edit.capture ? "Capture" : "Hunt"),
                        "Target count: " + std::to_string(edit.count),
                        "Time: " + (edit.minutes ? std::to_string(edit.minutes) + " minutes" : "Template"),
                        "Stars: " + std::to_string(edit.stars),
                        "Save quest"};
                detail = encounters.empty() ? "Template keeps its scripted map and monster placements" : "Placement/rewards: " + encounter.name;
                break;
            }
            case Page::Cheats:
                title = "Cheats";
                rows = {"No damage: " + std::string(noDamage ? "ON" : "OFF"), "No action stamina drain: " + std::string(noStamina ? "ON" : "OFF")};
                detail = "Native gameplay toggles apply to the current launch";
                break;
            }
            if (rows.empty())
                rows = {"No quests available"};
            selected = std::min(selected, rows.size() - 1);
            Draw(font, title, rows, selected, detail);
            const uint32_t buttons = input.Read();
            if (buttons & SCE_CTRL_UP)
                selected = (selected + rows.size() - 1) % rows.size();
            if (buttons & SCE_CTRL_DOWN)
                selected = (selected + 1) % rows.size();
            if (buttons & SCE_CTRL_LTRIGGER)
                selected = selected > 7 ? selected - 7 : 0;
            if (buttons & SCE_CTRL_RTRIGGER)
                selected = std::min(selected + 7, rows.size() - 1);
            if (buttons & SCE_CTRL_CIRCLE) {
                switch (page) {
                case Page::Quests:
                    enter(creating ? Page::Templates : Page::Categories);
                    break;
                case Page::Custom:
                    enter(Page::Categories);
                    break;
                case Page::Editor:
                    enter(Page::Main);
                    break;
                default:
                    enter(Page::Main);
                    break;
                }
                continue;
            }
            try {
                const bool accept = buttons & (SCE_CTRL_CROSS | SCE_CTRL_START);
                const int direction = (buttons & SCE_CTRL_LEFT) ? -1 : (buttons & SCE_CTRL_RIGHT) ? 1 : 0;
                if (page == Page::Editor && (direction || accept)) {
                    if (selected == 0 && accept)
                        EditName(edit.name);
                    const auto cycle = [&](size_t index, size_t count) {
                        return (index + count + (direction ? direction : 1)) % count;
                    };
                    if (selected == 1 && !encounters.empty() && !maps.empty()) {
                        const auto map = editorQuest(edit.base).map;
                        const auto found = std::find_if(maps.begin(), maps.end(), [&](auto id) { return editorQuest(id).map == map; });
                        edit.base = maps[cycle(found - maps.begin(), maps.size())];
                        edit.encounter = 0;
                        edit.count = editorQuest(edit.base).objectives.front().count;
                        edit.capture = false;
                        encounters = CompositionOptions(edit);
                    }
                    if (selected == 2 && !encounters.empty()) {
                        const auto found = std::find(encounters.begin(), encounters.end(), edit.encounter ? edit.encounter : edit.base);
                        edit.encounter = encounters[cycle(found == encounters.end() ? 0 : found - encounters.begin(), encounters.size())];
                        edit.count = editorQuest(edit.encounter).objectives.front().count;
                        if (edit.capture && !SupportsCapture(edit))
                            edit.capture = false;
                    }
                    if (selected == 3) {
                        if (!edit.capture && !SupportsCapture(edit))
                            throw std::runtime_error("This target/placement has no verified capture support; keep the hunt objective");
                        edit.capture = !edit.capture;
                    }
                    if (selected == 4)
                        edit.count = std::clamp<int>(int(edit.count) + (direction ? direction : 1), 1, editorQuest(edit.encounter ? edit.encounter : edit.base).objectives.front().count);
                    if (selected == 5)
                        edit.minutes = std::clamp<int>(int(edit.minutes) + (direction ? direction : 1), 0, 50);
                    if (selected == 6)
                        edit.stars = std::clamp<int>(int(edit.stars) + (direction ? direction : 1), 1, 7);
                    if (selected == 7 && accept) {
                        SaveCustomQuest(user, edit);
                        Log("custom quest saved: %s (template %u, encounter %u, %u stars)", edit.name.c_str(), unsigned(edit.base), unsigned(edit.encounter), unsigned(edit.stars));
                        Message("Custom quest saved. Open Quests > Custom quests to play it.");
                        enter(Page::Main);
                    }
                    continue;
                }
                if (page == Page::Custom && !custom.empty() && (buttons & SCE_CTRL_TRIANGLE)) {
                    edit = custom[selected];
                    refreshEditor();
                    enter(Page::Editor);
                    continue;
                }
                if (!accept)
                    continue;
                switch (page) {
                case Page::Main:
                    if (selected == 0) {
                        start("title screen");
                        return true;
                    }
                    if (selected == 1) {
                        creating = false;
                        saves = ProbeSaves(user);
                        if (saves.empty()) {
                            Message("No usable MH3 hunter save was found. Select Start game and create a character through the game's normal save screens, or copy your existing RMHP08 NAND save. "
                                    "Quests require an existing valid character.");
                            break;
                        }
                        save = saves.front();
                        enter(saves.size() > 1 ? Page::Saves : Page::Categories);
                    } else if (selected == 2) {
                        creating = true;
                        enter(Page::Templates);
                    } else
                        enter(Page::Cheats);
                    break;
                case Page::Saves:
                    save = saves[selected];
                    enter(Page::Categories);
                    break;
                case Page::Categories:
                case Page::Templates:
                    if (selected == 3) {
                        custom = LoadCustomQuests(user);
                        enter(Page::Custom);
                        break;
                    }
                    set = QuestCatalog::Set(selected);
                    quests = QuestCatalog::Load(set);
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
                        edit = {"Custom " + q.name.substr(0, 36), set, q.id, q.objectives[0].count, q.objectives[0].type == 0x81, q.stars};
                        refreshEditor();
                        enter(Page::Editor);
                    } else {
                        const auto& q = quests[selected];
                        StartVillageBoot(save.file, save.character);
                        RequestQuestJump(q.id, set, -1);
                        start("quest " + std::to_string(q.id) + " " + q.name + ", " + saveName());
                        return true;
                    }
                    break;
                case Page::Custom:
                    if (!custom.empty()) {
                        const auto& q = custom[selected];
                        StartVillageBoot(save.file, save.character);
                        try {
                            ActivateCustomQuest(q);
                        } catch (...) {
                            FinishVillageBoot();
                            throw;
                        }
                        RequestQuestJump(q.base, q.set, -1);
                        start("custom quest " + q.name + " on template " + std::to_string(q.base) + ", " + saveName());
                        return true;
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
            } catch (const std::exception& error) {
                if (handedOff)
                    throw;
                Message(error.what());
            }
        }
        return false;
    } catch (const std::exception& error) {
        if (handedOff)
            throw;
        Message(error.what());
        return false;
    }
}
} // namespace VitaLauncher
