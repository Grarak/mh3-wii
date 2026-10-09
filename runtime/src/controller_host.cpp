#include "controller_host.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <cstdio>
#include <string>
#include <vector>
#ifdef MH3_HAS_AURORA
#include <SDL3/SDL.h>
#endif
namespace {
std::optional<ClassicControllerSample> testSample;
#ifdef MH3_DEVELOPMENT
uint64_t inputFrame = 0;

// Frame-indexed input logs for repeatable runs. Each line is
// "frame buttons(hex) lx ly rx ry": the sample in effect from that paced
// frame on. MH3_INPUT_RECORD writes one line per change of the host input;
// MH3_INPUT_PLAYBACK replaces the host input with a log.
bool SameInput(const ClassicControllerSample& a, const ClassicControllerSample& b) {
    return a.buttons == b.buttons && a.leftX == b.leftX && a.leftY == b.leftY &&
           a.rightX == b.rightX && a.rightY == b.rightY;
}
void RecordInput(const ClassicControllerSample& sample) {
    static const char* path = std::getenv("MH3_INPUT_RECORD");
    if (!path) return;
    static std::ofstream log(path);
    static std::optional<ClassicControllerSample> last;
    if (last && SameInput(*last, sample)) return;
    last = sample;
    log << inputFrame << ' ' << std::hex << sample.buttons << std::dec << ' ' << sample.leftX << ' '
        << sample.leftY << ' ' << sample.rightX << ' ' << sample.rightY << std::endl;
}
std::optional<ClassicControllerSample> PlaybackInput() {
    static const char* path = std::getenv("MH3_INPUT_PLAYBACK");
    if (!path) return std::nullopt;
    struct Entry { uint64_t frame; ClassicControllerSample sample; };
    static const std::vector<Entry> entries = [] {
        std::vector<Entry> result;
        std::ifstream input(path);
        uint64_t frame; unsigned buttons; int lx, ly, rx, ry;
        while (input >> frame >> std::hex >> buttons >> std::dec >> lx >> ly >> rx >> ry) {
            if (!result.empty() && frame < result.back().frame)
                throw std::runtime_error("MH3_INPUT_PLAYBACK frames must not decrease");
            result.push_back({frame, {true, uint16_t(buttons), int16_t(lx), int16_t(ly), int16_t(rx), int16_t(ry)}});
        }
        if (!input.eof() || result.empty()) throw std::runtime_error("Invalid MH3_INPUT_PLAYBACK log");
        return result;
    }();
    static size_t next = 0;
    const uint64_t frame = inputFrame;
    while (next < entries.size() && entries[next].frame <= frame) ++next;
    if (next == entries.size()) {
        static bool reported = false;
        if (!reported) std::fprintf(stderr, "MH3 input playback: finished at frame %llu\n", (unsigned long long)frame);
        reported = true;
    }
    if (next == 0) return ClassicControllerSample{true};
    return entries[next - 1].sample;
}
#endif
}
void SetControllerTestSample(std::optional<ClassicControllerSample> sample) { testSample = sample; }
#ifdef MH3_DEVELOPMENT
void SetInputFrame(uint64_t frame) { inputFrame = frame; }
ClassicControllerSample ReadHostController() {
    if (testSample) return *testSample;
    if (auto played = PlaybackInput()) return *played;
    const ClassicControllerSample sample = ReadLiveController();
    RecordInput(sample);
    return sample;
}
#else
void SetInputFrame(uint64_t) {}
ClassicControllerSample ReadHostController() {
    if (testSample) return *testSample;
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
        unsigned buttons; int lx, ly, rx, ry;
        if (!(input >> std::hex >> buttons >> std::dec >> lx >> ly >> rx >> ry) ||
            buttons > 0xFFFF || lx < -512 || lx > 511 || ly < -512 || ly > 511 ||
            rx < -512 || rx > 511 || ry < -512 || ry > 511)
            throw std::runtime_error("Invalid MH3_INPUT_FILE: expected hex buttons and four signed axes (-512..511)");
        sample = {true, uint16_t(buttons), int16_t(lx), int16_t(ly), int16_t(rx), int16_t(ry)};
        return sample;
    }
#endif
#ifdef MH3_HAS_AURORA
    sample.connected = true; // Keyboard is a virtual Classic Controller on port 1.
    if (!SDL_WasInit(SDL_INIT_VIDEO)) return sample;
    if (!SDL_WasInit(SDL_INIT_GAMEPAD) && !SDL_InitSubSystem(SDL_INIT_GAMEPAD))
        throw std::runtime_error(std::string("SDL controller initialization: ") + SDL_GetError());
    const bool* keys = SDL_GetKeyboardState(nullptr);
    const auto key = [&](SDL_Scancode code, uint16_t button) { if (keys[code]) sample.buttons |= button; };
    if (SDL_GetKeyboardFocus()) {
        key(SDL_SCANCODE_SPACE, 0x10); key(SDL_SCANCODE_LSHIFT, 0x40);
        key(SDL_SCANCODE_E, 8); key(SDL_SCANCODE_R, 0x20);
        key(SDL_SCANCODE_Q, 0x2000); key(SDL_SCANCODE_F, 0x200);
        key(SDL_SCANCODE_Z, 0x80); key(SDL_SCANCODE_X, 4);
        key(SDL_SCANCODE_RETURN, 0x400); key(SDL_SCANCODE_BACKSPACE, 0x1000);
        key(SDL_SCANCODE_ESCAPE, 0x800);
        key(SDL_SCANCODE_UP, 1); key(SDL_SCANCODE_LEFT, 2);
        key(SDL_SCANCODE_DOWN, 0x4000); key(SDL_SCANCODE_RIGHT, 0x8000);
        sample.leftX = int16_t((int(keys[SDL_SCANCODE_D]) - int(keys[SDL_SCANCODE_A])) * 511);
        sample.leftY = int16_t((int(keys[SDL_SCANCODE_W]) - int(keys[SDL_SCANCODE_S])) * 511);
        sample.rightX = int16_t((int(keys[SDL_SCANCODE_L]) - int(keys[SDL_SCANCODE_J])) * 511);
        sample.rightY = int16_t((int(keys[SDL_SCANCODE_I]) - int(keys[SDL_SCANCODE_K])) * 511);
    }
    static SDL_Gamepad* pad = nullptr;
    if (pad && !SDL_GamepadConnected(pad)) { SDL_CloseGamepad(pad); pad = nullptr; }
    if (!pad) {
        int count = 0;
        auto* ids = SDL_GetGamepads(&count);
        if (ids && count) pad = SDL_OpenGamepad(ids[0]);
        SDL_free(ids);
    }
    if (pad) {
        const auto button = [&](SDL_GamepadButton source, uint16_t target) {
            if (SDL_GetGamepadButton(pad, source)) sample.buttons |= target;
        };
        button(SDL_GAMEPAD_BUTTON_EAST, 0x10); button(SDL_GAMEPAD_BUTTON_SOUTH, 0x40);
        button(SDL_GAMEPAD_BUTTON_NORTH, 8); button(SDL_GAMEPAD_BUTTON_WEST, 0x20);
        button(SDL_GAMEPAD_BUTTON_DPAD_UP, 1); button(SDL_GAMEPAD_BUTTON_DPAD_LEFT, 2);
        button(SDL_GAMEPAD_BUTTON_DPAD_DOWN, 0x4000); button(SDL_GAMEPAD_BUTTON_DPAD_RIGHT, 0x8000);
        button(SDL_GAMEPAD_BUTTON_START, 0x400); button(SDL_GAMEPAD_BUTTON_BACK, 0x1000);
        button(SDL_GAMEPAD_BUTTON_GUIDE, 0x800);
        button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, 0x2000); button(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 0x200);
        const auto axis = [&](SDL_GamepadAxis source, bool invert) {
            float value = SDL_GetGamepadAxis(pad, source) / 32767.0f;
            if (invert) value = -value;
            return int16_t(std::clamp(std::lround(value * 512), -512L, 511L));
        };
        if (!sample.leftX) sample.leftX = axis(SDL_GAMEPAD_AXIS_LEFTX, false);
        if (!sample.leftY) sample.leftY = axis(SDL_GAMEPAD_AXIS_LEFTY, true);
        if (!sample.rightX) sample.rightX = axis(SDL_GAMEPAD_AXIS_RIGHTX, false);
        if (!sample.rightY) sample.rightY = axis(SDL_GAMEPAD_AXIS_RIGHTY, true);
        sample.triggerL = uint8_t(std::max(0, int(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER))) * 255 / 32767);
        sample.triggerR = uint8_t(std::max(0, int(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER))) * 255 / 32767);
        if (sample.triggerL > 127) sample.buttons |= 0x80;
        if (sample.triggerR > 127) sample.buttons |= 4;
    }
#endif
    return sample;
}
