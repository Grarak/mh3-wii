// Thin RMHP08 ABI and owned producer registry. C11 owns resource/selector
// semantics; no guest function, GX, FIFO or vertex expansion is performed.
#include "abi_bridge.h"
#include "fast_map.h"
#include "guest_span.h"
#include "memory.h"
#include "native_graphics.h"
#include "runtime/native/adapters/model_pose_bridge.h"
#include "runtime/native/adapters/model_state_bridge.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

const mh3_res_memory& MH3NativeResourceMemory();
namespace {
// Pose owners are game-thread state.
FastMap<uint32_t, mh3_model_pose_owner> owners;
struct Region {
    uint32_t bytes;
    std::vector<uint32_t> scenes;
};
std::map<uint32_t, Region> regions;
FastMap<uint32_t, std::vector<uint32_t>> positions;
uint64_t pose_revision = 0;
constinit mh3_model_matrix_selectors selectors{};
// Advances on every selector change (deferred preparation copies them).
constinit uint64_t selectorRevision = 0;
// Loaded matrix slots per owner: UI/effect producers load the Game copy; model
// draws rebuild the Render copy while the render thread replays them.
struct MatrixPalette {
    mh3_model_palette palette{};
    uint64_t revision = 0;
};
constinit MatrixPalette matrixPalettes[NativeOwnerCount]{};
MatrixPalette& Palette(NativeOwner owner) {
    return matrixPalettes[unsigned(owner)];
}
[[noreturn, gnu::cold, gnu::noinline]] void CheckedFailed(mh3_res_status status) {
    throw std::runtime_error("Native model pose input failed: " + std::to_string(status));
}
inline void Checked(mh3_res_status status) {
    if (status != MH3_RES_OK) [[unlikely]]
        CheckedFailed(status);
}
uint32_t Canonical(uint32_t address) {
    return address ? (address & 0x3fffffffu) | 0x80000000u : 0;
}
void Reindex(const FastMap<uint32_t, mh3_model_pose_owner>& next) {
    std::map<uint32_t, Region> ranges;
    FastMap<uint32_t, std::vector<uint32_t>> arrays;
    auto add = [&](uint32_t start, uint32_t bytes, uint32_t scene) {
        if (!bytes)
            return;
        auto found = ranges.lower_bound(start);
        if (found != ranges.end() && found->first == start && found->second.bytes == bytes) {
            found->second.scenes.push_back(scene);
            return;
        }
        if ((found != ranges.end() && uint64_t(start) + bytes > found->first) || (found != ranges.begin() && uint64_t(std::prev(found)->first) + std::prev(found)->second.bytes > start))
            throw std::runtime_error("Native pose allocation has overlapping or inconsistent owners");
        ranges.emplace(start, Region{bytes, {scene}});
    };
    for (const auto& entry : next) {
        const auto& owner = entry.second;
        add(owner.scene_address, 0x104, entry.first);
        add(owner.model_address, 0x70, entry.first);
        for (unsigned i = 0; i < 3; i++)
            if (owner.base[i])
                add(owner.base[i], owner.views * owner.slice_bytes[i], entry.first);
        for (unsigned v = 0; v < owner.views; v++)
            if (owner.base[0] && owner.count)
                arrays[owner.base[0] + v * owner.slice_bytes[0]].push_back(entry.first);
    }
    regions = std::move(ranges);
    positions = std::move(arrays);
}
mh3_model_pose_owner& Declare(uint32_t scene) {
    mh3_model_pose_owner next{};
    Checked(mh3_model_pose_owner_read(&MH3NativeResourceMemory(), scene, &next));
    const auto found = owners.find(next.scene_address);
    if (found != owners.end()) {
        const auto& before = found->second;
        if (before.model_address == next.model_address && before.count == next.count && before.views == next.views &&
            std::equal(std::begin(before.base), std::end(before.base), std::begin(next.base))) {
            found->second.current_view = next.current_view;
            return found->second;
        }
    }
    // Admission/owner replacement pays index construction once. Steady state
    // getters, producer completion and draws do not scan every scene.
    auto prospective = owners;
    prospective.insert_or_assign(next.scene_address, next);
    Reindex(prospective);
    owners = std::move(prospective);
    return owners.at(next.scene_address);
}
void PublishSelectors() {
    ++selectorRevision;
    SetNativeModelMatrixSelectors(selectors);
}
void Select(const uint32_t rows[8]) {
    mh3_model_matrix_select(&selectors, rows);
    PublishSelectors();
}
} // namespace

bool PublishNativeModelPose(uint32_t position, uint32_t normal, uint32_t environment, uint32_t count, uint32_t reference) {
    if (!count)
        return false;
    uint32_t model = 0;
    Checked(mh3_res_runtime_reference(&MH3NativeResourceMemory(), reference, 1, &model));
    if (pose_revision == UINT64_MAX)
        throw std::runtime_error("Native model pose revision overflow");
    const uint64_t revision = pose_revision + 1;
    std::vector<mh3_model_pose_owner> updates;
    const auto found = positions.find(Canonical(position));
    if (found == positions.end())
        return false;
    for (const uint32_t scene : found->second) {
        auto next = owners.at(scene);
        const auto status = mh3_model_pose_publish(&next, position, normal, environment, count, model, revision);
        if (status == MH3_RES_ABSENT)
            continue;
        Checked(status);
        updates.push_back(next);
    }
    for (const auto& next : updates)
        owners.at(next.scene_address) = next;
    if (!updates.empty())
        pose_revision = revision;
    return !updates.empty();
}
void RetireNativeModelPose(uint32_t address, uint32_t bytes) {
    if (!bytes)
        return;
    address = Canonical(address);
    const uint64_t end = uint64_t(address) + bytes;
    auto it = regions.upper_bound(address);
    if (it != regions.begin())
        --it;
    std::vector<uint32_t> affected;
    for (; it != regions.end() && it->first < end; ++it)
        if (uint64_t(it->first) + it->second.bytes > address)
            affected.insert(affected.end(), it->second.scenes.begin(), it->second.scenes.end());
    bool erased = false;
    for (const uint32_t scene : affected) {
        const auto found = owners.find(scene);
        if (found == owners.end())
            continue;
        mh3_model_pose_retire(&found->second, address, bytes);
        if (!found->second.scene_address) {
            owners.erase(found);
            erased = true;
        }
    }
    if (erased)
        Reindex(owners);
}
void ClearNativeModelPose() {
    owners.clear();
    regions.clear();
    positions.clear();
    selectors = {};
    ++selectorRevision;
}
mh3_matrix_sources ReadNativeModelPose(const uint32_t addresses[3], uint32_t model) {
    if (!addresses)
        throw std::runtime_error("Native model pose requires explicit source addresses");
    mh3_matrix_sources result{};
    bool found = false;
    const auto matching = positions.find(Canonical(addresses[0]));
    if (matching == positions.end())
        throw std::runtime_error("Native model matrices have no allocation owner");
    for (const uint32_t scene : matching->second) {
        const auto& owner = owners.at(scene);
        mh3_matrix_sources candidate{};
        const auto status = mh3_model_pose_sources(&MH3NativeResourceMemory(), &owner, addresses, model ? model : owner.model_address, &candidate);
        if (status == MH3_RES_ABSENT)
            continue;
        Checked(status);
        if (found)
            throw std::runtime_error("Native model pose has ambiguous live allocation owners");
        result = candidate;
        found = true;
    }
    if (!found)
        throw std::runtime_error("Native model matrices have no completed typed pose producer");
    return result;
}
void PublishNativeModelDualTexture(bool enabled) {
    mh3_model_matrix_dual(&selectors, enabled ? 1 : 0);
    PublishSelectors();
}
void ApplyNativeModelSelectors() {
    PublishSelectors();
}
mh3_model_matrix_selectors ReadNativeModelSelectors() {
    return selectors;
}
uint64_t ReadNativeModelSelectorsRevision() {
    return selectorRevision;
}
void SetNativeModelSelectors(const mh3_model_matrix_selectors& state) {
    selectors = state;
    PublishSelectors();
}
void PublishNativeModelMatrixRows(const uint32_t rows[8]) {
    Select(rows);
}
void PublishNativeModelPositionMatrix(uint32_t selector, const float matrix[12], NativeOwner owner) {
    auto& p = Palette(owner);
    ++p.revision;
    std::memcpy(&p.palette.position[selector / 3], matrix, 48);
    p.palette.position_valid |= uint16_t(1u << (selector / 3));
}
void PublishNativeModelNormalMatrix(uint32_t selector, const float matrix[9], NativeOwner owner) {
    auto& p = Palette(owner);
    ++p.revision;
    std::memcpy(&p.palette.normal[selector / 3], matrix, 36);
    p.palette.normal_valid |= uint16_t(1u << (selector / 3));
}
void PublishNativeModelTextureMatrix(uint32_t selector, const float matrix[12], bool matrix3x4, NativeOwner owner) {
    if (selector >= 64)
        return; // post/SRT matrices have an independent owner
    auto& p = Palette(owner);
    ++p.revision;
    std::memcpy(&p.palette.texture[(selector - 30) / 3], matrix, matrix3x4 ? 48 : 32);
    if (matrix3x4)
        p.palette.texture_valid |= uint16_t(1u << ((selector - 30) / 3));
}
const mh3_model_palette& ReadNativeModelPalette(NativeOwner owner) {
    return Palette(owner).palette;
}
uint64_t ReadNativeModelPaletteRevision(NativeOwner owner) {
    return Palette(owner).revision;
}
void PublishNativeModelPalette(const mh3_model_palette& palette, NativeOwner owner) {
    auto& p = Palette(owner);
    ++p.revision;
    p.palette = palette;
}

uint32_t NativeModelViewArray(uint32_t scene, unsigned kind) {
    const auto& owner = Declare(scene);
    uint32_t result = 0;
    Checked(mh3_model_pose_array(&owner, kind, &result));
    // The native records use cached aliases; the guest getter retains its input
    // base alias exactly, including uncached resources.
    const uint32_t base = Memory::Read32(scene + 0xf4 + kind * 4);
    if (base)
        result = base + (result - owner.base[kind]);
    return result;
}
extern "C" void MH3_ModelViewPosition(CpuContext* cpu) {
    cpu->gpr[3] = NativeModelViewArray(cpu->gpr[3], 0);
}
REGISTER_NATIVE_FUNCTION_AS(0x80080BC0, MH3_ModelViewPosition, "ModelViewPosition");
extern "C" void MH3_ModelViewNormal(CpuContext* cpu) {
    cpu->gpr[3] = NativeModelViewArray(cpu->gpr[3], 1);
}
REGISTER_NATIVE_FUNCTION_AS(0x80080C0C, MH3_ModelViewNormal, "ModelViewNormal");
extern "C" void MH3_ModelViewEnvironment(CpuContext* cpu) {
    cpu->gpr[3] = NativeModelViewArray(cpu->gpr[3], 2);
}
REGISTER_NATIVE_FUNCTION_AS(0x80080C68, MH3_ModelViewEnvironment, "ModelViewEnvironment");
extern "C" void MH3_ModelViewCount(CpuContext* cpu) {
    uint32_t count = 0;
    Checked(mh3_model_pose_count({GuestRange(cpu->gpr[3], 0x104), 0x104}, &count));
    cpu->gpr[3] = count;
}
REGISTER_NATIVE_FUNCTION_AS(0x8007FD0C, MH3_ModelViewCount, "ModelViewCount");

extern "C" void MH3_ModelMainMatrixReset(CpuContext*) {
    mh3_model_matrix_reset(&selectors);
    PublishSelectors();
}
REGISTER_NATIVE_FUNCTION_AS(0x8009B13C, MH3_ModelMainMatrixReset, "ModelMainMatrixReset");
extern "C" void MH3_ModelMainMatrixSelect(CpuContext* cpu) {
    uint32_t rows[8];
    for (uint32_t i = 0; i < 8; i++)
        rows[i] = Memory::Read32(cpu->gpr[3] + 4 * i);
    Select(rows);
}
REGISTER_NATIVE_FUNCTION_AS(0x8009B1A4, MH3_ModelMainMatrixSelect, "ModelMainMatrixSelect");
extern "C" void MH3_ModelCurrentMatrixReset(CpuContext* cpu) {
    if (!Memory::Read8(cpu->gpr[3])) {
        mh3_model_matrix_reset(&selectors);
        PublishSelectors();
        Memory::Write8(cpu->gpr[3], 1);
    }
}
REGISTER_NATIVE_FUNCTION_AS(0x80087CF0, MH3_ModelCurrentMatrixReset, "ModelCurrentMatrixReset");
extern "C" void MH3_ModelPreTextureMatrices(CpuContext* cpu) {
    if (int32_t(cpu->gpr[4]) < 0)
        return;
    uint32_t types[8], rows[8];
    int apply = 0;
    for (uint32_t i = 0; i < 8; i++)
        types[i] = Memory::Read32(cpu->gpr[3] + 4 * i);
    mh3_model_matrix_rows(types, int32_t(cpu->gpr[4]), rows, &apply);
    const bool identity = std::all_of(std::begin(rows), std::end(rows), [](uint32_t row) { return row == 60; });
    if (identity) {
        if (!Memory::Read8(0x80796db4)) {
            mh3_model_matrix_reset(&selectors);
            PublishSelectors();
            Memory::Write8(0x80796db4, 1);
        }
        return;
    }
    if (std::any_of(std::begin(types), std::end(types), [](uint32_t type) { return type == 1; })) {
        const auto binding = ReadNativeModelMatrixSources(0x80685530);
        const auto sources = ReadNativeModelPose(binding.address);
        mh3_shape_matrix_input input{};
        input.valid = 1;
        input.matrix_id = int32_t(cpu->gpr[4]);
        std::copy(std::begin(types), std::end(types), std::begin(input.texture_source));
        mh3_model_palette palette{};
        mh3_model_palette_rigid(&input, &sources, &palette);
        for (unsigned i = 0; i < 8; i++)
            if (types[i] == 1)
                SetNativeTextureMatrix(30 + i * 3, &palette.texture[i].v[0][0], true);
    }
    Select(rows);
    Memory::Write8(0x80796db4, 0);
}
REGISTER_NATIVE_FUNCTION_AS(0x80087D30, MH3_ModelPreTextureMatrices, "ModelPreTextureMatrices");
