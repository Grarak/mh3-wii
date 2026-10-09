// Thin RMHP08 ABI adapters for typed texture matrices and view-array ownership.
// Pure resource/math logic is C11; no GX, FIFO or guest callback is invoked.
#include "abi_bridge.h"
#include "memory.h"
#include "native_graphics.h"
#include "runtime/native/adapters/model_state_bridge.h"
#include "runtime/native/mh3_texture_matrices.h"
#include <stdexcept>
#include <string>

const mh3_res_memory& MH3NativeResourceMemory();
namespace {
void Checked(mh3_res_status status) {
    if (status != MH3_RES_OK)
        throw std::runtime_error("Native texture matrix input failed: " + std::to_string(status));
}
mh3_res_span Bytes(uint32_t address, size_t size) {
    return {Memory::GetPointer(address, size), size};
}
mh3_res_mut_span Mutable(uint32_t address, size_t size) {
    return {Memory::GetPointer(address, size), size};
}
uint32_t Resource(uint32_t reference) {
    uint32_t resource = 0;
    Checked(mh3_res_runtime_reference(&MH3NativeResourceMemory(), reference, 1, &resource));
    return resource;
}
mh3_matrix34 Matrix(uint32_t address) {
    mh3_matrix34 m{};
    Checked(mh3_matrix_read34({Bytes(address, 48), 1, 48, 1}, 0, &m));
    return m;
}
void StoreMatrix(uint32_t destination, const mh3_matrix34& m) {
    for (uint32_t r = 0; r < 3; r++)
        for (uint32_t c = 0; c < 4; c++)
            Memory::WriteFloat32(destination + (r * 4 + c) * 4, m.v[r][c]);
}
mh3_pack_texture_srt Srt(uint32_t address, uint32_t flags) {
    mh3_pack_texture_srt s{};
    s.flags = flags;
    s.scale[0] = Memory::ReadFloat32(address);
    s.scale[1] = Memory::ReadFloat32(address + 4);
    s.rotation_degrees = Memory::ReadFloat32(address + 8);
    s.translation[0] = Memory::ReadFloat32(address + 12);
    s.translation[1] = Memory::ReadFloat32(address + 16);
    return s;
}
} // namespace

extern "C" void MH3_CalcTextureSrt(CpuContext* cpu) {
    auto matrix = Matrix(cpu->gpr[3]);
    const auto srt = Srt(cpu->gpr[5], cpu->gpr[6]);
    Checked(mh3_texture_srt_calc(&srt, cpu->gpr[7], cpu->gpr[4] != 0, &matrix));
    StoreMatrix(cpu->gpr[3], matrix);
}
REGISTER_NATIVE_FUNCTION_AS(0x80075F00, MH3_CalcTextureSrt, "CalcTextureSrt");

extern "C" void MH3_CalcTextureSrtXsi(CpuContext* cpu) {
    const auto srt = Srt(cpu->gpr[5], cpu->gpr[6]);
    if (!(srt.flags & 1))
        throw std::runtime_error("XSI texture SRT lacks the existence flag");
    if (((srt.flags >> 1) & 7) == 7) {
        cpu->gpr[3] = 0;
        return;
    }
    auto matrix = Matrix(cpu->gpr[3]);
    Checked(mh3_texture_srt_calc(&srt, 1, cpu->gpr[4] != 0, &matrix));
    StoreMatrix(cpu->gpr[3], matrix);
    cpu->gpr[3] = 1;
}
REGISTER_NATIVE_FUNCTION_AS(0x800D754C, MH3_CalcTextureSrtXsi, "CalcTextureSrtXsi");

extern "C" void MH3_LoadModelTextureSrt(CpuContext* cpu) {
    LoadNativeModelTextureSrt(Resource(cpu->gpr[3]));
}
REGISTER_NATIVE_FUNCTION_AS(0x800871C0, MH3_LoadModelTextureSrt, "LoadModelTextureSrt");
void LoadNativeModelTextureSrt(uint32_t resource) {
    mh3_native_texture_srt srt{};
    Checked(mh3_texture_srt_read(Bytes(resource, 0x248), &srt));
    mh3_matrix34 maps[8]{};
    uint32_t sources[8]{};
    // The constructor defines these exact built-in mapping functions. Custom
    // slots may change their callback and type at runtime; reject those until
    // their own typed native producer is implemented.
    constexpr uint32_t functions[]{0x800850a0, 0x80084694, 0x80084a78, 0x80084694, 0x80084c00};
    constexpr uint32_t kinds[]{0, 1, 2, 1, 1};
    for (uint32_t i = 0; i < 8; i++) {
        const auto& slot = srt.slot[i];
        if (!(slot.flags & 1) || !slot.map_mode)
            continue;
        const uint32_t mode = slot.map_mode, entry = 0x80685560 + mode * 8;
        const uint32_t builtin = mode < 5 ? mode : 0;
        if (Memory::Read32(entry) != functions[builtin] || Memory::Read32(entry + 4) != kinds[builtin])
            throw std::runtime_error("Custom model texture mapping has no native producer: " + std::to_string(mode));
        if (builtin) {
            mh3_texture_mapping_input input{};
            Checked(mh3_texture_mapping_read(Mutable(0x80688d94, 0x1a34), Bytes(0x80686368, MH3_LIGHT_STATE_BYTES), builtin, slot.camera_reference, slot.light_reference, &input));
            Checked(mh3_texture_mapping(builtin, &input, &maps[i]));
        } else
            Checked(mh3_texture_mapping(0, nullptr, &maps[i]));
        sources[i] = kinds[builtin];
    }
    mh3_texture_matrices matrices{};
    uint32_t next = 0;
    Checked(mh3_texture_matrices_prepare(&srt, maps, sources, Memory::Read32(0x80796db8), &matrices, &next));
    for (uint32_t i = 0; i < 8; i++) {
        Memory::Write32(0x80685510 + i * 4, matrices.source[i]);
        if (matrices.apply & (1u << i))
            SetNativeTextureMatrix(64 + i * 3, &matrices.matrix[i].v[0][0], true);
    }
    Memory::Write32(0x80796db8, next);
}

extern "C" void MH3_NormalTextureMatrix(CpuContext* cpu) {
    mh3_matrix33 normal{};
    mh3_matrix34 matrix{};
    Checked(mh3_matrix_read33({Bytes(cpu->gpr[3], 36), 1, 36, 1}, 0, &normal));
    Checked(mh3_texture_matrix_from_normal(&normal, &matrix));
    SetNativeTextureMatrix(cpu->gpr[4], &matrix.v[0][0], true);
}
REGISTER_NATIVE_FUNCTION_AS(0x8009B330, MH3_NormalTextureMatrix, "NormalTextureMatrix");

extern "C" void MH3_ModelMatrixSourceState(CpuContext* cpu) {
    const uint32_t addresses[]{cpu->gpr[4], cpu->gpr[5], cpu->gpr[6]};
    StoreNativeModelMatrixSources(cpu->gpr[3], addresses);
}
REGISTER_NATIVE_FUNCTION_AS(0x80087E7C, MH3_ModelMatrixSourceState, "ModelMatrixSourceState");
extern "C" void MH3_ModelMatrixSources(CpuContext* cpu) {
    const uint32_t addresses[]{cpu->gpr[3], cpu->gpr[4], cpu->gpr[5]};
    StoreNativeModelMatrixSources(0x80685530, addresses);
}
REGISTER_NATIVE_FUNCTION_AS(0x80087E5C, MH3_ModelMatrixSources, "ModelMatrixSources");

extern "C" void MH3_ModelLightReference(CpuContext* cpu) {
    int32_t id = 0;
    Checked(mh3_model_light_reference(Bytes(cpu->gpr[3], MH3_LIGHT_STATE_BYTES), int32_t(cpu->gpr[4]), &id));
    cpu->gpr[3] = uint32_t(id);
}
REGISTER_NATIVE_FUNCTION_AS(0x80085BB8, MH3_ModelLightReference, "ModelLightReference");
extern "C" void MH3_CurrentModelLightReference(CpuContext* cpu) {
    int32_t id = 0;
    Checked(mh3_model_light_reference(Bytes(0x80686368, MH3_LIGHT_STATE_BYTES), int32_t(cpu->gpr[3]), &id));
    cpu->gpr[3] = uint32_t(id);
}
REGISTER_NATIVE_FUNCTION_AS(0x800882D4, MH3_CurrentModelLightReference, "CurrentModelLightReference");
