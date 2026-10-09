#include "controller_host.h"
#include "../native/mh3_classic_map.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <cstdio>
#include <string>
#include <vector>
#if defined(MH3_HAS_AURORA) && !defined(__vita__)
#include <SDL3/SDL.h>
#endif
namespace {
std::optional<ClassicControllerSample> testSample;
#ifdef MH3_DEVELOPMENT
uint64_t inputFrame = 0;
bool playbackHeld = false;

// Frame-indexed input logs for repeatable runs. Each line is
// "frame buttons(hex) lx ly rx ry": the sample in effect from that paced
// frame on. MH3_INPUT_RECORD writes one line per change of the host input;
// MH3_INPUT_PLAYBACK replaces the host input with a log.
bool SameInput(const ClassicControllerSample& a, const ClassicControllerSample& b) {
    return a.buttons == b.buttons && a.leftX == b.leftX && a.leftY == b.leftY && a.rightX == b.rightX && a.rightY == b.rightY;
}

void RecordInput(const ClassicControllerSample& sample) {
    static const char* path = std::getenv("MH3_INPUT_RECORD");

    if (!path)
        return;

    static std::ofstream log(path);
    static std::optional<ClassicControllerSample> last;

    if (last && SameInput(*last, sample))
        return;

    last = sample;
    log << inputFrame << ' ' << std::hex << sample.buttons << std::dec << ' ' << sample.leftX << ' ' << sample.leftY << ' ' << sample.rightX << ' ' << sample.rightY << std::endl;
}

std::optional<ClassicControllerSample> PlaybackInput() {
    static const char* path = std::getenv("MH3_INPUT_PLAYBACK");

    if (!path)
        return std::nullopt;

    struct Entry {
        uint64_t frame;
        ClassicControllerSample sample;
    };

    static const std::vector<Entry> entries = [] {
        std::vector<Entry> result;
        std::ifstream input(path);
        uint64_t frame;
        unsigned buttons;
        int lx, ly, rx, ry;

        while (input >> frame >> std::hex >> buttons >> std::dec >> lx >> ly >> rx >> ry) {
            if (!result.empty() && frame < result.back().frame)
                throw std::runtime_error("MH3_INPUT_PLAYBACK frames must not decrease");

            result.push_back({frame, {true, uint16_t(buttons), int16_t(lx), int16_t(ly), int16_t(rx), int16_t(ry)}});
        }

        if (!input.eof() || result.empty())
            throw std::runtime_error("Invalid MH3_INPUT_PLAYBACK log");

        return result;
    }();
    static size_t next = 0;

    // While held (the village shortcut still navigating), the log does not
    // reach its next entry; it resumes from there, later by the time held.
    static uint64_t delay = 0;
    uint64_t frame = inputFrame - delay;

    if (playbackHeld && next < entries.size() && entries[next].frame != 0 && entries[next].frame <= frame) {
        delay += frame - (entries[next].frame - 1);
        frame = entries[next].frame - 1;
    }

    static uint64_t reportedDelay = 0;

    if (!playbackHeld && delay != reportedDelay) {
        std::fprintf(stderr, "MH3 input playback: held %llu frames for the village shortcut\n", (unsigned long long)(delay - reportedDelay));
        reportedDelay = delay;
    }
    while (next < entries.size() && entries[next].frame <= frame)
        ++next;

    if (next == entries.size()) {
        static bool reported = false;

        if (!reported)
            std::fprintf(stderr, "MH3 input playback: finished at frame %llu\n", (unsigned long long)frame);

        reported = true;
    }

    if (next == 0)
        return ClassicControllerSample{true};

    return entries[next - 1].sample;
}
#endif
} // namespace

void SetControllerTestSample(std::optional<ClassicControllerSample> sample) {
    testSample = sample;
}
#ifdef MH3_DEVELOPMENT
void SetInputFrame(uint64_t frame) {
    inputFrame = frame;
}

void HoldInputPlayback(bool hold) {
    playbackHeld = hold;
}

ClassicControllerSample ReadHostController() {
    if (testSample)
        return *testSample;

    if (auto played = PlaybackInput()) {
        RecordInput(*played);
        return *played;
    }

    const ClassicControllerSample sample = ReadLiveController();
    RecordInput(sample);
    return sample;
}
#else
void SetInputFrame(uint64_t) {}
void HoldInputPlayback(bool) {}
ClassicControllerSample ReadHostController() {
    if (testSample)
        return *testSample;

    return ReadLiveController();
}
#endif
ClassicControllerSample ReadLiveController() {
    ClassicControllerSample sample;
#ifdef MH3_DEVELOPMENT
    if (const char* path = std::getenv("MH3_INPUT_FILE")) {
        // Explicit diagnostic controller; the normal source is SDL. This lets
        // unattended runs exercise the same guest input path while macOS is locked.
        std::ifstream input(path);
        unsigned buttons;
        int lx, ly, rx, ry;

        if (!(input >> std::hex >> buttons >> std::dec >> lx >> ly >> rx >> ry) || buttons > 0xFFFF || lx < -512 || lx > 511 || ly < -512 || ly > 511 || rx < -512 || rx > 511 || ry < -512 || ry > 511)
            throw std::runtime_error("Invalid MH3_INPUT_FILE: expected hex buttons and four signed axes (-512..511)");

        sample = {true, uint16_t(buttons), int16_t(lx), int16_t(ly), int16_t(rx), int16_t(ry)};
        return sample;
    }
#endif
#if defined(__vita__)
    (void)sample;
    return ReadVitaController();
#elif defined(MH3_HAS_AURORA)
    // The keyboard and the first SDL gamepad, as the Classic Controller on
    // channel 0 (docs/wpad-bridge.md, docs/vita-controls.md).
    sample.connected = true;

    if (!SDL_WasInit(SDL_INIT_VIDEO))
        return sample;

    if (!SDL_WasInit(SDL_INIT_GAMEPAD) && !SDL_InitSubSystem(SDL_INIT_GAMEPAD))
        throw std::runtime_error(std::string("SDL controller initialization: ") + SDL_GetError());

    const bool* keys = SDL_GetKeyboardState(nullptr);

    if (SDL_GetKeyboardFocus()) {
        static const struct {
            SDL_Scancode key;
            uint16_t button;
        } bindings[] = {
            {SDL_SCANCODE_SPACE, MH3_CL_A},       {SDL_SCANCODE_LSHIFT, MH3_CL_B},     {SDL_SCANCODE_E, MH3_CL_X},
            {SDL_SCANCODE_F, MH3_CL_Y},           {SDL_SCANCODE_Q, MH3_CL_L},          {SDL_SCANCODE_R, MH3_CL_R},
            {SDL_SCANCODE_Z, MH3_CL_ZL},          {SDL_SCANCODE_C, MH3_CL_ZR},         {SDL_SCANCODE_RETURN, MH3_CL_PLUS},
            {SDL_SCANCODE_BACKSPACE, MH3_CL_MINUS}, {SDL_SCANCODE_ESCAPE, MH3_CL_HOME}, {SDL_SCANCODE_HOME, MH3_CL_HOME},
            {SDL_SCANCODE_UP, MH3_CL_UP},         {SDL_SCANCODE_DOWN, MH3_CL_DOWN},    {SDL_SCANCODE_LEFT, MH3_CL_LEFT},
            {SDL_SCANCODE_RIGHT, MH3_CL_RIGHT},
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
#endif
    return sample;
}
