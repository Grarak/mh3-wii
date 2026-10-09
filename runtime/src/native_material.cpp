// Typed native material state, pure equation generation, and direct frame-slot
// uniforms. No GX state, command-list decoder, FIFO, or GPU wait participates.
#include "frame_split.h"
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
#include "../native/mh3_shade.h"
#ifdef MH3_DEVELOPMENT
#include "gfx/port_capture.hpp"
namespace aurora::gfx {
uint32_t current_frame() noexcept;
}
#endif
#include <array>
#include <cassert>
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

namespace {
constexpr uint64_t Bit(unsigned field) {
    return UINT64_C(1) << field;
}
mh3_native_material_state material{};
mh3_native_fog fogSource{};
mh3_shader_fog preparedFog{};
bool fogKnown = false, depthBeforeTexture = false;
mh3_native_light_slots lightSlots{};
std::array<mh3_shader_light, 8> preparedLights{};
struct TextureMatrix {
    std::array<float, 12> values{};
    bool known = false, matrix3x4 = false;
};
std::array<TextureMatrix, 10> textureMatrices{};
std::array<TextureMatrix, 20> postTextureMatrices{};
bool rasterObserverInstalled = false, publishingMaterialRaster = false;
void Owner() {
    if (!aurora::gl::on_render_thread())
        throw std::runtime_error("Native material changed outside graphics owner");
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
    for (const auto& field : fields) {
        material.pixel.known |= Bit(field[0]);
        material.pixel.values[field[0]] = field[1];
    }
    material.cull_mode = raster.cull_mode;
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
    const auto* binding = GetNativeTextureBinding(unit);
    if (!binding)
        throw std::runtime_error("Native material texture " + std::to_string(unit) + " has no typed producer");
    return *binding;
}
// ---- Fixed native shading: classified materials, hand-written programs ----
struct ShadeState {
    std::unordered_map<uint64_t, mh3_shade_material> materials;
    uint64_t lastKey = 0;
    const mh3_shade_material* last = nullptr;
    std::unordered_map<uint32_t, aurora::gl::ProgramPtr> programs;
    mh3_shade_scene scene{};
    // revision counts producer changes; upload counts rebuilt scene blocks.
    uint64_t revision = 0, built = UINT64_MAX, upload = 0;
    float width = -1.f;
    // The scene block's copy in the current frame's GPU-visible uniform memory.
    AuroraNativeUniforms sceneMemory{};
    uint64_t sceneMemoryUpload = 0;
} shade;
const mh3_shade_material& ShadeMaterial() {
    const uint64_t key = mh3_shade_structure_key(&material);
    if (shade.last && key == shade.lastKey)
        return *shade.last;
    auto [entry, inserted] = shade.materials.try_emplace(key);
    if (inserted) {
        const uint8_t quality = mh3_shade_classify(&material, &entry->second);
#ifdef MH3_DEVELOPMENT
        if (!(quality & MH3_SHADE_EXACT))
            std::fprintf(stderr, "[shade] approximated material structure %016llx quality %02x stages %u\n", (unsigned long long)key, quality, material.tev_count);
#else
        (void)quality;
#endif
    }
    shade.lastKey = key;
    shade.last = &entry->second;
    return entry->second;
}
const mh3_shade_scene& ShadeScene(uint64_t& upload) {
    const float width = GetNativeDrawState().viewport[2];
    if (shade.built != shade.revision || shade.width != width) {
        auto& scene = shade.scene;
        std::memset(&scene, 0, sizeof scene);
        for (unsigned i = 0; i < 8; ++i)
            if (lightSlots.valid_mask & (1u << i))
                scene.lights[i] = preparedLights[i];
        const uint32_t type = fogKnown ? fogSource.type : 0;
        auto fog = preparedFog;
        if (type)
            Check(mh3_material_prepare_fog(&fogSource, width, &fog), "Native fog draw preparation");
        mh3_shade_fog(type, &fog, &scene);
        shade.built = shade.revision;
        shade.width = width;
        ++shade.upload;
    }
    upload = shade.upload;
    return shade.scene;
}
aurora::gl::ProgramPtr& ShadeProgram(const NativeShaderVariant& variant) {
    const uint32_t key = uint32_t(variant.vertex) | uint32_t(variant.lighting) << 1 | uint32_t(variant.lights) << 3 | uint32_t(variant.model) << 7 | uint32_t(variant.textures) << 8 |
                         uint32_t(variant.overlay) << 10 | uint32_t(variant.alpha_test) << 11 | uint32_t(variant.fog) << 12 | uint32_t(variant.texgen[0]) << 13 | uint32_t(variant.texgen[1]) << 15 |
                         uint32_t(variant.texgen[2]) << 17;
    auto& program = shade.programs[key];
    if (!program) {
        auto sources = NativeShaderSource(variant);
        aurora::gl::ProgramDesc desc;
        desc.label = "shade " + NativeShaderVertexName(variant) + "+" + NativeShaderFragmentName(variant);
        desc.origin = "runtime/src/native_shader_source.cpp";
#ifdef AURORA_GXM
        desc.vertex = std::move(sources.cg_vertex);
        desc.fragment = std::move(sources.cg_fragment);
        desc.cg = true;
#else
        desc.vertex = std::move(sources.glsl_vertex);
        desc.fragment = std::move(sources.glsl_fragment);
#endif
        desc.samplers = {{"u_texture0", 0}, {"u_texture1", 1}, {"u_texture2", 2}};
        desc.blocks = {{"Draw", 0}, {"Scene", 1}, {"Palette", 2}, {"Shade", 3}};
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
std::array<uint8_t, 3> ShadeTextures(const mh3_shade_material& shading, mh3_shade_fragment& fragment) {
    std::array<uint8_t, 3> units{};
    for (unsigned role = 0; role < 3; ++role) {
        fragment.texture_bias[role] = 0.f;
        if (role >= shading.textures)
            continue;
        const unsigned map = shading.texture_map[role];
        (void)Texture(map);
        units[role] = uint8_t(map);
        fragment.texture_bias[role] = aurora::native_texture_shader_lod_bias(map);
    }
    fragment.texture_bias[3] = 0.f;
    return units;
}
void RequireEarlyDepth(const mh3_shade_material& shading) {
    const auto& raster = GetNativeDrawState().raster;
    if (depthBeforeTexture && raster.depth_test && raster.depth_write && shading.alpha_test)
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
constexpr uint32_t ShadePaletteOffset(uint8_t model) {
    return (MH3_SHADE_DRAW_BYTES(model) + 255u) & ~255u;
}
mh3_shade_draw* ShadeDrawMemory(uint8_t model, bool palette, AuroraNativeUniforms& memory) {
    if (!aurora_allocate_native_uniforms(palette ? ShadePaletteOffset(model) + uint32_t(sizeof(mh3_shade_palette)) : MH3_SHADE_DRAW_BYTES(model), &memory))
        return nullptr;
    auto* block = static_cast<mh3_shade_draw*>(memory.data);
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
    variant.textures = shading.textures;
    variant.overlay = shading.overlay != 0;
    variant.alpha_test = shading.alpha_test != 0;
    variant.fog = fogKnown && fogSource.type != 0;
    return variant;
}
void SubmitShadedModelMesh(const mh3_resolved_mesh_submit& mesh, const NativeModelGPUView& resident) {
    const auto& shading = ShadeMaterial();
    RequireEarlyDepth(shading);
    AuroraNativeUniforms memory{};
    auto* const block = ShadeDrawMemory(shading.model, true, memory);
    if (!block)
        return;
    auto& vertex = block->vertex;
    const auto lighting = ShadeChannels(shading, vertex);
    uint8_t coordinates = 0;
    for (unsigned role = 0; role < shading.textures; ++role)
        if (shading.texture_coordinate[role] < 8)
            coordinates |= uint8_t(1u << shading.texture_coordinate[role]);
    auto draw = mesh;
    draw.mesh = *resident.mesh;
    draw.material = &material;
    mh3_model_gpu_inputs inputs{};
    Check(mh3_model_gpu_prepare(&draw, coordinates, lighting.lit(), GetNativeDrawState().viewport[4], GetNativeDrawState().viewport[5], &inputs), "Native model GPU inputs");
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
    std::array<NativeShaderTexgen, 3> kinds{};
    for (unsigned role = 0; role < shading.textures; ++role) {
        const unsigned coordinate = shading.texture_coordinate[role];
        if (coordinate >= 8)
            continue;
        const auto& source = inputs.texgen[coordinate];
        const bool plain = source.source_row > 1 && !source.projection && !source.normalize && source.post_identity && !source.indexed;
        kinds[role] = !plain ? NativeShaderTexgen::General : source.main_identity ? NativeShaderTexgen::Identity : NativeShaderTexgen::Affine;
        auto& texgen = vertex.texgen[role];
        std::memcpy(texgen.main, &source.main, sizeof texgen.main);
        std::memcpy(texgen.post, &source.post, sizeof texgen.post);
        const unsigned row = source.source_row;
        texgen.mode[0] = row == 0 ? MH3_SHADE_TEXGEN_POSITION : row == 1 ? MH3_SHADE_TEXGEN_NORMAL : MH3_SHADE_TEXGEN_COORDINATE;
        texgen.mode[1] = (source.projection ? MH3_SHADE_TEXGEN_PROJECTIVE : 0) | (source.normalize ? MH3_SHADE_TEXGEN_NORMALIZE : 0) | (source.main_identity ? MH3_SHADE_TEXGEN_MAIN_IDENTITY : 0) |
                         (source.post_identity ? MH3_SHADE_TEXGEN_POST_IDENTITY : 0) | (source.indexed ? MH3_SHADE_TEXGEN_INDEXED : 0);
        if (row > 1)
            bind.location[3 + role] = int8_t(row <= 2 ? row : row - 1);
        if (source.indexed) {
            const unsigned packed = coordinate + 1;
            bind.location[7 + role] = int8_t(12 + packed / 4);
            bind.lane[7 + role] = int8_t(packed % 4);
        }
    }
    const auto units = ShadeTextures(shading, block->fragment);
    mh3_shade_fill(&shading, &material, block);
    auto variant = ShadeVariant(shading, NativeShaderVertex::Mesh, lighting);
    std::copy(kinds.begin(), kinds.end(), variant.texgen);
    const GLuint program = ShadeProgram(variant)->get();
    auto& palette = *reinterpret_cast<mh3_shade_palette*>(static_cast<char*>(memory.data) + ShadePaletteOffset(shading.model));
    std::memcpy(palette.position[0], inputs.position_palette, sizeof inputs.position_palette);
    std::memcpy(palette.position[10], inputs.texture_palette, sizeof inputs.texture_palette);
    std::memcpy(palette.normal[0], inputs.normal_palette, sizeof inputs.normal_palette);
    const auto blocks = ShadeBlocks(memory, shading.model, true);
    if (!blocks)
        return;
    aurora::gl::draw_shaded_mesh(*resident.buffer, layout, bind, resident.mesh->vertex_offset, resident.mesh->vertex_count, resident.mesh->index_offset + mesh.first_index * 2, mesh.index_count,
                                 program, *blocks, units);
#ifdef MH3_DEVELOPMENT
    if (aurora::gfx::port_capture::enabled())
        aurora::gfx::port_capture::native_mesh_source({aurora::gfx::current_frame(), program, mesh.asset_id, mesh.model_id, mesh.shape_id, mesh.mesh_index, mesh.first_index, mesh.index_count,
                                                       mesh.resident_generation, mesh.source_generation, mesh.pose_generation, material.generation, mesh.layout.flags});
#endif
}
void SubmitShadedMaterialGeometry(const AuroraNativeMaterialGeometry& geometry) {
    const auto& shading = ShadeMaterial();
    RequireEarlyDepth(shading);
    AuroraNativeUniforms memory{};
    auto* const block = ShadeDrawMemory(shading.model, false, memory);
    if (!block)
        return;
    const auto lighting = ShadeChannels(shading, block->vertex);
    std::array<uint8_t, 3> coordinates{};
    for (unsigned role = 0; role < shading.textures; ++role)
        coordinates[role] = shading.texture_coordinate[role] < 8 ? shading.texture_coordinate[role] : 0;
    const auto units = ShadeTextures(shading, block->fragment);
    mh3_shade_fill(&shading, &material, block);
    const GLuint program = ShadeProgram(ShadeVariant(shading, NativeShaderVertex::Geometry, lighting))->get();
    const auto blocks = ShadeBlocks(memory, shading.model, false);
    if (!blocks)
        return;
    aurora::gl::draw_shaded_geometry(geometry, coordinates, program, *blocks, units);
}
} // namespace

namespace {
// Keep the compatibility candidate and its full copy off the ordinary stack.
[[gnu::noinline]] void OverlayOverlappingMaterial(const mh3_native_material_state& source, uint32_t top) {
    mh3_native_material_state next;
    Check(mh3_material_state_overlay_candidate(&next, &material, &source, top), "Native material overlay");
    OverlayNativeMaterial(next, MH3_MATERIAL_TOP_ALL);
}
} // namespace
const mh3_native_material_state& GetNativeMaterialState() {
    Owner();
    ObserveRasterPublications();
    return material;
}
void SetNativeMaterial(const mh3_native_material_state& source) {
    OverlayNativeMaterial(source, MH3_MATERIAL_TOP_ALL);
}
void OverlayNativeModelMaterialGroup(const mh3_native_material_state& source, mh3_model_material_block kind, uint32_t count) {
    Owner();
    ObserveRasterPublications();
    if ((kind == MH3_MODEL_MATERIAL_INDIRECT || kind == MH3_MODEL_MATERIAL_TEXGEN) && !count)
        return;
    // Only pixel groups affect raster state. Preflight the small pixel record
    // before mutation so unsupported output features retain atomic publication.
    if (kind == MH3_MODEL_MATERIAL_PIXEL) {
        mh3_pack_pixel_inputs pixel;
        mh3_material_group_pixel_preview_unchecked(&material.pixel, &source.pixel, &pixel);
        auto raster = GetNativeDrawState().raster;
        Check(mh3_material_pixel_raster_unchecked(&pixel, &raster), "Native material group pixel raster");
        raster.cull_mode = material.cull_mode;
        struct Export {
            Export() { publishingMaterialRaster = true; }
            ~Export() { publishingMaterialRaster = false; }
        };
        {
            Export scoped;
            SetNativeRaster(raster);
        }
    }
    (void)mh3_material_group_publish_unchecked(&material, &source, kind, count, nullptr);
}
void SetNativeModelMatrixSelectors(const mh3_model_matrix_selectors& selectors) {
    Owner();
    ObserveRasterPublications();
    (void)mh3_model_matrix_overlay_unchecked(&selectors, &material);
}
void OverlayNativeMaterial(const mh3_native_material_state& source, uint32_t top_known) {
    Owner();
    ObserveRasterPublications();
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
    struct Export {
        Export() { publishingMaterialRaster = true; }
        ~Export() { publishingMaterialRaster = false; }
    };
    {
        Export scoped;
        SetNativeRaster(raster);
    }
    mh3_material_state_overlay_unchecked(&material, &source, top_known);
}
void SetNativeFog(const mh3_native_fog& source) {
    Owner();
    auto next = preparedFog;
    Check(mh3_material_prepare_fog(&source, GetNativeDrawState().viewport[2], &next), "Native fog producer");
    // Producers republish per model; the scene block changes only with content.
    if (!fogKnown || std::memcmp(&fogSource, &source, sizeof source) || std::memcmp(&preparedFog, &next, sizeof next))
        ++shade.revision;
    fogSource = source;
    preparedFog = next;
    fogKnown = true;
}
void SetNativeModelLights(const mh3_native_light_slots& source) {
    Owner();
    std::array<mh3_shader_light, 8> next{};
    for (unsigned i = 0; i < 8; ++i)
        if (source.valid_mask & (1u << i))
            Check(mh3_material_prepare_light(&source.lights[i], &next[i]), "Native light producer");
    if (lightSlots.valid_mask != source.valid_mask || std::memcmp(&preparedLights, &next, sizeof next))
        ++shade.revision;
    lightSlots = source;
    preparedLights = next;
}
void SetNativeTextureMatrix(uint32_t selector, const float matrix[12], bool matrix3x4) {
    Owner();
    TextureMatrix* destination = nullptr;
    if (selector >= 30 && selector < 60 && (selector - 30) % 3 == 0)
        destination = &textureMatrices[(selector - 30) / 3];
    else if (selector >= 64 && selector < 124 && (selector - 64) % 3 == 0)
        destination = &postTextureMatrices[(selector - 64) / 3];
    if (!destination || !matrix)
        throw std::runtime_error("Invalid native texture-matrix producer");
    for (unsigned i = 0; i < 12; ++i)
        if (!std::isfinite(matrix[i]))
            throw std::runtime_error("Nonfinite native texture matrix");
    PublishNativeModelTextureMatrix(selector, matrix, matrix3x4);
    std::memcpy(destination->values.data(), matrix, 48);
    destination->known = true;
    destination->matrix3x4 = matrix3x4;
}
bool GetNativeTextureMatrix(uint32_t selector, float matrix[12], bool* matrix3x4) {
    Owner();
    const TextureMatrix* source = nullptr;
    if (selector >= 30 && selector < 60 && (selector - 30) % 3 == 0)
        source = &textureMatrices[(selector - 30) / 3];
    else if (selector >= 64 && selector < 124 && (selector - 64) % 3 == 0)
        source = &postTextureMatrices[(selector - 64) / 3];
    if (!source || !matrix)
        throw std::runtime_error("Invalid native texture-matrix consumer");
    if (!source->known)
        return false;
    std::memcpy(matrix, source->values.data(), 48);
    if (matrix3x4)
        *matrix3x4 = source->matrix3x4;
    return true;
}
void CaptureNativeShadeSnapshot(NativeShadeSnapshot& out) {
    Owner();
    ObserveRasterPublications();
    out.material = material;
    out.raster = GetNativeDrawState().raster;
    out.lights = lightSlots;
    out.preparedLights = preparedLights;
    out.fog = fogSource;
    out.preparedFog = preparedFog;
    out.fogKnown = fogKnown;
    out.depthBeforeTexture = depthBeforeTexture;
    out.postKnown = out.post3x4 = 0;
    for (unsigned i = 0; i < 8; ++i) {
        const auto& post = postTextureMatrices[i];
        out.post[i] = post.values;
        if (post.known)
            out.postKnown |= uint8_t(1u << i);
        if (post.matrix3x4)
            out.post3x4 |= uint8_t(1u << i);
    }
    CaptureNativeTextureBindings(out);
}
void RestoreNativeShadeSnapshot(const NativeShadeSnapshot& snapshot) {
    Owner();
    ObserveRasterPublications();
    material = snapshot.material;
    {
        // The snapshot's material already holds its raster fields.
        publishingMaterialRaster = true;
        SetNativeRaster(snapshot.raster);
        publishingMaterialRaster = false;
    }
    if (lightSlots.valid_mask != snapshot.lights.valid_mask || std::memcmp(&preparedLights, &snapshot.preparedLights, sizeof preparedLights))
        ++shade.revision;
    lightSlots = snapshot.lights;
    preparedLights = snapshot.preparedLights;
    if (fogKnown != snapshot.fogKnown || std::memcmp(&fogSource, &snapshot.fog, sizeof fogSource) || std::memcmp(&preparedFog, &snapshot.preparedFog, sizeof preparedFog))
        ++shade.revision;
    fogSource = snapshot.fog;
    preparedFog = snapshot.preparedFog;
    fogKnown = snapshot.fogKnown;
    depthBeforeTexture = snapshot.depthBeforeTexture;
    for (unsigned i = 0; i < 8; ++i) {
        auto& post = postTextureMatrices[i];
        post.values = snapshot.post[i];
        post.known = (snapshot.postKnown >> i) & 1;
        post.matrix3x4 = (snapshot.post3x4 >> i) & 1;
    }
    RestoreNativeTextureBindings(snapshot);
}
void SetNativeDepthCompareBeforeTexture(bool before) {
    Owner();
    depthBeforeTexture = before;
}
void SubmitNativeMaterialGeometry(const AuroraNativeMaterialGeometry& geometry) {
    Owner();
    if (!NativeGraphicsFrameShouldRender())
        return;
    SubmitShadedMaterialGeometry(geometry);
}
void SubmitNativeModelMesh(const mh3_resolved_mesh_submit& mesh) {
    MH3_SPLIT_SCOPE(MH3_SPLIT_MESH_SUBMIT);
    Owner();
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
    if (!mesh.material)
        throw std::runtime_error("Model draw has no native material producer");
    if (mesh.material != &material)
        SetNativeMaterial(*mesh.material);
    SubmitShadedModelMesh(mesh, resident);
}
void ClearNativeMaterialPrograms() {
    Owner();
    shade.programs.clear();
    shade.materials.clear();
    shade.last = nullptr;
    ClearNativeModelGPU();
    if (rasterObserverInstalled)
        SetNativeRasterObserver(nullptr);
    rasterObserverInstalled = false;
}
