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

// Once a paced frame: the game time of the input logs, in 30 Hz frames
// (fractional: a log's entries take effect between whole frames).
void SetInputTime(double frames);

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

// Mouse look (SDL hosts): the mouse's motion since the last call while the
// mouse is captured (a click in the window captures it, Left Alt held frees
// the cursor), plus the debug port's; motion from before the last paced frame
// is dropped, so a menu's mouse movement does not turn the camera after it.
// The field camera takes it once a step (camera_input.cpp).
struct MouseMotion {
    float dx = 0, dy = 0;
};

MouseMotion TakeMouseMotion();

// Debug port `mouse DX DY [FRAMES]`: motion spread over FRAMES paced frames.
void AddMouseMotion(float dx, float dy, uint32_t frames);

// The item bar scroll the captured mouse's clicks asked for while L was held
// (Q): a right click +1 (forward), a left click -1, since the last call. The
// item bar takes it once a step (item_bar.cpp); letting go of L drops it.
int TakeItemBarScroll();

// Whether a click captures the mouse for the camera (on by default): the
// launcher's menus take the mouse themselves while they run.
void SetMouseLook(bool enabled);

// MH3_MOUSE_SENSITIVITY (default 1) and MH3_MOUSE_INVERT_Y=1.
float MouseSensitivity();
bool MouseInvertY();
void ServiceHostController(CpuContext* cpu, bool force = false);
void RunControllerSelfTests(CpuContext* cpu);
