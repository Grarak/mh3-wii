#include "native_graphics.h"
#include "native_effect_draw.h"
#include "fatal.h"
#include "native_shade_snapshot.h"
#include "bytes_equal.h"
#include "memory.h"
#include "../native/mh3_model_state.h"
#include "../native/adapters/model_state_bridge.h"
#include "../native/mh3_model_catalog.h"
#include "native_frame_packet.h"
#include "native_texture_memo.h"
#include "dev_options.h"
#include <cstdio>
#include <aurora/native_texture.hpp>
#include "gl/gl.hpp"
#include <array>
#include <cstring>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <vector>

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

// The backend source of a recorded binding (Render's palettes).
aurora::NativeTextureSource SourceOf(const mh3_native_texture_binding& binding) {
    const auto& palettes = T(NativeOwner::Render).palettes;
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

    // The guest memory the image is in, as the heap retirements number it.
    if (uint32_t owner; mh3_source_canonical_address(input.payload_address, uint32_t(bytes), &owner) == MH3_RES_OK) {
        source.owner = owner;
        source.owner_bytes = uint32_t(bytes);
    }

    // A movie frame converts from the packet's copy of the planes, which the
    // next decode does not rewrite; it keeps the planes' address as its
    // identity.
    if (source.format == aurora::NativeTextureFormatYuv420)
        if (const uint8_t* copy = NativeMovieFrameCopy(input.payload_address, input.generation, bytes)) {
            source.identity = source.bytes.data();
            source.bytes = {copy, bytes};
        }

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

    return source;
}

aurora::NativeTextureSampler SamplingOf(const mh3_native_texture_binding& binding) {
    const auto& sampler = binding.sampler;
    return {sampler.wrap_s, sampler.wrap_t, sampler.min_filter, sampler.mag_filter, sampler.anisotropy, binding.source.min_lod, binding.source.max_lod, sampler.lod_bias};
}
#ifdef MH3_DEVELOPMENT

// MH3_VERIFY=prep-image: a memo's rebind against the binding's source and
// sampling resolved in full; a line every 100000 rebinds.
void CheckRebind(const mh3_native_texture_binding& binding, const NativeTextureBindMemo& memo) {
    static const bool enabled = VerifyOptions().Has("prep-image");

    if (!enabled)
        return;

    static uint64_t checked = 0, differed = 0;
    const auto a = SourceOf(binding);
    const auto& b = memo.backend.source;
    const auto x = SamplingOf(binding);
    const auto& y = memo.backend.sampling;
    const bool same = a.bytes.data() == b.bytes.data() && a.bytes.size() == b.bytes.size() && a.palette.data() == b.palette.data() && a.palette.size() == b.palette.size() &&
                      a.width == b.width && a.height == b.height && a.levels == b.levels && a.format == b.format && a.asset == b.asset && a.palette_format == b.palette_format &&
                      a.palette_entries == b.palette_entries && a.generation == b.generation && a.palette_generation == b.palette_generation && a.mutable_source == b.mutable_source &&
                      a.original_image == b.original_image && a.original_palette == b.original_palette && a.identity == b.identity && a.owner == b.owner &&
                      a.owner_bytes == b.owner_bytes && x.wrap_s == y.wrap_s && x.wrap_t == y.wrap_t && x.min_filter == y.min_filter && x.mag_filter == y.mag_filter &&
                      x.anisotropy == y.anisotropy && x.min_lod == y.min_lod && x.max_lod == y.max_lod && x.lod_bias == y.lod_bias;
    ++checked;

    if (!same && differed++ < 10)
        std::fprintf(stderr, "MH3 texture rebind check: unit %u payload %08x rebinds another source\n", binding.map_id, binding.source.payload_address);

    if (checked % 100000 == 0)
        std::fprintf(stderr, "MH3 texture rebind check: %llu rebinds, %llu with another source\n", static_cast<unsigned long long>(checked), static_cast<unsigned long long>(differed));
}
#endif

// Render thread: bind one recorded texture to its backend unit; through
// `memo` without resolving the source again while what it read is unchanged.
void BindBackend(const mh3_native_texture_binding& binding, NativeTextureBindMemo* memo = nullptr) {
    const auto& palettes = T(NativeOwner::Render).palettes;
    auto& textures = T(NativeOwner::Render).textures;
    const auto& input = binding.source;
    const uint64_t revision = memo ? NativeTextureResourceRevision() : 0;

    if (memo && memo->revision == revision && (!input.indexed || (binding.tlut_id < palettes.size() && BytesEqual(&memo->palette, &palettes[binding.tlut_id], sizeof memo->palette))) &&
        aurora::rebind_native_texture(binding.map_id, memo->backend)) {
#ifdef MH3_DEVELOPMENT
        CheckRebind(binding, *memo);
#endif
        textures[binding.map_id] = {binding, true};
        return;
    }

    const auto source = SourceOf(binding);
    const auto sampling = SamplingOf(binding);

    if (memo) {
        aurora::bind_native_texture(binding.map_id, source, sampling, memo->backend);

        // A movie frame's planes are the packet's copy: never rebound.
        memo->revision = source.format == aurora::NativeTextureFormatYuv420 ? 0 : revision;

        if (input.indexed)
            memo->palette = palettes[binding.tlut_id];
    } else
        aurora::bind_native_texture(binding.map_id, source, sampling);

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

void BindNativeTexture(const mh3_native_texture_binding& binding, NativeOwner owner, NativeTextureBindMemo* memo) {
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
        BindBackend(binding, memo);
    else
        texture = {binding, true};

    Changed(owner);
}

void ResetNativeTextureBindings(NativeOwner owner) {
    T(owner) = {};
    Changed(owner);
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

namespace {
using OwnerRanges = std::vector<aurora::NativeTextureOwnerRange>;

// Guest memory a heap freed or handed out again since the last packet's
// begin, from any guest thread (native_resource_lifetime.cpp). A queued
// batch comes back from the render thread cleared: the few batches in flight
// keep their capacity instead of allocating each frame.
std::mutex retiredLock;
OwnerRanges retiredSources;
std::deque<OwnerRanges> retiredBatches;
std::vector<OwnerRanges*> freeRetiredBatches;
} // namespace

void RetireNativeTextureSources(uint32_t address, uint32_t bytes) {
    std::lock_guard guard(retiredLock);
    retiredSources.push_back({address, bytes});
}

void QueueNativeTextureRetirements() {
    OwnerRanges* batch;
    {
        std::lock_guard guard(retiredLock);

        if (retiredSources.empty())
            return;

        if (freeRetiredBatches.empty())
            batch = &retiredBatches.emplace_back();
        else {
            batch = freeRetiredBatches.back();
            freeRetiredBatches.pop_back();
        }

        batch->swap(retiredSources);
    }

    // At a packet's begin: after the draws of the packets recorded before the
    // frees, ahead of this one's tasks (a new scene's first textures). The task
    // queue keeps it when the render thread skips the packet: it runs with the
    // next one taken.
    QueueNativeGraphicsTask(NativeTaskOrder::BeforeDraws, [batch] {
        if (aurora::retire_native_textures(*batch))
            ForgetNativeTextureBindings();

        batch->clear();
        std::lock_guard guard(retiredLock);
        freeRetiredBatches.push_back(batch);
    });
}

void ForgetNativeTextureBindings() {
    for (auto& texture : T(NativeOwner::Render).textures)
        texture.valid = false;
}

void CopyNativeTextureStoreForDraw() {
    owners[unsigned(NativeOwner::Draw)] = owners[unsigned(NativeOwner::Game)];
    nativeStateRevision[unsigned(NativeOwner::Draw)][NATIVE_STATE_TEXTURES] = nativeStateRevision[unsigned(NativeOwner::Game)][NATIVE_STATE_TEXTURES];
}
