// Platform memory/ownership bridge to typed native C model inputs. No GX,
// guest rendering helper or source command-list decoder is called here.
#include "memory.h"
#include "runtime/native/adapters/model_state_bridge.h"
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <array>

namespace {
std::unordered_map<uint32_t,mh3_native_texture_binding> objects;
std::unordered_map<uint32_t,mh3_native_palette_source> palettes;
std::unordered_map<uint32_t,uint64_t> generations;
uint64_t resource_generation=0;
void Checked(mh3_res_status status) {
    if (status!=MH3_RES_OK) throw std::runtime_error("Native model resource input failed: "+std::to_string(status));
}
uint32_t Resource(uint32_t reference) {
    if (!reference) throw std::runtime_error("Null native resource reference");
    const uint32_t resource=Memory::Read32(reference);
    if (!resource) throw std::runtime_error("Null native resource data");
    return resource;
}
} // namespace

uint64_t ReadNativeTextureGeneration(uint32_t payload) {
    const auto found=generations.find(payload);
    return found==generations.end()?0:found->second;
}
void MarkNativeTextureProduced(uint32_t payload,uint64_t generation) {
    if (!payload || !generation) throw std::runtime_error("Native texture producer requires address and generation");
    auto& previous=generations[payload];
    if (generation<previous) throw std::runtime_error("Native texture producer generation went backwards");
    previous=generation;
    for (auto& item:objects) if (item.second.source.payload_address==payload)
        mh3_texture_source_updated(&item.second.source,generation);
    for (auto& item:palettes) if (item.second.payload_address==payload)
        item.second.generation=generation;
}
mh3_native_texture_source ReadNativeResTexture(uint32_t reference) {
    const uint32_t resource=Resource(reference),size=Memory::Read32(resource+4);
    mh3_native_texture_source source{};
    Checked(mh3_texture_source_read({Memory::GetPointer(resource,size),size},resource,&source));
    source.generation=ReadNativeTextureGeneration(source.payload_address);
    return source;
}
mh3_native_palette_source ReadNativeResPalette(uint32_t reference) {
    const uint32_t resource=Resource(reference),size=Memory::Read32(resource+4);
    mh3_native_palette_source source{};
    Checked(mh3_palette_source_read({Memory::GetPointer(resource,size),size},resource,&source));
    source.generation=ReadNativeTextureGeneration(source.payload_address);
    return source;
}
void StoreNativeTextureObject(uint32_t object,const mh3_native_texture_binding& binding) {
    Checked(mh3_texture_compat_store({Memory::GetPointer(object,32),32},&binding));
    auto copy=binding;
    mh3_texture_source_updated(&copy.source,ReadNativeTextureGeneration(copy.source.payload_address));
    objects.insert_or_assign(object,copy);
}
mh3_native_texture_binding ReadNativeTextureObject(uint32_t object) {
    const auto found=objects.find(object);
    if (found==objects.end()) throw std::runtime_error("Texture object has no typed native producer: "+std::to_string(object));
    return found->second;
}
void StoreNativePaletteObject(uint32_t object,const mh3_native_palette_source& palette) {
    Checked(mh3_palette_compat_store({Memory::GetPointer(object,12),12},&palette));
    auto copy=palette;
    const auto generation=ReadNativeTextureGeneration(copy.payload_address);
    if (generation>copy.generation) copy.generation=generation;
    palettes.insert_or_assign(object,copy);
}
mh3_native_palette_source ReadNativePaletteObject(uint32_t object) {
    const auto found=palettes.find(object);
    if (found==palettes.end()) throw std::runtime_error("Palette object has no typed native producer: "+std::to_string(object));
    return found->second;
}
void InitializeNativeTextureSource(uint32_t reference,bool palette) {
    const uint32_t payload=palette?ReadNativeResPalette(reference).payload_address:ReadNativeResTexture(reference).payload_address;
    const uint64_t previous=ReadNativeTextureGeneration(payload);
    if (resource_generation<previous) resource_generation=previous;
    if (resource_generation==UINT64_MAX) throw std::runtime_error("Native texture resource revision overflow");
    MarkNativeTextureProduced(payload,++resource_generation);
}
void CopyNativeResourceObjects(uint32_t destination,uint32_t source,bool palette) {
    if ((destination&3) || (source&3)) throw std::runtime_error("Unaligned native resource object copy");
    const uint32_t size=palette?100:260;
    // Resolve every typed producer first, so a missing source leaves RAM and
    // the native ownership maps untouched. Invalid slots preserve CPU bytes.
    std::array<mh3_native_texture_binding,8> textures{};
    std::array<mh3_native_palette_source,8> colors{};
    int present[8]{};
    const auto span=mh3_res_span{Memory::GetPointer(source,size),size};
    for (uint32_t i=0;i<8;i++) {
        Checked(mh3_resource_objects_present(span,i,palette,&present[i]));
        if (present[i]) {
            if (palette) colors[i]=ReadNativePaletteObject(source+4+i*12);
            else textures[i]=ReadNativeTextureObject(source+4+i*32);
        }
    }
    Checked(mh3_resource_objects_copy({Memory::GetPointer(destination,size),size},span,palette));
    for (uint32_t i=0;i<8;i++) {
        if (palette) {
            const uint32_t target=destination+4+i*12;
            if (present[i]) palettes.insert_or_assign(target,colors[i]);else palettes.erase(target);
        } else {
            const uint32_t target=destination+4+i*32;
            if (present[i]) objects.insert_or_assign(target,textures[i]);else objects.erase(target);
        }
    }
}
