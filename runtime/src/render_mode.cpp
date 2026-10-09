// Thin RMHP08 ABI boundary for native display-mode selection. No GX calls.
#include "abi_bridge.h"
#include "guest_call.h"
#include "memory.h"
#include "../native/mh3_render_mode.h"
#include <array>
#include <stdexcept>
#include <string>

namespace {
constexpr uint32_t ModeAddress = 0x8065A980;
constexpr std::array<uint32_t, 3> Defaults{0x8061B280, 0x8061B2BC, 0x8061B334};
constexpr int32_t ModePointer = -0x46B8;
void Checked(mh3_res_status status) {
    if (status != MH3_RES_OK)
        throw std::runtime_error("Native render mode input failed: " + std::to_string(status));
}
mh3_render_mode Read(uint32_t address) {
    mh3_render_mode mode{};
    Checked(mh3_render_mode_read({Memory::GetPointer(address, MH3_RENDER_MODE_BYTES), MH3_RENDER_MODE_BYTES}, &mode));
    return mode;
}
} // namespace

extern "C" void MH3_RenderInit(CpuContext* cpu) {
    GuestFrame frame(cpu, 0x10);
    GuestCall call{cpu};
    const auto sda = frame.Arg(13);
    const auto customAddress = frame.Arg(3);
    mh3_render_preferences preferences{};
    preferences.progressive = uint8_t(call(0x804DD1C0));
    Memory::Write8(sda - 0x46A0, preferences.progressive);
    preferences.widescreen = uint8_t(call(0x804DD060));
    Memory::Write8(sda - 0x469F, preferences.widescreen);
    Memory::Write8(sda - 0x469E, uint8_t(call(0x804DD150))); // language
    preferences.component_cable = uint8_t(call(0x804E9420));
    Memory::Write8(sda - 0x469B, preferences.component_cable);
    call(0x804E93C0); // retain VI initialization/query side effects
    preferences.tv_format = uint8_t(call(0x804E9360));
    Memory::Write8(sda - 0x469D, preferences.tv_format);
    Memory::Write8(sda - 0x469C, 0);

    std::array<mh3_render_mode, 3> defaults{};
    for (size_t index = 0; index < defaults.size(); ++index) defaults[index] = Read(Defaults[index]);
    mh3_render_configuration configuration{};
    configuration.mode = Read(ModeAddress);
    mh3_render_mode custom{};
    if (customAddress) custom = Read(customAddress);
    const auto status = mh3_render_mode_select(&preferences, defaults.data(),
        customAddress ? &custom : nullptr, &configuration);
    if (status == MH3_RES_UNSUPPORTED) {
        cpu->cr &= ~(1u << (31 - 6)); // integer-only variadic panic
        call(0x804CDD30, {0x8057CFE0, 0x2ED, 0x8057CFEC});
    }
    Checked(status);
    Checked(mh3_render_mode_store({Memory::GetPointer(ModeAddress, MH3_RENDER_MODE_BYTES), MH3_RENDER_MODE_BYTES},
        &configuration.mode));
    Memory::Write8(sda - 0x469C, configuration.pal);
    if (!customAddress) {
        cpu->cr &= ~(1u << (31 - 6));
        call(0x804CDCA0, {0x8057D010, configuration.mode.tv_mode});
    }
    Memory::Write32(sda + uint32_t(ModePointer), ModeAddress);
    Memory::Write8(sda - 0x468F, configuration.widescreen);
    Memory::Write16(sda - 0x468E, configuration.mode.vi_x_origin);
    Memory::Write16(sda - 0x468C, configuration.mode.vi_y_origin);
}
REGISTER_NATIVE_FUNCTION_AS(0x8003F794, MH3_RenderInit, "RenderInit");

extern "C" void MH3_ApplyWidemode(CpuContext* cpu) {
    GuestFrame frame(cpu, 0x60);
    const auto wide = uint8_t(frame.Arg(3));
    const auto sda = frame.Arg(13);
    if (wide == Memory::Read8(sda - 0x468F)) return;
    mh3_render_preferences preferences{};
    preferences.progressive = Memory::Read8(sda - 0x46A0);
    preferences.tv_format = Memory::Read8(sda - 0x469D);
    preferences.widescreen = wide;
    std::array<mh3_render_mode, 3> defaults{};
    for (size_t i = 0; i < defaults.size(); ++i) defaults[i] = Read(Defaults[i]);
    const auto address = Memory::Read32(sda + uint32_t(ModePointer));
    auto mode = Read(address);
    const auto status = mh3_render_mode_widescreen(&preferences, defaults.data(), &mode);
    if (status == MH3_RES_UNSUPPORTED) return;
    Checked(status);
    // Publish only the two fields written by the original caller.
    Memory::Write16(address + 14, mode.vi_width);
    Memory::Write16(address + 10, mode.vi_x_origin);
    GuestCall{cpu}(0x804E85E0, {address});
    Memory::Write16(0x8065B4BC, 640);
    Memory::Write16(0x8065B4BE, 448);
    const float width = Memory::ReadFloat32(wide == 1 ? 0x80797F60 : 0x80797F6C);
    const float height = Memory::ReadFloat32(0x80797F64);
    const float left = Memory::ReadFloat32(0x80797F78);
    const float top = Memory::ReadFloat32(0x80797F7C);
    Memory::WriteFloat32(0x8065B4C0, width);
    Memory::Write32(0x8065B4C8, Memory::Read32(wide == 1 ? 0x80797F68 : 0x80797F70));
    Memory::Write16(0x8065B4F8, uint16_t(int32_t(width)));
    Memory::Write16(0x8065B4FA, Memory::Read16(address + 6));
    Memory::Write8(0x8065B4D5, wide == 1);
    Memory::WriteFloat32(0x8065B4C4, height);
    Memory::WriteFloat32(0x8065B4E8, left);
    Memory::WriteFloat32(0x8065B4EC, top);
    Memory::WriteFloat32(0x8065B4F0, width - left);
    Memory::WriteFloat32(0x8065B4F4, height - top);
    Memory::Write8(sda - 0x468F, wide);
}
REGISTER_NATIVE_FUNCTION_AS(0x8003FA48, MH3_ApplyWidemode, "ApplyWidemode");
