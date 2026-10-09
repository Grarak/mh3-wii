// RMHP08 camera, fog and CPU cache ABI adapters. Resource arithmetic and
// serialized updates live in portable C. Backend receives ordinary records;
// no guest rendering functions, GX calls or source command lists are used.
#include "abi_bridge.h"
#include <aurora/fast_map.h>
#include "guest_span.h"
#include "memory.h"
#include "native_graphics.h"
#include "runtime/native/adapters/model_state_bridge.h"
#include "runtime/native/adapters/model_scope_bridge.h"
#include "runtime/native/mh3_model_state.h"
#include "model_render_prep.h"
#include <stdexcept>
#include <string>

const mh3_res_memory& MH3NativeResourceMemory();
namespace {
[[noreturn, gnu::cold, gnu::noinline]] void CheckedFailed(mh3_res_status status) {
    throw std::runtime_error("Native scene input failed: " + std::to_string(status));
}
inline void Checked(mh3_res_status status) {
    if (status != MH3_RES_OK) [[unlikely]]
        CheckedFailed(status);
}
uint32_t Resource(uint32_t reference) {
    uint32_t result = 0;
    Checked(mh3_res_runtime_reference(&MH3NativeResourceMemory(), reference, 1, &result));
    return result;
}
mh3_res_mut_span Mutable(uint32_t address, size_t size) {
    return {GuestRange(address, size), size};
}
mh3_res_span Bytes(uint32_t address, size_t size) {
    return {GuestRange(address, size), size};
}
mh3_native_camera Camera(uint32_t resource) {
    mh3_native_camera camera{};
    Checked(mh3_camera_read(Bytes(resource, 268), &camera));
    return camera;
}
void StoreCamera(uint32_t resource, const mh3_native_camera& camera) {
    Checked(mh3_camera_store_matrices(Mutable(resource, 116), &camera));
}
void StoreFloats(uint32_t destination, const float* data, size_t count) {
    for (size_t i = 0; i < count; ++i)
        Memory::WriteFloat32(destination + uint32_t(i * 4), data[i]);
}
void ApplyScissor(const mh3_native_camera& camera) {
    int32_t rectangle[4];
    Checked(mh3_camera_effective_scissor(&camera, rectangle));
    SetNativeScissor(rectangle[0], rectangle[1], rectangle[2], rectangle[3]);
}
// Imported light sets by LightState address (game thread). revision advances
// whenever the slots' lights, source IDs or valid mask may change (an import
// or a selection of another set); reselecting the current set changes only
// the masks and the ambient colour.
struct LightSlots {
    mh3_native_light_slots slots;
    uint64_t revision;
};
FastMap<uint32_t, LightSlots> light_slots;
uint64_t light_revision = 0;
} // namespace
void RestoreNativeModelCamera() {
    // 802BDCF4 ->800473FC ->ScnRoot::GetCamera, not an inherited backend
    // projection. MyPlayerIndex is the signed byte8065AA87 (800CF3E8).
    const uint32_t scene = Memory::Read32(0x80796e34);
    mh3_res_memory_view view{};
    Checked(mh3_res_runtime_map(&MH3NativeResourceMemory(), scene, 248, &view));
    uint32_t resource = 0;
    Checked(mh3_scene_camera_select({view.span.data + view.offset, view.span.size - view.offset}, scene, Memory::Read8(0x8065b4d6) != 0, int8_t(Memory::Read8(0x8065aa87)), Bytes(0x8065b50c, 4),
                                    &resource));
    auto camera = Camera(resource);
    Checked(mh3_camera_prepare_projection(&camera));
    StoreCamera(resource, camera);
    // 800E47B8 explicitly submits perspective, independent of CameraData.type.
    SetNativeProjection(camera.projection, true);
}
extern "C" void MH3_RestoreModelCamera(CpuContext*) {
    RestoreNativeModelCamera();
}
REGISTER_NATIVE_FUNCTION_AS(0x800E47B8, MH3_RestoreModelCamera, "RestoreModelCamera");

extern "C" void MH3_CameraProject(CpuContext* cpu) {
    if (!cpu->gpr[4])
        return;
    const uint32_t resource = Resource(cpu->gpr[3]);
    auto camera = Camera(resource);
    const mh3_vec3 point{{Memory::ReadFloat32(cpu->gpr[5]), Memory::ReadFloat32(cpu->gpr[5] + 4), Memory::ReadFloat32(cpu->gpr[5] + 8)}};
    mh3_vec3 result{};
    Checked(mh3_camera_project(&camera, &point, &result));
    StoreCamera(resource, camera);
    StoreFloats(cpu->gpr[4], result.v, 3);
}
REGISTER_NATIVE_FUNCTION_AS(0x800752BC, MH3_CameraProject, "CameraProject");

extern "C" void MH3_CameraView(CpuContext* cpu) {
    const uint32_t resource = Resource(cpu->gpr[3]);
    auto camera = Camera(resource);
    Checked(mh3_camera_prepare_view(&camera));
    StoreCamera(resource, camera);
    StoreFloats(cpu->gpr[4], &camera.view.v[0][0], 12);
}
REGISTER_NATIVE_FUNCTION_AS(0x800753F8, MH3_CameraView, "CameraView");
extern "C" void MH3_CameraProjection(CpuContext* cpu) {
    const uint32_t resource = Resource(cpu->gpr[3]);
    auto camera = Camera(resource);
    Checked(mh3_camera_prepare_projection(&camera));
    StoreCamera(resource, camera);
    StoreFloats(cpu->gpr[4], camera.projection, 16);
}
REGISTER_NATIVE_FUNCTION_AS(0x800754A4, MH3_CameraProjection, "CameraProjection");
extern "C" void MH3_CameraPrepareProjection(CpuContext* cpu) {
    const uint32_t resource = Resource(cpu->gpr[3]);
    auto camera = Camera(resource);
    // Original helper recalculates unconditionally.
    camera.flags &= ~128u;
    Checked(mh3_camera_prepare_projection(&camera));
    StoreCamera(resource, camera);
}
REGISTER_NATIVE_FUNCTION_AS(0x80075E3C, MH3_CameraPrepareProjection, "CameraPrepareProjection");
extern "C" void MH3_CameraApplyViewport(CpuContext* cpu) {
    const auto camera = Camera(Resource(cpu->gpr[3]));
    auto y = camera.viewport[1];
    if (Memory::Read8(0x8068a7e0) && !(camera.flags & 256))
        y -= 0.5f;
    SetNativeViewport(camera.viewport[0], y, camera.viewport[2], camera.viewport[3], camera.viewport[4], camera.viewport[5]);
}
REGISTER_NATIVE_FUNCTION_AS(0x80075740, MH3_CameraApplyViewport, "CameraApplyViewport");
extern "C" void MH3_CameraApplyProjection(CpuContext* cpu) {
    const uint32_t resource = Resource(cpu->gpr[3]);
    auto camera = Camera(resource);
    Checked(mh3_camera_prepare_projection(&camera));
    StoreCamera(resource, camera);
    SetNativeProjection(camera.projection, camera.projection_type == 0);
}
REGISTER_NATIVE_FUNCTION_AS(0x8007580C, MH3_CameraApplyProjection, "CameraApplyProjection");
extern "C" void MH3_CameraApplyScissor(CpuContext* cpu) {
    ApplyScissor(Camera(Resource(cpu->gpr[3])));
}
REGISTER_NATIVE_FUNCTION_AS(0x800758AC, MH3_CameraApplyScissor, "CameraApplyScissor");
extern "C" void MH3_CameraApplyScissorOffset(CpuContext* cpu) {
    ApplyScissor(Camera(Resource(cpu->gpr[3])));
}
REGISTER_NATIVE_FUNCTION_AS(0x8007592C, MH3_CameraApplyScissorOffset, "CameraApplyScissorOffset");

extern "C" void MH3_FogRange(CpuContext* cpu) {
    const uint32_t resource = Resource(cpu->gpr[3]);
    float projection[16];
    uint16_t table[10];
    for (uint32_t i = 0; i < 16; i++)
        projection[i] = Memory::ReadFloat32(cpu->gpr[6] + i * 4);
    Checked(mh3_fog_range_table(uint16_t(cpu->gpr[4]), projection, table));
    Checked(mh3_fog_range_store(Mutable(resource, 48), uint16_t(cpu->gpr[5]), table));
}
REGISTER_NATIVE_FUNCTION_AS(0x8007A30C, MH3_FogRange, "FogRange");
extern "C" void MH3_FogApply(CpuContext* cpu) {
    mh3_native_fog fog{};
    Checked(mh3_fog_read(Bytes(Resource(cpu->gpr[3]), 48), &fog));
    SetNativeFog(fog);
}
REGISTER_NATIVE_FUNCTION_AS(0x8007A3B8, MH3_FogApply, "FogApply");
extern "C" void MH3_FogSelect(CpuContext* cpu) {
    mh3_native_fog fog{};
    int apply = 0;
    Checked(mh3_fog_state_select(Mutable(cpu->gpr[3], 1544), cpu->gpr[4], &fog, &apply));
    if (apply)
        SetNativeFog(fog);
}
REGISTER_NATIVE_FUNCTION_AS(0x80088200, MH3_FogSelect, "FogSelect");

extern "C" void MH3_ModelLightSetting(CpuContext* cpu) {
    const uint32_t state = cpu->gpr[3], setting = cpu->gpr[4];
    const uint16_t count = Memory::Read16(setting), sets = Memory::Read16(setting + 2);
    if (count > 128 || sets > 128)
        throw std::runtime_error("Native LightSetting capacity exceeds128");
    mh3_matrix34 view{};
    Checked(mh3_scene_view_matrix(Bytes(0x80688d94, 52 + 32 * 48), &view));
    mh3_native_light_slots slots{};
    Checked(mh3_light_state_import(Mutable(state, MH3_LIGHT_STATE_BYTES), count, sets, count ? Bytes(Memory::Read32(setting + 4), size_t(count) * 68) : mh3_res_span{},
                                   count ? Bytes(Memory::Read32(setting + 8), size_t(count) * 4) : mh3_res_span{}, sets ? Bytes(Memory::Read32(setting + 12), size_t(sets) * 12) : mh3_res_span{},
                                   &view, &slots));
    const auto& entry = light_slots.insert_or_assign(state, LightSlots{slots, ++light_revision}).first->second;
    SetNativeModelLightSelection(entry.slots, &entry, entry.revision);
}
REGISTER_NATIVE_FUNCTION_AS(0x8008551C, MH3_ModelLightSetting, "ModelLightSetting");
const mh3_native_light_slots& SelectNativeModelLightSet(uint32_t state, uint32_t set_id) {
    const auto found = light_slots.find(state);
    if (found == light_slots.end())
        throw std::runtime_error("LightState selection has no typed import producer");
    auto& entry = found->second;
    const auto bytes = Mutable(state, MH3_LIGHT_STATE_BYTES);
    // mh3_light_state_select decodes and reassigns slots only for a set other
    // than the one selected last (LightState +0x10); see its no-set path too.
    if (set_id < 128 && Memory::Read32(state + 16) != set_id)
        entry.revision = ++light_revision;
    Checked(mh3_light_state_select(bytes, set_id, &entry.slots));
    SetNativeModelLightSelection(entry.slots, &entry, entry.revision);
    return entry.slots;
}
extern "C" void MH3_ModelLightSelect(CpuContext* cpu) {
    const auto& slots = SelectNativeModelLightSet(cpu->gpr[3], cpu->gpr[4]);
    const uint32_t masks[]{slots.diffuse_color, slots.diffuse_alpha, slots.specular_color, slots.specular_alpha};
    for (uint32_t i = 0; i < 4; i++)
        if (cpu->gpr[5 + i])
            Memory::Write32(cpu->gpr[5 + i], masks[i]);
    if (cpu->gpr[9]) {
        auto* destination = GuestRange(cpu->gpr[9], 4);
        destination[0] = slots.ambient.r;
        destination[1] = slots.ambient.g;
        destination[2] = slots.ambient.b;
        destination[3] = slots.ambient.a;
    }
}
REGISTER_NATIVE_FUNCTION_AS(0x80085758, MH3_ModelLightSelect, "ModelLightSelect");

void InvalidateNativeModelCaches(uint32_t fields) {
    const mh3_model_cache_storage storage{Mutable(0x80685480, 1),  Mutable(0x80685500, 2),  Mutable(0x8068530c, 4),  Mutable(0x80796dbc, 4), Mutable(0x80685300, 12),
                                          Mutable(0x80685504, 12), Mutable(0x80796db4, 1),  Mutable(0x80685510, 32), Mutable(0x80796db8, 4), Mutable(0x80796dc8, 4),
                                          Mutable(0x80685d60, 4),  Mutable(0x80686378, 28), Mutable(0x80685530, 12), Mutable(0x80796db0, 1)};
    Checked(mh3_model_cache_invalidate(storage, fields));
    // The render thread's deferred preparation mirrors these caches.
    NoteNativeModelPrepInvalidate(fields);
    if (fields & 1024) {
        const uint32_t absent[]{0, 0, 0};
        StoreNativeModelMatrixSources(0x80685530, absent);
    }
}
extern "C" void MH3_ModelInvalidate(CpuContext* cpu) {
    InvalidateNativeModelCaches(cpu->gpr[3]);
}
REGISTER_NATIVE_FUNCTION_AS(0x800885F4, MH3_ModelInvalidate, "ModelInvalidate");
extern "C" void MH3_ModelRenderMode(CpuContext* cpu) {
    cpu->gpr[3] = 0x8068a7c8;
}
REGISTER_NATIVE_FUNCTION_AS(0x800885E8, MH3_ModelRenderMode, "ModelRenderMode");

extern "C" void MH3_ModelTextureObject(CpuContext* cpu) {
    const uint32_t info = Resource(cpu->gpr[3]), objects = Resource(cpu->gpr[5]);
    const auto source = ReadNativeResTexture(cpu->gpr[4]);
    mh3_native_texture_binding binding{};
    Checked(mh3_texture_binding_read(Bytes(info, 52), &source, &binding));
    StoreNativeTextureObject(objects + 4 + binding.map_id * 32, binding);
    Checked(mh3_texture_info_store_source(Mutable(info, 16), source.resource_address, 0));
    Checked(mh3_resource_objects_validate(Mutable(objects, 260), binding.map_id, 0));
}
REGISTER_NATIVE_FUNCTION_AS(0x800976C0, MH3_ModelTextureObject, "ModelTextureObject");
extern "C" void MH3_ModelPaletteObject(CpuContext* cpu) {
    const uint32_t info = Resource(cpu->gpr[3]), objects = Resource(cpu->gpr[5]), slot = Memory::Read32(info + 16);
    if (slot >= 8)
        throw std::runtime_error("Native material palette slot outside0..7");
    const auto palette = ReadNativeResPalette(cpu->gpr[4]);
    StoreNativePaletteObject(objects + 4 + slot * 12, palette);
    Checked(mh3_texture_info_store_source(Mutable(info, 16), palette.resource_address, 1));
    Checked(mh3_resource_objects_validate(Mutable(objects, 100), slot, 1));
}
REGISTER_NATIVE_FUNCTION_AS(0x80097990, MH3_ModelPaletteObject, "ModelPaletteObject");

extern "C" void MH3_ModelTextureReplace(CpuContext* cpu) {
    const uint32_t objects = Resource(cpu->gpr[3]), palettes = Resource(cpu->gpr[4]), result = cpu->gpr[5];
    const auto texture_mask = Memory::Read8(result), palette_mask = Memory::Read8(result + 1);
    for (uint32_t i = 0; i < 8; i++) {
        if (texture_mask & (1u << i)) {
            const uint32_t object = objects + 4 + i * 32;
            auto binding = ReadNativeTextureObject(object);
            binding.source = ReadNativeResTexture(result + 4 + i * 4);
            binding.map_id = i;
            binding.tlut_id = i;
            StoreNativeTextureObject(object, binding);
        }
        if (palette_mask & (1u << i))
            StoreNativePaletteObject(palettes + 4 + i * 12, ReadNativeResPalette(result + 0x24 + i * 4));
    }
}
REGISTER_NATIVE_FUNCTION_AS(0x80069A2C, MH3_ModelTextureReplace, "ModelTextureReplace");
extern "C" void MH3_ModelTextureSelect(CpuContext* cpu) {
    SelectNativeModelTextureObjects(cpu->gpr[3], Resource(cpu->gpr[4]));
}
REGISTER_NATIVE_FUNCTION_AS(0x80085EAC, MH3_ModelTextureSelect, "ModelTextureSelect");
void SelectNativeModelTextureObjects(uint32_t state, uint32_t objects) {
    uint8_t changed = 0;
    Checked(mh3_resource_objects_cache(Mutable(state, 257), Bytes(objects, 260), 0, &changed));
    for (uint32_t i = 0; i < 8; i++) {
        int present = 0;
        Checked(mh3_resource_objects_present(Bytes(objects, 260), i, 0, &present));
        if (!present)
            continue;
        auto binding = ReadNativeTextureObject(objects + 4 + i * 32);
        binding.map_id = i;
        // Producer generation can change without changing CPU object bytes.
        // The backend checks explicit identity/revision, never texture content.
        BindNativeTexture(binding);
        if (changed & (1u << i)) {
            if (binding.source.width > UINT16_MAX || binding.source.height > UINT16_MAX)
                throw std::runtime_error("Native G3D texture dimensions exceed coordinate cache");
            Checked(mh3_texture_scale_size(Mutable(0x8068530c, 36), i, uint16_t(binding.source.width), uint16_t(binding.source.height)));
            PublishNativeModelMapSize(i, uint16_t(binding.source.width), uint16_t(binding.source.height));
        }
    }
}
extern "C" void MH3_ModelPaletteSelect(CpuContext* cpu) {
    SelectNativeModelPaletteObjects(cpu->gpr[3], Resource(cpu->gpr[4]));
}
REGISTER_NATIVE_FUNCTION_AS(0x800860A8, MH3_ModelPaletteSelect, "ModelPaletteSelect");
void SelectNativeModelPaletteObjects(uint32_t state, uint32_t objects) {
    uint8_t changed = 0;
    Checked(mh3_resource_objects_cache(Mutable(state, 98), Bytes(objects, 100), 1, &changed));
    for (uint32_t i = 0; i < 8; i++) {
        int present = 0;
        Checked(mh3_resource_objects_present(Bytes(objects, 100), i, 1, &present));
        if (present)
            BindNativePalette(i, ReadNativePaletteObject(objects + 4 + i * 12));
    }
    Checked(mh3_texture_cache_palette_changed(Mutable(0x80685380, 257), changed));
}
extern "C" void MH3_ModelDepthCompare(CpuContext* cpu) {
    int apply = 0;
    Checked(mh3_depth_compare_cache(Mutable(cpu->gpr[3], 5), Mutable(0x80796db0, 1), uint8_t(cpu->gpr[4]) != 0, &apply));
    if (apply)
        SetNativeDepthCompareBeforeTexture(uint8_t(cpu->gpr[4]) != 0);
}
REGISTER_NATIVE_FUNCTION_AS(0x80085DFC, MH3_ModelDepthCompare, "ModelDepthCompare");
extern "C" void MH3_ModelTextureInitialize(CpuContext* cpu) {
    InitializeNativeTextureSource(cpu->gpr[3], false);
}
REGISTER_NATIVE_FUNCTION_AS(0x8009A784, MH3_ModelTextureInitialize, "ModelTextureInitialize");
extern "C" void MH3_ModelPaletteInitialize(CpuContext* cpu) {
    InitializeNativeTextureSource(cpu->gpr[3], true);
}
REGISTER_NATIVE_FUNCTION_AS(0x80093AFC, MH3_ModelPaletteInitialize, "ModelPaletteInitialize");
extern "C" void MH3_ModelTextureCopy(CpuContext* cpu) {
    const uint32_t destination = cpu->gpr[4];
    CopyNativeResourceObjects(destination, Resource(cpu->gpr[3]), false);
    cpu->gpr[3] = destination;
}
REGISTER_NATIVE_FUNCTION_AS(0x80094C00, MH3_ModelTextureCopy, "ModelTextureCopy");
extern "C" void MH3_ModelPaletteCopy(CpuContext* cpu) {
    const uint32_t destination = cpu->gpr[4];
    CopyNativeResourceObjects(destination, Resource(cpu->gpr[3]), true);
    cpu->gpr[3] = destination;
}
REGISTER_NATIVE_FUNCTION_AS(0x80095114, MH3_ModelPaletteCopy, "ModelPaletteCopy");
