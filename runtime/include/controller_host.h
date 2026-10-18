#pragma once
#include <cstdint>
#include <optional>
#include <utility>
struct CpuContext;
struct ClassicControllerSample {
    bool connected = false;
    uint16_t buttons = 0;
    int16_t leftX = 0, leftY = 0, rightX = 0, rightY = 0;
    uint8_t triggerL = 0, triggerR = 0;

    // The Wii Remote's pointer: the Vita's front touch screen, the mouse's
    // left button elsewhere. While held, where on the game's picture, 0..1
    // across and down (wpad_services.cpp).
    bool pointing = false;
    float pointX = 0, pointY = 0;
};

// The host input as the game sees it (after MH3_INPUT playback / record).
ClassicControllerSample ReadHostController();

// The frame index of the input logs (the frame pacer's frame count).
void SetInputFrame(uint64_t frame);

// Holds MH3_INPUT playback before its next entry (while the village shortcut drives input).
void HoldInputPlayback(bool hold);
#ifdef MH3_DEVELOPMENT
// MH3_INPUT=camera=N[:D]: the monster the field camera looks at, or -1, and
// the eye's distance from it (0: the game's eye).
int DebugCameraMonster(float* distance = nullptr);
#endif

// The live controls: SDL keyboard/gamepad on Linux, the Vita's pad on the
// Vita; MH3_INPUT=file= replaces them.
ClassicControllerSample ReadLiveController();
#ifdef __vita__
// sceCtrl through mh3_vita_pad_to_classic (runtime/vita/vita_controller.cpp).
ClassicControllerSample ReadVitaController();
#endif
void SetControllerTestSample(std::optional<ClassicControllerSample> sample);

// Holds the pointer at (x, y) on the game's picture (0..1), or lets the host
// pointer through again (debug port `point`).
void SetPointerTestSample(std::optional<std::pair<float, float>> point);

// A point on the window (the Vita's screen), in drawable pixels, as a point on
// the game's picture: false outside it or before the first frame.
bool PointOnPicture(float x, float y, float& pictureX, float& pictureY);
void ServiceHostController(CpuContext* cpu, bool force = false);
void RunControllerSelfTests(CpuContext* cpu);
