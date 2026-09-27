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
ClassicControllerSample ReadHostController();
void SetControllerTestSample(std::optional<ClassicControllerSample> sample);
void ServiceHostController(CpuContext* cpu, bool force = false);
void RunControllerSelfTests(CpuContext* cpu);
