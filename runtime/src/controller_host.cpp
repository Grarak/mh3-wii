#include "controller_host.h"
#include "fatal.h"
#include "dev_options.h"
#include "../native/mh3_classic_map.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <stdexcept>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>
#if defined(MH3_HAS_AURORA) && !defined(__vita__)
#include <SDL3/SDL.h>
#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#endif
#endif
namespace {
std::optional<ClassicControllerSample> testSample;
std::optional<std::pair<float, float>> testPointer;

// Game time in 30 Hz frames (fractional) and paced frames so far.
double inputTime = 0;
uint64_t pacedFrame = 0;
bool playbackHeld = false;

// The debug port's mouse motion, a share each paced frame.
struct InjectedMotion {
    float dx = 0, dy = 0;
    uint32_t frames = 0;
    uint64_t frame = 0;
} injectedMotion;

uint64_t motionFrame = 0;

// Off while the launcher's menus take the mouse (SetMouseLook).
std::atomic<bool> mouseLook{true};
#if defined(MH3_HAS_AURORA) && !defined(__vita__)
// The window's mouse capture (relative mode): `freed` while Left Alt frees the
// cursor, `swallow` the click that captured it until its button is let go.
// The camera takes no motion until the paced frame after that: browsers
// report the cursor's jump into the pointer lock as the movement of the
// capturing click's release.
struct MouseCapture {
    bool freed = false, swallow = false;
    uint64_t settledFrame = 0;
} capture;

// Clicks since the last controller read: SDL's event watch sees clicks
// shorter than a frame, which the polled button state misses. A captured
// click holds its button for 0.1 s of game time, longer than a step at any
// rate.
std::atomic<bool> clickPending{false}, rightClickPending{false};
double leftHeldTo = -1, rightHeldTo = -1;

// Clicks while L is held: the item bar's scroll (TakeItemBarScroll).
int itemBarScroll = 0;

bool SDLCALL WatchMouse(void*, SDL_Event* event) {
    if (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN && event->button.button == SDL_BUTTON_LEFT)
        clickPending.store(true, std::memory_order_relaxed);

    if (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN && event->button.button == SDL_BUTTON_RIGHT)
        rightClickPending.store(true, std::memory_order_relaxed);

    return true;
}

#ifdef __EMSCRIPTEN__
// The page's pointer lock (mh3-web.js): SDL keeps its relative mode when the
// browser drops the lock (Esc), so captured also needs the page's word that
// the lock holds.
std::atomic<bool> webPointerLocked{false};
#endif
bool Captured(SDL_Window* window) {
#ifdef __EMSCRIPTEN__
    if (!webPointerLocked.load(std::memory_order_relaxed))
        return false;
#endif
    return window && SDL_GetWindowRelativeMouseMode(window);
}
#endif

// MH3_INPUT=[file=PATH][,record=PATH][,playback=PATH][,camera=N]: scripted controller
// input. file: a one-sample control file, read every frame, replaces the host
// controller. record and playback: input logs for repeatable runs; each line
// is "frame buttons(hex) lx ly rx ry", the sample in effect from that game
// time on, in 30 Hz frames (fractional, so changes closer than a frame keep
// their order and spacing; whole frames as older logs have them work too).
// record writes one line per change of the host input; playback replaces the
// host input with a log. camera=N[:D]
// (development builds): the field camera looks at monster N, from D units
// away (field_camera.cpp).
const DevOptions& InputOptions() {
    static const DevOptions options("MH3_INPUT", {"file", "record", "playback", "camera"});
    return options;
}

bool SameInput(const ClassicControllerSample& a, const ClassicControllerSample& b) {
    return a.buttons == b.buttons && a.leftX == b.leftX && a.leftY == b.leftY && a.rightX == b.rightX && a.rightY == b.rightY;
}

void RecordInput(const ClassicControllerSample& sample) {
    static const char* path = InputOptions().Value("record");

    if (!path)
        return;

    static std::ofstream log(path);
    static std::optional<ClassicControllerSample> last;

    if (last && SameInput(*last, sample))
        return;

    last = sample;
    log << std::fixed << std::setprecision(3) << inputTime << ' ' << std::hex << sample.buttons << std::dec << ' ' << sample.leftX << ' ' << sample.leftY << ' ' << sample.rightX << ' ' << sample.rightY << std::endl;
}

std::optional<ClassicControllerSample> PlaybackInput() {
    static const char* path = InputOptions().Value("playback");

    if (!path)
        return std::nullopt;

    struct Entry {
        double frame;
        ClassicControllerSample sample;
    };

    static const std::vector<Entry> entries = [] {
        std::vector<Entry> result;
        std::ifstream input(path);
        double frame;
        unsigned buttons;
        int lx, ly, rx, ry;

        while (input >> frame >> std::hex >> buttons >> std::dec >> lx >> ly >> rx >> ry) {
            if (!result.empty() && frame < result.back().frame)
                MH3_THROW(std::runtime_error("MH3_INPUT playback frames must not decrease"));

            result.push_back({frame, {true, uint16_t(buttons), int16_t(lx), int16_t(ly), int16_t(rx), int16_t(ry)}});
        }

        if (!input.eof() || result.empty())
            MH3_THROW(std::runtime_error("Invalid MH3_INPUT playback log"));

        return result;
    }();
    static size_t next = 0;

    // While held (the village shortcut still navigating), the log does not
    // reach its next entry; it resumes from there, later by the time held.
    static double delay = 0;
    double frame = inputTime - delay;

    if (playbackHeld && next < entries.size() && entries[next].frame != 0 && entries[next].frame <= frame) {
        delay += frame - (entries[next].frame - 1);
        frame = entries[next].frame - 1;
    }

    static double reportedDelay = 0;

    if (!playbackHeld && delay != reportedDelay) {
        std::fprintf(stderr, "MH3 input playback: held %.1f frames for the village shortcut\n", delay - reportedDelay);
        reportedDelay = delay;
    }
    while (next < entries.size() && entries[next].frame <= frame)
        ++next;

    if (next == entries.size()) {
        static bool reported = false;

        if (!reported)
            std::fprintf(stderr, "MH3 input playback: finished at frame %.1f\n", frame);

        reported = true;
    }

    if (next == 0)
        return ClassicControllerSample{true};

    return entries[next - 1].sample;
}
} // namespace
#if defined(MH3_HAS_AURORA) && defined(__EMSCRIPTEN__)
extern "C" EMSCRIPTEN_KEEPALIVE void mh3_web_pointer_locked(int locked) {
    webPointerLocked.store(locked != 0, std::memory_order_relaxed);
}
#endif
#ifdef MH3_DEVELOPMENT
int DebugCameraMonster(float* distance) {
    static float eyeDistance = 0.0f;
    static const int monster = [] {
        const char* value = InputOptions().Value("camera");

        if (!value)
            return -1;

        if (const char* colon = std::strchr(value, ':'))
            eyeDistance = float(std::atof(colon + 1));

        return std::atoi(value);
    }();

    if (distance)
        *distance = eyeDistance;

    return monster;
}
#endif

void SetControllerTestSample(std::optional<ClassicControllerSample> sample) {
    testSample = sample;
}

void SetPointerTestSample(std::optional<std::pair<float, float>> point) {
    testPointer = point;
}

void SetInputTime(double frames) {
    inputTime = frames;
    ++pacedFrame;
}

void HoldInputPlayback(bool hold) {
    playbackHeld = hold;
}

ClassicControllerSample ReadHostController() {
    ClassicControllerSample sample;

    if (testSample) {
        sample = *testSample;
    } else if (auto played = PlaybackInput()) {
        // The logs hold the Classic report: the pointer stays live.
        const ClassicControllerSample live = ReadLiveController();
        sample = *played;
        sample.pointing = live.pointing;
        sample.pointX = live.pointX;
        sample.pointY = live.pointY;
        RecordInput(sample);
    } else {
        sample = ReadLiveController();
        RecordInput(sample);
    }

    if (testPointer) {
        sample.pointing = true;
        sample.pointX = testPointer->first;
        sample.pointY = testPointer->second;
    }

    return sample;
}

MouseMotion TakeMouseMotion() {
    MouseMotion motion;

    // Motion gathered while nothing took it (a menu, a cutscene) is old.
    const bool fresh = pacedFrame <= motionFrame + 1;
    motionFrame = pacedFrame;
#if defined(MH3_HAS_AURORA) && !defined(__vita__)
    if (SDL_WasInit(SDL_INIT_VIDEO)) {
        float x = 0, y = 0;
        SDL_GetRelativeMouseState(&x, &y);

        if (fresh && Captured(SDL_GetKeyboardFocus()) && !capture.swallow && pacedFrame > capture.settledFrame) {
            motion.dx = x;
            motion.dy = y;
        }
#ifdef MH3_DEVELOPMENT
        // MH3_TRACE=input: motion the camera did not take, and why.
        static const TraceFlag trace("input");

        if (trace && (x != 0 || y != 0) && motion.dx == 0 && motion.dy == 0)
            std::fprintf(stderr, "MH3 mouse: dropped %.1f %.1f (%s)\n", x, y,
                         !fresh                                  ? "stale"
                         : !Captured(SDL_GetKeyboardFocus())    ? "not captured"
                         : capture.swallow                       ? "capturing click"
                                                                 : "settling");
#endif
    }
#else
    (void)fresh;
#endif
    // The injection's share for each paced frame since it was added.
    auto& injected = injectedMotion;

    for (; injected.frames && injected.frame < pacedFrame; ++injected.frame, --injected.frames) {
        motion.dx += injected.dx;
        motion.dy += injected.dy;
    }

    return motion;
}

int TakeItemBarScroll() {
#if defined(MH3_HAS_AURORA) && !defined(__vita__)
    return std::exchange(itemBarScroll, 0);
#else
    return 0;
#endif
}

void AddMouseMotion(float dx, float dy, uint32_t frames) {
    frames = std::max<uint32_t>(frames, 1);
    injectedMotion = {dx / float(frames), dy / float(frames), frames, pacedFrame};
}

float MouseSensitivity() {
    static const float sensitivity = [] {
        const char* value = std::getenv("MH3_MOUSE_SENSITIVITY");
        const float n = value ? std::strtof(value, nullptr) : 1.0f;
        return n > 0.0f ? n : 1.0f;
    }();
    return sensitivity;
}

void SetMouseLook(bool enabled) {
    mouseLook.store(enabled, std::memory_order_relaxed);
#if defined(MH3_HAS_AURORA) && defined(__EMSCRIPTEN__)
    // The page asks for the pointer lock on a click only while it holds
    // (mh3-web.js).
    MAIN_THREAD_ASYNC_EM_ASM({ Module['mh3MouseLook'] = $0 != 0; }, enabled);
#endif
}

bool MouseInvertY() {
    static const bool invert = [] {
        const char* value = std::getenv("MH3_MOUSE_INVERT_Y");
        return value && std::strcmp(value, "0") != 0;
    }();
    return invert;
}

ClassicControllerSample ReadLiveController() {
    ClassicControllerSample sample;

    if (const char* path = InputOptions().Value("file")) {
        // Explicit diagnostic controller; the normal source is SDL. This lets
        // unattended runs exercise the same guest input path while macOS is locked.
        std::ifstream input(path);
        unsigned buttons;
        int lx, ly, rx, ry;

        if (!(input >> std::hex >> buttons >> std::dec >> lx >> ly >> rx >> ry) || buttons > 0xFFFF || lx < -512 || lx > 511 || ly < -512 || ly > 511 || rx < -512 || rx > 511 || ry < -512 || ry > 511)
            MH3_THROW(std::runtime_error("Invalid MH3_INPUT file: expected hex buttons and four signed axes (-512..511)"));

        sample = {true, uint16_t(buttons), int16_t(lx), int16_t(ly), int16_t(rx), int16_t(ry)};
        return sample;
    }
#if defined(__vita__)
    (void)sample;
    return ReadVitaController();
#elif defined(MH3_HAS_AURORA)
    // The keyboard and the first SDL gamepad, as the Classic Controller on
    // channel 0 (docs/optimizations/system/controller-input.md, docs/vita-controls.md).
    sample.connected = true;

    if (!SDL_WasInit(SDL_INIT_VIDEO))
        return sample;

    if (!SDL_WasInit(SDL_INIT_GAMEPAD) && !SDL_InitSubSystem(SDL_INIT_GAMEPAD))
        MH3_THROW(std::runtime_error(std::string("SDL controller initialization: ") + SDL_GetError()));

    const bool* keys = SDL_GetKeyboardState(nullptr);

    if (SDL_GetKeyboardFocus()) {
        // Monster Hunter: World's keyboard defaults where the game has the
        // action: Space evades (B), Shift dashes (R), E uses an item or
        // sheathes (Y), F talks and gathers (A), Q resets the camera (L),
        // Esc pauses (+), Tab the quest menu (-).
        static const struct {
            SDL_Scancode key;
            uint16_t button;
        } bindings[] = {
            {SDL_SCANCODE_SPACE, MH3_CL_B},        {SDL_SCANCODE_LSHIFT, MH3_CL_R},    {SDL_SCANCODE_E, MH3_CL_Y},
            {SDL_SCANCODE_F, MH3_CL_A},            {SDL_SCANCODE_Q, MH3_CL_L},         {SDL_SCANCODE_RETURN, MH3_CL_A},
            {SDL_SCANCODE_BACKSPACE, MH3_CL_B},    {SDL_SCANCODE_Z, MH3_CL_ZL},        {SDL_SCANCODE_C, MH3_CL_ZR},
            {SDL_SCANCODE_ESCAPE, MH3_CL_PLUS},    {SDL_SCANCODE_TAB, MH3_CL_MINUS},   {SDL_SCANCODE_M, MH3_CL_MINUS},
            {SDL_SCANCODE_HOME, MH3_CL_HOME},      {SDL_SCANCODE_UP, MH3_CL_UP},       {SDL_SCANCODE_DOWN, MH3_CL_DOWN},
            {SDL_SCANCODE_LEFT, MH3_CL_LEFT},      {SDL_SCANCODE_RIGHT, MH3_CL_RIGHT},
        };

        for (const auto& binding : bindings)
            if (keys[binding.key])
                sample.buttons |= binding.button;
        sample.leftX = int16_t((int(keys[SDL_SCANCODE_D]) - int(keys[SDL_SCANCODE_A])) * 511);
        sample.leftY = int16_t((int(keys[SDL_SCANCODE_W]) - int(keys[SDL_SCANCODE_S])) * 511);
        sample.rightX = int16_t((int(keys[SDL_SCANCODE_L]) - int(keys[SDL_SCANCODE_J])) * 511);
        sample.rightY = int16_t((int(keys[SDL_SCANCODE_I]) - int(keys[SDL_SCANCODE_K])) * 511);
    }

    static SDL_Gamepad* pad = nullptr;

    if (pad && !SDL_GamepadConnected(pad)) {
        SDL_CloseGamepad(pad);
        pad = nullptr;
    }

    if (!pad) {
        int count = 0;
        auto* ids = SDL_GetGamepads(&count);

        if (ids && count)
            pad = SDL_OpenGamepad(ids[0]);

        SDL_free(ids);
    }

    if (pad) {
        // Face buttons by position (east = A, south = B, north = X, west =
        // Y). The analog triggers are the Classic L/R triggers, the bumpers
        // ZL/ZR, as on the original Classic Controller.
        static const struct {
            SDL_GamepadButton source;
            uint16_t button;
        } bindings[] = {
            {SDL_GAMEPAD_BUTTON_EAST, MH3_CL_A},           {SDL_GAMEPAD_BUTTON_SOUTH, MH3_CL_B},          {SDL_GAMEPAD_BUTTON_NORTH, MH3_CL_X},
            {SDL_GAMEPAD_BUTTON_WEST, MH3_CL_Y},           {SDL_GAMEPAD_BUTTON_DPAD_UP, MH3_CL_UP},       {SDL_GAMEPAD_BUTTON_DPAD_DOWN, MH3_CL_DOWN},
            {SDL_GAMEPAD_BUTTON_DPAD_LEFT, MH3_CL_LEFT},   {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, MH3_CL_RIGHT}, {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, MH3_CL_ZL},
            {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, MH3_CL_ZR}, {SDL_GAMEPAD_BUTTON_START, MH3_CL_PLUS},      {SDL_GAMEPAD_BUTTON_BACK, MH3_CL_MINUS},
            {SDL_GAMEPAD_BUTTON_GUIDE, MH3_CL_HOME},
        };

        for (const auto& binding : bindings)
            if (SDL_GetGamepadButton(pad, binding.source))
                sample.buttons |= binding.button;
        const auto axis = [&](SDL_GamepadAxis source) {
            return std::clamp(SDL_GetGamepadAxis(pad, source) / 32767.0f, -1.0f, 1.0f);
        };

        // SDL's y axes point down; the Classic report's point up.
        if (!sample.leftX && !sample.leftY)
            mh3_classic_stick(axis(SDL_GAMEPAD_AXIS_LEFTX), -axis(SDL_GAMEPAD_AXIS_LEFTY), &mh3_gamepad_stick_curve, &sample.leftX, &sample.leftY);

        if (!sample.rightX && !sample.rightY)
            mh3_classic_stick(axis(SDL_GAMEPAD_AXIS_RIGHTX), -axis(SDL_GAMEPAD_AXIS_RIGHTY), &mh3_gamepad_stick_curve, &sample.rightX, &sample.rightY);

        sample.triggerL = uint8_t(std::lround(std::max(0.0f, axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER)) * 255));
        sample.triggerR = uint8_t(std::lround(std::max(0.0f, axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)) * 255));

        // The Classic's digital L/R close at the end of the trigger's travel;
        // half way is kinder to worn gamepad triggers.
        if (sample.triggerL > 127)
            sample.buttons |= MH3_CL_L;

        if (sample.triggerR > 127)
            sample.buttons |= MH3_CL_R;
    }

    // A digital L/R (keyboard) is a fully pressed trigger.
    if (sample.buttons & MH3_CL_L)
        sample.triggerL = 255;

    if (sample.buttons & MH3_CL_R)
        sample.triggerR = 255;

    // A click in the window captures the mouse for the camera
    // (TakeMouseMotion); Left Alt held frees the cursor. Captured, the left
    // and right buttons attack (X, A), or scroll the item bar while L is held;
    // free, the left button over the game's picture is the Wii Remote's
    // pointer.
    static bool watching = false;

    if (!watching)
        watching = SDL_AddEventWatch(WatchMouse, nullptr);

    float x = 0, y = 0;
    SDL_Window* window = SDL_GetKeyboardFocus();
    const SDL_MouseButtonFlags mouse = SDL_GetMouseState(&x, &y);
    const bool alt = window && keys[SDL_SCANCODE_LALT];
    const bool clicked = clickPending.exchange(false, std::memory_order_relaxed);

    const bool rightClicked = rightClickPending.exchange(false, std::memory_order_relaxed);

    // L held (Q, or a gamepad's trigger) opens the item bar: there the clicks
    // scroll it (item_bar.cpp) instead of attacking.
    const bool itemBar = sample.buttons & MH3_CL_L;

    if (Captured(window) && alt) {
        SDL_SetWindowRelativeMouseMode(window, false);
        capture.freed = true;
    } else if (window && !alt && !Captured(window) && mouseLook.load(std::memory_order_relaxed) && (capture.freed || (clicked && SDL_GetMouseFocus() == window))) {
        capture.swallow = !capture.freed;
        capture.freed = false;
        SDL_SetWindowRelativeMouseMode(window, true);
    } else if (Captured(window) && itemBar) {
        itemBarScroll += int(rightClicked) - int(clicked);
    } else if (Captured(window)) {
        if (clicked)
            leftHeldTo = inputTime + 3;

        if (rightClicked)
            rightHeldTo = inputTime + 3;
    }

    if (!itemBar)
        itemBarScroll = 0;

    if (capture.swallow && !(mouse & SDL_BUTTON_LMASK)) {
        capture.swallow = false;
        capture.settledFrame = pacedFrame + 1;
    }
#ifdef MH3_DEVELOPMENT
    static bool reported = false;

    if (Captured(window) != reported) {
        reported = Captured(window);
        std::fprintf(stderr, "MH3 mouse: %s\n", reported ? "captured for the camera" : "freed");
    }
#endif

    if (Captured(window) && !itemBar) {
        if (((mouse & SDL_BUTTON_LMASK) && !capture.swallow) || inputTime <= leftHeldTo)
            sample.buttons |= MH3_CL_X;

        if ((mouse & SDL_BUTTON_RMASK) || inputTime <= rightHeldTo)
            sample.buttons |= MH3_CL_A;
    } else if (SDL_Window* over = SDL_GetMouseFocus(); over && (mouse & SDL_BUTTON_LMASK)) {
        const float density = SDL_GetWindowPixelDensity(over);
        sample.pointing = PointOnPicture(x * density, y * density, sample.pointX, sample.pointY);
    }
#endif
    return sample;
}
