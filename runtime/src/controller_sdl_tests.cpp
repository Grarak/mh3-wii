#include "controller_host.h"
#include <SDL3/SDL.h>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {
void Check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(std::string(message) + ": " + SDL_GetError());
}
SDL_JoystickID Attach() {
    SDL_VirtualJoystickDesc desc{};
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.vendor_id = 0xF00D; desc.product_id = 0xBEEF;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
    desc.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1;
    desc.name = "MH3 virtual input regression";
    auto id = SDL_AttachVirtualJoystick(&desc);
    Check(id != 0, "attach virtual gamepad");
    return id;
}
}
int main() {
    try {
        unsetenv("MH3_INPUT_FILE");
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
        SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT, "0xF00D/0xBEEF");
        Check(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD), "initialize SDL");
        auto id = Attach();
        auto* stick = SDL_OpenJoystick(id);
        Check(stick != nullptr, "open virtual joystick");
        SDL_UpdateJoysticks();
        Check(ReadHostController().connected, "keyboard fallback connected");
        Check(SDL_SetJoystickVirtualButton(stick, SDL_GAMEPAD_BUTTON_EAST, true), "press east");
        Check(SDL_SetJoystickVirtualButton(stick, SDL_GAMEPAD_BUTTON_DPAD_DOWN, true), "press down");
        Check(SDL_SetJoystickVirtualAxis(stick, SDL_GAMEPAD_AXIS_LEFTX, 32767), "left x");
        Check(SDL_SetJoystickVirtualAxis(stick, SDL_GAMEPAD_AXIS_LEFTY, -32768), "left y");
        Check(SDL_SetJoystickVirtualAxis(stick, SDL_GAMEPAD_AXIS_RIGHTX, -32768), "right x");
        Check(SDL_SetJoystickVirtualAxis(stick, SDL_GAMEPAD_AXIS_RIGHTY, 32767), "right y");
        Check(SDL_SetJoystickVirtualAxis(stick, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 32767), "left trigger");
        SDL_UpdateJoysticks();
        auto state = ReadHostController();
        Check((state.buttons & 0x4090) == 0x4090, "Classic A, down, ZL mapping");
        Check(state.leftX == 511 && state.leftY == 511 &&
              state.rightX == -512 && state.rightY == -512, "stick ranges and Y inversion");
        Check(state.triggerL == 255, "analog trigger mapping");
        Check(SDL_SetJoystickVirtualButton(stick, SDL_GAMEPAD_BUTTON_EAST, false), "release east");
        Check(SDL_SetJoystickVirtualButton(stick, SDL_GAMEPAD_BUTTON_DPAD_DOWN, false), "release down");
        Check(SDL_SetJoystickVirtualAxis(stick, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, -32768), "release trigger");
        SDL_UpdateJoysticks();
        Check(ReadHostController().buttons == 0, "release mapping");
        Check(SDL_DetachVirtualJoystick(id), "detach gamepad");
        SDL_CloseJoystick(stick);
        SDL_UpdateJoysticks();
        state = ReadHostController();
        Check(state.connected && state.buttons == 0 && state.leftX == 0 && state.rightY == 0,
              "keyboard fallback after disconnect");
        id = Attach();
        stick = SDL_OpenJoystick(id);
        Check(stick != nullptr, "open reconnected joystick");
        Check(SDL_SetJoystickVirtualButton(stick, SDL_GAMEPAD_BUTTON_SOUTH, true), "press south");
        SDL_UpdateJoysticks();
        Check(ReadHostController().buttons == 0x40, "reconnect and Classic B mapping");
        Check(SDL_DetachVirtualJoystick(id), "detach reconnected gamepad");
        SDL_CloseJoystick(stick);
        SDL_UpdateJoysticks();
        ReadHostController(); // Close the host's disconnected SDL handle before shutdown.
        SDL_Quit();
        std::cout << "MH3 SDL gamepad mapping and hotplug test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        SDL_Quit();
        return 1;
    }
}
