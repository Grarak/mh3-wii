// Typed native material state, pure equation generation, and direct frame-slot
// uniforms. No GX state, command-list decoder, FIFO, or GPU wait participates.
#include "frame_split.h"
#include "bytes_equal.h"
#include "native_graphics.h"
#include "model_assets.h"
#include "../native/mh3_material_state.h"
#include "../native/mh3_material_group_publication.h"
#include "../native/mh3_model_gpu_inputs.h"
#include "../native/mh3_model_pose.h"
#include "../native/adapters/model_gpu_bridge.h"
#include "../native/adapters/model_pose_bridge.h"
#include "../native/adapters/model_material_bridge.h"
#include <algorithm>
#include <aurora/aurora.h>
#include <aurora/native_texture.hpp>
#include "gl/gl.hpp"
#include "native_shader_source.h"
#include "native_shade_snapshot.h"
#include "native_frame_packet.h"
#include "native_material_gpu.h"
#include "model_cull.h"
#include "../native/mh3_shade.h"
#ifdef MH3_DEVELOPMENT
#include "gfx/port_capture.hpp"
#endif
namespace aurora::gfx {
uint32_t current_frame() noexcept;
}
#include <array>
#include <atomic>
#include <cassert>
#include <memory>
#include <chrono>
#include <mutex>
#include <thread>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

constinit uint32_t nativeStateRevision[NativeOwnerCount][NATIVE_STATE_COMPONENTS]{};
namespace {
constexpr uint64_t Bit(unsigned field) {
    return UINT64_C(1) << field;
}
struct TextureMatrix {
    std::array<float, 12> values{};
    bool known = false, matrix3x4 = false;
};
// The SRT behind a post texture matrix (PublishNativeTextureSrt): pure when
// the matrix is exactly the slot's XSI SRT, so the vertex program builds it.
struct TextureSrt {
    mh3_shade_gpu_srt params{};
    bool pure = false;
};
// Logical material state per owner: the game thread records draws against
// Game; the render thread restores snapshots into Render and shades from it.
struct MaterialOwner {
    mh3_native_material_state material{};
    // Classification identity is separate from snapshot/uniform publication.
    // Each owner keeps its own dirtiness across restoration.
    bool shadeStructureDirty = true;
    mh3_native_fog fogSource{};
    mh3_shader_fog preparedFog{};
    bool fogKnown = false, depthBeforeTexture = false;
    mh3_native_light_slots lightSlots{};
    std::array<mh3_shader_light, 8> preparedLights{};
    std::array<TextureMatrix, 10> textureMatrices{};
    std::array<TextureMatrix, 20> postTextureMatrices{};
    std::array<TextureSrt, 8> postSrt{};
    // Advances whenever a post SRT's purity or parameters change.
    uint32_t srtRevision = 0;
};
constinit MaterialOwner owners[NativeOwnerCount]{};
MaterialOwner& M(NativeOwner owner = NativeOwner::Game) {
    return owners[unsigned(owner)];
}
MaterialOwner& game = owners[unsigned(NativeOwner::Game)];
MaterialOwner& render = owners[unsigned(NativeOwner::Render)];
// Game: the material imports the draw state's raster publications.
constinit bool rasterObserverInstalled = false, publishingMaterialRaster = false;
void Changed(NativeStateComponent component, NativeOwner owner = NativeOwner::Game) {
    ++nativeStateRevision[unsigned(owner)][component];
}
void Check(mh3_res_status status, const char* operation) {
    if (status != MH3_RES_OK)
        throw std::runtime_error(std::string(operation) + ": status " + std::to_string(status));
}
void ImportRaster(const mh3_raster_state& raster) {
    const uint32_t fields[][2] = {{MH3_PIXEL_DEPTH_TEST, raster.depth_test},        {MH3_PIXEL_DEPTH_COMPARE, raster.depth_compare},  {MH3_PIXEL_DEPTH_WRITE, raster.depth_write},
                                  {MH3_PIXEL_BLEND_ENABLE, raster.blend_mode == 1}, {MH3_PIXEL_LOGIC_ENABLE, raster.blend_mode == 2}, {MH3_PIXEL_BLEND_SUBTRACT, raster.blend_mode == 3},
                                  {MH3_PIXEL_BLEND_SOURCE, raster.src_factor},      {MH3_PIXEL_BLEND_DESTINATION, raster.dst_factor}, {MH3_PIXEL_LOGIC_OPERATION, raster.logic_op},
                                  {MH3_PIXEL_COLOR_WRITE, raster.color_write},      {MH3_PIXEL_ALPHA_WRITE, raster.alpha_write}};
    auto& material = game.material;
    bool changed = material.cull_mode != raster.cull_mode;
    for (const auto& field : fields) {
        changed |= !(material.pixel.known & Bit(field[0])) || material.pixel.values[field[0]] != field[1];
        material.pixel.known |= Bit(field[0]);
        material.pixel.values[field[0]] = field[1];
    }
    material.cull_mode = raster.cull_mode;
    if (changed)
        Changed(NATIVE_STATE_MATERIAL);
}
void ObserveRaster(const mh3_raster_state& raster) {
    if (!publishingMaterialRaster)
        ImportRaster(raster);
}
void ObserveRasterPublications() {
    if (rasterObserverInstalled)
        return;
    SetNativeRasterObserver(ObserveRaster);
    rasterObserverInstalled = true;
    ImportRaster(GetNativeDrawState().raster);
}
const mh3_native_texture_binding& Texture(unsigned unit) {
    const auto* binding = GetNativeTextureBinding(unit, NativeOwner::Render);
    if (!binding)
        throw std::runtime_error("Native material texture " + std::to_string(unit) + " has no typed producer");
    return *binding;
}
// ---- Fixed native shading: classified materials, hand-written programs ----
// One classified structure (a NativeMaterialGpuRef record): immutable once
// published, shared by the game and render threads. Material uniforms in GPU
// memory (docs/native-material-gpu.md): its Material block is baked at
// classification and uploaded into persistent GPU memory by the render thread
// at its first draw; supported is false when a constant has two animated
// factors (per-draw Shade uniforms, the old path). index 0: no reference
// (record table full), drawn with the old path.
struct ShadeEntry {
    uint64_t key = 0;
    uint32_t index = 0;
    bool supported = false, dynamic = false;
    mh3_shade_material shading{};
    mh3_shade_gpu_material material{};
};
constexpr uint32_t MaxMaterialRecords = 4096;
struct RecordTable {
    std::mutex lock;
    std::unordered_map<uint64_t, const ShadeEntry*> byKey;
    std::array<std::atomic<const ShadeEntry*>, MaxMaterialRecords> records{};
    uint32_t count = 0;
} recordTable;
const ShadeEntry& ClassifyRecord(uint64_t key, const mh3_native_material_state& state, ShadeEntry& out) {
    out.key = key;
    const uint8_t quality = mh3_shade_classify(&state, &out.shading);
#ifdef MH3_DEVELOPMENT
    if (!(quality & MH3_SHADE_EXACT))
        std::fprintf(stderr, "[shade] approximated material structure %016llx quality %02x stages %u\n", (unsigned long long)key, quality, state.tev_count);
#else
    (void)quality;
#endif
    uint8_t dynamic = 0;
    out.supported = mh3_shade_gpu_bake(&out.shading, &out.material, &dynamic) != 0;
    out.dynamic = dynamic != 0;
    return out;
}
const ShadeEntry* AcquireRecord(uint64_t key, const mh3_native_material_state& state) {
    std::lock_guard lock(recordTable.lock);
    if (const auto found = recordTable.byKey.find(key); found != recordTable.byKey.end())
        return found->second;
    if (recordTable.count == MaxMaterialRecords)
        return nullptr;
    auto* record = new ShadeEntry;
    ClassifyRecord(key, state, *record);
    record->index = recordTable.count + 1;
    recordTable.records[recordTable.count].store(record, std::memory_order_release);
    ++recordTable.count;
    recordTable.byKey.emplace(key, record);
    return record;
}
const ShadeEntry* RecordOf(uint32_t index) {
    return index && index <= MaxMaterialRecords ? recordTable.records[index - 1].load(std::memory_order_acquire) : nullptr;
}
struct ShadeState {
    // Render thread: classifications beyond the record table (old path).
    std::unordered_map<uint64_t, ShadeEntry> overflow;
    uint64_t lastKey = 0;
    const ShadeEntry* last = nullptr;
    std::unordered_map<uint64_t, aurora::gl::ProgramPtr> programs;
    mh3_shade_scene scene{};
    // built is the render thread's lighting revision of the scene block;
    // upload counts rebuilt scene blocks.
    uint64_t built = UINT64_MAX, upload = 0;
    float width = -1.f;
    // The scene block's copy in the current frame's GPU-visible uniform memory.
    AuroraNativeUniforms sceneMemory{};
    uint64_t sceneMemoryUpload = 0;
} shade;
const ShadeEntry& ShadeMaterial() {
    const auto& material = render.material;
    auto& shadeStructureDirty = render.shadeStructureDirty;
    if (!shadeStructureDirty && shade.last)
        return *shade.last;
    const uint64_t key = mh3_shade_structure_key(&material);
    if (shade.last && key == shade.lastKey) {
        shadeStructureDirty = false;
        return *shade.last;
    }
    const ShadeEntry* entry = AcquireRecord(key, material);
    if (!entry) {
        auto [overflow, inserted] = shade.overflow.try_emplace(key);
        if (inserted)
            ClassifyRecord(key, material, overflow->second);
        entry = &overflow->second;
    }
    shade.lastKey = key;
    shade.last = entry;
    shadeStructureDirty = false;
    return *entry;
}
const mh3_shade_scene& ShadeScene(uint64_t& upload) {
    const float width = GetNativeDrawState(NativeOwner::Render).viewport[2];
    const uint32_t revision = nativeStateRevision[unsigned(NativeOwner::Render)][NATIVE_STATE_LIGHTING];
    if (shade.built != revision || shade.width != width) {
        auto& scene = shade.scene;
        std::memset(&scene, 0, sizeof scene);
        for (unsigned i = 0; i < 8; ++i)
            if (render.lightSlots.valid_mask & (1u << i))
                scene.lights[i] = render.preparedLights[i];
        const uint32_t type = render.fogKnown ? render.fogSource.type : 0;
        auto fog = render.preparedFog;
        if (type)
            Check(mh3_material_prepare_fog(&render.fogSource, width, &fog), "Native fog draw preparation");
        mh3_shade_fog(type, &fog, &scene);
        shade.built = revision;
        shade.width = width;
        ++shade.upload;
    }
    upload = shade.upload;
    return shade.scene;
}
aurora::gl::ProgramPtr& ShadeProgram(const NativeShaderVariant& variant) {
    const uint64_t key = uint64_t(variant.vertex) | uint64_t(variant.lighting) << 1 | uint64_t(variant.lights) << 3 | uint64_t(variant.model) << 7 | uint64_t(variant.textures) << 8 |
                         uint64_t(variant.overlay) << 10 | uint64_t(variant.alpha_test) << 11 | uint64_t(variant.fog) << 12 | uint64_t(variant.texgen[0]) << 13 | uint64_t(variant.texgen[1]) << 16 |
                         uint64_t(variant.texgen[2]) << 19 | uint64_t(variant.linear_fog) << 22 | uint64_t(variant.pure_modulate) << 23 | uint64_t(variant.target_alpha_one) << 24 |
                         uint64_t(variant.material_gpu) << 25 | uint64_t(variant.dynamic) << 26;
    auto& program = shade.programs[key];
    if (!program) {
        auto sources = NativeShaderSource(variant);
        aurora::gl::ProgramDesc desc;
        desc.label = "shade " + NativeShaderVertexName(variant) + "+" + NativeShaderFragmentName(variant);
        desc.origin = "runtime/src/native_shader_source.cpp";
        desc.preservesTargetAlphaOne = variant.target_alpha_one;
#ifdef AURORA_GXM
        desc.vertex = std::move(sources.cg_vertex);
        desc.fragment = std::move(sources.cg_fragment);
        desc.cg = true;
#else
        desc.vertex = std::move(sources.glsl_vertex);
        desc.fragment = std::move(sources.glsl_fragment);
#endif
        desc.samplers = {{"u_texture0", 0}, {"u_texture1", 1}, {"u_texture2", 2}};
        desc.blocks = {{"Draw", 0}, {"Scene", 1}, {"Palette", 2}, {"Shade", 3}, {"Material", 4}, {"Animated", 5}};
        program = std::make_shared<aurora::gl::Program>(std::move(desc));
    }
    return program;
}
struct ShadeLighting {
    NativeShaderLighting kind = NativeShaderLighting::None;
    uint8_t lights = 0;
    bool lit() const { return kind != NativeShaderLighting::None; }
};
// Lighting channel feeding the vertex colour: channel 0 unless the material
// reads colour channel 1 (channel 1 then replaces channel 0, an approximation).
// Colour-only spot lighting with clamped diffuse gets the Spot program with the
// active light indices packed one per nibble.
ShadeLighting ShadeChannels(const mh3_shade_material& shading, mh3_shade_vertex& out) {
    const auto& material = render.material;
    const auto& lightSlots = render.lightSlots;
    const auto& channel = material.channel[(shading.quality & MH3_SHADE_CHANNEL1) ? 1 : 0];
    for (unsigned i = 0; i < 4; ++i) {
        out.channel_material[i] = channel.material_color[i] / 255.f;
        out.channel_ambient[i] = channel.ambient_color[i] / 255.f;
    }
    const auto& color = channel.color;
    const auto& alpha = channel.alpha;
    const int32_t bits =
        (color.lighting ? 1 : 0) | (alpha.lighting ? 2 : 0) | (color.material_source ? 4 : 0) | (color.ambient_source ? 8 : 0) | (alpha.material_source ? 16 : 0) | (alpha.ambient_source ? 32 : 0);
    out.channel[0] = int32_t(color.light_mask & lightSlots.valid_mask);
    out.channel[1] = int32_t(alpha.light_mask & lightSlots.valid_mask);
    out.channel[2] = bits;
    out.channel[3] = int32_t((color.diffuse | color.attenuation << 2) | (alpha.diffuse | alpha.attenuation << 2) << 8);
    if (!(bits & 3))
        return {};
    if ((bits & 2) || color.diffuse != 2 /* GX_DF_CLAMP */ || color.attenuation != 1 /* GX_AF_SPOT */)
        return {NativeShaderLighting::General, 0};
    ShadeLighting lighting{NativeShaderLighting::Spot, 0};
    uint32_t packed = 0;
    for (unsigned light = 0; light < 8; ++light)
        if (out.channel[0] & (1 << light))
            packed |= light << (4 * lighting.lights++);
    out.channel[0] = int32_t(packed);
    return lighting;
}
// Fragment-side per-role texture units (GX map IDs) and LOD bias.
std::array<uint8_t, 3> ShadeTextures(const mh3_shade_material& shading, float bias[4]) {
    std::array<uint8_t, 3> units{};
    for (unsigned role = 0; role < 3; ++role) {
        bias[role] = 0.f;
        if (role >= shading.textures)
            continue;
        const unsigned map = shading.texture_map[role];
        (void)Texture(map);
        units[role] = uint8_t(map);
        bias[role] = aurora::native_texture_shader_lod_bias(map);
    }
    bias[3] = 0.f;
    return units;
}
void RequireEarlyDepth(const mh3_shade_material& shading) {
    const auto& raster = GetNativeDrawState(NativeOwner::Render).raster;
    if (render.depthBeforeTexture && raster.depth_test && raster.depth_write && shading.alpha_test)
        throw std::runtime_error("Native early depth write with alpha discard is unsupported");
}
// Copies the scene block into the frame's uniform memory once per frame and
// scene change; false when the frame has no uniform space (frame canceled).
bool ShadeSceneMemory(uint64_t& upload) {
    const auto& scene = ShadeScene(upload);
    auto& memory = shade.sceneMemory;
    if (memory.data && memory.frame == aurora::gfx::current_frame() && memory.slot == aurora_native_frame_slot() && shade.sceneMemoryUpload == upload)
        return true;
    if (!aurora_allocate_native_uniforms(sizeof scene, &memory))
        return false;
    std::memcpy(memory.data, &scene, sizeof scene);
    shade.sceneMemoryUpload = upload;
    return true;
}
// One per-draw allocation holds Draw, Shade and (mesh draws) the palette at
// ShadePaletteOffset; it is filled in place and uploaded/bound once.
// Material-GPU draws have no Shade: the palette follows Draw at 512.
constexpr uint32_t ShadePaletteOffset(uint8_t model) {
    return (MH3_SHADE_DRAW_BYTES(model) + 255u) & ~255u;
}
constexpr uint32_t ShadeGpuPaletteOffset = MH3_SHADE_FRAGMENT_OFFSET;
mh3_shade_draw* ShadeDrawMemory(uint8_t model, bool palette, AuroraNativeUniforms& memory) {
    if (!aurora_allocate_native_uniforms(palette ? ShadePaletteOffset(model) + uint32_t(sizeof(mh3_shade_palette)) : MH3_SHADE_DRAW_BYTES(model), &memory))
        return nullptr;
    auto* block = static_cast<mh3_shade_draw*>(memory.data);
    std::memset(block, 0, MH3_SHADE_FRAGMENT_OFFSET);
    return block;
}
mh3_shade_vertex* ShadeGpuDrawMemory(bool palette, AuroraNativeUniforms& memory) {
    if (!aurora_allocate_native_uniforms(palette ? ShadeGpuPaletteOffset + uint32_t(sizeof(mh3_shade_palette)) : MH3_SHADE_FRAGMENT_OFFSET, &memory))
        return nullptr;
    auto* block = static_cast<mh3_shade_vertex*>(memory.data);
    std::memset(block, 0, MH3_SHADE_FRAGMENT_OFFSET);
    return block;
}
std::optional<aurora::gl::ShadedBlocks> ShadeBlocks(const AuroraNativeUniforms& draw, uint8_t model, bool palette) {
    aurora::gl::ShadedBlocks blocks;
    if (!ShadeSceneMemory(blocks.sceneRevision))
        return std::nullopt;
    blocks.draw = draw.data;
    blocks.drawBytes = draw.size;
    blocks.shadeOffset = MH3_SHADE_FRAGMENT_OFFSET;
    blocks.shadeBytes = MH3_SHADE_FRAGMENT_BYTES(model);
    if (palette) {
        blocks.paletteOffset = ShadePaletteOffset(model);
        blocks.paletteBytes = sizeof(mh3_shade_palette);
    }
    blocks.scene = shade.sceneMemory.data;
    blocks.sceneBytes = sizeof(mh3_shade_scene);
    return blocks;
}
NativeShaderVariant ShadeVariant(const mh3_shade_material& shading, NativeShaderVertex vertex, ShadeLighting lighting) {
    NativeShaderVariant variant;
    variant.vertex = vertex;
    variant.lighting = lighting.kind;
    variant.lights = lighting.lights;
    variant.model = shading.model == MH3_SHADE_MODULATE ? NativeShaderModel::Modulate : NativeShaderModel::Basic;
    variant.pure_modulate = shading.pure_modulate != 0;
    variant.target_alpha_one = aurora::gl::shaded_target_alpha_one();
    variant.textures = shading.textures;
    variant.overlay = shading.overlay != 0;
    variant.alpha_test = shading.alpha_test != 0;
    variant.fog = render.fogKnown && render.fogSource.type != 0;
    variant.linear_fog = variant.fog && (render.fogSource.type & 7) < 4;
    return variant;
}

// ---- Material uniforms in GPU memory (docs/native-material-gpu.md) ----
// MH3_MATERIAL_GPU=0 keeps the per-draw Shade uniforms (mh3_shade_fill).
bool MaterialGpu() {
    static const bool enabled = [] {
        const char* value = std::getenv("MH3_MATERIAL_GPU");
        return !(value && value[0] == '0');
    }();
    return enabled;
}
// Frame uniform memory for Animated blocks and resolved Material copies,
// carved in 16-byte units from 4 KiB frame allocations (GXM's uniform
// alignment; GLES uploads them).
struct FrameCarve {
    AuroraNativeUniforms chunk{};
    uint32_t used = 0;
} carve;
void* CarveUniforms(uint32_t bytes) {
    constexpr uint32_t Chunk = 4096;
    bytes = (bytes + 15u) & ~15u;
    auto& c = carve.chunk;
    if (!c.data || c.frame != aurora::gfx::current_frame() || c.slot != aurora_native_frame_slot() || carve.used + bytes > c.size) {
        if (!aurora_allocate_native_uniforms(std::max(Chunk, bytes), &c)) {
            c = {};
            return nullptr;
        }
        carve.used = 0;
    }
    void* out = static_cast<char*>(c.data) + carve.used;
    carve.used += bytes;
    return out;
}
// The Animated block of the current draw: rebuilt only when the material,
// textures or post SRT changed, rewritten only when its bytes changed.
struct AnimatedState {
    mh3_shade_gpu_animated values{};
    aurora::gl::UniformBlock block{};
    uint32_t frame = UINT32_MAX, slot = UINT32_MAX;
    // Inputs of the last build.
    uint32_t material = UINT32_MAX, textures = UINT32_MAX, srt = UINT32_MAX;
    const ShadeEntry* entry = nullptr;
    uint8_t srtRoles = 0xff;
} animated;
// Per-draw resolved Material copy (BASIC with animated picks).
struct ResolvedState {
    mh3_shade_gpu_material values{};
    aurora::gl::UniformBlock block{};
    const ShadeEntry* entry = nullptr;
    uint32_t frame = UINT32_MAX, slot = UINT32_MAX;
    mh3_shade_gpu_animated palette{};
} resolved;
NativeMaterialGpuStatistics gpuStatistics{};
// Render thread: the persistent Material block of each record, uploaded at
// its first draw (0 not yet, 1 uploaded, 2 no block: old path).
struct RecordBlocks {
    std::array<aurora::gl::UniformBlock, MaxMaterialRecords> blocks{};
    std::array<uint8_t, MaxMaterialRecords> state{};
} recordBlocks;
const aurora::gl::UniformBlock* RecordBlock(const ShadeEntry& entry) {
    if (!entry.index)
        return nullptr;
    auto& state = recordBlocks.state[entry.index - 1];
    auto& block = recordBlocks.blocks[entry.index - 1];
    if (!state) {
        state = 2;
        if (!entry.supported)
            ++gpuStatistics.bake_unsupported;
        else if (block = aurora::gl::create_uniform_block(&entry.material, sizeof entry.material); block.valid()) {
            state = 1;
            ++gpuStatistics.baked;
        } else
            ++gpuStatistics.bake_space;
    }
    return state == 1 ? &block : nullptr;
}
// BASIC programs do not evaluate picks (secondary attribute budget): their
// animated coefficients are resolved into a per-draw copy of the block.
bool ResolvesOnCpu(const ShadeEntry& entry) {
    return entry.dynamic && entry.shading.model != MH3_SHADE_MODULATE;
}
// Post SRT of each role: bit r of the result when role r's post matrix is a
// pure SRT (its parameters are in srt[r]).
uint8_t ShadeSrtRoles(const mh3_shade_material& shading, mh3_shade_gpu_srt srt[3]) {
    uint8_t roles = 0;
    for (unsigned role = 0; role < 3; ++role) {
        mh3_shade_gpu_srt_identity(&srt[role]);
        if (role >= shading.textures)
            continue;
        const unsigned coordinate = shading.texture_coordinate[role];
        if (coordinate >= 8 || coordinate >= render.material.texgen_count)
            continue;
        const auto& texgen = render.material.texgen[coordinate];
        if (!texgen.values[MH3_TEXGEN_DUAL_TEXTURE] || texgen.values[MH3_TEXGEN_POST_MATRIX_ROW] == 61)
            continue;
        const unsigned slot = texgen.values[MH3_TEXGEN_POST_MATRIX_ROW] / 3u;
        if (slot >= 8 || !render.postSrt[slot].pure)
            continue;
        srt[role] = render.postSrt[slot].params;
        roles |= uint8_t(1u << role);
    }
    return roles;
}
bool SrtIdentity(const mh3_shade_gpu_srt& srt) {
    return srt.scale_rotation[0] == 1.f && srt.scale_rotation[1] == 1.f && srt.scale_rotation[2] == 0.f && srt.scale_rotation[3] == 1.f && srt.translation[0] == 0.f &&
           srt.translation[1] == 0.f;
}
// Fills next (palette, alpha range, texture bias, SRT) and returns the block
// to bind, writing frame memory only when the bytes differ from the last.
std::optional<aurora::gl::UniformBlock> ShadeAnimated(const ShadeEntry& entry, const float bias[4], const mh3_shade_gpu_srt srt[3], uint8_t srtRoles) {
    const uint32_t materialRevision = nativeStateRevision[unsigned(NativeOwner::Render)][NATIVE_STATE_MATERIAL];
    const uint32_t textureRevision = nativeStateRevision[unsigned(NativeOwner::Render)][NATIVE_STATE_TEXTURES];
    auto& a = animated;
    const bool current = a.block.valid() && a.frame == aurora::gfx::current_frame() && a.slot == aurora_native_frame_slot();
    if (current && a.material == materialRevision && a.textures == textureRevision && a.srt == render.srtRevision && a.entry == &entry && a.srtRoles == srtRoles) {
        ++gpuStatistics.animated_reused;
        return a.block;
    }
    mh3_shade_gpu_animated next;
    mh3_shade_gpu_palette(&render.material, &next);
    mh3_shade_gpu_alpha_range(render.material.pixel.values, next.alpha_range);
    std::memcpy(next.texture_bias, bias, sizeof next.texture_bias);
    std::memcpy(next.srt, srt, sizeof next.srt);
    a.material = materialRevision;
    a.textures = textureRevision;
    a.srt = render.srtRevision;
    a.entry = &entry;
    a.srtRoles = srtRoles;
    if (current && BytesEqual(&next, &a.values, sizeof next)) {
        ++gpuStatistics.animated_equal;
        return a.block;
    }
    void* memory = CarveUniforms(sizeof next);
    if (!memory)
        return std::nullopt;
    std::memcpy(memory, &next, sizeof next);
    a.values = next;
    a.block = {memory, 0, 0, uint32_t(sizeof next)};
    a.frame = aurora::gfx::current_frame();
    a.slot = aurora_native_frame_slot();
    ++gpuStatistics.animated_written;
    return a.block;
}
// The Material block to bind: the baked block, or a per-draw resolved copy.
std::optional<aurora::gl::UniformBlock> ShadeMaterialBlock(const ShadeEntry& entry, const aurora::gl::UniformBlock& baked, const mh3_shade_gpu_animated& values) {
    if (!ResolvesOnCpu(entry))
        return baked;
    auto& r = resolved;
    const bool current = r.block.valid() && r.frame == aurora::gfx::current_frame() && r.slot == aurora_native_frame_slot();
    if (current && r.entry == &entry && BytesEqual(r.palette.palette, values.palette, sizeof r.palette.palette)) {
        ++gpuStatistics.resolved_reused;
        return r.block;
    }
    // Static coefficients times their picks; the copy picks nothing (one).
    r.values = entry.material;
    mh3_shade_fragment fragment;
    mh3_shade_gpu_resolve(&entry.shading, &entry.material, &values, &fragment);
    const auto& basic = fragment.shading.basic;
    for (unsigned i = 0; i < 4; ++i) {
        std::memcpy(r.values.coefficient[MH3_SHADE_GPU_BASIC_OFFSET + i], basic.color_input[i].offset, 16);
        std::memcpy(r.values.coefficient[MH3_SHADE_GPU_BASIC_SCALE + i], basic.color_input[i].scale, 16);
    }
    std::memcpy(r.values.coefficient[MH3_SHADE_GPU_BASIC_OVERLAY], fragment.overlay, 16);
    for (auto& pick : r.values.pick) {
        const mh3_shade_gpu_pick one = {{float(MH3_SHADE_GPU_ONE), float(MH3_SHADE_GPU_ONE), 0.f, 0.f}, {0.f, 0.f, 0.f, 1.f}, {0.f, 0.f, 0.f, 1.f}};
        pick = one;
    }
    void* memory = CarveUniforms(sizeof r.values);
    if (!memory)
        return std::nullopt;
    std::memcpy(memory, &r.values, sizeof r.values);
    std::memcpy(r.palette.palette, values.palette, sizeof r.palette.palette);
    r.entry = &entry;
    r.block = {memory, 0, 0, uint32_t(sizeof r.values)};
    r.frame = aurora::gfx::current_frame();
    r.slot = aurora_native_frame_slot();
    ++gpuStatistics.resolved_written;
    return r.block;
}

// ---- Producer-written Animated ring and persistent Animated sections ----
// (native_material_gpu.h) The ring has RingSegments segments; a packet's
// writer uses segment sequence % RingSegments and reuses it only after the
// render thread ended that segment's previous packet (or a later one: the
// previous one was replaced) and the GPU completed what was submitted then.
constexpr unsigned RingSegments = 8;
constexpr uint32_t RingSegmentBytes = 64u << 10;
constexpr uint32_t AnimatedBytes = sizeof(mh3_shade_gpu_animated);
struct RingSegment {
    std::atomic<uint64_t> sequence{UINT64_MAX};
    aurora::gl::GpuFence fence{};
};
struct MaterialGpuRing {
    std::atomic<uint8_t*> data{nullptr};
    aurora::gl::UniformRing memory{};
    std::array<RingSegment, RingSegments> segments{};
    // One more than the highest packet sequence the render thread ended.
    std::atomic<uint64_t> ended{0};
    // Writer cursor.
    unsigned segment = 0;
    uint32_t used = RingSegmentBytes;
    std::atomic<uint64_t> waits{0}, full{0}, timeouts{0};
} ringState;
void EnsureMaterialGpuRing() {
    auto& r = ringState;
    if (r.memory.data)
        return;
    r.memory = aurora::gl::create_uniform_ring(RingSegments * RingSegmentBytes);
    r.data.store(r.memory.data, std::memory_order_release);
}
void ReleaseMaterialGpuRing() {
    auto& r = ringState;
    if (!r.memory.data)
        return;
    r.data.store(nullptr, std::memory_order_release);
    aurora::gl::destroy_uniform_ring(r.memory);
    r.memory = {};
}
constexpr uint32_t AnimatedPersistent = 0x80000000u;
aurora::gl::UniformBlock RingBlock(uint32_t offset) {
    uint8_t* data = ringState.data.load(std::memory_order_acquire);
    if (!data || offset > RingSegments * RingSegmentBytes - AnimatedBytes)
        throw std::runtime_error("Animated ring reference without a ring section");
    return {data + offset, 0, 0, AnimatedBytes};
}
// Persistent Animated sections: CPU copies made on any thread, uploaded by
// the render thread at first use, retired after release.
constexpr uint32_t MaxPersistentAnimated = 8192;
struct PersistentAnimatedSlot {
    mh3_shade_gpu_animated values{};
    aurora::gl::UniformBlock block{}; // render thread
    uint8_t state = 0;                // render thread: 0 not uploaded, 1 uploaded, 2 failed
};
struct PersistentAnimated {
    std::mutex lock;
    std::unique_ptr<PersistentAnimatedSlot[]> slots;
    std::vector<uint32_t> free, released;
    uint32_t count = 0;
} persistentAnimated;
void RetireReleasedAnimated() {
    auto& p = persistentAnimated;
    std::vector<uint32_t> released;
    {
        std::lock_guard lock(p.lock);
        released.swap(p.released);
    }
    if (released.empty())
        return;
    for (const uint32_t index : released) {
        auto& slot = p.slots[index];
        if (slot.state == 1)
            aurora::gl::retire_uniform_block(slot.block);
        slot.state = 0;
        slot.block = {};
    }
    std::lock_guard lock(p.lock);
    p.free.insert(p.free.end(), released.begin(), released.end());
}
std::optional<aurora::gl::UniformBlock> PersistentAnimatedBlock(uint32_t index) {
    auto& p = persistentAnimated;
    if (!p.slots || index >= MaxPersistentAnimated)
        throw std::runtime_error("Persistent Animated reference out of range");
    auto& slot = p.slots[index];
    if (!slot.state) {
        slot.block = aurora::gl::create_uniform_block(&slot.values, AnimatedBytes);
        slot.state = slot.block.valid() ? 1 : 2;
    }
    if (slot.state != 1)
        return std::nullopt;
    return slot.block;
}
// SetNativeMaterialGpuDraw: blocks for the next model mesh submission.
struct ExternalDraw {
    bool set = false;
    uint32_t material = 0, animated = UINT32_MAX;
    uint8_t srtRoles = 0;
} externalDraw;
#ifdef MH3_DEVELOPMENT
// MH3_MATERIAL_GPU_RING=1: the render thread routes its model draws'
// Animated sections through the producer ring (a self-test of the game
// thread's write path: one packet per rendered frame).
bool MaterialGpuRingSelfTest() {
    static const bool enabled = std::getenv("MH3_MATERIAL_GPU_RING") != nullptr;
    return enabled;
}
struct RingSelfTest {
    uint32_t frame = UINT32_MAX;
    uint64_t sequence = 0;
} ringSelfTest;
#endif
std::optional<aurora::gl::ShadedBlocks> ShadeGpuBlocks(const AuroraNativeUniforms& draw, bool palette, const aurora::gl::UniformBlock& material, const aurora::gl::UniformBlock& animatedBlock) {
    aurora::gl::ShadedBlocks blocks;
    if (!ShadeSceneMemory(blocks.sceneRevision))
        return std::nullopt;
    blocks.draw = draw.data;
    blocks.drawBytes = draw.size;
    blocks.shadeOffset = MH3_SHADE_FRAGMENT_OFFSET;
    blocks.shadeBytes = 0;
    if (palette) {
        blocks.paletteOffset = ShadeGpuPaletteOffset;
        blocks.paletteBytes = sizeof(mh3_shade_palette);
    }
    blocks.scene = shade.sceneMemory.data;
    blocks.sceneBytes = sizeof(mh3_shade_scene);
    blocks.material = material;
    blocks.animated = animatedBlock;
    return blocks;
}
#ifdef MH3_DEVELOPMENT
// MH3_MATERIAL_GPU_VERIFY=1: compare the coefficients the programs derive
// from the Material and Animated blocks with mh3_shade_fill, and the SRT rows
// with the post matrices, for every material-GPU draw.
bool MaterialGpuVerify() {
    static const bool enabled = std::getenv("MH3_MATERIAL_GPU_VERIFY") != nullptr;
    return enabled;
}
struct VerifyState {
    uint64_t draws = 0, mismatches = 0, srtRoles = 0, srtMismatches = 0;
} verifyState;
bool Near(float a, float b) {
    return std::fabs(a - b) <= 1e-5f * std::max(1.f, std::max(std::fabs(a), std::fabs(b)));
}
void VerifyMaterialGpu(const ShadeEntry& entry, const mh3_shade_gpu_material& material, const mh3_shade_gpu_animated& values, const mh3_resolved_mesh_submit* mesh) {
    auto& v = verifyState;
    ++v.draws;
    mh3_shade_draw expected;
    std::memset(&expected, 0, sizeof expected);
    mh3_shade_fill(&entry.shading, &render.material, &expected);
    mh3_shade_fragment actual;
    mh3_shade_gpu_resolve(&entry.shading, &material, &values, &actual);
    const auto& e = expected.fragment;
    std::string diff;
    const auto lanes = [&](const char* name, const float* x, const float* y, unsigned first, unsigned last) {
        for (unsigned i = first; i < last; ++i)
            if (!Near(x[i], y[i])) {
                char text[96];
                std::snprintf(text, sizeof text, " %s[%u] %g/%g", name, i, x[i], y[i]);
                diff += text;
                return;
            }
    };
    if (entry.shading.alpha_test)
        lanes("alpha", e.alpha_range, actual.alpha_range, 0, 2);
    if (entry.shading.overlay)
        lanes("overlay", e.overlay, actual.overlay, 0, 3);
    if (!entry.shading.pure_modulate) {
        if (entry.shading.model == MH3_SHADE_MODULATE) {
            const auto &x = e.shading.modulate, &y = actual.shading.modulate;
            lanes("w0", x.w0, y.w0, 0, 4);
            lanes("w1", x.w1, y.w1, 0, 4);
            lanes("offset", x.offset, y.offset, 0, 4);
            lanes("kv", x.kv, y.kv, 0, 4);
            lanes("kc", x.kc, y.kc, 0, 4);
            lanes("d", x.d, y.d, 0, 4);
            lanes("s", x.s, y.s, 0, 4);
        } else {
            const auto &x = e.shading.basic, &y = actual.shading.basic;
            for (unsigned i = 0; i < 4; ++i) {
                lanes("color.offset", x.color_input[i].offset, y.color_input[i].offset, 0, 3);
                lanes("color.scale", x.color_input[i].scale, y.color_input[i].scale, 0, 3);
                lanes("color.select", x.color_input[i].select, y.color_input[i].select, 0, 4);
                lanes("color.swizzle", x.color_input[i].swizzle, y.color_input[i].swizzle, 0, 1);
                lanes("alpha.offset", x.alpha_input[i].offset, y.alpha_input[i].offset, 3, 4);
                lanes("alpha.scale", x.alpha_input[i].scale, y.alpha_input[i].scale, 3, 4);
                lanes("alpha.select", x.alpha_input[i].select, y.alpha_input[i].select, 0, 4);
            }
            lanes("color.equation", x.color_equation, y.color_equation, 0, 3);
            lanes("alpha.equation", x.alpha_equation, y.alpha_equation, 0, 3);
        }
    }
    if (mesh) {
        // Pure-SRT roles: the rows the vertex program builds against the post matrix.
        for (unsigned role = 0; role < entry.shading.textures; ++role) {
            mh3_shade_gpu_srt srt[3];
            const uint8_t roles = ShadeSrtRoles(entry.shading, srt);
            if (!(roles & (1u << role)))
                continue;
            ++v.srtRoles;
            const unsigned coordinate = entry.shading.texture_coordinate[role];
            const unsigned slot = render.material.texgen[coordinate].values[MH3_TEXGEN_POST_MATRIX_ROW] / 3u;
            const float* post = mesh->texture_matrices[slot];
            float row0[3], row1[3];
            mh3_shade_gpu_srt_rows(&srt[role], row0, row1);
            const float expect[12] = {row0[0], row0[1], 0.f, row0[2], row1[0], row1[1], 0.f, row1[2], 0.f, 0.f, 1.f, 0.f};
            for (unsigned i = 0; i < 12; ++i)
                if (!Near(post[i], expect[i])) {
                    ++v.srtMismatches;
                    char text[96];
                    std::snprintf(text, sizeof text, " srt%u[%u] %g/%g", role, i, post[i], expect[i]);
                    diff += text;
                    break;
                }
        }
    }
    if (!diff.empty() && v.mismatches++ < 40)
        std::fprintf(stderr, "MH3 material GPU verify: draw %llu structure %016llx model %u:%s\n", (unsigned long long)v.draws, (unsigned long long)entry.key, entry.shading.model, diff.c_str());
    if (v.draws % 20000 == 0)
        std::fprintf(stderr, "MH3 material GPU verify: %llu draws, %llu mismatches, %llu SRT roles (%llu mismatched), %llu baked\n", (unsigned long long)v.draws, (unsigned long long)v.mismatches,
                     (unsigned long long)v.srtRoles, (unsigned long long)v.srtMismatches, (unsigned long long)gpuStatistics.baked);
}
#endif

#ifdef MH3_DEVELOPMENT
// MH3_STATS: material-GPU counters every 300 rendered frames.
void ReportMaterialGpu() {
    static const bool enabled = std::getenv("MH3_STATS") != nullptr;
    static uint32_t lastFrame = 0;
    static NativeMaterialGpuStatistics last{};
    const uint32_t frame = aurora::gfx::current_frame();
    if (!enabled || frame - lastFrame < 300)
        return;
    const auto now = ReadNativeMaterialGpuStatistics();
    const double frames = double(frame - lastFrame);
    std::fprintf(stderr,
                 "MH3 material GPU: %llu records baked (%llu without GPU form, %llu out of space), %.1f KiB persistent; per frame: animated reused %.1f equal %.1f written %.1f, "
                 "resolved reused %.1f written %.1f; ring waits %llu full %llu timeouts %llu\n",
                 (unsigned long long)now.baked, (unsigned long long)now.bake_unsupported, (unsigned long long)now.bake_space, now.persistent_bytes / 1024.0,
                 (now.animated_reused - last.animated_reused) / frames, (now.animated_equal - last.animated_equal) / frames, (now.animated_written - last.animated_written) / frames,
                 (now.resolved_reused - last.resolved_reused) / frames, (now.resolved_written - last.resolved_written) / frames, (unsigned long long)now.ring_waits,
                 (unsigned long long)now.ring_full, (unsigned long long)now.ring_timeouts);
    last = now;
    lastFrame = frame;
}
#endif
void SubmitShadedModelMesh(const mh3_resolved_mesh_submit& mesh, const NativeModelGPUView& resident) {
#ifdef MH3_DEVELOPMENT
    ReportMaterialGpu();
#endif
    // A packet item's references (SetNativeMaterialGpuDraw) replace the
    // classification and Animated section derived from the Render owner.
    const auto external = externalDraw;
    externalDraw.set = false;
    const ShadeEntry* externalEntry = external.set ? RecordOf(external.material) : nullptr;
    if (external.set && !externalEntry)
        throw std::runtime_error("Material GPU draw references no material record");
    const auto& entry = externalEntry ? *externalEntry : ShadeMaterial();
    const auto& shading = entry.shading;
    RequireEarlyDepth(shading);
    const auto* baked = MaterialGpu() || external.set ? RecordBlock(entry) : nullptr;
    if (external.set && !baked)
        throw std::runtime_error("Material GPU draw references a material without a GPU block");
    const bool gpu = baked != nullptr;
    AuroraNativeUniforms memory{};
    mh3_shade_draw* block = nullptr;
    mh3_shade_vertex* vertexBlock;
    if (gpu)
        vertexBlock = ShadeGpuDrawMemory(true, memory);
    else {
        block = ShadeDrawMemory(shading.model, true, memory);
        vertexBlock = block ? &block->vertex : nullptr;
    }
    if (!vertexBlock)
        return;
    auto& vertex = *vertexBlock;
    const auto lighting = ShadeChannels(shading, vertex);
    uint8_t coordinates = 0;
    for (unsigned role = 0; role < shading.textures; ++role)
        if (shading.texture_coordinate[role] < 8)
            coordinates |= uint8_t(1u << shading.texture_coordinate[role]);
    const auto& material = render.material;
    auto draw = mesh;
    draw.mesh = *resident.mesh;
    draw.material = &material;
    mh3_model_gpu_draw_inputs inputs;
    auto& palette = *reinterpret_cast<mh3_shade_palette*>(static_cast<char*>(memory.data) + (gpu ? ShadeGpuPaletteOffset : ShadePaletteOffset(shading.model)));
    const auto& viewport = GetNativeDrawState(NativeOwner::Render).viewport;
    Check(mh3_model_gpu_prepare_draw(&draw, coordinates, lighting.lit(), viewport[4], viewport[5], &inputs,
                                   reinterpret_cast<mh3_matrix34*>(palette.position[0]), reinterpret_cast<mh3_matrix34*>(palette.normal[0]),
                                   reinterpret_cast<mh3_matrix34*>(palette.position[10])), "Native model GPU inputs");
    const auto& layout = *resident.layout;
    const auto present = [&](unsigned location) {
        return std::any_of(layout.attributes.begin(), layout.attributes.end(), [&](const auto& a) { return a.location == location; });
    };
    // Shader inputs: 0 position, 1 normal, 2 colour0, 3..5 role coordinates,
    // 6 PN selector, 7..9 role texture-matrix selectors (byte lanes).
    aurora::gl::ShadedMeshInputs bind;
    bind.location = {0, 1, 2, -1, -1, -1, -1, -1, -1, -1};
    bind.lane.fill(-1);
    std::memcpy(vertex.proj, inputs.projection, sizeof vertex.proj);
    std::array<float, 2> size{};
    GetNativeRenderViewportSize(size.data());
    vertex.viewport[0] = size[0];
    vertex.viewport[1] = size[1];
    vertex.viewport[2] = -1.f / (6.f * std::max(std::fabs(size[0]), 1.f));
    vertex.viewport[3] = 1.f / (6.f * std::max(std::fabs(size[1]), 1.f));
    vertex.flags[0] = (inputs.matrix_index_mask & 1 ? MH3_SHADE_FLAG_MATRIX_SELECTOR : 0) | (present(2) ? MH3_SHADE_FLAG_COLOR0 : 0);
    if (inputs.matrix_index_mask & 1) {
        bind.location[6] = 12;
        bind.lane[6] = 0;
    }
    // Material GPU: a role whose post matrix is a pure SRT gets it from the
    // Animated block (Srt kind, or General with MH3_SHADE_TEXGEN_POST_SRT).
    mh3_shade_gpu_srt srt[3];
    uint8_t srtRoles = 0;
    if (external.set) {
        // The writer's SRT: roles flagged by the item are pure, non-identity.
        for (auto& role : srt)
            mh3_shade_gpu_srt_identity(&role);
        srtRoles = external.srtRoles;
    } else if (gpu)
        srtRoles = ShadeSrtRoles(shading, srt);
    std::array<NativeShaderTexgen, 3> kinds{};
    for (unsigned role = 0; role < shading.textures; ++role) {
        const unsigned coordinate = shading.texture_coordinate[role];
        if (coordinate >= 8)
            continue;
        const auto& source = inputs.texgen[coordinate];
        auto& texgen = vertex.texgen[role];
        std::memcpy(texgen.main, &source.main, sizeof texgen.main);
        const unsigned row = source.source_row;
        texgen.mode[0] = row == 0 ? MH3_SHADE_TEXGEN_POSITION : row == 1 ? MH3_SHADE_TEXGEN_NORMAL : MH3_SHADE_TEXGEN_COORDINATE;
        texgen.mode[1] = (source.projection ? MH3_SHADE_TEXGEN_PROJECTIVE : 0) | (source.normalize ? MH3_SHADE_TEXGEN_NORMALIZE : 0) | (source.main_identity ? MH3_SHADE_TEXGEN_MAIN_IDENTITY : 0) |
                         (source.post_identity ? MH3_SHADE_TEXGEN_POST_IDENTITY : 0) | (source.indexed ? MH3_SHADE_TEXGEN_INDEXED : 0);
        if (srtRoles & (1u << role)) {
            // Exactly the SRT matrix: identity parameters are the identity post.
            const bool plainRow = row > 1 && !source.projection && !source.normalize && !source.indexed;
            // External sections are not read back: flagged roles are non-identity.
            if (!external.set && SrtIdentity(srt[role]))
                kinds[role] = !plainRow ? NativeShaderTexgen::General : source.main_identity ? NativeShaderTexgen::Identity : NativeShaderTexgen::Affine;
            else
                kinds[role] = plainRow ? NativeShaderTexgen::Srt : NativeShaderTexgen::General;
            texgen.mode[1] |= MH3_SHADE_TEXGEN_POST_SRT;
        } else {
            const bool plain = mh3_model_gpu_texgen_is_plain(&source);
            kinds[role] = !plain ? NativeShaderTexgen::General : source.main_identity ? NativeShaderTexgen::Identity : NativeShaderTexgen::Affine;
            std::memcpy(texgen.post, &source.post, sizeof texgen.post);
        }
        if (row > 1)
            bind.location[3 + role] = int8_t(row <= 2 ? row : row - 1);
        if (source.indexed) {
            const unsigned packed = coordinate + 1;
            bind.location[7 + role] = int8_t(12 + packed / 4);
            bind.lane[7 + role] = int8_t(packed % 4);
        }
    }
    float bias[4];
    const auto units = ShadeTextures(shading, bias);
    auto variant = ShadeVariant(shading, NativeShaderVertex::Mesh, lighting);
    std::copy(kinds.begin(), kinds.end(), variant.texgen);
    std::optional<aurora::gl::ShadedBlocks> blocks;
    if (external.set) {
        std::optional<aurora::gl::UniformBlock> animatedBlock;
        mh3_shade_gpu_animated values;
        if (external.animated & AnimatedPersistent) {
            animatedBlock = PersistentAnimatedBlock(external.animated & ~AnimatedPersistent);
            values = persistentAnimated.slots[external.animated & ~AnimatedPersistent].values;
        } else {
            animatedBlock = RingBlock(external.animated);
            if (ResolvesOnCpu(entry))
                std::memcpy(&values, animatedBlock->data, sizeof values);
        }
        const auto materialBlock = animatedBlock ? ShadeMaterialBlock(entry, *baked, values) : std::nullopt;
        if (!materialBlock)
            return;
        variant.material_gpu = true;
        variant.dynamic = entry.dynamic && !ResolvesOnCpu(entry);
        blocks = ShadeGpuBlocks(memory, true, *materialBlock, *animatedBlock);
    } else if (gpu) {
        auto animatedBlock = ShadeAnimated(entry, bias, srt, srtRoles);
        const auto materialBlock = animatedBlock ? ShadeMaterialBlock(entry, *baked, animated.values) : std::nullopt;
        if (!materialBlock)
            return;
#ifdef MH3_DEVELOPMENT
        if (MaterialGpuVerify())
            VerifyMaterialGpu(entry, ResolvesOnCpu(entry) ? resolved.values : entry.material, animated.values, &draw);
        if (MaterialGpuRingSelfTest()) {
            // One producer packet per rendered frame; ended at the next frame.
            auto& t = ringSelfTest;
            if (t.frame != aurora::gfx::current_frame()) {
                if (t.frame != UINT32_MAX)
                    EndNativeMaterialGpuPacket(t.sequence);
                EnsureMaterialGpuRing();
                BeginNativeMaterialGpuPacket(++t.sequence);
                t.frame = aurora::gfx::current_frame();
            }
            NativeMaterialGpuAnimatedRef ref;
            if (auto* section = WriteNativeMaterialGpuAnimated(&ref)) {
                std::memcpy(section, &animated.values, sizeof animated.values);
                animatedBlock = RingBlock(ref.value);
            }
        }
#endif
        variant.material_gpu = true;
        variant.dynamic = entry.dynamic && !ResolvesOnCpu(entry);
        blocks = ShadeGpuBlocks(memory, true, *materialBlock, *animatedBlock);
    } else {
        std::memcpy(block->fragment.texture_bias, bias, sizeof bias);
        mh3_shade_fill(&shading, &material, block);
        blocks = ShadeBlocks(memory, shading.model, true);
    }
    if (!blocks)
        return;
    const GLuint program = ShadeProgram(variant)->get();
    aurora::gl::draw_shaded_mesh(*resident.buffer, layout, bind, resident.mesh->vertex_offset, resident.mesh->vertex_count, resident.mesh->index_offset + mesh.first_index * 2, mesh.index_count,
                                 program, *blocks, units);
#ifdef MH3_DEVELOPMENT
    if (aurora::gfx::port_capture::enabled())
        aurora::gfx::port_capture::native_mesh_source({aurora::gfx::current_frame(), program, mesh.asset_id, mesh.model_id, mesh.shape_id, mesh.mesh_index, mesh.first_index, mesh.index_count,
                                                       mesh.resident_generation, mesh.source_generation, mesh.pose_generation, material.generation, mesh.layout.flags});
#endif
}
void SubmitShadedMaterialGeometry(const AuroraNativeMaterialGeometry& geometry) {
    const auto& entry = ShadeMaterial();
    const auto& shading = entry.shading;
    RequireEarlyDepth(shading);
    const auto* baked = MaterialGpu() ? RecordBlock(entry) : nullptr;
    const bool gpu = baked != nullptr;
    AuroraNativeUniforms memory{};
    mh3_shade_draw* block = nullptr;
    mh3_shade_vertex* vertexBlock;
    if (gpu)
        vertexBlock = ShadeGpuDrawMemory(false, memory);
    else {
        block = ShadeDrawMemory(shading.model, false, memory);
        vertexBlock = block ? &block->vertex : nullptr;
    }
    if (!vertexBlock)
        return;
    const auto lighting = ShadeChannels(shading, *vertexBlock);
    std::array<uint8_t, 3> coordinates{};
    for (unsigned role = 0; role < shading.textures; ++role)
        coordinates[role] = shading.texture_coordinate[role] < 8 ? shading.texture_coordinate[role] : 0;
    float bias[4];
    const auto units = ShadeTextures(shading, bias);
    auto variant = ShadeVariant(shading, NativeShaderVertex::Geometry, lighting);
    std::optional<aurora::gl::ShadedBlocks> blocks;
    if (gpu) {
        // Pre-transformed geometry: no texgen, identity SRT.
        mh3_shade_gpu_srt srt[3];
        for (auto& role : srt)
            mh3_shade_gpu_srt_identity(&role);
        const auto animatedBlock = ShadeAnimated(entry, bias, srt, 0);
        const auto materialBlock = animatedBlock ? ShadeMaterialBlock(entry, *baked, animated.values) : std::nullopt;
        if (!materialBlock)
            return;
#ifdef MH3_DEVELOPMENT
        if (MaterialGpuVerify())
            VerifyMaterialGpu(entry, ResolvesOnCpu(entry) ? resolved.values : entry.material, animated.values, nullptr);
#endif
        variant.material_gpu = true;
        variant.dynamic = entry.dynamic && !ResolvesOnCpu(entry);
        blocks = ShadeGpuBlocks(memory, false, *materialBlock, *animatedBlock);
    } else {
        std::memcpy(block->fragment.texture_bias, bias, sizeof bias);
        mh3_shade_fill(&shading, &render.material, block);
        blocks = ShadeBlocks(memory, shading.model, false);
    }
    if (!blocks)
        return;
    const GLuint program = ShadeProgram(variant)->get();
    aurora::gl::draw_shaded_geometry(geometry, coordinates, program, *blocks, units);
}
} // namespace

namespace {
// Keep the compatibility candidate and its full copy off the ordinary stack.
[[gnu::noinline]] void OverlayOverlappingMaterial(const mh3_native_material_state& source, uint32_t top) {
    mh3_native_material_state next;
    Check(mh3_material_state_overlay_candidate(&next, &game.material, &source, top), "Native material overlay");
    OverlayNativeMaterial(next, MH3_MATERIAL_TOP_ALL);
}
// True when every field known in target is known in source as well (top-level
// counts and references are not tracked by known bits).
bool KnownSuperset(const mh3_native_material_state& source, const mh3_native_material_state& target) {
    const auto covers = [](auto from, auto to) { return (to & ~from) == 0; };
    for (unsigned i = 0; i < 16; ++i)
        if (!covers(source.tev[i].known, target.tev[i].known))
            return false;
    for (unsigned i = 0; i < 8; ++i)
        if (!covers(source.texgen[i].known, target.texgen[i].known))
            return false;
    for (unsigned i = 0; i < 2; ++i)
        if (!covers(source.channel[i].flags, target.channel[i].flags))
            return false;
    for (unsigned i = 0; i < 4; ++i)
        if (!covers(source.color[i].known, target.color[i].known) || !covers(source.konst[i].known, target.konst[i].known) ||
            !covers(source.swaps[i].known, target.swaps[i].known) || !covers(source.indirect[i].known, target.indirect[i].known))
            return false;
    for (unsigned i = 0; i < 3; ++i)
        if (!covers(source.indirect_matrix[i].known, target.indirect_matrix[i].known))
            return false;
    return covers(source.pixel.known, target.pixel.known) && (source.clip_mode_known || !target.clip_mode_known);
}
// Publishes a material-derived raster without importing it back. Other
// owners publish logically (no observer imports their raster).
void PublishMaterialRaster(const mh3_raster_state& raster, NativeOwner owner = NativeOwner::Game) {
    if (owner != NativeOwner::Game) {
        SetNativeRaster(raster, owner);
        return;
    }
    struct Export {
        Export() { publishingMaterialRaster = true; }
        ~Export() { publishingMaterialRaster = false; }
    };
    Export scoped;
    SetNativeRaster(raster);
}
} // namespace
const mh3_native_material_state& GetNativeMaterialState(NativeOwner owner) {
    if (owner == NativeOwner::Game)
        ObserveRasterPublications();
    return M(owner).material;
}
void SetNativeMaterial(const mh3_native_material_state& source) {
    // The merge observes only fields this producer writes. Equal publications
    // preserve the revision without comparing unknown storage or padding.
    OverlayNativeMaterial(source, MH3_MATERIAL_TOP_ALL);
}
void OverlayNativeModelMaterialGroup(const mh3_native_material_state& source, mh3_model_material_block kind, uint32_t count, NativeOwner owner) {
    if (owner == NativeOwner::Game)
        ObserveRasterPublications();
    if ((kind == MH3_MODEL_MATERIAL_INDIRECT || kind == MH3_MODEL_MATERIAL_TEXGEN) && !count)
        return;
    auto& m = M(owner);
    auto& material = m.material;
    // Only pixel groups affect raster state. Preflight the small pixel record
    // before mutation so unsupported output features retain atomic publication.
    if (kind == MH3_MODEL_MATERIAL_PIXEL) {
        mh3_pack_pixel_inputs pixel;
        mh3_material_group_pixel_preview_unchecked(&material.pixel, &source.pixel, &pixel);
        auto raster = GetNativeDrawState(owner).raster;
        Check(mh3_material_pixel_raster_unchecked(&pixel, &raster), "Native material group pixel raster");
        raster.cull_mode = material.cull_mode;
        PublishMaterialRaster(raster, owner);
    }
    const uint32_t structure = kind == MH3_MODEL_MATERIAL_PIXEL ? MH3_SHADE_STRUCTURE_PIXEL :
        kind == MH3_MODEL_MATERIAL_PIPELINE ? MH3_SHADE_STRUCTURE_TEV | MH3_SHADE_STRUCTURE_SWAPS : 0;
    const bool structureChanged = !m.shadeStructureDirty && structure &&
        mh3_shade_structure_overlay_changes(&material, &source, 0, structure);
    if (mh3_material_group_publish_mutation_unchecked(&material, &source, kind, count))
        Changed(NATIVE_STATE_MATERIAL, owner);
    m.shadeStructureDirty |= structureChanged;
}
void SetNativeModelMatrixSelectors(const mh3_model_matrix_selectors& selectors, NativeOwner owner) {
    if (owner == NativeOwner::Game)
        ObserveRasterPublications();
    if (mh3_model_matrix_overlay_unchecked(&selectors, &M(owner).material))
        Changed(NATIVE_STATE_MATERIAL, owner);
}
void OverlayNativeMaterial(const mh3_native_material_state& source, uint32_t top_known) {
    ObserveRasterPublications();
    auto& material = game.material;
    // Typed renderer producers own this internal boundary. External resource
    // admission keeps its checked APIs; release publication merges only declared
    // fields, with no whole-state candidate or repeated domain validation.
    const uintptr_t from = reinterpret_cast<uintptr_t>(&source), to = reinterpret_cast<uintptr_t>(&material);
    const bool overlap = from != to && (from < to ? to - from : from - to) < sizeof(material);
    if (overlap) {
        OverlayOverlappingMaterial(source, top_known);
        return;
    }
    mh3_pack_pixel_inputs pixel = material.pixel;
    uint64_t fields = source.pixel.known;
    while (fields) {
        const unsigned i = std::countr_zero(fields);
        assert(i < MH3_PIXEL_FIELD_COUNT);
        pixel.values[i] = source.pixel.values[i];
        fields &= fields - 1;
    }
    pixel.known |= source.pixel.known;
    if (top_known & MH3_MATERIAL_TOP_PIXEL_CLASSIFICATION)
        pixel.classification = source.pixel.classification;
    auto raster = GetNativeDrawState().raster;
    Check(mh3_material_pixel_raster_unchecked(&pixel, &raster), "Native material pixel raster");
    raster.cull_mode = (top_known & MH3_MATERIAL_TOP_CULL) ? source.cull_mode : material.cull_mode;
    PublishMaterialRaster(raster);
    const bool structureChanged = !game.shadeStructureDirty &&
        mh3_shade_structure_overlay_changes(&material, &source, top_known, MH3_SHADE_STRUCTURE_ALL);
    if (top_known == MH3_MATERIAL_TOP_ALL && KnownSuperset(source, material)) {
        // The effect, UI and scene producers publish a copy of the current
        // state with a few fields changed: every field the state knows is
        // known in the source, so the field-by-field merge would produce the
        // source itself. Compare and copy it whole instead.
        if (!BytesEqual(&material, &source, sizeof material)) {
            material = source;
            Changed(NATIVE_STATE_MATERIAL);
        }
    } else if (mh3_material_state_overlay_mutation_unchecked(&material, &source, top_known))
        Changed(NATIVE_STATE_MATERIAL);
    game.shadeStructureDirty |= structureChanged;
}
namespace {
// A patch without a pixel record keeps the material's pixel-derived raster
// and republishes it with the (possibly declared) cull mode.
void PublishPixelRaster(uint32_t cull, NativeOwner owner = NativeOwner::Game) {
    auto raster = GetNativeDrawState(owner).raster;
    Check(mh3_material_pixel_raster_unchecked(&M(owner).material.pixel, &raster), "Native material pixel raster");
    raster.cull_mode = cull;
    PublishMaterialRaster(raster, owner);
}
} // namespace
void OverlayNativeMaterialTop(uint32_t top_known, uint32_t texgens, uint32_t channels, uint32_t tevs, uint32_t indirect, uint32_t cull, NativeOwner owner) {
    if (owner == NativeOwner::Game)
        ObserveRasterPublications();
    auto& m = M(owner);
    auto& material = m.material;
    PublishPixelRaster((top_known & MH3_MATERIAL_TOP_CULL) ? cull : material.cull_mode, owner);
    m.shadeStructureDirty |= (top_known & MH3_MATERIAL_TOP_TEV_COUNT) && material.tev_count != tevs;
    if (mh3_material_overlay_top_mutation_unchecked(&material, top_known, texgens, channels, tevs, indirect, cull))
        Changed(NATIVE_STATE_MATERIAL, owner);
}
void OverlayNativeMaterialChannels(const mh3_pack_channel channels[2], NativeOwner owner) {
    if (owner == NativeOwner::Game)
        ObserveRasterPublications();
    auto& m = M(owner);
    PublishPixelRaster(m.material.cull_mode, owner);
    if (mh3_material_overlay_channels_mutation_unchecked(&m.material, channels))
        Changed(NATIVE_STATE_MATERIAL, owner);
}
void OverlayNativeMaterialIndirectMatrices(const mh3_pack_indirect_matrix matrices[3], NativeOwner owner) {
    if (owner == NativeOwner::Game)
        ObserveRasterPublications();
    auto& m = M(owner);
    PublishPixelRaster(m.material.cull_mode, owner);
    if (mh3_material_overlay_indirect_matrices_mutation_unchecked(&m.material, matrices))
        Changed(NATIVE_STATE_MATERIAL, owner);
}
void SetNativeFog(const mh3_native_fog& source) {
    auto next = game.preparedFog;
    Check(mh3_material_prepare_fog(&source, GetNativeDrawState().viewport[2], &next), "Native fog producer");
    // Producers republish per model; the scene block changes only with content.
    if (!game.fogKnown || std::memcmp(&game.fogSource, &source, sizeof source) || std::memcmp(&game.preparedFog, &next, sizeof next))
        Changed(NATIVE_STATE_LIGHTING);
    game.fogSource = source;
    game.preparedFog = next;
    game.fogKnown = true;
}
namespace {
// Producer identity of the Game owner's light slots (SetNativeModelLightSelection).
constinit const void* lightOwner = nullptr;
constinit uint64_t lightOwnerRevision = 0;
void SetNativeModelLights(const mh3_native_light_slots& source) {
    // Models reselect their light set per draw; prepared lights follow the slots.
    if (BytesEqual(&game.lightSlots, &source, sizeof source))
        return;
    std::array<mh3_shader_light, 8> next{};
    for (unsigned i = 0; i < 8; ++i)
        if (source.valid_mask & (1u << i))
            Check(mh3_material_prepare_light(&source.lights[i], &next[i]), "Native light producer");
    if (game.lightSlots.valid_mask != source.valid_mask || !BytesEqual(&game.preparedLights, &next, sizeof next) || !BytesEqual(&game.lightSlots, &source, sizeof source))
        Changed(NATIVE_STATE_LIGHTING);
    game.lightSlots = source;
    game.preparedLights = next;
}
} // namespace
void SetNativeModelLightSelection(const mh3_native_light_slots& source, const void* owner, uint64_t revision) {
    auto& current = game.lightSlots;
    // Reselecting the set already published from these slots: the lights,
    // source IDs and valid mask are unchanged (and so are the prepared
    // lights), which saves comparing all eight light records per draw.
    if (owner && owner == lightOwner && revision == lightOwnerRevision) {
        if (current.diffuse_color == source.diffuse_color && current.diffuse_alpha == source.diffuse_alpha && current.specular_color == source.specular_color &&
            current.specular_alpha == source.specular_alpha && !std::memcmp(&current.ambient, &source.ambient, sizeof source.ambient))
            return;
        current.diffuse_color = source.diffuse_color;
        current.diffuse_alpha = source.diffuse_alpha;
        current.specular_color = source.specular_color;
        current.specular_alpha = source.specular_alpha;
        current.ambient = source.ambient;
        Changed(NATIVE_STATE_LIGHTING);
        return;
    }
    SetNativeModelLights(source);
    lightOwner = owner;
    lightOwnerRevision = revision;
}
void SetNativeTextureMatrix(uint32_t selector, const float matrix[12], bool matrix3x4, NativeOwner owner) {
    auto& m = M(owner);
    TextureMatrix* destination = selector >= 64 ? &m.postTextureMatrices[(selector - 64) / 3] : &m.textureMatrices[(selector - 30) / 3];
    PublishNativeModelTextureMatrix(selector, matrix, matrix3x4, owner);
    // Materials republish their post matrices per draw; only a change advances
    // the revision the frame packet captures on.
    const bool same = destination->known && destination->matrix3x4 == matrix3x4 && !std::memcmp(destination->values.data(), matrix, 48);
    if (selector >= 64 && (selector - 64) / 3 < m.postSrt.size() && m.postSrt[(selector - 64) / 3].pure) {
        m.postSrt[(selector - 64) / 3].pure = false;
        ++m.srtRevision;
    }
    std::memcpy(destination->values.data(), matrix, 48);
    destination->known = true;
    destination->matrix3x4 = matrix3x4;
    if (selector >= 64 && !same)
        Changed(NATIVE_STATE_POST, owner);
}
void PublishNativeTextureSrt(const mh3_native_texture_srt& srt, uint32_t applied, NativeOwner owner) {
    auto& m = M(owner);
    for (unsigned i = 0; i < 8; ++i) {
        if (!(applied & (1u << i)))
            continue;
        const auto& slot = srt.slot[i];
        TextureSrt next;
        // mh3_texture_matrices_prepare: without a mapping and with the effect
        // skipped (identity flag), the matrix is exactly the XSI SRT.
        next.pure = (slot.flags & 1) && !slot.map_mode && (slot.effect_flags & 1) && srt.mode == 1;
        if (next.pure)
            mh3_shade_gpu_srt_from_slot(&slot, &next.params);
        auto& current = m.postSrt[i];
        if (current.pure != next.pure || (next.pure && std::memcmp(&current.params, &next.params, sizeof next.params))) {
            current = next;
            ++m.srtRevision;
        }
    }
}
bool GetNativeTextureMatrix(uint32_t selector, float matrix[12], bool* matrix3x4, NativeOwner owner) {
    const auto& m = M(owner);
    const TextureMatrix* source = selector >= 64 ? &m.postTextureMatrices[(selector - 64) / 3] : &m.textureMatrices[(selector - 30) / 3];
    if (!source->known)
        return false;
    std::memcpy(matrix, source->values.data(), 48);
    if (matrix3x4)
        *matrix3x4 = source->matrix3x4;
    return true;
}
void CaptureNativeMaterialSnapshot(NativeMaterialSnapshot& out) {
    ObserveRasterPublications();
    out.material = game.material;
    out.depthBeforeTexture = game.depthBeforeTexture;
}
void CaptureNativeLightingSnapshot(NativeLightingSnapshot& out) {
    out.lights = game.lightSlots;
    out.preparedLights = game.preparedLights;
    out.fog = game.fogSource;
    out.preparedFog = game.preparedFog;
    out.fogKnown = game.fogKnown;
}
void CaptureNativePostSnapshot(NativePostSnapshot& out) {
    out.known = out.matrix3x4 = 0;
    for (unsigned i = 0; i < 8; ++i) {
        const auto& post = game.postTextureMatrices[i];
        out.post[i] = post.values;
        if (post.known)
            out.known |= uint8_t(1u << i);
        if (post.matrix3x4)
            out.matrix3x4 |= uint8_t(1u << i);
    }
}
void RestoreNativeMaterialSnapshot(const NativeMaterialSnapshot& snapshot) {
    if (!render.shadeStructureDirty && !mh3_shade_structure_equal(&render.material, &snapshot.material))
        render.shadeStructureDirty = true;
    render.material = snapshot.material;
    render.depthBeforeTexture = snapshot.depthBeforeTexture;
    Changed(NATIVE_STATE_MATERIAL, NativeOwner::Render);
}
void RestoreNativeLightingSnapshot(const NativeLightingSnapshot& snapshot) {
    render.lightSlots = snapshot.lights;
    render.preparedLights = snapshot.preparedLights;
    render.fogSource = snapshot.fog;
    render.preparedFog = snapshot.preparedFog;
    render.fogKnown = snapshot.fogKnown;
    Changed(NATIVE_STATE_LIGHTING, NativeOwner::Render);
}
void RestoreNativePostSnapshot(const NativePostSnapshot& snapshot) {
    for (unsigned i = 0; i < 8; ++i) {
        auto& post = render.postTextureMatrices[i];
        post.values = snapshot.post[i];
        post.known = (snapshot.known >> i) & 1;
        post.matrix3x4 = (snapshot.matrix3x4 >> i) & 1;
        render.postSrt[i].pure = false;
    }
    ++render.srtRevision;
    Changed(NATIVE_STATE_POST, NativeOwner::Render);
}
void SetNativeDepthCompareBeforeTexture(bool before, NativeOwner owner) {
    auto& m = M(owner);
    if (m.depthBeforeTexture != before) {
        m.depthBeforeTexture = before;
        Changed(NATIVE_STATE_MATERIAL, owner);
    }
}
const mh3_native_light_slots& GetNativeModelLights(NativeOwner owner) {
    return M(owner).lightSlots;
}
void CopyNativeMaterialOwner(NativeOwner from, NativeOwner to) {
    if (from == NativeOwner::Game)
        ObserveRasterPublications();
    auto& source = M(from);
    auto& target = M(to);
    target.material = source.material;
    target.shadeStructureDirty = source.shadeStructureDirty;
    target.depthBeforeTexture = source.depthBeforeTexture;
    target.postTextureMatrices = source.postTextureMatrices;
    target.postSrt = source.postSrt;
    ++target.srtRevision;
    Changed(NATIVE_STATE_MATERIAL, to);
    Changed(NATIVE_STATE_POST, to);
}
#ifdef MH3_DEVELOPMENT
std::string CompareNativeMaterialOwners(NativeOwner a, NativeOwner b) {
    const auto& x = M(a);
    const auto& y = M(b);
    std::string result;
    auto material = x.material, other = y.material;
    material.generation = other.generation = 0;
    if (std::memcmp(&material, &other, sizeof material)) {
        result += " material";
        const auto* p = reinterpret_cast<const uint8_t*>(&material);
        const auto* q = reinterpret_cast<const uint8_t*>(&other);
        for (size_t i = 0; i < sizeof material; ++i)
            if (p[i] != q[i]) {
                char text[48];
                std::snprintf(text, sizeof text, "@%zu(%02x/%02x)", i, p[i], q[i]);
                result += text;
                break;
            }
    }
    if (x.depthBeforeTexture != y.depthBeforeTexture)
        result += " depth-before-texture";
    for (unsigned i = 0; i < 8; ++i) {
        const auto& p = x.postTextureMatrices[i];
        const auto& q = y.postTextureMatrices[i];
        if (p.known != q.known || (p.known && (p.matrix3x4 != q.matrix3x4 || std::memcmp(p.values.data(), q.values.data(), 48))))
            result += " post" + std::to_string(i);
    }
    return result;
}
#endif
void SubmitNativeMaterialGeometry(const AuroraNativeMaterialGeometry& geometry) {
    if (NativeGraphicsFrameShouldRender())
        RecordNativeMaterialGeometry(geometry);
}
void DrawNativeMaterialGeometry(const AuroraNativeMaterialGeometry& geometry) {
    SubmitShadedMaterialGeometry(geometry);
}
NativeMaterialGpuStatistics ReadNativeMaterialGpuStatistics() {
    auto result = gpuStatistics;
    result.persistent_bytes = aurora::gl::uniform_block_bytes();
    result.ring_waits = ringState.waits.load(std::memory_order_relaxed);
    result.ring_full = ringState.full.load(std::memory_order_relaxed);
    result.ring_timeouts = ringState.timeouts.load(std::memory_order_relaxed);
    return result;
}
NativeMaterialGpuRef AcquireNativeMaterialGpu(const mh3_native_material_state& state) {
    const auto* record = AcquireRecord(mh3_shade_structure_key(&state), state);
    return {record ? record->index : 0u};
}
NativeMaterialGpuInfo DescribeNativeMaterialGpu(NativeMaterialGpuRef material) {
    NativeMaterialGpuInfo info;
    const auto* record = RecordOf(material.index);
    if (!record)
        return info;
    const auto& m = record->shading;
    info.valid = true;
    info.supported = record->supported;
    info.dynamic = record->dynamic;
    info.cpu_resolve = ResolvesOnCpu(*record);
    info.model = m.model;
    info.textures = m.textures;
    info.alpha_test = m.alpha_test;
    for (unsigned role = 0; role < 3; ++role)
        info.texture_coordinate[role] = role < m.textures ? m.texture_coordinate[role] : 0xff;
    info.structure_key = record->key;
    return info;
}
NativeMaterialGpuAnimatedRef CreateNativeMaterialGpuAnimated(const mh3_shade_gpu_animated& values) {
    auto& p = persistentAnimated;
    std::lock_guard lock(p.lock);
    if (!p.slots)
        p.slots = std::make_unique<PersistentAnimatedSlot[]>(MaxPersistentAnimated);
    uint32_t index;
    if (!p.free.empty()) {
        index = p.free.back();
        p.free.pop_back();
    } else if (p.count < MaxPersistentAnimated)
        index = p.count++;
    else
        return {};
    p.slots[index].values = values;
    return {index | AnimatedPersistent};
}
void ReleaseNativeMaterialGpuAnimated(NativeMaterialGpuAnimatedRef animated) {
    if (animated.value == UINT32_MAX || !(animated.value & AnimatedPersistent))
        return;
    auto& p = persistentAnimated;
    std::lock_guard lock(p.lock);
    p.released.push_back(animated.value & ~AnimatedPersistent);
}
void BeginNativeMaterialGpuPacket(uint64_t sequence) {
    auto& r = ringState;
    r.used = RingSegmentBytes;
    if (!r.data.load(std::memory_order_acquire))
        return;
    const unsigned segment = unsigned(sequence % RingSegments);
    auto& slot = r.segments[segment];
    const uint64_t previous = slot.sequence.load(std::memory_order_relaxed);
    if (previous != UINT64_MAX) {
        // The previous packet of this segment ended (or was replaced by an
        // ended later one) and the GPU finished the work submitted by then.
        const auto start = std::chrono::steady_clock::now();
        bool waited = false;
        while (r.ended.load(std::memory_order_acquire) <= previous || !aurora::gl::gpu_fence_done(slot.fence)) {
            waited = true;
            if (std::chrono::steady_clock::now() - start > std::chrono::seconds(1)) {
                // No EndNativeMaterialGpuPacket: this packet writes no sections.
                r.timeouts.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        if (waited)
            r.waits.fetch_add(1, std::memory_order_relaxed);
    }
    slot.sequence.store(sequence, std::memory_order_release);
    r.segment = segment;
    r.used = 0;
}
mh3_shade_gpu_animated* WriteNativeMaterialGpuAnimated(NativeMaterialGpuAnimatedRef* animated) {
    auto& r = ringState;
    if (r.used > RingSegmentBytes - AnimatedBytes) {
        if (r.used != RingSegmentBytes)
            r.full.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    const uint32_t offset = r.segment * RingSegmentBytes + r.used;
    r.used += AnimatedBytes;
    animated->value = offset;
    return reinterpret_cast<mh3_shade_gpu_animated*>(r.data.load(std::memory_order_relaxed) + offset);
}
void EndNativeMaterialGpuPacket(uint64_t sequence) {
    EnsureMaterialGpuRing();
    RetireReleasedAnimated();
    auto& r = ringState;
    auto& slot = r.segments[sequence % RingSegments];
    if (slot.sequence.load(std::memory_order_acquire) == sequence)
        slot.fence = aurora::gl::submitted_gpu_fence();
    if (r.ended.load(std::memory_order_relaxed) < sequence + 1)
        r.ended.store(sequence + 1, std::memory_order_release);
}
void SetNativeMaterialGpuDraw(NativeMaterialGpuRef material, NativeMaterialGpuAnimatedRef animated, uint8_t srt_roles) {
    externalDraw = {true, material.index, animated.value, srt_roles};
}
void SubmitNativeModelMesh(const mh3_resolved_mesh_submit& mesh) {
    if (!mesh.index_count)
        return;
    ObserveNativeModelAssetDemand(mesh.resident_handle, mesh.resident_generation);
    if (!NativeGraphicsFrameShouldRender())
        return;
    if (!EnsureNativeModelAssetGPU(mesh.resident_handle, mesh.resident_generation)) {
        CancelNativeGraphicsFrame();
        return;
    }
    const auto resident = ResolveNativeModelGPU(mesh);
    if (mesh.material != &render.material)
        throw std::runtime_error("Model draw material is not the render owner's");
    // Residency and demand above stay counted: a culled mesh keeps its assets.
    if (CullNativeModelMesh(mesh))
        return;
    SubmitShadedModelMesh(mesh, resident);
}
void ClearNativeMaterialPrograms() {
    shade.programs.clear();
    // Records stay (CPU classifications); their GPU blocks go with the context.
    for (unsigned i = 0; i < MaxMaterialRecords; ++i)
        if (recordBlocks.state[i] == 1)
            aurora::gl::retire_uniform_block(recordBlocks.blocks[i]);
    recordBlocks.state = {};
    recordBlocks.blocks = {};
    ReleaseMaterialGpuRing();
    shade.overflow.clear();
    shade.last = nullptr;
    animated = {};
    resolved = {};
    carve = {};
    render.shadeStructureDirty = true;
    ClearNativeModelGPU();
    if (rasterObserverInstalled)
        SetNativeRasterObserver(nullptr);
    rasterObserverInstalled = false;
}
