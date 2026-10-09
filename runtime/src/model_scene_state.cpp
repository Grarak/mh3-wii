// RMHP08 camera, fog and CPU cache ABI adapters. Resource arithmetic and
// serialized updates live in portable C. Backend receives ordinary records;
// no guest rendering functions, GX calls or source command lists are used.
#include "abi_bridge.h"
#include "fatal.h"
#include <aurora/fast_map.h>
#include "guest_span.h"
#include "memory.h"
#include "native_graphics.h"
#include "runtime/native/adapters/model_state_bridge.h"
#include "runtime/native/adapters/model_scope_bridge.h"
#include "runtime/native/mh3_model_state.h"
#include "model_render_prep.h"
#include "port_check.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <utility>
#include <string>
#include <cstring>

const mh3_res_memory& MH3NativeResourceMemory();
namespace {
[[noreturn, gnu::cold, gnu::noinline]] void CheckedFailed(mh3_res_status status) {
    MH3_THROW(std::runtime_error("Native scene input failed: " + std::to_string(status)));
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
    mh3_native_camera camera; // mh3_camera_read writes every field
    Checked(mh3_camera_read(Bytes(resource, 268), &camera));
    return camera;
}

void StoreCamera(uint32_t resource, const mh3_native_camera& camera) {
    Checked(mh3_camera_store_matrices(Mutable(resource, 116), &camera));
}

// The preparations compute a matrix only while its valid flag (8 view, 128
// projection) is clear, and then set it: an unchanged flags word means the
// record already holds what a store would write.
void StorePreparedCamera(uint32_t resource, const mh3_native_camera& camera, uint32_t flags) {
    if (camera.flags != flags)
        StoreCamera(resource, camera);
}

// The getters' matrix the record already holds (its valid flag set) is
// copied out as stored, without decoding the 268-byte record: the scene walk
// asks for the view and the projection per model, and only the first request
// after a camera change prepares them.
bool CopyPreparedMatrix(uint32_t resource, uint32_t flag, uint32_t offset, uint32_t bytes, uint32_t destination) {
    if (!(Memory::Read32(resource + 112) & flag))
        return false;

    std::memmove(GuestRange(destination, bytes), GuestRange(resource + offset, bytes), bytes);
    return true;
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
    const uint32_t flags = camera.flags;
    Checked(mh3_camera_prepare_projection(&camera));
    StorePreparedCamera(resource, camera, flags);

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
    const uint32_t flags = camera.flags;
    const mh3_vec3 point{{Memory::ReadFloat32(cpu->gpr[5]), Memory::ReadFloat32(cpu->gpr[5] + 4), Memory::ReadFloat32(cpu->gpr[5] + 8)}};
    mh3_vec3 result{};
    Checked(mh3_camera_project(&camera, &point, &result));
    StorePreparedCamera(resource, camera, flags);
    StoreFloats(cpu->gpr[4], result.v, 3);
}
REGISTER_NATIVE_FUNCTION_AS(0x800752BC, MH3_CameraProject, "CameraProject");

namespace {
void CameraView(uint32_t resource, uint32_t destination) {
    if (CopyPreparedMatrix(resource, 8, 0, 48, destination))
        return;

    auto camera = Camera(resource);
    const uint32_t flags = camera.flags;
    Checked(mh3_camera_prepare_view(&camera));
    StorePreparedCamera(resource, camera, flags);
    StoreFloats(destination, &camera.view.v[0][0], 12);
}

void CameraProjection(uint32_t resource, uint32_t destination) {
    if (CopyPreparedMatrix(resource, 128, 48, 64, destination))
        return;

    auto camera = Camera(resource);
    const uint32_t flags = camera.flags;
    Checked(mh3_camera_prepare_projection(&camera));
    StorePreparedCamera(resource, camera, flags);
    StoreFloats(destination, camera.projection, 16);
}

void ApplyViewport(uint32_t resource) {
    const auto camera = Camera(resource);
    auto y = camera.viewport[1];

    if (Memory::Read8(0x8068a7e0) && !(camera.flags & 256))
        y -= 0.5f;

    SetNativeViewport(camera.viewport[0], y, camera.viewport[2], camera.viewport[3], camera.viewport[4], camera.viewport[5]);
}

void ApplyProjection(uint32_t resource) {
    auto camera = Camera(resource);
    const uint32_t flags = camera.flags;
    Checked(mh3_camera_prepare_projection(&camera));
    StorePreparedCamera(resource, camera, flags);
    SetNativeProjection(camera.projection, camera.projection_type == 0);
}
} // namespace

extern "C" void MH3_CameraView(CpuContext* cpu) {
    CameraView(Resource(cpu->gpr[3]), cpu->gpr[4]);
}
REGISTER_NATIVE_FUNCTION_AS(0x800753F8, MH3_CameraView, "CameraView");
extern "C" void MH3_CameraProjection(CpuContext* cpu) {
    CameraProjection(Resource(cpu->gpr[3]), cpu->gpr[4]);
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
    ApplyViewport(Resource(cpu->gpr[3]));
}
REGISTER_NATIVE_FUNCTION_AS(0x80075740, MH3_CameraApplyViewport, "CameraApplyViewport");
extern "C" void MH3_CameraApplyProjection(CpuContext* cpu) {
    ApplyProjection(Resource(cpu->gpr[3]));
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

namespace {
void FogRange(uint32_t resource, uint16_t width, uint16_t center, const float projection[16]) {
    uint16_t table[10];
    Checked(mh3_fog_range_table(width, projection, table));
    Checked(mh3_fog_range_store(Mutable(resource, 48), center, table));
}
} // namespace

extern "C" void MH3_FogRange(CpuContext* cpu) {
    float projection[16];

    for (uint32_t i = 0; i < 16; i++)
        projection[i] = Memory::ReadFloat32(cpu->gpr[6] + i * 4);

    FogRange(Resource(cpu->gpr[3]), uint16_t(cpu->gpr[4]), uint16_t(cpu->gpr[5]), projection);
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

namespace {
void ModelLightSetting(uint32_t state, uint32_t setting) {
    const uint16_t count = Memory::Read16(setting), sets = Memory::Read16(setting + 2);

    if (count > 128 || sets > 128)
        MH3_THROW(std::runtime_error("Native LightSetting capacity exceeds128"));

    mh3_matrix34 view{};
    Checked(mh3_scene_view_matrix(Bytes(0x80688d94, 52 + 32 * 48), &view));

    // Imported straight into the state's entry (a failed import stops the run).
    auto& entry = light_slots[state];
    Checked(mh3_light_state_import(Mutable(state, MH3_LIGHT_STATE_BYTES), count, sets, count ? Bytes(Memory::Read32(setting + 4), size_t(count) * 68) : mh3_res_span{},
                                   count ? Bytes(Memory::Read32(setting + 8), size_t(count) * 4) : mh3_res_span{}, sets ? Bytes(Memory::Read32(setting + 12), size_t(sets) * 12) : mh3_res_span{},
                                   &view, &entry.slots));
    entry.revision = ++light_revision;
    SetNativeModelLightSelection(entry.slots, &entry, entry.revision);
}
} // namespace

extern "C" void MH3_ModelLightSetting(CpuContext* cpu) {
    ModelLightSetting(cpu->gpr[3], cpu->gpr[4]);
}
REGISTER_NATIVE_FUNCTION_AS(0x8008551C, MH3_ModelLightSetting, "ModelLightSetting");

namespace {
// G3D's camera state (0x80688D94: u16 flags, the selected index, then 32
// views, projections, texture and environment matrices), fog state
// (0x80685D60: flags, the selected index, 32 FogData records) and light state.
constexpr uint32_t kCameraState = 0x80688d94, kFogState = 0x80685d60, kLightState = 0x80686368;

// FloatToU16Gqr3 (80082F7C): psq_st through GQR3, the game's u16 store type:
// scaled, clamped and truncated.
uint16_t QuantizeU16(float value, uint32_t gqr) {
    if ((gqr & 7) != 5)
        MH3_THROW(std::runtime_error("SetGlbSettings: GQR3 is not a u16 store type"));

    const int scale = int(gqr >> 8 & 0x3F);
    return uint16_t(std::clamp(value * std::ldexp(1.0f, scale < 32 ? scale : scale - 64), 0.0f, 65535.0f));
}

// The record's projection, prepared and stored first while its valid flag is clear.
void PreparedProjection(uint32_t resource, float out[16]) {
    if (!(Memory::Read32(resource + 112) & 128)) {
        auto camera = Camera(resource);
        const uint32_t flags = camera.flags;
        Checked(mh3_camera_prepare_projection(&camera));
        StorePreparedCamera(resource, camera, flags);
    }

    for (uint32_t i = 0; i < 16; i++)
        out[i] = Memory::ReadFloat32(resource + 48 + i * 4);
}

// G3dStateSetCamera (80085CD4) for one of the root's CameraData records.
void SetStateCamera(uint32_t resource, uint32_t index) {
    float texture[12], environment[12];
    CameraView(resource, kCameraState + 0x34 + index * 48);
    CameraProjection(resource, kCameraState + 0x634 + index * 64);
    Checked(mh3_camera_texture_matrices(Bytes(resource, 268), texture, environment));
    StoreFloats(kCameraState + 0xE34 + index * 48, texture, 12);
    StoreFloats(kCameraState + 0x1434 + index * 48, environment, 12);
}

// The fog state's slot (800880EC): a changed record for the selected slot
// clears the state's flags so the next selection applies it.
void SetStateFog(uint32_t resource, uint32_t index) {
    const uint8_t* fog = GuestRange(resource, 48);
    uint8_t* slot = GuestRange(kFogState + 8 + index * 48, 48);

    if (Memory::Read32(kFogState + 4) == index) {
        if (!std::memcmp(fog, slot, 48))
            return;

        Memory::Write32(kFogState, 0);
    }

    std::memcpy(slot, fog, 48);
}

// 80082D40 ScnRoot::SetGlbSettings: the current camera's viewport, projection
// and scissor; the G3D camera state for all 32 CameraData records (+F8,
// stride 268) with the current one (+F4) selected; the 32 FogData records
// (+2278, stride 48) given the camera's planes and orthographic flag, range
// tables for the range-adjusted ones, and copied to the fog state; then the
// root's LightSetting (+2878) imported against the selected view (800882B4).
void SetGlbSettings(CpuContext* cpu) {
    const uint32_t root = cpu->gpr[3], current = Memory::Read8(root + 0xF4), camera = root + 0xF8 + current * 268;
    ApplyViewport(camera);
    ApplyProjection(camera);

    // CameraApplyScissor and CameraApplyScissorOffset set the same rectangle.
    ApplyScissor(Camera(camera));

    for (uint32_t i = 0; i < 32; i++)
        SetStateCamera(root + 0xF8 + i * 268, i);

    if (current < 32) {
        Memory::Write16(kCameraState + 2, uint16_t(current));
        Memory::Write16(kCameraState, Memory::Read16(kCameraState) & 0xFFFE);
    }

    const float near_plane = Memory::ReadFloat32(camera + 180), far_plane = Memory::ReadFloat32(camera + 184);
    const uint32_t orthographic = Memory::Read32(camera + 168) == 1 ? 8 : 0;
    float projection[16];
    uint16_t width = 0, center = 0;
    bool ranged = false;

    for (uint32_t i = 0; i < 32; i++) {
        const uint32_t fog = root + 0x2278 + i * 48;
        Memory::WriteFloat32(fog + 12, near_plane);
        Memory::WriteFloat32(fog + 16, far_plane);

        if (const uint32_t type = Memory::Read32(fog))
            Memory::Write32(fog, (type & ~8u) | orthographic);

        if (Memory::Read8(fog + 24) == 1) {
            if (!ranged) {
                const float x = Memory::ReadFloat32(camera + 220), extent = Memory::ReadFloat32(camera + 228);
                PreparedProjection(camera, projection);
                width = QuantizeU16(extent, cpu->gqr[3]);
                center = QuantizeU16(x + 0.5f * extent, cpu->gqr[3]);
                ranged = true;
            }

            FogRange(fog, width, center, projection);
        }

        SetStateFog(fog, i);
    }

    ModelLightSetting(kLightState, root + 0x2878);
}
} // namespace

extern "C" void func_80082D40(CpuContext* ctx);

extern "C" void MH3_ScnRootSetGlbSettings(CpuContext* cpu) {
#ifdef MH3_DEVELOPMENT
    // Its writes: the G3D camera, fog and light states and the root's camera
    // and fog records (the walk runs beside the game thread, so no RAM-wide diff).
    if (PortCheck::Enabled("ScnRootSetGlbSettings")) [[unlikely]] {
        const uint32_t root = cpu->gpr[3];
        PortCheck::RunOutput(cpu, "ScnRootSetGlbSettings", func_80082D40, SetGlbSettings,
                             {{kCameraState, 0x1A34}, {kFogState, 8 + 32 * 48}, {kLightState, MH3_LIGHT_STATE_BYTES}, {root + 0xF8, 32 * 268}, {root + 0x2278, 32 * 48}});
        return;
    }
#endif
    SetGlbSettings(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x80082D40, MH3_ScnRootSetGlbSettings, "ScnRootSetGlbSettings");
const mh3_native_light_slots& SelectNativeModelLightSet(uint32_t state, uint32_t set_id) {
    const auto found = light_slots.find(state);

    if (found == light_slots.end())
        MH3_THROW(std::runtime_error("LightState selection has no typed import producer"));

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

// The overlapped draw (docs/optimizations/threading/pipelined-draw.md): the model caches above are the
// draw thread's while it draws the scene. A logic step's invalidation (the
// scene update's, at the step's start) is kept for the next scene, which
// applies it when it starts (ApplyDeferredModelInvalidate).
bool deferModelInvalidate = false;
uint32_t deferredModelInvalidate = 0;

void SetModelInvalidateDeferral(bool enabled) {
    deferModelInvalidate = enabled;
}

void ApplyDeferredModelInvalidate() {
    if (const uint32_t fields = std::exchange(deferredModelInvalidate, 0))
        InvalidateNativeModelCaches(fields);
}

extern "C" void MH3_ModelInvalidate(CpuContext* cpu) {
    if (deferModelInvalidate && PpcThread::CurrentThread() == PpcThread::Game) {
        deferredModelInvalidate |= cpu->gpr[3];
        return;
    }

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
        MH3_THROW(std::runtime_error("Native material palette slot outside0..7"));

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
                MH3_THROW(std::runtime_error("Native G3D texture dimensions exceed coordinate cache"));

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
