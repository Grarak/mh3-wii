// Thin resource/material ABI boundary. Plain CPU records are decoded in C;
// certified list groups come from predecoded M3MP/live producers, never FIFO.
#include "abi_bridge.h"
#include "memory.h"
#include "native_graphics.h"
#include "runtime/native/adapters/model_catalog_bridge.h"
#include "runtime/native/adapters/model_material_bridge.h"
#include "runtime/native/adapters/model_state_bridge.h"
#include "runtime/native/mh3_model_material_prep.h"
#include <array>
#include <cstring>
#include <stdexcept>
#include <string>

const mh3_res_memory& MH3NativeResourceMemory();
namespace {
constexpr uint32_t genmode = 0x80685300, sync_address = 0x80796db0;
void Checked(mh3_res_status status) {
    if (status != MH3_RES_OK)
        throw std::runtime_error("Native material preparation failed: " + std::to_string(status));
}
uint32_t Resource(uint32_t reference, bool required = false) {
    uint32_t resource = 0;
    Checked(mh3_res_runtime_reference(&MH3NativeResourceMemory(), reference, required, &resource));
    return resource;
}
mh3_res_span Bytes(uint32_t address, size_t size) {
    return {Memory::GetPointer(address, size), size};
}
mh3_res_mut_span Mutable(uint32_t address, size_t size) {
    return {Memory::GetPointer(address, size), size};
}
void Publish(const mh3_native_material_state& patch, uint32_t top = 0) {
    OverlayNativeMaterial(patch, top);
}
void PublishGenMode(uint32_t address, bool xf) {
    mh3_native_material_state patch{};
    uint32_t top = 0;
    int apply = 0;
    Checked(mh3_model_genmode_publish(Mutable(address, 12), xf, &patch, &top, &apply));
    if (apply)
        Publish(patch, top);
}
void Group(CpuContext* cpu, mh3_model_material_block kind, bool xf) {
    const uint32_t address = Resource(cpu->gpr[3]);
    if (!address)
        return;
    mh3_model_genmode mode{};
    Checked(mh3_model_genmode_read(Bytes(genmode, 12), &mode));
    const uint32_t count = kind == MH3_MODEL_MATERIAL_INDIRECT ? mode.indirect : kind == MH3_MODEL_MATERIAL_TEXGEN ? mode.texgens : 1;
    const auto* fields = count ? &ReadNativeLiveMaterialBlockView(address, kind).fields : nullptr;
    // The original consumes SyncGX even for a zero-count group. This is now
    // CPU compatibility only; all typed state survives skipped GPU frames.
    if (!xf)
        PublishGenMode(genmode, false);
    Memory::GetPointer(sync_address, 1)[0] = 0;
    if (count)
        OverlayNativeModelMaterialGroup(*fields, kind, count);
    if (xf)
        PublishGenMode(genmode, true);
}
void ChannelControl(CpuContext* cpu, bool force_off) {
    mh3_native_material_state patch{};
    Checked(mh3_model_channel_control(&patch, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], force_off));
    Publish(patch);
}
void ChannelColor(CpuContext* cpu, bool ambient) {
    mh3_native_material_state patch{};
    Checked(mh3_model_channel_color(&patch, cpu->gpr[3], Memory::GetPointer(cpu->gpr[4], 4), ambient));
    Publish(patch);
}
mh3_model_material_misc Misc(uint32_t reference) {
    mh3_model_material_misc misc{};
    Checked(mh3_model_material_misc_read(Bytes(Resource(reference, true), 12), &misc));
    return misc;
}
} // namespace
extern "C" void MH3_ModelGenModeLoad(CpuContext* cpu) {
    const uint32_t address = Resource(cpu->gpr[3]);
    if (address)
        Checked(mh3_model_genmode_select(Mutable(genmode, 12), Bytes(address, 8)));
}
REGISTER_NATIVE_FUNCTION_AS(0x800861F8, MH3_ModelGenModeLoad, "ModelGenModeLoad");
extern "C" void MH3_ModelGenModeSelect(CpuContext* cpu) {
    Checked(mh3_model_genmode_select(Mutable(cpu->gpr[3], 12), Bytes(Resource(cpu->gpr[4], true), 8)));
}
REGISTER_NATIVE_FUNCTION_AS(0x80086244, MH3_ModelGenModeSelect, "ModelGenModeSelect");
extern "C" void MH3_ModelGenModeBP(CpuContext* cpu) {
    PublishGenMode(cpu->gpr[3], false);
}
REGISTER_NATIVE_FUNCTION_AS(0x80086810, MH3_ModelGenModeBP, "ModelGenModeBP");
extern "C" void MH3_ModelGenModeXF(CpuContext* cpu) {
    PublishGenMode(cpu->gpr[3], true);
}
REGISTER_NATIVE_FUNCTION_AS(0x80086FF8, MH3_ModelGenModeXF, "ModelGenModeXF");
extern "C" void MH3_ModelMiscLoad(CpuContext* cpu) {
    const uint32_t address = Resource(cpu->gpr[3]);
    if (!address)
        return;
    mh3_model_material_misc misc{};
    Checked(mh3_model_material_misc_read(Bytes(address, 12), &misc));
    int apply = 0;
    Checked(mh3_depth_compare_cache(Mutable(0x80796dc8, 5), Mutable(sync_address, 1), misc.depth_before_texture, &apply));
    if (apply)
        SetNativeDepthCompareBeforeTexture(misc.depth_before_texture);
}
REGISTER_NATIVE_FUNCTION_AS(0x80085DB0, MH3_ModelMiscLoad, "ModelMiscLoad");
extern "C" void MH3_ModelCurrentFog(CpuContext* cpu) {
    mh3_native_fog fog{};
    int apply = 0;
    Checked(mh3_fog_state_select(Mutable(0x80685d60, 1544), cpu->gpr[3], &fog, &apply));
    if (apply)
        SetNativeFog(fog);
}
REGISTER_NATIVE_FUNCTION_AS(0x800881F0, MH3_ModelCurrentFog, "ModelCurrentFog");
extern "C" void MH3_ModelCurrentLights(CpuContext* cpu) {
    const auto slots = SelectNativeModelLightSet(0x80686368, cpu->gpr[3]);
    const uint32_t masks[]{slots.diffuse_color, slots.diffuse_alpha, slots.specular_color, slots.specular_alpha};
    for (unsigned i = 0; i < 4; i++)
        if (cpu->gpr[4 + i])
            Memory::Write32(cpu->gpr[4 + i], masks[i]);
    if (cpu->gpr[8])
        std::memcpy(Memory::GetPointer(cpu->gpr[8], 4), &slots.ambient, 4);
}
REGISTER_NATIVE_FUNCTION_AS(0x80088304, MH3_ModelCurrentLights, "ModelCurrentLights");
extern "C" void MH3_ModelPixelLoad(CpuContext* cpu) {
    Group(cpu, MH3_MODEL_MATERIAL_PIXEL, false);
}
REGISTER_NATIVE_FUNCTION_AS(0x800869E4, MH3_ModelPixelLoad, "ModelPixelLoad");
extern "C" void MH3_ModelColorLoad(CpuContext* cpu) {
    Group(cpu, MH3_MODEL_MATERIAL_COLORS, false);
}
REGISTER_NATIVE_FUNCTION_AS(0x80086A38, MH3_ModelColorLoad, "ModelColorLoad");
extern "C" void MH3_ModelIndirectLoad(CpuContext* cpu) {
    Group(cpu, MH3_MODEL_MATERIAL_INDIRECT, false);
}
REGISTER_NATIVE_FUNCTION_AS(0x80086A8C, MH3_ModelIndirectLoad, "ModelIndirectLoad");
extern "C" void MH3_ModelTexgenLoad(CpuContext* cpu) {
    Group(cpu, MH3_MODEL_MATERIAL_TEXGEN, true);
}
REGISTER_NATIVE_FUNCTION_AS(0x80087148, MH3_ModelTexgenLoad, "ModelTexgenLoad");
extern "C" void MH3_ModelChannelsLoad(CpuContext* cpu) {
    const uint32_t address = Resource(cpu->gpr[3]);
    if (!address)
        return;
    mh3_model_genmode mode{};
    Checked(mh3_model_genmode_read(Bytes(genmode, 12), &mode));
    const uint32_t masks[]{cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7]};
    mh3_native_material_state patch{};
    Checked(mh3_model_channels_prepare(Bytes(address, 40), mode.channels, masks, Memory::GetPointer(cpu->gpr[8], 4), cpu->gpr[9] != 0, &patch));
    Publish(patch);
    PublishGenMode(genmode, true);
}
REGISTER_NATIVE_FUNCTION_AS(0x80086B90, MH3_ModelChannelsLoad, "ModelChannelsLoad");
extern "C" void MH3_ModelChannelMaterial(CpuContext* cpu) {
    ChannelColor(cpu, false);
}
REGISTER_NATIVE_FUNCTION_AS(0x80077470, MH3_ModelChannelMaterial, "ModelChannelMaterial");
extern "C" void MH3_ModelChannelAmbient(CpuContext* cpu) {
    ChannelColor(cpu, true);
}
REGISTER_NATIVE_FUNCTION_AS(0x800870C8, MH3_ModelChannelAmbient, "ModelChannelAmbient");
extern "C" void MH3_ModelChannelControl(CpuContext* cpu) {
    ChannelControl(cpu, false);
}
REGISTER_NATIVE_FUNCTION_AS(0x80087068, MH3_ModelChannelControl, "ModelChannelControl");
extern "C" void MH3_ModelChannelControlOff(CpuContext* cpu) {
    ChannelControl(cpu, true);
}
REGISTER_NATIVE_FUNCTION_AS(0x80087098, MH3_ModelChannelControlOff, "ModelChannelControlOff");
extern "C" void MH3_ModelMaterialLightReference(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(int32_t(Misc(cpu->gpr[3]).light_reference));
}
REGISTER_NATIVE_FUNCTION_AS(0x80095930, MH3_ModelMaterialLightReference, "ModelMaterialLightReference");
extern "C" void MH3_ModelMaterialFogReference(CpuContext* cpu) {
    cpu->gpr[3] = uint32_t(int32_t(Misc(cpu->gpr[3]).fog_reference));
}
REGISTER_NATIVE_FUNCTION_AS(0x800959A4, MH3_ModelMaterialFogReference, "ModelMaterialFogReference");
