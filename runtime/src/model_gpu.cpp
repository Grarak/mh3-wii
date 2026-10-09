// Own immutable GPU payloads independently of changing material/pose state.
#include "../native/adapters/model_gpu_bridge.h"
#include "gl/gl.hpp"
#include "gfx/native_mesh_layout.hpp"
#include <map>
#include <memory>
#include <stdexcept>
#include <vector>
namespace aurora::gfx {
uint32_t current_frame() noexcept;
}

namespace {
struct Resident {
    mh3_mesh_pack* pack;
    uint64_t generation;
    std::unique_ptr<aurora::gl::NativeMeshBuffer> buffer;
    std::vector<aurora::gl::MeshLayout> layouts;
    std::vector<mh3_pack_mesh> meshes;
};
std::map<uintptr_t, Resident> residents;
// Advances whenever a resident is registered, retired or cleared.
uint64_t residentsRevision = 0;
// The resident the last submission resolved: a model's meshes draw in a row.
// Forgotten when a resident retires (map insertions keep it valid).
uintptr_t lastHandle = 0;
const Resident* lastResident = nullptr;
void Owner() {
    if (!aurora::gl::on_render_thread())
        throw std::runtime_error("Model GPU ownership outside graphics owner");
}
aurora::gl::MeshLayout Layout(const mh3_pack_layout& source) {
    using F = aurora::gl::MeshAttributeFormat;
    if (!source.stride || source.stride > UINT16_MAX)
        throw std::runtime_error("Compact model stride exceeds hardware limits");
    aurora::gl::MeshLayout out;
    out.stride = uint16_t(source.stride);
    out.matrixIndexMask = uint16_t((source.flags >> 8) & 0x1ffu);
    for (unsigned i = 0; i < source.count; ++i) {
        const auto& a = source.attributes[i];
        F format;
        switch (a.format) {
        case 9:
            format = F::F32;
            break;
        case 8:
            format = F::F16;
            break;
        case 7:
            format = F::S16N;
            break;
        case 4:
            format = F::U8N;
            break;
        case 0:
            format = F::U8;
            break;
        default:
            throw std::runtime_error("Unsupported compact model attribute format");
        }
        if (a.semantic > 14)
            throw std::runtime_error("Compact model needs an unsupported attribute producer");
        out.attributes.push_back({a.offset, uint8_t(a.semantic), a.components, format});
    }
    aurora::gfx::validate_mesh_layout(out);
    return out;
}
} // namespace
void RegisterNativeModelGPU(mh3_mesh_pack& pack, const char* path, uintptr_t handle, uint64_t generation) {
    Owner();
    if (!handle || !generation || !path || residents.contains(handle))
        throw std::runtime_error("Invalid or duplicate model GPU registration");
    Resident next{&pack, generation, std::make_unique<aurora::gl::NativeMeshBuffer>(path, pack.gpu_file_offset, pack.gpu_bytes), {}, {}};
    auto bound = pack;
    const auto bindStatus = mh3_mesh_pack_bind_gpu(&bound, {next.buffer->data(), next.buffer->size()});
    if (bindStatus != MH3_PACK_OK)
        throw std::runtime_error("Compact model GPU binding failed (status " + std::to_string(bindStatus) + "): " + path);
    const auto indexStatus = mh3_mesh_pack_validate_indices(&bound);
    if (indexStatus != MH3_PACK_OK)
        throw std::runtime_error("Compact model GPU index validation failed (status " + std::to_string(indexStatus) + "): " + path);
    next.layouts.reserve(pack.counts[MH3_PACK_LAYOUTS]);
    for (unsigned i = 0; i < pack.counts[MH3_PACK_LAYOUTS]; ++i) {
        mh3_pack_layout layout{};
        if (mh3_mesh_pack_layout(&bound, i, &layout) != MH3_PACK_OK)
            throw std::runtime_error("Invalid model vertex layout");
        next.layouts.push_back(Layout(layout));
    }
    next.meshes.resize(pack.counts[MH3_PACK_MESHES]);
    for (unsigned i = 0; i < next.meshes.size(); ++i)
        if (mh3_mesh_pack_mesh(&bound, i, &next.meshes[i]) != MH3_PACK_OK)
            throw std::runtime_error("Invalid resident model mesh");
    residents.emplace(handle, std::move(next));
    ++residentsRevision;
    // Only the GPU span changes; the game thread keeps reading the metadata.
    pack.gpu = bound.gpu;
}
void UnregisterNativeModelGPU(uintptr_t handle, uint64_t generation) {
    Owner();
    const auto found = residents.find(handle);
    if (found == residents.end() || found->second.generation != generation)
        throw std::runtime_error("Stale model GPU retirement");
    found->second.pack->gpu = {};
    // NativeMeshBuffer retirement preserves every submitted frame's backing.
    residents.erase(found);
    ++residentsRevision;
    lastHandle = 0;
    lastResident = nullptr;
}
void ClearNativeModelGPU() {
    Owner();
    for (auto& [handle, resident] : residents)
        resident.pack->gpu = {};
    residents.clear();
    ++residentsRevision;
    lastHandle = 0;
    lastResident = nullptr;
}
size_t NativeModelGPUAllocationBytes(uint32_t bytes) {
    Owner();
    return aurora::gl::native_mesh_allocation_bytes(bytes);
}
size_t NativeModelGPUResidentBytes() {
    Owner();
    return aurora::gl::native_mesh_resident_bytes();
}
// Plain reads (no owner check): the asset manager asks them for every draw.
uint32_t NativeModelGPUFrameSerial() {
    return aurora::gfx::current_frame();
}
uint64_t NativeModelGPURevision() {
    return residentsRevision;
}
NativeModelGPUView ResolveNativeModelGPU(const mh3_resolved_mesh_submit& mesh) {
    Owner();
    if (mesh.resident_handle != lastHandle || !lastResident) {
        const auto found = residents.find(mesh.resident_handle);
        if (found == residents.end())
            throw std::runtime_error("Stale or unregistered model GPU submission");
        lastHandle = mesh.resident_handle;
        lastResident = &found->second;
    }
    const auto& resident = *lastResident;
    if (resident.generation != mesh.resident_generation || resident.pack != mesh.pack || !mesh.source_generation || !mesh.pose_generation || mesh.mesh_index >= resident.meshes.size())
        throw std::runtime_error("Stale or unregistered model GPU submission");
    const auto& immutable = resident.meshes[mesh.mesh_index];
    if (immutable.asset_id != mesh.asset_id || immutable.model_id != mesh.model_id || immutable.source_id != mesh.shape_id || uint64_t(mesh.first_index) + mesh.index_count > immutable.index_count)
        throw std::runtime_error("Resident model identity or draw range mismatch");
    return {resident.buffer.get(), &resident.layouts.at(immutable.layout_index), &immutable};
}
