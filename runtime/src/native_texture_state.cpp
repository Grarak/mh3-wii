#include "native_graphics.h"
#include "fatal.h"
#include "native_shade_snapshot.h"
#include "bytes_equal.h"
#include "memory.h"
#include "../native/mh3_model_state.h"
#include "../native/adapters/model_state_bridge.h"
#include <aurora/native_texture.hpp>
#include "gl/gl.hpp"
#include <array>
#include <cstring>
#include <stdexcept>

namespace {
struct TextureBinding {
    mh3_native_texture_binding value;
    bool valid = false;
};

// Logical bindings per owner. The game thread records Game; only the render
// thread resolves Render's to backend textures (aurora::bind_native_texture).
struct TextureOwner {
    std::array<mh3_native_palette_source, 20> palettes{};
    std::array<TextureBinding, 8> textures{};
};

constinit TextureOwner owners[NativeOwnerCount]{};
TextureOwner& T(NativeOwner owner = NativeOwner::Game) {
    return owners[NativeOwnerIndex(owner)];
}

void Changed(NativeOwner owner = NativeOwner::Game) {
    ++nativeStateRevision[NativeOwnerIndex(owner)][NATIVE_STATE_TEXTURES];
}

// Render thread: bind one recorded texture to its backend unit.
void BindBackend(const mh3_native_texture_binding& binding) {
    auto& [palettes, textures] = T(NativeOwner::Render);
    const auto& input = binding.source;
    aurora::NativeTextureSource source;
    source.width = input.width;
    source.height = input.height;
    source.levels = input.levels;
    source.format = input.format;
    source.asset = input.asset_id;
    source.generation = input.generation;
    source.mutable_source = input.mutable_source;
    source.original_image = NativeTextureResourceIsOriginal(input.payload_address, input.generation);
    const size_t bytes = aurora::native_texture_source_size(source.format, source.width, source.height, source.levels);
    source.bytes = {Memory::GetPointer(input.payload_address, bytes), bytes};

    if (input.indexed) {
        if (binding.tlut_id >= palettes.size())
            MH3_THROW(std::runtime_error("Invalid native texture palette slot"));

        const auto& palette = palettes[binding.tlut_id];

        if (palette.entries) {
            const size_t size = size_t(palette.entries) * 2;
            source.palette = {Memory::GetPointer(palette.payload_address, size), size};
            source.palette_format = palette.format;
            source.palette_entries = palette.entries;
            source.palette_generation = palette.generation;
            source.original_palette = NativeTextureResourceIsOriginal(palette.payload_address, palette.generation);
        }
    }

    const auto& sampler = binding.sampler;
    aurora::bind_native_texture(binding.map_id, source, {sampler.wrap_s, sampler.wrap_t, sampler.min_filter, sampler.mag_filter, sampler.anisotropy, input.min_lod, input.max_lod, sampler.lod_bias});
    textures[binding.map_id] = {binding, true};
}
} // namespace

const mh3_native_texture_binding* GetNativeTextureBinding(uint32_t unit, NativeOwner owner) {
    auto& textures = T(owner).textures;

    if (unit >= textures.size())
        MH3_THROW(std::runtime_error("Invalid native texture unit"));

    return textures[unit].valid ? &textures[unit].value : nullptr;
}

const mh3_native_palette_source* GetNativePaletteBinding(uint32_t slot) {
    auto& palettes = T().palettes;

    if (slot >= palettes.size())
        MH3_THROW(std::runtime_error("Invalid native palette slot"));

    const auto& palette = palettes[slot];
    return palette.entries && palette.payload_address ? &palette : nullptr;
}

void BindNativePalette(uint32_t slot, const mh3_native_palette_source& palette, NativeOwner owner) {
    auto& [palettes, textures] = T(owner);

    if (slot >= palettes.size() || !palette.payload_address || !palette.entries || palette.entries > 16384 || palette.format > 2)
        MH3_THROW(std::runtime_error("Invalid native palette binding"));
    // Materials rebind their palettes on every draw; an unchanged binding keeps
    // the revision, so the frame packet need not capture and compare a copy.
    if (!std::memcmp(&palettes[slot], &palette, sizeof palette))
        return;

    const bool rebind = owner == NativeOwner::Render;
    palettes[slot] = palette;

    if (rebind) {
        // Indexed images resolve their palette when bound: rebind them.
        for (auto& texture : textures)
            if (texture.valid && texture.value.source.indexed && texture.value.tlut_id == slot)
                BindBackend(texture.value);
    }

    Changed(owner);
}

void BindNativeTexture(const mh3_native_texture_binding& binding, NativeOwner owner) {
    auto& [palettes, textures] = T(owner);

    if (binding.map_id >= textures.size())
        MH3_THROW(std::runtime_error("Invalid native texture unit"));

    if (binding.source.indexed && binding.tlut_id >= palettes.size())
        MH3_THROW(std::runtime_error("Invalid native texture palette slot"));

    auto& texture = textures[binding.map_id];

    if (texture.valid && BytesEqual(&texture.value, &binding, sizeof binding))
        return;
    // Deferred model preparation binds the Render backend directly.
    if (owner == NativeOwner::Render)
        BindBackend(binding);
    else
        texture = {binding, true};

    Changed(owner);
}

void CopyNativeTextureBindings(NativeOwner from, NativeOwner to) {
    T(to) = T(from);
    Changed(to);
}

bool EqualNativeTextureBindings(NativeOwner a, NativeOwner b) {
    const auto& x = T(a);
    const auto& y = T(b);

    if (std::memcmp(x.palettes.data(), y.palettes.data(), sizeof x.palettes))
        return false;

    for (unsigned unit = 0; unit < x.textures.size(); ++unit) {
        if (x.textures[unit].valid != y.textures[unit].valid)
            return false;

        if (x.textures[unit].valid && std::memcmp(&x.textures[unit].value, &y.textures[unit].value, sizeof x.textures[unit].value))
            return false;
    }

    return true;
}

bool NativeTextureSnapshotCurrent(const NativeTextureSnapshot& snapshot) {
    const auto& [palettes, textures] = T();
    uint8_t valid = 0;

    for (unsigned unit = 0; unit < textures.size(); ++unit)
        valid |= uint8_t(textures[unit].valid) << unit;

    if (valid != snapshot.valid)
        return false;
    // As captured: every unit's record, valid or not, and the palettes.
    for (unsigned unit = 0; unit < textures.size(); ++unit)
        if (!BytesEqual(&textures[unit].value, &snapshot.textures[unit], sizeof textures[unit].value))
            return false;
    return BytesEqual(palettes.data(), snapshot.palettes.data(), sizeof palettes);
}

void CaptureNativeTextureSnapshot(NativeTextureSnapshot& out) {
    const auto& [palettes, textures] = T();
    out.valid = 0;

    for (unsigned unit = 0; unit < textures.size(); ++unit) {
        out.textures[unit] = textures[unit].value;

        if (textures[unit].valid)
            out.valid |= uint8_t(1u << unit);
    }

    out.palettes = palettes;
}

void RestoreNativeTextureSnapshot(const NativeTextureSnapshot& snapshot) {
    auto& [palettes, textures] = T(NativeOwner::Render);

    // Rebind only what differs; a palette change re-resolves indexed images.
    const bool paletteChanged = !BytesEqual(palettes.data(), snapshot.palettes.data(), sizeof palettes);

    if (paletteChanged)
        palettes = snapshot.palettes;

    for (unsigned unit = 0; unit < textures.size(); ++unit) {
        if (!(snapshot.valid & (1u << unit)))
            continue;

        const auto& binding = snapshot.textures[unit];

        if (textures[unit].valid && !(paletteChanged && binding.source.indexed) && BytesEqual(&textures[unit].value, &binding, sizeof binding))
            continue;

        BindBackend(binding);
    }

    Changed(NativeOwner::Render);
}

void ForgetNativeTextureBindings() {
    for (auto& texture : T(NativeOwner::Render).textures)
        texture.valid = false;
}

void CopyNativeTextureStoreForDraw() {
    owners[unsigned(NativeOwner::Draw)] = owners[unsigned(NativeOwner::Game)];
    nativeStateRevision[unsigned(NativeOwner::Draw)][NATIVE_STATE_TEXTURES] = nativeStateRevision[unsigned(NativeOwner::Game)][NATIVE_STATE_TEXTURES];
}
