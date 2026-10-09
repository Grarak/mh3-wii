#include "native_graphics.h"
#include "memory.h"
#include "../native/mh3_model_state.h"
#include <aurora/native_texture.hpp>
#include "gl/gl.hpp"
#include <array>
#include <stdexcept>

namespace {
std::array<mh3_native_palette_source,20> palettes{};
struct TextureBinding { mh3_native_texture_binding value; bool valid=false; };
std::array<TextureBinding,8> textures{};
}
void BindNativePalette(uint32_t slot,const mh3_native_palette_source& palette) {
    if(!aurora::gl::on_render_thread()) throw std::runtime_error("Palette changed outside graphics owner");
    if(slot>=palettes.size() || !palette.payload_address || !palette.entries || palette.entries>16384 || palette.format>2)
        throw std::runtime_error("Invalid native palette binding");
    palettes[slot]=palette;
    for(const auto& texture:textures)
        if(texture.valid && texture.value.source.indexed && texture.value.tlut_id==slot)
            BindNativeTexture(texture.value);
}
void BindNativeTexture(const mh3_native_texture_binding& binding) {
    if(binding.map_id>=textures.size()) throw std::runtime_error("Invalid native texture unit");
    const auto& input=binding.source;
    aurora::NativeTextureSource source;
    source.width=input.width;source.height=input.height;source.levels=input.levels;source.format=input.format;
    source.asset=input.asset_id;source.generation=input.generation;source.mutable_source=input.mutable_source;
    const size_t bytes=aurora::native_texture_source_size(source.format,source.width,source.height,source.levels);
    source.bytes={Memory::GetPointer(input.payload_address,bytes),bytes};
    if(input.indexed) {
        if(binding.tlut_id>=palettes.size()) throw std::runtime_error("Invalid native texture palette slot");
        const auto& palette=palettes[binding.tlut_id];
        if(palette.entries) {
            const size_t size=size_t(palette.entries)*2;
            source.palette={Memory::GetPointer(palette.payload_address,size),size};
            source.palette_format=palette.format;source.palette_entries=palette.entries;
            source.palette_generation=palette.generation;
        }
    }
    const auto& sampler=binding.sampler;
    aurora::bind_native_texture(binding.map_id,source,
        {sampler.wrap_s,sampler.wrap_t,sampler.min_filter,sampler.mag_filter,sampler.anisotropy,
         input.min_lod,input.max_lod});
    textures[binding.map_id]={binding,true};
}
