// Platform memory/ownership bridge to typed native C model inputs. No GX,
// guest rendering helper or source command-list decoder is called here.
#include "memory.h"
#include "fatal.h"
#include <aurora/fast_map.h>
#include "guest_span.h"
#include "runtime/native/adapters/model_catalog_bridge.h"
#include "runtime/native/adapters/model_state_bridge.h"
#include <stdexcept>
#include <string>
#include <array>
#include <mutex>

namespace {
// Typed texture and palette objects. The game thread and the draw thread
// both store and read them (a material bound on one while the other draws),
// so every access takes objectsLock: a lookup while the other thread's insert
// rehashed the map missed, and threw "no typed native producer" (a first load
// of the village on the Vita, which inserts most of its objects). Recursive:
// the copies read and store under one hold.
std::recursive_mutex objectsLock;

struct Objects {
    FastMap<uint32_t, mh3_native_texture_binding> textures;
    FastMap<uint32_t, mh3_native_palette_source> palettes;

    // Advanced by every change of either map (ReadNativeTextureObjectRevision).
    uint64_t revision = 1;
} objects;

Objects& O() {
    return objects;
}

// The render thread reads texture revisions while binding; the game thread
// produces them. Only these two maps are shared.
std::mutex generationLock;
FastMap<uint32_t, uint64_t> generations;
FastMap<uint32_t, uint64_t> initialized_generations;

// Bound view arrays (game thread draw state).
FastMap<uint32_t, mh3_matrix_binding> matrix_sources;
uint64_t resource_generation = 0;
[[noreturn, gnu::cold, gnu::noinline]] void CheckedFailed(mh3_res_status status) {
    MH3_THROW(std::runtime_error("Native model resource input failed: " + std::to_string(status)));
}

inline void Checked(mh3_res_status status) {
    if (status != MH3_RES_OK) [[unlikely]]
        CheckedFailed(status);
}

uint32_t Resource(uint32_t reference) {
    if (!reference)
        MH3_THROW(std::runtime_error("Null native resource reference"));

    const uint32_t resource = Memory::Read32(reference);

    if (!resource)
        MH3_THROW(std::runtime_error("Null native resource data"));

    return resource;
}

// Re-storing an unchanged object (UI and effects do every frame) keeps the revision.
void PutTexture(uint32_t object, const mh3_native_texture_binding& binding) {
    std::lock_guard lock(objectsLock);
    auto [it, inserted] = O().textures.try_emplace(object, binding);

    if (!inserted) {
        if (EqualNativeTextureBinding(it->second, binding))
            return;

        it->second = binding;
    }

    ++O().revision;
}

void PutPalette(uint32_t object, const mh3_native_palette_source& palette) {
    std::lock_guard lock(objectsLock);
    auto [it, inserted] = O().palettes.try_emplace(object, palette);

    if (!inserted) {
        if (EqualNativePaletteSource(it->second, palette))
            return;

        it->second = palette;
    }

    ++O().revision;
}

void PutCopiedObjects(uint32_t destination, bool palette, const std::array<bool, 8>& present, const std::array<mh3_native_texture_binding, 8>& textures,
                      const std::array<mh3_native_palette_source, 8>& colors) {
    std::lock_guard lock(objectsLock);
    auto& o = O();

    for (uint32_t i = 0; i < 8; i++) {
        if (palette) {
            const uint32_t target = destination + 4 + i * 12;

            if (present[i])
                PutPalette(target, colors[i]);
            else if (o.palettes.erase(target))
                ++o.revision;
        } else {
            const uint32_t target = destination + 4 + i * 32;

            if (present[i])
                PutTexture(target, textures[i]);
            else if (o.textures.erase(target))
                ++o.revision;
        }
    }
}
} // namespace

uint64_t ReadNativeTextureGeneration(uint32_t payload) {
    std::lock_guard lock(generationLock);
    return NativeTextureGeneration(payload);
}

uint64_t NativeTextureGeneration(uint32_t payload) {
    const auto found = generations.find(payload);
    return found == generations.end() ? 0 : found->second;
}

bool NativeTextureResourceIsOriginal(uint32_t payload, uint64_t generation) {
    std::lock_guard lock(generationLock);
    const auto found = initialized_generations.find(payload);
    const auto current = generations.find(payload);
    return generation && found != initialized_generations.end() && found->second == generation && current != generations.end() && current->second == generation;
}

void MarkNativeTextureProduced(uint32_t payload, uint64_t generation) {
    if (!payload || !generation)
        MH3_THROW(std::runtime_error("Native texture producer requires address and generation"));

    {
        std::lock_guard lock(generationLock);
        auto& previous = generations[payload];

        if (generation < previous)
            MH3_THROW(std::runtime_error("Native texture producer generation went backwards"));

        previous = generation;
    }

    std::lock_guard lock(objectsLock);

    for (auto& item : O().textures)
        if (item.second.source.payload_address == payload && generation > item.second.source.generation) {
            mh3_texture_source_updated(&item.second.source, generation);
            ++O().revision;
        }
    for (auto& item : O().palettes)
        if (item.second.payload_address == payload && item.second.generation != generation) {
            item.second.generation = generation;
            ++O().revision;
        }
}

mh3_native_texture_source ReadNativeResTexture(uint32_t reference) {
    const uint32_t resource = Resource(reference), size = Memory::Read32(resource + 4);
    mh3_native_texture_source source{};
    Checked(mh3_texture_source_read({GuestRange(resource, size), size}, resource, &source));
    source.generation = ReadNativeTextureGeneration(source.payload_address);
    return source;
}

mh3_native_palette_source ReadNativeResPalette(uint32_t reference) {
    const uint32_t resource = Resource(reference), size = Memory::Read32(resource + 4);
    mh3_native_palette_source source{};
    Checked(mh3_palette_source_read({GuestRange(resource, size), size}, resource, &source));
    source.generation = ReadNativeTextureGeneration(source.payload_address);
    return source;
}

void StoreNativeTextureObject(uint32_t object, const mh3_native_texture_binding& binding) {
    Checked(mh3_texture_compat_store({GuestRange(object, 32), 32}, &binding));
    auto copy = binding;
    mh3_texture_source_updated(&copy.source, ReadNativeTextureGeneration(copy.source.payload_address));
    PutTexture(object, copy);
}

bool EqualNativeTextureBinding(const mh3_native_texture_binding& a, const mh3_native_texture_binding& b) {
    const auto& x = a.source;
    const auto& y = b.source;
    const auto& p = a.sampler;
    const auto& q = b.sampler;
    return x.resource_address == y.resource_address && x.payload_address == y.payload_address && x.asset_id == y.asset_id && x.format == y.format && x.width == y.width &&
           x.height == y.height && x.levels == y.levels && x.min_lod == y.min_lod && x.max_lod == y.max_lod && x.generation == y.generation && x.indexed == y.indexed &&
           x.mutable_source == y.mutable_source && p.wrap_s == q.wrap_s && p.wrap_t == q.wrap_t && p.min_filter == q.min_filter && p.mag_filter == q.mag_filter &&
           p.anisotropy == q.anisotropy && p.lod_bias == q.lod_bias && p.bias_clamp == q.bias_clamp && p.edge_lod == q.edge_lod && a.map_id == b.map_id && a.tlut_id == b.tlut_id;
}

bool EqualNativePaletteSource(const mh3_native_palette_source& a, const mh3_native_palette_source& b) {
    return a.resource_address == b.resource_address && a.payload_address == b.payload_address && a.format == b.format && a.entries == b.entries && a.generation == b.generation;
}

uint64_t ReadNativeTextureObjectRevision() {
    std::lock_guard lock(objectsLock);
    return O().revision;
}

mh3_native_texture_binding NativeTextureObject(uint32_t object) {
    std::lock_guard lock(objectsLock);
    const auto& textures = O().textures;
    const auto found = textures.find(object);

    if (found == textures.end())
        MH3_THROW(std::runtime_error("Texture object has no typed native producer: " + std::to_string(object)));

    return found->second;
}

mh3_native_texture_binding ReadNativeTextureObject(uint32_t object) {
    return NativeTextureObject(object);
}

void StoreNativePaletteObject(uint32_t object, const mh3_native_palette_source& palette) {
    Checked(mh3_palette_compat_store({GuestRange(object, 12), 12}, &palette));
    auto copy = palette;
    const auto generation = ReadNativeTextureGeneration(copy.payload_address);

    if (generation > copy.generation)
        copy.generation = generation;

    PutPalette(object, copy);
}

mh3_native_palette_source NativePaletteObject(uint32_t object) {
    std::lock_guard lock(objectsLock);
    const auto& palettes = O().palettes;
    const auto found = palettes.find(object);

    if (found == palettes.end())
        MH3_THROW(std::runtime_error("Palette object has no typed native producer: " + std::to_string(object)));

    return found->second;
}

mh3_native_palette_source ReadNativePaletteObject(uint32_t object) {
    return NativePaletteObject(object);
}

uint64_t ProduceNativeTextureSource(uint32_t payload) {
    const uint64_t previous = ReadNativeTextureGeneration(payload);

    if (resource_generation < previous)
        resource_generation = previous;

    if (resource_generation == UINT64_MAX)
        MH3_THROW(std::runtime_error("Native texture resource revision overflow"));

    MarkNativeTextureProduced(payload, ++resource_generation);
    return resource_generation;
}

void InitializeNativeTextureSource(uint32_t reference, bool palette) {
    const uint32_t payload = palette ? ReadNativeResPalette(reference).payload_address : ReadNativeResTexture(reference).payload_address;
    const uint64_t generation = ProduceNativeTextureSource(payload);
    std::lock_guard lock(generationLock);
    initialized_generations.insert_or_assign(payload, generation);
}

void CopyNativeResourceObjects(uint32_t destination, uint32_t source, bool palette) {
    std::lock_guard lock(objectsLock);

    if ((destination & 3) || (source & 3))
        MH3_THROW(std::runtime_error("Unaligned native resource object copy"));

    const uint32_t size = palette ? 100 : 260;

    // Resolve every typed producer first, so a missing source leaves RAM and
    // the native ownership maps untouched. Invalid slots preserve CPU bytes.
    std::array<mh3_native_texture_binding, 8> textures{};
    std::array<mh3_native_palette_source, 8> colors{};
    int present[8]{};
    const auto span = mh3_res_span{GuestRange(source, size), size};

    for (uint32_t i = 0; i < 8; i++) {
        Checked(mh3_resource_objects_present(span, i, palette, &present[i]));

        if (present[i]) {
            if (palette)
                colors[i] = ReadNativePaletteObject(source + 4 + i * 12);
            else
                textures[i] = ReadNativeTextureObject(source + 4 + i * 32);
        }
    }

    Checked(mh3_resource_objects_copy({GuestRange(destination, size), size}, span, palette));
    std::array<bool, 8> kept{};

    for (uint32_t i = 0; i < 8; i++)
        kept[i] = present[i] != 0;

    PutCopiedObjects(destination, palette, kept, textures, colors);
}

void StoreNativeModelMatrixSources(uint32_t state, const uint32_t addresses[3]) {
    auto next = matrix_sources.contains(state) ? matrix_sources.at(state) : mh3_matrix_binding{};

    if (next.generation == UINT64_MAX)
        MH3_THROW(std::runtime_error("Native matrix source revision overflow"));

    mh3_matrix_bind(&next, addresses);
    Checked(mh3_matrix_store_sources({GuestRange(state, 12), 12}, addresses));
    matrix_sources.insert_or_assign(state, next);
}

mh3_matrix_binding ReadNativeModelMatrixSources(uint32_t state) {
    const auto found = matrix_sources.find(state);

    if (found == matrix_sources.end())
        MH3_THROW(std::runtime_error("Matrix arrays have no typed bind producer"));

    return found->second;
}
