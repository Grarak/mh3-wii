#include "native_graphics.h"
#include "native_shade_snapshot.h"
#include "memory.h"
#include "../native/mh3_model_state.h"
#include "../native/adapters/model_state_bridge.h"
#include <aurora/native_texture.hpp>
#include "gl/gl.hpp"
#include <array>
#include <cstring>
#include <stdexcept>

namespace {
// Logical bindings, one copy per thread. Only the render thread resolves them
// to backend textures (aurora::bind_native_texture).
thread_local constinit std::array<mh3_native_palette_source, 20> palettes{};
struct TextureBinding {
    mh3_native_texture_binding value;
    bool valid = false;
};
thread_local constinit std::array<TextureBinding, 8> textures{};
void Changed() {
    ++nativeStateRevision[NATIVE_STATE_TEXTURES];
}
} // namespace
const mh3_native_texture_binding* GetNativeTextureBinding(uint32_t unit) {
    if (unit >= textures.size())
        throw std::runtime_error("Invalid native texture unit");
    return textures[unit].valid ? &textures[unit].value : nullptr;
}
const mh3_native_palette_source* GetNativePaletteBinding(uint32_t slot) {
    if (!OnNativeGraphicsThread())
        throw std::runtime_error("Palette read outside graphics owner");
    if (slot >= palettes.size())
        throw std::runtime_error("Invalid native palette slot");
    const auto& palette = palettes[slot];
    return palette.entries && palette.payload_address ? &palette : nullptr;
}
void BindNativePalette(uint32_t slot, const mh3_native_palette_source& palette) {
    if (!OnNativeGraphicsThread())
        throw std::runtime_error("Palette changed outside graphics owner");
    if (slot >= palettes.size() || !palette.payload_address || !palette.entries || palette.entries > 16384 || palette.format > 2)
        throw std::runtime_error("Invalid native palette binding");
    palettes[slot] = palette;
    Changed();
    if (!aurora::gl::on_render_thread())
        return;
    for (const auto& texture : textures)
        if (texture.valid && texture.value.source.indexed && texture.value.tlut_id == slot)
            BindNativeTexture(texture.value);
}
void BindNativeTexture(const mh3_native_texture_binding& binding) {
    if (binding.map_id >= textures.size())
        throw std::runtime_error("Invalid native texture unit");
    if (!aurora::gl::on_render_thread()) {
        if (binding.source.indexed && binding.tlut_id >= palettes.size())
            throw std::runtime_error("Invalid native texture palette slot");
        textures[binding.map_id] = {binding, true};
        Changed();
        return;
    }
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
            throw std::runtime_error("Invalid native texture palette slot");
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
void CaptureNativeTextureSnapshot(NativeTextureSnapshot& out) {
    out.valid = 0;
    for (unsigned unit = 0; unit < textures.size(); ++unit) {
        out.textures[unit] = textures[unit].value;
        if (textures[unit].valid)
            out.valid |= uint8_t(1u << unit);
    }
    out.palettes = palettes;
}
void RestoreNativeTextureSnapshot(const NativeTextureSnapshot& snapshot) {
    // Rebind only what differs; a palette change re-resolves indexed images.
    const bool paletteChanged = std::memcmp(palettes.data(), snapshot.palettes.data(), sizeof palettes) != 0;
    if (paletteChanged)
        palettes = snapshot.palettes;
    for (unsigned unit = 0; unit < textures.size(); ++unit) {
        if (!(snapshot.valid & (1u << unit)))
            continue;
        const auto& binding = snapshot.textures[unit];
        if (textures[unit].valid && !(paletteChanged && binding.source.indexed) && !std::memcmp(&textures[unit].value, &binding, sizeof binding))
            continue;
        BindNativeTexture(binding);
    }
    Changed();
}
void ForgetNativeTextureBindings() {
    for (auto& texture : textures)
        texture.valid = false;
}
