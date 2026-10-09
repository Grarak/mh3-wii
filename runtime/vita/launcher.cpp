#include "vita_launcher.h"
#include "launcher_font.h"
#include "launcher_cheats.h"
#include "native_graphics.h"
#include "player_health.h"
#include "village_boot.h"
#include "field_jump.h"
#include "disc_image.h"
#include <aurora/aurora.h>
#include <aurora/event.h>
#include <aurora/native_geometry.h>
#include <aurora/thread_role.hpp>
#include "gl/gl.hpp"
#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/message_dialog.h>
#include <psp2/ime_dialog.h>
#include <psp2/vshbridge.h>
#include <psp2/sysmodule.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

namespace VitaLauncher {
namespace {
constexpr uint32_t ink = 0xff28302c, paper = 0xffc8d7d8, gold = 0xff73b9de, light = 0xffe1ebed;
bool Pump() {
    for (const auto* e = aurora_update(); e && e->type != AURORA_NONE; ++e)
        if (e->type == AURORA_EXIT)
            return false;
    return true;
}
void Rect(std::vector<Rectangle>& list, float x, float y, float w, float h, uint32_t color) {
    list.push_back({x, y, w, h, color});
}
void Submit(const std::vector<Rectangle>& rectangles) {
    mh3_raster_state raster{};
    raster.depth_compare = 7;
    raster.color_write = raster.alpha_write = 1;
    SetNativeRaster(raster);
    // Write once directly into the admitted CDRAM slot. Individual rectangles
    // share a draw; splitting below 16-bit index limits preserves draw order.
    for (size_t base = 0; base < rectangles.size(); base += 12000) {
        const size_t count = std::min<size_t>(12000, rectangles.size() - base);
        AuroraNativeGeometry geometry{};
        if (!aurora_allocate_native_geometry(count * 4, count * 6, &geometry))
            return;
        for (size_t i = 0; i < count; ++i) {
            const auto& r = rectangles[base + i];
            for (unsigned corner = 0; corner < 4; ++corner) {
                auto& v = geometry.vertices[i * 4 + corner];
                v.position[0] = (r.x + (corner & 1 ? r.w : 0)) / 480.f - 1;
                v.position[1] = 1 - (r.y + (corner & 2 ? r.h : 0)) / 272.f;
                v.position[2] = 0;
                v.position[3] = 1;
                v.uv[0] = v.uv[1] = 0;
                for (unsigned channel = 0; channel < 4; ++channel)
                    v.color[channel] = r.color >> (8 * channel);
            }
            const uint16_t first = i * 4;
            const uint16_t indices[] = {first, uint16_t(first + 1), uint16_t(first + 2), uint16_t(first + 2), uint16_t(first + 1), uint16_t(first + 3)};
            std::copy(std::begin(indices), std::end(indices), geometry.indices + i * 6);
        }
        aurora_submit_native_geometry(&geometry, 0);
    }
}
void BlankFrame() {
    BeginNativeGraphicsFrame();
    EndNativeGraphicsFrame();
    sceKernelDelayThread(8000);
}
void Message(const std::string& text) {
    SceMsgDialogUserMessageParam user{};
    user.buttonType = SCE_MSG_DIALOG_BUTTON_TYPE_OK;
    user.msg = reinterpret_cast<const SceChar8*>(text.c_str());
    SceMsgDialogParam param{};
    sceMsgDialogParamInit(&param);
    param.mode = SCE_MSG_DIALOG_MODE_USER_MSG;
    param.userMsgParam = &user;
    if (sceMsgDialogInit(&param) < 0) {
        std::fprintf(stderr, "MH3 launcher: %s\n", text.c_str());
        return;
    }
    aurora_set_common_dialog_active(true);
    while (sceMsgDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED && Pump())
        BlankFrame();
    aurora_set_common_dialog_active(false);
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
    aurora_set_common_dialog_active(true);
    while (sceImeDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED && Pump())
        BlankFrame();
    SceImeDialogResult result{};
    sceImeDialogGetResult(&result);
    aurora_set_common_dialog_active(false);
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
        return edges;
    }

  private:
    uint32_t previous = 0, lastDirection = 0;
    uint64_t nextRepeat = 0;
};
void Draw(Font& font, const std::string& title, const std::vector<std::string>& rows, size_t selected, const std::string& detail,
          const std::string& footer = "X Select   O Back   D-pad / left stick Move") {
    const bool render = BeginNativeGraphicsFrame();
    if (render) {
        std::vector<Rectangle> rects;
        rects.reserve(18000);
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
        Submit(rects);
    }
    EndNativeGraphicsFrame();
    sceKernelDelayThread(8000);
}
bool EnsureDisc(Font& font, const std::filesystem::path& base) {
    auto output = base;
    if (DiscReady(output)) {
        aurora::gl::open_native_texture_pack((output / "native-textures.bin").string().c_str());
        return true;
    }
    if (!std::filesystem::exists(base / "sys/boot.bin"))
        output = base / "disc";
    if (DiscReady(output)) {
        setenv("MH3_DISC_DIR", output.string().c_str(), 1);
        aurora::gl::open_native_texture_pack((output / "native-textures.bin").string().c_str());
        return true;
    }
    std::filesystem::path iso;
    if (!std::filesystem::exists(base / "sys/boot.bin")) {
        for (const auto& entry : std::filesystem::directory_iterator(base))
            if (entry.is_regular_file() && (entry.path().extension() == ".iso" || entry.path().extension() == ".rvz" || entry.path().extension() == ".wia")) {
                if (!iso.empty())
                    throw std::runtime_error("Place exactly one RMHP08 disc image in " + base.string());
                iso = entry.path();
            }
        if (iso.empty())
            throw std::runtime_error("Game data is missing. Place your European RMHP08 ISO or RVZ in " + base.string() + ". It will be processed automatically on the next launch.");
    }
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
            if (iso.empty())
                PrepareTextures(base, progress);
            else
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
        if (BeginNativeGraphicsFrame()) {
            std::vector<Rectangle> rects;
            Rect(rects, 0, 0, 960, 544, ink);
            Rect(rects, 120, 178, 720, 184, paper);
            font.Text(rects, 150, 203, "Preparing your game", ink);
            font.Text(rects, 150, 243, label, ink, 650);
            Rect(rects, 150, 288, 660, 25, ink);
            Rect(rects, 153, 291, 654 * percent / 100.f, 19, gold);
            font.Text(rects, 150, 327, std::to_string(percent) + "%  Keep the application open", ink);
            Submit(rects);
        }
        EndNativeGraphicsFrame();
        Pump();
        sceKernelDelayThread(12000);
    }
    worker.join();
    if (!error.empty())
        throw std::runtime_error(error);
    if (!DiscReady(output))
        throw std::runtime_error("Prepared game data did not pass validation");
    setenv("MH3_DISC_DIR", output.string().c_str(), 1);
    aurora::gl::open_native_texture_pack((output / "native-textures.bin").string().c_str());
    return true;
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
} // namespace
bool Run() {
    try {
        InitializeNativeGraphics(960, 544, false);
        SceCommonDialogConfigParam config{};
        sceCommonDialogConfigParamInit(&config);
        sceCommonDialogSetConfigParam(&config);
        const uint32_t search[2]{};
        if (_vshKernelSearchModuleByName("kubridge", search) < 0) {
            Message("Kubridge is not installed. Install kubridge.skprx under *KERNEL in your taiHEN config.txt, then restart your PS Vita before launching Monster Hunter Tri.");
            return false;
        }
        sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
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
        };
        const auto launch = [&](uint16_t id, QuestCatalog::Set category) {
            StartVillageBoot(save.file, save.character);
            RequestQuestJump(id, category, -1);
        };
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
                    detail = QuestCatalog::MapName(quests[selected].map) + " - " + quests[selected].goal;
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
                    if (selected == 0)
                        return true;
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
                        launch(quests[selected].id, set);
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
                    break;
                case Page::Editor:
                    break;
                }
            } catch (const std::exception& error) {
                Message(error.what());
            }
        }
        return false;
    } catch (const std::exception& error) {
        Message(error.what());
        return false;
    }
}
} // namespace VitaLauncher
