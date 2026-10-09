// RMHP08 light ABI boundary. All light calculations/default construction are
// native C. CPU compatibility records do not submit GX or GPU commands.
#include "abi_bridge.h"
#include "fatal.h"
#include "memory.h"
#include "../native/mh3_lighting.h"
#include <cstring>
#include <stdexcept>
#include <string>

namespace {
void Checked(mh3_res_status status) {
    if (status != MH3_RES_OK)
        MH3_THROW(std::runtime_error("Native light input failed: " + std::to_string(status)));
}

mh3_res_mut_span Record(uint32_t address) {
    return {Memory::GetPointer(address, MH3_LIGHT_RECORD_BYTES), MH3_LIGHT_RECORD_BYTES};
}

mh3_light Read(uint32_t address) {
    const auto record = Record(address);
    mh3_light light{};
    Checked(mh3_light_read({record.data, record.size}, &light));
    return light;
}

void Store(uint32_t address, const mh3_light& light) {
    Checked(mh3_light_store(Record(address), &light));
}
} // namespace

extern "C" void MH3_LightCopy(CpuContext* cpu) {
    const auto source = Record(cpu->gpr[4]);
    Checked(mh3_light_copy_record(Record(cpu->gpr[3]), {source.data, source.size}));
}
REGISTER_NATIVE_FUNCTION_AS(0x8007A57C, MH3_LightCopy, "LightCopy");
extern "C" void MH3_LightClear(CpuContext* cpu) {
    Checked(mh3_light_clear_record(Record(cpu->gpr[3])));
}
REGISTER_NATIVE_FUNCTION_AS(0x8007A5C8, MH3_LightClear, "LightClear");
extern "C" void MH3_LightColor(CpuContext* cpu) {
    auto light = Read(cpu->gpr[3]);
    std::memcpy(&light.color, Memory::GetPointer(cpu->gpr[4], 4), 4);
    Store(cpu->gpr[3], light);
}
REGISTER_NATIVE_FUNCTION_AS(0x8007A5DC, MH3_LightColor, "LightColor");
extern "C" void MH3_LightPosition(CpuContext* cpu) {
    auto light = Read(cpu->gpr[3]);
    mh3_light_set_position(&light, float(cpu->fpr[1].d), float(cpu->fpr[2].d), float(cpu->fpr[3].d));
    Store(cpu->gpr[3], light);
}
REGISTER_NATIVE_FUNCTION_AS(0x8007A60C, MH3_LightPosition, "LightPosition");
extern "C" void MH3_LightDirection(CpuContext* cpu) {
    auto light = Read(cpu->gpr[3]);
    mh3_light_set_direction(&light, float(cpu->fpr[1].d), float(cpu->fpr[2].d), float(cpu->fpr[3].d), 0);
    Store(cpu->gpr[3], light);
}
REGISTER_NATIVE_FUNCTION_AS(0x8007A648, MH3_LightDirection, "LightDirection");
extern "C" void MH3_LightSpot(CpuContext* cpu) {
    auto light = Read(cpu->gpr[3]);
    mh3_light_set_spot(&light, float(cpu->fpr[1].d), cpu->gpr[4]);
    Store(cpu->gpr[3], light);
}
REGISTER_NATIVE_FUNCTION_AS(0x8007A688, MH3_LightSpot, "LightSpot");
extern "C" void MH3_LightAngularAttenuation(CpuContext* cpu) {
    auto light = Read(cpu->gpr[3]);
    mh3_light_set_angular(&light, float(cpu->fpr[1].d), float(cpu->fpr[2].d), float(cpu->fpr[3].d));
    Store(cpu->gpr[3], light);
}
REGISTER_NATIVE_FUNCTION_AS(0x8007A6C8, MH3_LightAngularAttenuation, "LightAngularAttenuation");
extern "C" void MH3_LightDistanceAttenuation(CpuContext* cpu) {
    auto light = Read(cpu->gpr[3]);
    mh3_light_set_distance_attenuation(&light, float(cpu->fpr[1].d), float(cpu->fpr[2].d), cpu->gpr[4]);
    Store(cpu->gpr[3], light);
}
REGISTER_NATIVE_FUNCTION_AS(0x8007A708, MH3_LightDistanceAttenuation, "LightDistanceAttenuation");
extern "C" void MH3_LightDistancePolynomial(CpuContext* cpu) {
    auto light = Read(cpu->gpr[3]);
    mh3_light_set_distance(&light, float(cpu->fpr[1].d), float(cpu->fpr[2].d), float(cpu->fpr[3].d));
    Store(cpu->gpr[3], light);
}
REGISTER_NATIVE_FUNCTION_AS(0x8007A748, MH3_LightDistancePolynomial, "LightDistancePolynomial");
extern "C" void MH3_LightSpecularDirection(CpuContext* cpu) {
    auto light = Read(cpu->gpr[3]);
    mh3_light_set_direction(&light, float(cpu->fpr[1].d), float(cpu->fpr[2].d), float(cpu->fpr[3].d), 1);
    Store(cpu->gpr[3], light);
}
REGISTER_NATIVE_FUNCTION_AS(0x8007A788, MH3_LightSpecularDirection, "LightSpecularDirection");
extern "C" void MH3_LightShininess(CpuContext* cpu) {
    auto light = Read(cpu->gpr[3]);
    mh3_light_set_shininess(&light, float(cpu->fpr[1].d));
    Store(cpu->gpr[3], light);
}
REGISTER_NATIVE_FUNCTION_AS(0x8007A7CC, MH3_LightShininess, "LightShininess");
extern "C" void MH3_LightGetPosition(CpuContext* cpu) {
    if (!cpu->gpr[4])
        return;

    const auto light = Read(cpu->gpr[3]);
    Checked(mh3_light_store_vector({Memory::GetPointer(cpu->gpr[4], 12), 12}, &light.position));
}
REGISTER_NATIVE_FUNCTION_AS(0x8007A82C, MH3_LightGetPosition, "LightGetPosition");
extern "C" void MH3_LightGetDirection(CpuContext* cpu) {
    if (!cpu->gpr[4])
        return;

    const auto light = Read(cpu->gpr[3]);
    const auto direction = mh3_light_ray_direction(&light);
    Checked(mh3_light_store_vector({Memory::GetPointer(cpu->gpr[4], 12), 12}, &direction));
}
REGISTER_NATIVE_FUNCTION_AS(0x8007A848, MH3_LightGetDirection, "LightGetDirection");
extern "C" void MH3_LightGetColor(CpuContext* cpu) {
    if (!cpu->gpr[4])
        return;

    const auto light = Read(cpu->gpr[3]);
    std::memcpy(Memory::GetPointer(cpu->gpr[4], 4), &light.color, 4);
}
REGISTER_NATIVE_FUNCTION_AS(0x8007A864, MH3_LightGetColor, "LightGetColor");
extern "C" void MH3_LightApplyView(CpuContext* cpu) {
    auto light = Read(cpu->gpr[3]);
    mh3_matrix34 view{};
    Checked(mh3_matrix_read34({{Memory::GetPointer(cpu->gpr[4], 48), 48}, 1, 48, 1}, 0, &view));
    mh3_light_apply_view(&light, &view);
    Store(cpu->gpr[3], light);
}
REGISTER_NATIVE_FUNCTION_AS(0x8007A878, MH3_LightApplyView, "LightApplyView");
extern "C" void MH3_LightSettingDefaults(CpuContext* cpu) {
    const auto count = uint16_t(cpu->gpr[6]);
    const auto sets = uint16_t(cpu->gpr[8]);

    if (!count || count > MH3_LIGHT_MAX || !sets || sets > MH3_LIGHT_SET_MAX)
        MH3_THROW(std::runtime_error("Native light setting count exceeds the scene allocation"));

    Checked(mh3_light_setting_store_defaults(
        {Memory::GetPointer(cpu->gpr[3], 16), 16}, {Memory::GetPointer(cpu->gpr[4], size_t(count) * MH3_LIGHT_RECORD_BYTES), size_t(count) * MH3_LIGHT_RECORD_BYTES},
        {Memory::GetPointer(cpu->gpr[5], size_t(count) * 4), size_t(count) * 4},
        {Memory::GetPointer(cpu->gpr[7], size_t(sets) * MH3_LIGHT_SET_RECORD_BYTES), size_t(sets) * MH3_LIGHT_SET_RECORD_BYTES}, count, sets, cpu->gpr[4], cpu->gpr[5], cpu->gpr[7]));
}
REGISTER_NATIVE_FUNCTION_AS(0x8007A944, MH3_LightSettingDefaults, "LightSettingDefaults");
