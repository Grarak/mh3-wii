#pragma once
#include <cstdint>
#include <optional>
struct CpuContext;
struct ClassicControllerSample {
    bool connected = false;
    uint16_t buttons = 0;
    int16_t leftX = 0, leftY = 0, rightX = 0, rightY = 0;
    uint8_t triggerL = 0, triggerR = 0;
};
// The host input as the game sees it (after MH3_INPUT_PLAYBACK / _RECORD).
ClassicControllerSample ReadHostController();
// The frame index of the input logs (the frame pacer's frame count).
void SetInputFrame(uint64_t frame);
// SDL keyboard/gamepad, or MH3_INPUT_FILE.
ClassicControllerSample ReadLiveController();
void SetControllerTestSample(std::optional<ClassicControllerSample> sample);
void ServiceHostController(CpuContext* cpu, bool force = false);
void RunControllerSelfTests(CpuContext* cpu);
