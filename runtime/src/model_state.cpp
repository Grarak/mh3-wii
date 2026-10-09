// Platform memory/ownership bridge to typed native C model inputs. No GX,
// guest rendering helper or source command-list decoder is called here.
#include "memory.h"
#include "runtime/native/adapters/model_state_bridge.h"
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {
std::unordered_map<uint32_t,mh3_native_texture_binding> objects;
std::unordered_map<uint32_t,uint64_t> generations;
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
