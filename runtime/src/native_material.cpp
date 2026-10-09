// Typed native material state, pure equation generation, and direct frame-slot
// uniforms. No GX state, command-list decoder, FIFO, or GPU wait participates.
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
#include <aurora/native_texture.hpp>
#include "gx/gx.hpp"
#include "gx/shader_info.hpp"
#include "gl/gl.hpp"
#include "native_shader_source.h"
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
#include <xxhash.h>

namespace {
using aurora::gx::ShaderConfig;
using aurora::gx::ShaderInfo;
constexpr uint64_t Bit(unsigned field) {
    return UINT64_C(1) << field;
}
constexpr uint64_t AlphaFields = Bit(MH3_PIXEL_ALPHA_COMPARE0) | Bit(MH3_PIXEL_ALPHA_COMPARE1) | Bit(MH3_PIXEL_ALPHA_OPERATION);
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
struct BaseSuffix {
    uint8_t actual = 0, portable = 0;
    uint64_t hash = 0;
    bool known = false;
    aurora::gl::ProgramPtr program;
};
struct ModelSuffix {
    std::array<uint32_t, 17> layout{};
    uint8_t actual = 0, portable = 0, nonzero = 0, zero = 0;
    uint64_t hash = 0;
    bool known = false;
    aurora::gl::ProgramPtr program;
};
struct PreparedMaterial {
    ShaderConfig config;
    ShaderInfo info;
    mh3_material_requirements requirements{};
    uint64_t configHash = 0;
    bool configHashKnown = false;
    BaseSuffix baseSuffix;
    ModelSuffix modelSuffix;
};
PreparedMaterial unprepared;
bool preparedKnown = false;
bool preparedConfigKnown = false, preparedAdmissionPending = false;
uint64_t preparedRevision = 0;
struct BasePreparation {
    mh3_native_material_state source;
    PreparedMaterial state;
    uint64_t filter = 0, revision = 0, lastUse = 0;
};
constexpr size_t BasePreparationCapacity = 64;
std::vector<BasePreparation> basePreparations;
size_t activePreparation = BasePreparationCapacity;
uint64_t preparationClock = 0, nextPreparationRevision = 0;
PreparedMaterial& ActivePrepared() {
    return activePreparation < basePreparations.size() ? basePreparations[activePreparation].state : unprepared;
}
struct ModelVariantKey {
    uint64_t baseRevision;
    uint16_t attributes, matrixIndices;
    uint8_t texcoords;
    std::array<std::array<uint8_t, 7>, 8> recipes{};
    bool operator==(const ModelVariantKey&) const = default;
};
struct ModelVariant {
    ModelVariantKey key;
    PreparedMaterial state;
};
constexpr size_t ModelVariantCapacity = 64;
static_assert(sizeof(BasePreparation) * BasePreparationCapacity + sizeof(ModelVariant) * ModelVariantCapacity < 768 * 1024);
std::vector<ModelVariant> modelVariants;
size_t modelVariantNext = 0;
struct PreparationCounters {
    uint64_t needs = 0, dirtyLookups = 0, baseHits = 0, fullPreparations = 0, baseEvictions = 0;
    uint64_t modelHits = 0, modelMisses = 0, configHashes = 0, suffixHashes = 0;
} preparationCounters;
struct ProgramCounters {
    uint64_t baseMemoHits = 0, modelMemoHits = 0, baseLookups = 0, modelLookups = 0, configComparisons = 0, suffixReplacements = 0;
} programCounters;
bool rasterObserverInstalled = false, publishingMaterialRaster = false;
struct ProgramKey {
    ShaderConfig config;
    uint8_t zeroBias;
    uint8_t portableZeroBias = 0;
};
struct ModelProgramKey {
    ProgramKey material;
    std::array<uint32_t, 17> layout{};
    uint8_t nonzeroProjection = 0;
    uint8_t zeroProjection = 0;
};
// Keep the captured key ABI. Only cached entries own a configuration copy;
// transparent lookup views refer to the finalized preparation's live bytes.
template <typename Key>
struct HashedProgramKey {
    Key key;
    uint64_t hash;
};
struct ProgramLookup {
    const ShaderConfig& config;
    uint8_t zeroBias, portableZeroBias;
    uint64_t hash;
};
struct ModelProgramLookup {
    ProgramLookup material;
    const std::array<uint32_t, 17>& layout;
    uint8_t nonzeroProjection, zeroProjection;
    uint64_t hash;
};
uint64_t ConfigHash(const ShaderConfig& config) {
    static_assert(std::has_unique_object_representations_v<ShaderConfig>);
    return XXH3_64bits(&config, sizeof config);
}
uint64_t ConfigDigest(PreparedMaterial& state) {
    if (!state.configHashKnown) {
        ++preparationCounters.configHashes;
        state.configHash = ConfigHash(state.config);
        state.configHashKnown = true;
    }
    return state.configHash;
}
ProgramLookup Lookup(const ShaderConfig& config, uint64_t configHash, uint8_t zeroBias, uint8_t portableZeroBias) {
    const std::array<uint8_t, 2> suffix{zeroBias, portableZeroBias};
    return {config, zeroBias, portableZeroBias, XXH3_64bits_withSeed(suffix.data(), suffix.size(), configHash)};
}
ModelProgramLookup Lookup(const ShaderConfig& config, uint64_t configHash, uint8_t zeroBias, uint8_t portableZeroBias, const std::array<uint32_t, 17>& layout, uint8_t nonzeroProjection,
                          uint8_t zeroProjection) {
    std::array<uint32_t, 18> suffix;
    for (unsigned i = 0; i < layout.size(); ++i)
        suffix[i] = layout[i];
    suffix[17] = uint32_t(zeroBias) | (uint32_t(portableZeroBias) << 8) | (uint32_t(nonzeroProjection) << 16) | (uint32_t(zeroProjection) << 24);
    const uint64_t hash = XXH3_64bits_withSeed(suffix.data(), sizeof suffix, configHash);
    return {{config, zeroBias, portableZeroBias, 0}, layout, nonzeroProjection, zeroProjection, hash};
}
ProgramLookup MaterialLookup(PreparedMaterial& state, uint8_t actual, uint8_t portable) {
    auto& suffix = state.baseSuffix;
    if (!suffix.known || suffix.actual != actual || suffix.portable != portable) {
        ++preparationCounters.suffixHashes;
        if (suffix.program)
            ++programCounters.suffixReplacements;
        suffix = {actual, portable, Lookup(state.config, ConfigDigest(state), actual, portable).hash, true};
    }
    return {state.config, actual, portable, suffix.hash};
}
ModelProgramLookup ModelLookup(PreparedMaterial& state, uint8_t actual, uint8_t portable, const std::array<uint32_t, 17>& layout, uint8_t nonzero, uint8_t zero) {
    auto& suffix = state.modelSuffix;
    if (!suffix.known || suffix.layout != layout || suffix.actual != actual || suffix.portable != portable || suffix.nonzero != nonzero || suffix.zero != zero) {
        ++preparationCounters.suffixHashes;
        if (suffix.program)
            ++programCounters.suffixReplacements;
        suffix = {layout, actual, portable, nonzero, zero, Lookup(state.config, ConfigDigest(state), actual, portable, layout, nonzero, zero).hash, true};
    }
    return {{state.config, actual, portable, 0}, layout, nonzero, zero, suffix.hash};
}
struct ProgramHash {
    using is_transparent = void;
    template <typename Key>
    size_t operator()(const Key& key) const noexcept {
        return size_t(key.hash);
    }
};
bool Equal(const ProgramKey& key, const ProgramLookup& view) {
    if (key.zeroBias != view.zeroBias || key.portableZeroBias != view.portableZeroBias)
        return false;
    ++programCounters.configComparisons;
    return std::memcmp(&key.config, &view.config, sizeof key.config) == 0;
}
struct ProgramEqual {
    using is_transparent = void;
    bool operator()(const HashedProgramKey<ProgramKey>& a, const ProgramLookup& b) const { return a.hash == b.hash && Equal(a.key, b); }
    bool operator()(const ProgramLookup& a, const HashedProgramKey<ProgramKey>& b) const { return b.hash == a.hash && Equal(b.key, a); }
    bool operator()(const HashedProgramKey<ProgramKey>& a, const HashedProgramKey<ProgramKey>& b) const {
        return a.hash == b.hash && Equal(a.key, {b.key.config, b.key.zeroBias, b.key.portableZeroBias, b.hash});
    }
};
bool Equal(const ModelProgramKey& key, const ModelProgramLookup& view) {
    return key.layout == view.layout && key.nonzeroProjection == view.nonzeroProjection && key.zeroProjection == view.zeroProjection && Equal(key.material, view.material);
}
struct ModelProgramEqual {
    using is_transparent = void;
    bool operator()(const HashedProgramKey<ModelProgramKey>& a, const ModelProgramLookup& b) const { return a.hash == b.hash && Equal(a.key, b); }
    bool operator()(const ModelProgramLookup& a, const HashedProgramKey<ModelProgramKey>& b) const { return b.hash == a.hash && Equal(b.key, a); }
    bool operator()(const HashedProgramKey<ModelProgramKey>& a, const HashedProgramKey<ModelProgramKey>& b) const {
        const auto& k = b.key;
        return a.hash == b.hash && Equal(a.key, {{k.material.config, k.material.zeroBias, k.material.portableZeroBias, 0}, k.layout, k.nonzeroProjection, k.zeroProjection, b.hash});
    }
};
// Hash only the typed CPU shader configuration and exact small key suffixes.
// The digest indexes candidates; complete equality remains collision safe.
std::unordered_map<HashedProgramKey<ProgramKey>, aurora::gl::ProgramPtr, ProgramHash, ProgramEqual> programs;
std::unordered_map<HashedProgramKey<ModelProgramKey>, aurora::gl::ProgramPtr, ProgramHash, ModelProgramEqual> modelPrograms;
template <typename Factory>
const aurora::gl::ProgramPtr& MaterialProgram(PreparedMaterial& state, const ProgramLookup& lookup, Factory&& create) {
    // MaterialLookup updated this exact suffix immediately before selection.
    // A retained pointer belongs to this immutable config and suffix; no map
    // candidate or hash-only identity decision is made on this path.
    auto& memo = state.baseSuffix.program;
    if (memo) {
        ++programCounters.baseMemoHits;
        return memo;
    }
    ++programCounters.baseLookups;
    auto entry = programs.find(lookup);
    if (entry == programs.end())
        entry = programs.emplace(HashedProgramKey<ProgramKey>{{state.config, lookup.zeroBias, lookup.portableZeroBias}, lookup.hash}, create()).first;
    memo = entry->second;
    return memo;
}
template <typename Factory>
const aurora::gl::ProgramPtr& ModelProgram(PreparedMaterial& state, const ModelProgramLookup& lookup, Factory&& create) {
    auto& memo = state.modelSuffix.program;
    if (memo) {
        ++programCounters.modelMemoHits;
        return memo;
    }
    ++programCounters.modelLookups;
    auto entry = modelPrograms.find(lookup);
    if (entry == modelPrograms.end())
        entry =
            modelPrograms
                .emplace(HashedProgramKey<ModelProgramKey>{{{state.config, lookup.material.zeroBias, lookup.material.portableZeroBias}, lookup.layout, lookup.nonzeroProjection, lookup.zeroProjection},
                                                           lookup.hash},
                         create())
                .first;
    memo = entry->second;
    return memo;
}
struct PortableCapture {
    uint8_t zeroBias = 0;
    bool active = false;
    std::array<aurora::NativeTexturePortableSampling, 8> routes{};
};
PortableCapture PortableSampling(const ShaderInfo& info, uint8_t actualZeroBias) {
    PortableCapture result;
    result.zeroBias = actualZeroBias;
#if defined(MH3_DEVELOPMENT) && !defined(AURORA_GXM)
    if (aurora::gfx::port_capture::enabled() && info.sampledTextures.any()) {
        result.zeroBias = 0;
        result.active = true;
        for (unsigned i = 0; i < 8; ++i)
            if (info.sampledTextures.test(i)) {
                const auto route = aurora::native_texture_portable_sampling(i);
                result.routes[i] = route;
                if (route.shader_bias == 0)
                    result.zeroBias |= 1u << i;
            }
    }
#endif
    return result;
}
std::string PortableOrigin(const PortableCapture& capture, const ShaderInfo& info) {
    const auto biasText = [](float bias) {
        char text[32];
        std::snprintf(text, sizeof text, "%.9g", double(bias));
        return std::string(text);
    };
    std::string result = "GXM zero_bias_mask=" + std::to_string(capture.zeroBias);
    for (unsigned i = 0; i < 8; ++i)
        if (info.sampledTextures.test(i)) {
            const auto& route = capture.routes[i];
            result += " unit=" + std::to_string(i) + " asset=" + std::to_string(route.asset) + " generation=" + std::to_string(route.generation) +
                      " palette_generation=" + std::to_string(route.palette_generation) + " dimensions=" + std::to_string(route.width) + "x" + std::to_string(route.height) +
                      " mips=" + std::to_string(route.mips) + " format=" + std::to_string(route.format) + " min=" + std::to_string(route.min_filter) + " mag=" + std::to_string(route.mag_filter) +
                      " anisotropy=" + std::to_string(route.anisotropy) + " authored_bias=" + biasText(route.authored_bias) + " shader_bias=" + biasText(route.shader_bias) +
                      " sampler_bias=" + biasText(route.sampler_bias) + ";";
        }
    return result;
}
void Owner() {
    if (!aurora::gl::on_render_thread())
        throw std::runtime_error("Native material changed outside graphics owner");
}
void Check(mh3_res_status status, const char* operation) {
    if (status != MH3_RES_OK)
        throw std::runtime_error(std::string(operation) + ": status " + std::to_string(status));
}
void Validate(const mh3_material_requirements* requirements, mh3_material_requirement_proof& proof) {
#ifndef NDEBUG
    mh3_material_diagnostic diagnostic{};
    // This proof lives only inside one owner-only immutable preparation.
    // Each discovery boundary checks newly consumed fields before reading them.
    const auto status = requirements ? mh3_material_state_require_step(&proof, &material, requirements, &diagnostic) : mh3_material_state_require_begin(&proof, &material, &diagnostic);
    assert(status == MH3_RES_OK);
#else
    (void)requirements;
    (void)proof;
#endif
}
void RequireCapabilities(const mh3_material_requirements& requirements) {
    const auto unsupported = [](mh3_material_group group, uint64_t fields) {
        throw std::runtime_error("Native material status " + std::to_string(MH3_RES_UNSUPPORTED) + ", group " + std::to_string(group) + ", index 0, fields " + std::to_string(fields));
    };
    if (material.clip_mode_known && material.clip_disabled && !(requirements.supported_features & MH3_MATERIAL_SUPPORT_CLIP_DISABLED))
        unsupported(MH3_MATERIAL_CLIP, 1);
    constexpr unsigned fields[]{MH3_PIXEL_DITHER, MH3_PIXEL_DESTINATION_ALPHA_ENABLE, MH3_PIXEL_DEPTH_TEXTURE_OPERATION};
    constexpr uint32_t capabilities[]{MH3_MATERIAL_SUPPORT_DITHER, MH3_MATERIAL_SUPPORT_DESTINATION_ALPHA, MH3_MATERIAL_SUPPORT_DEPTH_TEXTURE};
    for (unsigned i = 0; i < 3; ++i)
        if ((material.pixel.known & Bit(fields[i])) && material.pixel.values[fields[i]] && !(requirements.supported_features & capabilities[i]))
            unsupported(MH3_MATERIAL_PIXEL, Bit(fields[i]));
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
bool ColorUses(const mh3_pack_tev_stage& stage, uint8_t arg) {
    return stage.color.a == arg || stage.color.b == arg || stage.color.c == arg || stage.color.d == arg;
}
bool AlphaUses(const mh3_pack_tev_stage& stage, uint8_t arg) {
    return stage.alpha.a == arg || stage.alpha.b == arg || stage.alpha.c == arg || stage.alpha.d == arg;
}
uint64_t PassFields(const mh3_pack_tev_pass& pass, unsigned base) {
    // Comparisons do not consume the regular equation's bias and scale.
    uint64_t mask = (UINT64_C(511) << base);
    if (pass.operation >= 8)
        mask &= ~(Bit(base + 5) | Bit(base + 6));
    return mask;
}
void ColorPass(const mh3_pack_tev_pass& from, aurora::gx::TevStage& to) {
    to.colorPass = {GXTevColorArg(from.a), GXTevColorArg(from.b), GXTevColorArg(from.c), GXTevColorArg(from.d)};
    to.colorOp.op = GXTevOp(from.operation);
    to.colorOp.clamp = from.clamp;
    to.colorOp.outReg = GXTevRegID(from.output);
    if (from.operation <= 1) {
        to.colorOp.bias = GXTevBias(from.bias);
        to.colorOp.scale = GXTevScale(from.scale);
    }
}
void AlphaPass(const mh3_pack_tev_pass& from, aurora::gx::TevStage& to) {
    to.alphaPass = {GXTevAlphaArg(from.a), GXTevAlphaArg(from.b), GXTevAlphaArg(from.c), GXTevAlphaArg(from.d)};
    to.alphaOp.op = GXTevOp(from.operation);
    to.alphaOp.clamp = from.clamp;
    to.alphaOp.outReg = GXTevRegID(from.output);
    if (from.operation <= 1) {
        to.alphaOp.bias = GXTevBias(from.bias);
        to.alphaOp.scale = GXTevScale(from.scale);
    }
}
void KonstColor(uint8_t selection, mh3_material_requirements& requirements) {
    if (selection >= 12 && selection < 16)
        requirements.konst[selection - 12] |= 7;
    else if (selection >= 16)
        requirements.konst[(selection - 16) % 4] |= 1u << ((selection - 16) / 4);
}
void KonstAlpha(uint8_t selection, mh3_material_requirements& requirements) {
    if (selection >= 16)
        requirements.konst[(selection - 16) % 4] |= 1u << ((selection - 16) / 4);
}
unsigned Channel(uint8_t semantic) {
    return semantic == 1 || semantic == 3 || semantic == 5 ? 1 : 0;
}
void ChannelConfig(const mh3_pack_channel_control& source, aurora::gx::ColorChannelConfig& out) {
    out.matSrc = GXColorSrc(source.material_source);
    out.ambSrc = GXColorSrc(source.ambient_source);
    out.lightingEnabled = source.lighting;
    out.diffFn = GXDiffuseFn(source.diffuse);
    out.attnFn = GXAttnFn(source.attenuation);
}
PreparedMaterial PrepareMaterial() {
    mh3_material_requirement_proof proof{};
    RequireCapabilities({});
    Validate(nullptr, proof);
    assert(material.tev_count);
    assert(material.fog_reference < 0 || fogKnown);
    PreparedMaterial result;
    auto& config = result.config;
    auto& requirements = result.requirements;
    config.vtxStride = sizeof(AuroraNativeMaterialVertex);
    config.attrs[GX_VA_POS] = {GX_DIRECT, 3, GX_F32};
    config.attrs[GX_VA_NRM] = {GX_DIRECT, 3, GX_F32};
    config.attrs[GX_VA_CLR0] = {GX_DIRECT, 4, GX_RGBA8};
    config.attrs[GX_VA_CLR1] = {GX_DIRECT, 4, GX_RGBA8};
    for (unsigned i = 0; i < 8; ++i)
        config.attrs[GX_VA_TEX0 + i] = {GX_DIRECT, 2, GX_F32};
    config.tevStageCount = material.tev_count;
    config.numTexGens = material.texgen_count;
    config.numIndStages = material.indirect_count;
    requirements.pixel = AlphaFields;
    // An unknown operation must be diagnosed before it can narrow dependencies.
    for (unsigned i = 0; i < material.tev_count; ++i)
        requirements.tev[i] = Bit(MH3_TEV_COLOR_OPERATION) | Bit(MH3_TEV_ALPHA_OPERATION);
    Validate(&requirements, proof);
    for (unsigned i = 0; i < material.tev_count; ++i) {
        requirements.tev[i] |= PassFields(material.tev[i].color, 0) | PassFields(material.tev[i].alpha, 9);
    }
    Validate(&requirements, proof);
    bool anyTexture = false;
    std::array<uint8_t, 2> usedChannelComponents{};
    for (unsigned i = 0; i < material.tev_count; ++i) {
        const auto& source = material.tev[i];
        auto& stage = config.tevStages[i];
        ColorPass(source.color, stage);
        AlphaPass(source.alpha, stage);
        const bool textureRgb = ColorUses(source, 8), textureAlpha = ColorUses(source, 9) || AlphaUses(source, 4);
        const bool rasterRgb = ColorUses(source, 10), rasterAlpha = ColorUses(source, 11) || AlphaUses(source, 5);
        if (textureRgb || textureAlpha)
            requirements.tev[i] |= Bit(MH3_TEV_TEXTURE_MAP);
        if (rasterRgb || rasterAlpha)
            requirements.tev[i] |= Bit(MH3_TEV_RASTER_CHANNEL);
        if (ColorUses(source, 14))
            requirements.tev[i] |= Bit(MH3_TEV_KONST_COLOR);
        if (AlphaUses(source, 6))
            requirements.tev[i] |= Bit(MH3_TEV_KONST_ALPHA);
    }
    Validate(&requirements, proof);
    for (unsigned i = 0; i < material.tev_count; ++i) {
        const auto& source = material.tev[i];
        if ((ColorUses(source, 8) || ColorUses(source, 9) || AlphaUses(source, 4)) && source.texture_map != 255 && material.texgen_count)
            anyTexture = true;
    }
    for (unsigned i = 0; i < material.tev_count; ++i) {
        const auto& source = material.tev[i];
        auto& stage = config.tevStages[i];
        const bool textureRgb = ColorUses(source, 8), textureAlpha = ColorUses(source, 9) || AlphaUses(source, 4);
        const bool rasterRgb = ColorUses(source, 10), rasterAlpha = ColorUses(source, 11) || AlphaUses(source, 5);
        if (textureRgb || textureAlpha) {
            stage.texMapId = GXTexMapID(source.texture_map);
            if (source.texture_map != 255 && material.texgen_count) {
                requirements.tev[i] |= Bit(MH3_TEV_TEXTURE_COORDINATE) | Bit(MH3_TEV_TEXTURE_SWAP);
            }
        }
        if (rasterRgb || rasterAlpha) {
            stage.channelId = GXChannelID(source.raster_channel);
            if (source.raster_channel <= 5)
                requirements.tev[i] |= Bit(MH3_TEV_RASTER_SWAP);
            if (source.raster_channel == 7 || source.raster_channel == 8)
                requirements.tev[i] |= Bit(MH3_TEV_INDIRECT_ALPHA);
        }
        if (ColorUses(source, 14)) {
            stage.kcSel = GXTevKColorSel(source.konst_color);
            KonstColor(source.konst_color, requirements);
        }
        if (AlphaUses(source, 6)) {
            stage.kaSel = GXTevKAlphaSel(source.konst_alpha);
            KonstAlpha(source.konst_alpha, requirements);
        }
        if (anyTexture)
            requirements.tev[i] |= Bit(MH3_TEV_INDIRECT_MATRIX) | Bit(MH3_TEV_INDIRECT_WRAP_S) | Bit(MH3_TEV_INDIRECT_WRAP_T) | Bit(MH3_TEV_INDIRECT_ADD_PREVIOUS);
    }
    Validate(&requirements, proof);
    for (unsigned i = 0; i < material.tev_count; ++i) {
        const auto& source = material.tev[i];
        auto& stage = config.tevStages[i];
        const bool textureRgb = ColorUses(source, 8), textureAlpha = ColorUses(source, 9) || AlphaUses(source, 4);
        const bool rasterRgb = ColorUses(source, 10), rasterAlpha = ColorUses(source, 11) || AlphaUses(source, 5);
        if (anyTexture) {
            stage.indTexMtxId = GXIndTexMtxID(source.indirect_matrix);
            stage.indTexWrapS = GXIndTexWrap(source.indirect_wrap_s);
            stage.indTexWrapT = GXIndTexWrap(source.indirect_wrap_t);
            stage.indTexAddPrev = source.indirect_add_previous;
        }
        if ((textureRgb || textureAlpha) && source.texture_map != 255 && material.texgen_count) {
            stage.texCoordId = GXTexCoordID(source.texture_coordinate);
            stage.tevSwapTex = GXTevSwapSel(source.texture_swap);
            requirements.swaps[source.texture_swap] |= (textureRgb ? 7u : 0u) | (textureAlpha ? 8u : 0u);
        }
        if ((rasterRgb || rasterAlpha) && source.raster_channel <= 5) {
            stage.tevSwapRas = GXTevSwapSel(source.raster_swap);
            requirements.swaps[source.raster_swap] |= (rasterRgb ? 7u : 0u) | (rasterAlpha ? 8u : 0u);
        }
        if (source.raster_channel == 7 || source.raster_channel == 8) {
            if (rasterRgb || rasterAlpha) {
                stage.indTexAlphaSel = GXIndTexAlphaSel(source.indirect_alpha);
                if (source.indirect_alpha)
                    requirements.tev[i] |= Bit(MH3_TEV_INDIRECT_STAGE) | Bit(MH3_TEV_INDIRECT_FORMAT);
                else
                    stage.channelId = GX_COLOR_ZERO;
            }
        }
        if (stage.indTexMtxId != GX_ITM_OFF)
            requirements.tev[i] |= Bit(MH3_TEV_INDIRECT_STAGE);
    }
    Validate(&requirements, proof);
    for (unsigned i = 0; i < material.tev_count; ++i) {
        const auto& source = material.tev[i];
        auto& stage = config.tevStages[i];
        if (requirements.tev[i] & Bit(MH3_TEV_INDIRECT_STAGE))
            stage.indTexStage = GXIndTexStageID(source.indirect_stage);
        const bool bump = stage.channelId == GX_ALPHA_BUMP || stage.channelId == GX_ALPHA_BUMPN;
        if ((stage.indTexMtxId != GX_ITM_OFF || (bump && stage.indTexAlphaSel != GX_ITBA_OFF)) && stage.indTexStage < material.indirect_count) {
            requirements.tev[i] |= Bit(MH3_TEV_INDIRECT_FORMAT) | Bit(MH3_TEV_INDIRECT_BIAS) | Bit(MH3_TEV_INDIRECT_ALPHA) | Bit(MH3_TEV_INDIRECT_ORIGINAL_LOD);
            requirements.indirect[stage.indTexStage] = 15;
        }
        if (aurora::gx::tev_stage_needs_fixed_texcoord_state(config, i) && material.texgen_count)
            requirements.tev[i] |= Bit(MH3_TEV_TEXTURE_COORDINATE);
    }
    Validate(&requirements, proof);
    for (unsigned i = 0; i < material.tev_count; ++i) {
        const auto& source = material.tev[i];
        auto& stage = config.tevStages[i];
        if (requirements.tev[i] & Bit(MH3_TEV_TEXTURE_COORDINATE))
            stage.texCoordId = GXTexCoordID(source.texture_coordinate);
        if (requirements.tev[i] & Bit(MH3_TEV_INDIRECT_FORMAT)) {
            stage.indTexFormat = GXIndTexFormat(source.indirect_format);
            stage.indTexAlphaSel = GXIndTexAlphaSel(source.indirect_alpha);
            if (requirements.tev[i] & Bit(MH3_TEV_INDIRECT_BIAS))
                stage.indTexBiasSel = GXIndTexBiasSel(source.indirect_bias);
            if (requirements.tev[i] & Bit(MH3_TEV_INDIRECT_ORIGINAL_LOD))
                stage.indTexUseOrigLOD = source.indirect_original_lod;
        }
        if (ColorUses(source, 10) || ColorUses(source, 11) || AlphaUses(source, 5))
            if (source.raster_channel <= 5) {
                const unsigned channel = Channel(source.raster_channel), swap = source.raster_swap;
                const uint32_t outputs = (ColorUses(source, 10) ? 7u : 0u) | ((ColorUses(source, 11) || AlphaUses(source, 5)) ? 8u : 0u);
                for (unsigned component = 0; component < 4; ++component)
                    if (outputs & (1u << component)) {
                        const uint8_t input = material.swaps[swap].rgba[component];
                        usedChannelComponents[channel] |= input == 3 ? 8 : 7;
                    }
            }
    }
    for (unsigned swap = 0; swap < 4; ++swap) {
        auto& target = config.tevSwapTable[swap];
        const auto& source = material.swaps[swap];
        // Unconsumed mappings stay canonical and do not read unknown bytes.
        if (requirements.swaps[swap] & 1)
            target.red = GXTevColorChan(source.rgba[0]);
        if (requirements.swaps[swap] & 2)
            target.green = GXTevColorChan(source.rgba[1]);
        if (requirements.swaps[swap] & 4)
            target.blue = GXTevColorChan(source.rgba[2]);
        if (requirements.swaps[swap] & 8)
            target.alpha = GXTevColorChan(source.rgba[3]);
    }
    for (unsigned i = 0; i < 4; ++i)
        if (requirements.indirect[i]) {
            const auto& source = material.indirect[i];
            if (source.map == 255)
                throw std::runtime_error("Native indirect sampling has a null texture map");
            config.indStages[i] = {GXTexCoordID(source.coordinate), GXTexMapID(source.map), GXIndTexScale(source.scale_s), GXIndTexScale(source.scale_t)};
        }
    for (unsigned i = 0; i < 2; ++i) {
        // Native vertices contain both channels. A component never consumed by
        // the equation uses its vertex input to avoid inventing register colors.
        config.colorChannels[i].matSrc = GX_SRC_VTX;
        config.colorChannels[GX_ALPHA0 + i].matSrc = GX_SRC_VTX;
        if (usedChannelComponents[i] & 7)
            requirements.channel[i] |= MH3_CHANNEL_CONTROL_COLOR;
        if (usedChannelComponents[i] & 8)
            requirements.channel[i] |= MH3_CHANNEL_CONTROL_ALPHA;
    }
    Validate(&requirements, proof);
    for (unsigned i = 0; i < 2; ++i) {
        const auto& source = material.channel[i];
        if (usedChannelComponents[i] & 7) {
            ChannelConfig(source.color, config.colorChannels[i]);
            if (!source.color.material_source)
                requirements.channel[i] |= MH3_CHANNEL_MATERIAL_COLOR;
            if (source.color.lighting && !source.color.ambient_source)
                requirements.channel[i] |= MH3_CHANNEL_AMBIENT_COLOR;
        }
        if (usedChannelComponents[i] & 8) {
            ChannelConfig(source.alpha, config.colorChannels[GX_ALPHA0 + i]);
            if (!source.alpha.material_source)
                requirements.channel[i] |= MH3_CHANNEL_MATERIAL_ALPHA;
            if (source.alpha.lighting && !source.alpha.ambient_source)
                requirements.channel[i] |= MH3_CHANNEL_AMBIENT_ALPHA;
        }
    }
    const auto& pixel = material.pixel.values;
    config.alphaCompare = {GXCompare(pixel[MH3_PIXEL_ALPHA_COMPARE0]), 0, GXAlphaOp(pixel[MH3_PIXEL_ALPHA_OPERATION]), GXCompare(pixel[MH3_PIXEL_ALPHA_COMPARE1]), 0};
    if (pixel[MH3_PIXEL_ALPHA_COMPARE0] != 0 && pixel[MH3_PIXEL_ALPHA_COMPARE0] != 7)
        requirements.pixel |= Bit(MH3_PIXEL_ALPHA_REFERENCE0);
    if (pixel[MH3_PIXEL_ALPHA_COMPARE1] != 0 && pixel[MH3_PIXEL_ALPHA_COMPARE1] != 7)
        requirements.pixel |= Bit(MH3_PIXEL_ALPHA_REFERENCE1);
    Validate(&requirements, proof);
    if (requirements.pixel & Bit(MH3_PIXEL_ALPHA_REFERENCE0))
        config.alphaCompare.ref0 = pixel[MH3_PIXEL_ALPHA_REFERENCE0];
    if (requirements.pixel & Bit(MH3_PIXEL_ALPHA_REFERENCE1))
        config.alphaCompare.ref1 = pixel[MH3_PIXEL_ALPHA_REFERENCE1];
    if (fogKnown) {
        config.fogType = fogSource.type;
        config.fogRangeAdjust = fogSource.range_enabled;
    }
    // Native STQ is final, so no generated texgen or postmatrix can be consumed.
    result.info = aurora::gx::build_shader_info(config);
    for (unsigned i = 0; i < 8; ++i)
        if (result.info.sampledTexCoords.test(i))
            requirements.texgen[i] = 1u << MH3_TEXGEN_PROJECTION;
    for (unsigned i = 0; i < 4; ++i) {
        if (result.info.loadsTevRegRgb.test(i))
            requirements.color[i] |= 7;
        if (result.info.loadsTevRegAlpha.test(i))
            requirements.color[i] |= 8;
    }
    for (unsigned i = 0; i < material.tev_count; ++i) {
        const auto& stage = config.tevStages[i];
        const unsigned selection = stage.indTexMtxId;
        if (stage.indTexStage >= config.numIndStages)
            continue;
        if (selection >= 1 && selection <= 3)
            requirements.indirect_matrix[selection - 1] |= 127;
        else if (selection >= 5 && selection <= 7)
            requirements.indirect_matrix[selection - 5] |= 64;
        else if (selection >= 9 && selection <= 11)
            requirements.indirect_matrix[selection - 9] |= 64;
    }
    Validate(&requirements, proof);
    for (unsigned i = 0; i < 8; ++i)
        if (result.info.sampledTexCoords.test(i))
            config.tcgs[i].type = material.texgen[i].values[MH3_TEXGEN_PROJECTION] ? GX_TG_MTX3x4 : GX_TG_MTX2x4;
    // Projection changes varying types, but cannot introduce matrix dependencies.
    result.info = aurora::gx::build_shader_info(config);
    return result;
}
// Compare only the previous successful derivation's consumed structural
// fields; uniform values remain live in material and are read by WriteUniforms
// on every admitted draw. Internal producer contracts are DEBUG assertions.
template <typename T>
bool ChangedFields(const T* a, const T* b, uint64_t fields, unsigned count) {
    while (fields) {
        const unsigned i = std::countr_zero(fields);
        if (i >= count)
            break;
        if (a[i] != b[i])
            return true;
        fields &= fields - 1;
    }
    return false;
}
bool SameMaterialStructure(const mh3_native_material_state& source, const mh3_native_material_state& next, const mh3_material_requirements& requirements) {
    if (source.tev_count != next.tev_count || source.texgen_count != next.texgen_count || source.channel_count != next.channel_count || source.indirect_count != next.indirect_count)
        return false;
    static_assert(sizeof(mh3_pack_tev_pass) == 9);
    static_assert(offsetof(mh3_pack_tev_stage, indirect_add_previous) - offsetof(mh3_pack_tev_stage, color) == 33);
    for (unsigned i = 0; i < 16; ++i) {
        if ((next.tev[i].known & requirements.tev[i]) != requirements.tev[i])
            return false;
        if (ChangedFields(reinterpret_cast<const uint8_t*>(&source.tev[i].color), reinterpret_cast<const uint8_t*>(&next.tev[i].color), requirements.tev[i], MH3_TEV_FIELD_COUNT))
            return false;
    }
    for (unsigned i = 0; i < 8; ++i) {
        if ((next.texgen[i].known & requirements.texgen[i]) != requirements.texgen[i])
            return false;
        if (ChangedFields(source.texgen[i].values, next.texgen[i].values, requirements.texgen[i], MH3_TEXGEN_FIELD_COUNT))
            return false;
    }
    for (unsigned i = 0; i < 2; ++i) {
        static_assert(offsetof(mh3_pack_channel_control, light_mask) == 5);
        const uint32_t controls = requirements.channel[i] & (MH3_CHANNEL_CONTROL_COLOR | MH3_CHANNEL_CONTROL_ALPHA);
        if ((next.channel[i].flags & controls) != controls)
            return false;
        if ((requirements.channel[i] & MH3_CHANNEL_CONTROL_COLOR) && std::memcmp(&source.channel[i].color, &next.channel[i].color, 5) != 0)
            return false;
        if ((requirements.channel[i] & MH3_CHANNEL_CONTROL_ALPHA) && std::memcmp(&source.channel[i].alpha, &next.channel[i].alpha, 5) != 0)
            return false;
    }
    for (unsigned i = 0; i < 4; ++i) {
        if ((next.swaps[i].known & requirements.swaps[i]) != requirements.swaps[i] || (next.indirect[i].known & requirements.indirect[i]) != requirements.indirect[i])
            return false;
        if (ChangedFields(source.swaps[i].rgba, next.swaps[i].rgba, requirements.swaps[i], 4))
            return false;
        static_assert(offsetof(mh3_pack_indirect_stage, scale_t) - offsetof(mh3_pack_indirect_stage, coordinate) == 3);
        if (ChangedFields(&source.indirect[i].coordinate, &next.indirect[i].coordinate, requirements.indirect[i], 4))
            return false;
    }
    if ((next.pixel.known & requirements.pixel) != requirements.pixel)
        return false;
    return !ChangedFields(source.pixel.values, next.pixel.values, requirements.pixel, MH3_PIXEL_FIELD_COUNT);
}
bool MaterialOverlayStructureChanged(const mh3_native_material_state& patch, uint32_t top) {
    const auto& used = ActivePrepared().requirements;
    if (((top & MH3_MATERIAL_TOP_TEV_COUNT) && patch.tev_count != material.tev_count) || ((top & MH3_MATERIAL_TOP_TEXGEN_COUNT) && patch.texgen_count != material.texgen_count) ||
        ((top & MH3_MATERIAL_TOP_CHANNEL_COUNT) && patch.channel_count != material.channel_count) || ((top & MH3_MATERIAL_TOP_INDIRECT_COUNT) && patch.indirect_count != material.indirect_count))
        return true;
    for (unsigned i = 0; i < 16; ++i)
        if (ChangedFields(reinterpret_cast<const uint8_t*>(&material.tev[i].color), reinterpret_cast<const uint8_t*>(&patch.tev[i].color), used.tev[i] & patch.tev[i].known, MH3_TEV_FIELD_COUNT))
            return true;
    for (unsigned i = 0; i < 8; ++i)
        if (ChangedFields(material.texgen[i].values, patch.texgen[i].values, used.texgen[i] & patch.texgen[i].known, MH3_TEXGEN_FIELD_COUNT))
            return true;
    for (unsigned i = 0; i < 2; ++i) {
        const uint32_t controls = used.channel[i] & patch.channel[i].flags;
        if ((controls & MH3_CHANNEL_CONTROL_COLOR) && std::memcmp(&material.channel[i].color, &patch.channel[i].color, 5) != 0)
            return true;
        if ((controls & MH3_CHANNEL_CONTROL_ALPHA) && std::memcmp(&material.channel[i].alpha, &patch.channel[i].alpha, 5) != 0)
            return true;
    }
    for (unsigned i = 0; i < 4; ++i) {
        if (ChangedFields(material.swaps[i].rgba, patch.swaps[i].rgba, used.swaps[i] & patch.swaps[i].known, 4))
            return true;
        if (ChangedFields(&material.indirect[i].coordinate, &patch.indirect[i].coordinate, used.indirect[i] & patch.indirect[i].known, 4))
            return true;
    }
    return ChangedFields(material.pixel.values, patch.pixel.values, used.pixel & patch.pixel.known, MH3_PIXEL_FIELD_COUNT);
}
bool SameRequirements(const mh3_material_requirements& a, const mh3_material_requirements& b) {
    // Compare typed dependencies: the C structure has trailing padding, which
    // is not part of a recipe's identity and need not survive a value copy.
    const auto same = [](const auto& x, const auto& y) {
        return std::equal(std::begin(x), std::end(x), std::begin(y));
    };
    return same(a.tev, b.tev) && a.pixel == b.pixel && same(a.texgen, b.texgen) && same(a.channel, b.channel) && same(a.color, b.color) && same(a.konst, b.konst) && same(a.swaps, b.swaps) &&
           same(a.indirect, b.indirect) && same(a.indirect_matrix, b.indirect_matrix) && a.supported_features == b.supported_features && a.require_clip == b.require_clip;
}
void RequirePrepared(const PreparedMaterial& state) {
    RequireCapabilities(state.requirements);
#ifndef NDEBUG
    assert(material.fog_reference < 0 || fogKnown);
    mh3_material_diagnostic diagnostic{};
    const auto status = mh3_material_state_require(&material, &state.requirements, &diagnostic);
    assert(status == MH3_RES_OK);
#endif
}
bool PreparationFilter(uint64_t& filter) {
    // Integer rejection tag only. Never read an unknown equation operation,
    // and never hash an input state merely to search the bounded recipe cache.
    const uint64_t operations = Bit(MH3_TEV_COLOR_OPERATION) | Bit(MH3_TEV_ALPHA_OPERATION);
    if (!material.tev_count || (material.tev[0].known & operations) != operations || (material.pixel.known & AlphaFields) != AlphaFields)
        return false;
    const auto& pixel = material.pixel.values;
    const auto compare0 = pixel[MH3_PIXEL_ALPHA_COMPARE0], compare1 = pixel[MH3_PIXEL_ALPHA_COMPARE1];
    const bool reference0 = compare0 != 0 && compare0 != 7, reference1 = compare1 != 0 && compare1 != 7;
    if ((reference0 && !(material.pixel.known & Bit(MH3_PIXEL_ALPHA_REFERENCE0))) || (reference1 && !(material.pixel.known & Bit(MH3_PIXEL_ALPHA_REFERENCE1))))
        return false;
    filter = uint64_t(material.tev_count) | (uint64_t(material.texgen_count) << 5) | (uint64_t(material.channel_count) << 9) | (uint64_t(material.indirect_count) << 11) |
             (uint64_t(material.tev[0].color.operation) << 14) | (uint64_t(material.tev[0].alpha.operation) << 22) | (uint64_t(fogKnown ? fogSource.type : 0) << 30) |
             (uint64_t(fogKnown && fogSource.range_enabled) << 38) | (uint64_t(fogKnown) << 39) | (uint64_t(compare0) << 40) | (uint64_t(compare1) << 43) |
             (uint64_t(pixel[MH3_PIXEL_ALPHA_OPERATION]) << 46) | (uint64_t(reference0 ? pixel[MH3_PIXEL_ALPHA_REFERENCE0] : 0) << 48) |
             (uint64_t(reference1 ? pixel[MH3_PIXEL_ALPHA_REFERENCE1] : 0) << 56);
    return true;
}
void ActivatePreparation(size_t index) {
    activePreparation = index;
    auto& entry = basePreparations[index];
    entry.lastUse = ++preparationClock;
    preparedRevision = entry.revision;
    preparedKnown = preparedConfigKnown = true;
    preparedAdmissionPending = false;
}
PreparedMaterial& EnsurePreparedMaterial() {
    ++preparationCounters.needs;
    if (!preparedKnown) {
        ++preparationCounters.dirtyLookups;
        uint64_t filter = 0;
        const bool filterKnown = PreparationFilter(filter);
        auto matches = [&](size_t i) {
            const auto& entry = basePreparations[i];
            return filterKnown && entry.filter == filter && SameMaterialStructure(entry.source, material, entry.state.requirements);
        };
        size_t hit = BasePreparationCapacity;
        // Coalesced skipped publications commonly return to the active recipe.
        if (activePreparation < basePreparations.size() && matches(activePreparation))
            hit = activePreparation;
        else
            for (size_t i = 0; i < basePreparations.size(); ++i)
                if (matches(i)) {
                    hit = i;
                    break;
                }
        if (hit != BasePreparationCapacity) {
            RequirePrepared(basePreparations[hit].state);
            ++preparationCounters.baseHits;
            ActivatePreparation(hit);
        } else {
            auto next = PrepareMaterial();
            ++preparationCounters.fullPreparations;
            // Equal canonical outputs can keep the same token and lazy memo,
            // even if an unused count or first-known NONE fog changed the raw
            // readset. Requirements must also agree before variants are reused.
            size_t same = BasePreparationCapacity;
            for (size_t i = 0; i < basePreparations.size(); ++i) {
                const auto& entry = basePreparations[i];
                if (entry.state.config == next.config && SameRequirements(entry.state.requirements, next.requirements)) {
                    same = i;
                    break;
                }
            }
            if (same != BasePreparationCapacity) {
                auto& entry = basePreparations[same];
                entry.source = material;
                entry.filter = filter;
                ActivatePreparation(same);
            } else {
                if (preparedConfigKnown && next.config == ActivePrepared().config) {
                    next.configHash = ActivePrepared().configHash;
                    next.configHashKnown = ActivePrepared().configHashKnown;
                    next.baseSuffix = ActivePrepared().baseSuffix;
                    next.modelSuffix = ActivePrepared().modelSuffix;
                }
                BasePreparation entry{material, next, filter, ++nextPreparationRevision, 0};
                size_t index = basePreparations.size();
                if (basePreparations.capacity() < BasePreparationCapacity)
                    basePreparations.reserve(BasePreparationCapacity);
                if (index < BasePreparationCapacity)
                    basePreparations.push_back(entry);
                else {
                    index = std::min_element(basePreparations.begin(), basePreparations.end(), [](const auto& a, const auto& b) { return a.lastUse < b.lastUse; }) - basePreparations.begin();
                    const auto retired = basePreparations[index].revision;
                    std::erase_if(modelVariants, [&](const auto& variant) { return variant.key.baseRevision == retired; });
                    modelVariantNext %= ModelVariantCapacity;
                    basePreparations[index] = entry;
                    ++preparationCounters.baseEvictions;
                }
                ActivatePreparation(index);
            }
        }
    } else if (preparedAdmissionPending) {
        RequirePrepared(ActivePrepared());
        preparedAdmissionPending = false;
    }
    return ActivePrepared();
}
std::array<uint32_t, 17> ModelLayout(const aurora::gl::MeshLayout& source) {
    std::array<uint32_t, 17> result{};
    result[0] = source.stride;
    result[1] = source.matrixIndexMask;
    for (unsigned i = 0; i < source.attributes.size(); ++i) {
        const auto& a = source.attributes[i];
        result[i + 2] = a.offset | (uint32_t(a.location) << 16) | (uint32_t(a.components) << 20) | (uint32_t(a.format) << 24);
    }
    return result;
}
ModelVariantKey ModelRecipe(const mh3_model_gpu_inputs& inputs, const aurora::gl::MeshLayout& layout) {
    ModelVariantKey key{preparedRevision, 0, inputs.matrix_index_mask, inputs.texcoord_mask, {}};
    for (const auto& attribute : layout.attributes)
        if (attribute.location < 12)
            key.attributes |= 1u << attribute.location;
    const bool absolute = (inputs.matrix_index_mask & 1) || ((inputs.matrix_index_mask >> 1) & inputs.texcoord_mask);
    for (unsigned i = 0; i < 8; ++i)
        if (inputs.texcoord_mask & (1u << i)) {
            const auto& recipe = inputs.texgen[i];
            key.recipes[i] = {recipe.source_row,
                              recipe.projection,
                              recipe.input_ab11,
                              recipe.normalize,
                              recipe.main_identity,
                              recipe.post_identity,
                              uint8_t(absolute && !recipe.main_identity ? recipe.main_slot : 0)};
        }
    return key;
}
PreparedMaterial& ModelPreparation(const mh3_model_gpu_inputs& inputs, const aurora::gl::MeshLayout& layout) {
    const auto key = ModelRecipe(inputs, layout);
    for (auto& variant : modelVariants)
        if (variant.key == key) {
            ++preparationCounters.modelHits;
            return variant.state;
        }
    ++preparationCounters.modelMisses;
    auto state = ActivePrepared();
    auto& config = state.config;
    config.attrs = {};
    config.vtxStride = 0;
    for (unsigned location = 0; location < 12; ++location)
        if (key.attributes & (1u << location)) {
            const GXAttr attr = location == 0 ? GX_VA_POS : location == 1 ? GX_VA_NRM : location < 4 ? GXAttr(GX_VA_CLR0 + location - 2) : GXAttr(GX_VA_TEX0 + location - 4);
            config.attrs[attr] = {GX_DIRECT, uint8_t(location < 2 ? 3 : location < 4 ? 4 : 2), uint8_t(location >= 2 && location < 4 ? GX_RGBA8 : GX_F32)};
        }
    if (inputs.matrix_index_mask & 1)
        config.attrs[GX_VA_PNMTXIDX] = {GX_DIRECT, 1, GX_U8};
    for (unsigned i = 0; i < 8; ++i)
        if (inputs.matrix_index_mask & (2u << i))
            config.attrs[GX_VA_TEX0MTXIDX + i] = {GX_DIRECT, 1, GX_U8};
    const bool absolute = (inputs.matrix_index_mask & 1) || ((inputs.matrix_index_mask >> 1) & inputs.texcoord_mask);
    config.dualTexEnabled = false;
    constexpr GXTexGenSrc source[]{GX_TG_POS, GX_TG_NRM, GX_TG_COLOR0, GX_TG_BINRM, GX_TG_TANGENT, GX_TG_TEX0, GX_TG_TEX1, GX_TG_TEX2, GX_TG_TEX3, GX_TG_TEX4, GX_TG_TEX5, GX_TG_TEX6, GX_TG_TEX7};
    for (unsigned i = 0; i < 8; ++i)
        if (inputs.texcoord_mask & (1u << i)) {
            const auto& recipe = inputs.texgen[i];
            auto& gen = config.tcgs[i];
            gen.src = source[recipe.source_row];
            gen.inputFormAB11 = recipe.input_ab11;
            gen.type = recipe.projection ? GX_TG_MTX3x4 : GX_TG_MTX2x4;
            gen.mtx = recipe.main_identity ? GX_IDENTITY : GXTexMtx(absolute ? 3 * recipe.main_slot : 30 + 3 * i);
            gen.postMtx = recipe.post_identity ? GX_PTIDENTITY : GXPTTexMtx(64 + 3 * i);
            gen.normalize = recipe.normalize;
            if (gen.normalize || !recipe.post_identity)
                config.dualTexEnabled = true;
        }
    state.info = aurora::gx::build_shader_info(config);
    // Only identical bytes can inherit the base digest. Model-only use need
    // never hash the unused base configuration.
    if (!(config == ActivePrepared().config)) {
        state.configHash = 0;
        state.configHashKnown = false;
        state.baseSuffix = {};
        state.modelSuffix = {};
    }
    if (modelVariants.capacity() < ModelVariantCapacity)
        modelVariants.reserve(ModelVariantCapacity);
    if (modelVariants.size() < ModelVariantCapacity) {
        modelVariants.push_back({key, state});
        return modelVariants.back().state;
    }
    auto& victim = modelVariants[modelVariantNext];
    modelVariantNext = (modelVariantNext + 1) % ModelVariantCapacity;
    victim = {key, state};
    return victim.state;
}
void ClearMaterialPreparations() {
    preparedKnown = preparedConfigKnown = preparedAdmissionPending = false;
    basePreparations.clear();
    activePreparation = BasePreparationCapacity;
    preparationClock = nextPreparationRevision = 0;
    modelVariants.clear();
    modelVariantNext = 0;
    preparedRevision = 0;
    // Cumulative owner statistics survive graphics teardown. Retained vector
    // capacities are still included in the reported fixed CPU storage.
}
void ClearMaterialProgramMemos() {
    const auto clear = [](PreparedMaterial& state) {
        state.baseSuffix.program.reset();
        state.modelSuffix.program.reset();
    };
    clear(unprepared);
    for (auto& entry : basePreparations)
        clear(entry.state);
    for (auto& variant : modelVariants)
        clear(variant.state);
}
const mh3_native_texture_binding& Texture(unsigned unit) {
    const auto* binding = GetNativeTextureBinding(unit);
    if (!binding)
        throw std::runtime_error("Native material texture " + std::to_string(unit) + " has no typed producer");
    return *binding;
}
std::array<float, 4> CoordinateScale(unsigned coordinate, const PreparedMaterial& state) {
    if (!aurora::gx::shader_uses_fixed_texcoord_state(state.config) && !state.info.usedIndStages.any())
        return {1, 1, 0, 0};
    std::array<float, 4> result{};
    bool known = false;
    const auto add = [&](unsigned map) {
        const auto& source = Texture(map).source;
        if (known && (result[0] != source.width || result[1] != source.height))
            throw std::runtime_error("Native fixed coordinate requires an explicit scale producer for conflicting texture dimensions");
        result = {float(source.width), float(source.height), 0, 0};
        known = true;
    };
    for (unsigned stage = 0; stage < state.config.tevStageCount; ++stage) {
        const auto dependency = aurora::gx::tev_stage_texture_dependency(state.config, stage);
        if (dependency.texCoordId == int(coordinate) && dependency.texMapId >= 0)
            add(unsigned(dependency.texMapId));
    }
    for (unsigned stage = 0; stage < 4; ++stage)
        if (state.info.usedIndStages.test(stage)) {
            const auto& indirect = state.config.indStages[stage];
            if (aurora::gx::tev_effective_texcoord(state.config, indirect.texCoordId) == int(coordinate))
                add(indirect.texMapId);
        }
    if (!known)
        throw std::runtime_error("Native fixed coordinate scale has no typed texture dimensions");
    return result;
}
struct UniformWriter {
    uint8_t* destination;
    size_t capacity, offset = 0;
    void Bytes(const void* value, size_t size) {
        if (size > capacity - offset)
            throw std::runtime_error("Native material uniform layout overflow");
        std::memcpy(destination + offset, value, size);
        offset += size;
    }
    template <typename T>
    void Value(const T& value) {
        Bytes(&value, sizeof value);
    }
};
std::array<float, 4> Constant(const mh3_pack_tev_color& color, uint32_t consumed) {
    std::array<float, 4> out{};
    for (unsigned component = 0; component < 4; ++component)
        if (consumed & (1u << component))
            out[component] = color.rgba[component] / 255.f;
    return out;
}
std::array<float, 4> ChannelColor(const uint8_t rgba[4], bool alpha) {
    std::array<float, 4> out{};
    if (alpha)
        out[3] = rgba[3] / 255.f;
    else
        for (unsigned component = 0; component < 3; ++component)
            out[component] = rgba[component] / 255.f;
    return out;
}
void WriteUniforms(const PreparedMaterial& state, AuroraNativeUniforms& uniforms, const mh3_model_gpu_inputs* model = nullptr, const mh3_model_texcoord_sizes* sizes = nullptr) {
    const auto& info = state.info;
    const auto& config = state.config;
    // The native vertex shader does not consume the declared GX matrix prefix.
    // Zero unused declarations/padding once; append actual material fields in order.
    std::memset(uniforms.data, 0, uniforms.size);
    if (model) {
        UniformWriter prefix{static_cast<uint8_t*>(uniforms.data), uniforms.size};
        prefix.offset = 8;
        std::array<float, 2> viewport{};
        GetNativeRenderViewportSize(viewport.data());
        prefix.Value(viewport);
        prefix.Value(std::array<float, 2>{GetNativeDrawState().viewport[2], GetNativeDrawState().viewport[3]});
        prefix.offset = 80;
        prefix.Bytes(model->projection, 64);
        for (unsigned i = 0; i < info.matrixLayout.postexCount; ++i) {
            const unsigned slot = info.matrixLayout.postexSlots[i];
            if (slot == aurora::gx::UniformMatrixLayout::kCurrentPnMtx)
                prefix.Value(model->position);
            else if (info.matrixLayout.absolutePosRegion && slot < 10)
                prefix.Value(model->position_palette[slot]);
            else if (info.matrixLayout.absolutePosRegion && slot < 20)
                prefix.Value(model->texture_palette[slot - 10]);
            else if (slot >= 10 && slot < 18 && (model->texcoord_mask & (1u << (slot - 10))))
                prefix.Value(model->texgen[slot - 10].main);
            else
                throw std::runtime_error("Compact model main matrix layout mismatch");
        }
        for (unsigned i = 0; i < info.matrixLayout.nrmCount; ++i) {
            const unsigned slot = info.matrixLayout.nrmSlots[i];
            if (slot == aurora::gx::UniformMatrixLayout::kCurrentPnMtx)
                prefix.Value(model->normal);
            else if (info.matrixLayout.absolutePosRegion && slot < 10)
                prefix.Value(model->normal_palette[slot]);
            else
                throw std::runtime_error("Compact model normal matrix layout mismatch");
        }
    }
    UniformWriter writer{static_cast<uint8_t*>(uniforms.data), uniforms.size, 144 + 48u * (info.matrixLayout.postexCount + info.matrixLayout.nrmCount)};
    for (unsigned i = 0; i < 4; ++i)
        if (info.loadsTevRegRgb.test(i) || info.loadsTevRegAlpha.test(i))
            writer.Value(Constant(material.color[i], state.requirements.color[i]));
    if (info.lightingEnabled) {
        std::array<uint32_t, 4> masks{};
        for (unsigned i = 0; i < 2; ++i) {
            if (config.colorChannels[i].lightingEnabled)
                masks[i] = material.channel[i].color.light_mask;
            if (config.colorChannels[GX_ALPHA0 + i].lightingEnabled)
                masks[GX_ALPHA0 + i] = material.channel[i].alpha.light_mask;
        }
        for (unsigned mask : masks)
            if (mask & ~lightSlots.valid_mask)
                throw std::runtime_error("Native material lighting reads an unpublished light slot");
        writer.Value(preparedLights);
        writer.Value(masks);
    }
    for (unsigned i = 0; i < 2; ++i)
        if (info.sampledColorChannels.test(i)) {
            const auto& source = material.channel[i];
            const auto& rgb = config.colorChannels[i];
            const auto& alpha = config.colorChannels[GX_ALPHA0 + i];
            if (rgb.lightingEnabled && rgb.ambSrc == GX_SRC_REG)
                writer.Value(ChannelColor(source.ambient_color, false));
            if (rgb.matSrc == GX_SRC_REG)
                writer.Value(ChannelColor(source.material_color, false));
            if (alpha.lightingEnabled && alpha.ambSrc == GX_SRC_REG)
                writer.Value(ChannelColor(source.ambient_color, true));
            if (alpha.matSrc == GX_SRC_REG)
                writer.Value(ChannelColor(source.material_color, true));
        }
    for (unsigned i = 0; i < 4; ++i)
        if (info.sampledKColors.test(i))
            writer.Value(Constant(material.konst[i], state.requirements.konst[i]));
    if (info.usesPTTexMtx.any()) {
        if (!model)
            throw std::runtime_error("Native final STQ unexpectedly requires generated postmatrices");
        for (unsigned i = 0; i < aurora::gx::MaxPTTexMtx; ++i) {
            if (info.usesPTTexMtx.test(i)) {
                if (i >= 8 || !(model->texcoord_mask & (1u << i)))
                    throw std::runtime_error("Compact model post matrix layout mismatch");
                writer.Value(model->texgen[i].post);
            } else
                writer.Value(mh3_matrix34{});
        }
    }
    if (info.usesFog) {
        auto fog = preparedFog;
        Check(mh3_material_prepare_fog(&fogSource, GetNativeDrawState().viewport[2], &fog), "Native fog draw preparation");
        writer.Value(fog);
    }
    if (info.usedIndTexMtxs.any())
        for (unsigned i = 0; i < 3; ++i) {
            const auto& matrix = material.indirect_matrix[i];
            const uint32_t used = state.requirements.indirect_matrix[i];
            std::array<int32_t, 8> words{};
            // Source is ordinary row-major 2x3. Shader consumes column pairs.
            if (used & 63)
                words = {matrix.coefficients[0], matrix.coefficients[3], matrix.coefficients[1], matrix.coefficients[4], matrix.coefficients[2], matrix.coefficients[5], 0, 0};
            if (used & 64)
                words[6] = -matrix.scale_exponent;
            writer.Value(words);
        }
    for (unsigned i = 0; i < 8; ++i)
        if (info.sampledTexCoords.test(i)) {
            if (!model)
                writer.Value(CoordinateScale(i, state));
            else if (aurora::gx::shader_uses_fixed_texcoord_state(config) || info.usedIndStages.any()) {
                if (!sizes || !(sizes->known & (1u << i)) || !sizes->width[i] || !sizes->height[i])
                    throw std::runtime_error("Compact model lacks an explicit coordinate scale producer");
                writer.Value(std::array<float, 4>{float(sizes->width[i]), float(sizes->height[i]), 0, 0});
            } else
                writer.Value(std::array<float, 4>{1, 1, 0, 0});
        }
    for (unsigned i = 0; i < 8; ++i)
        if (info.sampledTextures.test(i)) {
            const auto& texture = Texture(i).source;
            writer.Value(std::array<float, 4>{float(texture.width), float(texture.height), aurora::native_texture_shader_lod_bias(i), 0});
        }
    if (writer.offset > uniforms.size)
        throw std::runtime_error("Native material uniform size mismatch");
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
bool ShadedPrograms() {
#ifdef MH3_DEVELOPMENT
    // Transitional A/B reference: MH3_TEV_PROGRAMS=1 keeps the generated TEV path.
    static const bool tev = std::getenv("MH3_TEV_PROGRAMS") != nullptr;
    return !tev;
#else
    return true;
#endif
}
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
std::optional<aurora::gl::ShadedBlocks> ShadeBlocks(const AuroraNativeUniforms& draw, uint8_t model, const AuroraNativeUniforms* palette) {
    aurora::gl::ShadedBlocks blocks;
    if (!ShadeSceneMemory(blocks.sceneRevision))
        return std::nullopt;
    blocks.draw = draw.data;
    blocks.drawBytes = MH3_SHADE_DRAW_BYTES(model);
    blocks.shadeOffset = MH3_SHADE_FRAGMENT_OFFSET;
    blocks.shadeBytes = MH3_SHADE_FRAGMENT_BYTES(model);
    if (palette) {
        blocks.palette = palette->data;
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
    mh3_shade_draw block;
    std::memset(&block, 0, MH3_SHADE_FRAGMENT_OFFSET);
    auto& vertex = block.vertex;
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
    const auto units = ShadeTextures(shading, block.fragment);
    mh3_shade_fill(&shading, &material, &block);
    auto variant = ShadeVariant(shading, NativeShaderVertex::Mesh, lighting);
    std::copy(kinds.begin(), kinds.end(), variant.texgen);
    const GLuint program = ShadeProgram(variant)->get();
    AuroraNativeUniforms drawMemory{}, paletteMemory{};
    if (!aurora_allocate_native_uniforms(MH3_SHADE_DRAW_BYTES(shading.model), &drawMemory) || !aurora_allocate_native_uniforms(sizeof(mh3_shade_palette), &paletteMemory))
        return;
    std::memcpy(drawMemory.data, &block, MH3_SHADE_DRAW_BYTES(shading.model));
    auto& palette = *static_cast<mh3_shade_palette*>(paletteMemory.data);
    std::memcpy(palette.position[0], inputs.position_palette, sizeof inputs.position_palette);
    std::memcpy(palette.position[10], inputs.texture_palette, sizeof inputs.texture_palette);
    std::memcpy(palette.normal[0], inputs.normal_palette, sizeof inputs.normal_palette);
    const auto blocks = ShadeBlocks(drawMemory, shading.model, &paletteMemory);
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
    mh3_shade_draw block;
    std::memset(&block, 0, MH3_SHADE_FRAGMENT_OFFSET);
    const auto lighting = ShadeChannels(shading, block.vertex);
    std::array<uint8_t, 3> coordinates{};
    for (unsigned role = 0; role < shading.textures; ++role)
        coordinates[role] = shading.texture_coordinate[role] < 8 ? shading.texture_coordinate[role] : 0;
    const auto units = ShadeTextures(shading, block.fragment);
    mh3_shade_fill(&shading, &material, &block);
    const GLuint program = ShadeProgram(ShadeVariant(shading, NativeShaderVertex::Geometry, lighting))->get();
    AuroraNativeUniforms drawMemory{};
    if (!aurora_allocate_native_uniforms(MH3_SHADE_DRAW_BYTES(shading.model), &drawMemory))
        return;
    std::memcpy(drawMemory.data, &block, MH3_SHADE_DRAW_BYTES(shading.model));
    const auto blocks = ShadeBlocks(drawMemory, shading.model, nullptr);
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
    const auto* consumed = preparedKnown ? &ActivePrepared().requirements : nullptr;
    if (mh3_material_group_publish_unchecked(&material, &source, kind, count, consumed))
        preparedKnown = false;
    preparedAdmissionPending = true;
}
void SetNativeModelMatrixSelectors(const mh3_model_matrix_selectors& selectors) {
    Owner();
    ObserveRasterPublications();
    const uint16_t changed = mh3_model_matrix_overlay_unchecked(&selectors, &material);
    if (!changed)
        return;
    if (preparedKnown) {
        const auto& consumed = ActivePrepared().requirements.texgen;
        for (unsigned i = 0; i < 8; ++i) {
            if (((changed & (1u << i)) && (consumed[i] & (1u << MH3_TEXGEN_MATRIX_ROW))) || ((changed & (1u << (8 + i))) && (consumed[i] & (1u << MH3_TEXGEN_DUAL_TEXTURE)))) {
                preparedKnown = false;
                break;
            }
        }
    }
    preparedAdmissionPending = true;
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
    const bool structural = preparedKnown && MaterialOverlayStructureChanged(source, top_known);
    struct Export {
        Export() { publishingMaterialRaster = true; }
        ~Export() { publishingMaterialRaster = false; }
    };
    {
        Export scoped;
        SetNativeRaster(raster);
    }
    mh3_material_state_overlay_unchecked(&material, &source, top_known);
    if (structural)
        preparedKnown = false;
    preparedAdmissionPending = true;
}
void SetNativeFog(const mh3_native_fog& source) {
    Owner();
    auto next = preparedFog;
    Check(mh3_material_prepare_fog(&source, GetNativeDrawState().viewport[2], &next), "Native fog producer");
    const bool structural = !fogKnown || fogSource.type != source.type || fogSource.range_enabled != source.range_enabled;
    fogSource = source;
    preparedFog = next;
    fogKnown = true;
    ++shade.revision;
    if (structural)
        preparedKnown = false;
}
void SetNativeModelLights(const mh3_native_light_slots& source) {
    Owner();
    std::array<mh3_shader_light, 8> next{};
    for (unsigned i = 0; i < 8; ++i)
        if (source.valid_mask & (1u << i))
            Check(mh3_material_prepare_light(&source.lights[i], &next[i]), "Native light producer");
    lightSlots = source;
    preparedLights = next;
    ++shade.revision;
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
void SetNativeDepthCompareBeforeTexture(bool before) {
    Owner();
    depthBeforeTexture = before;
}
void SubmitNativeMaterialGeometry(const AuroraNativeMaterialGeometry& geometry) {
    Owner();
    if (!NativeGraphicsFrameShouldRender())
        return;
    if (ShadedPrograms()) {
        SubmitShadedMaterialGeometry(geometry);
        return;
    }
    EnsurePreparedMaterial();
    const auto& raster = GetNativeDrawState().raster;
    if (depthBeforeTexture && raster.depth_test && raster.depth_write && bool(ActivePrepared().config.alphaCompare))
        throw std::runtime_error("Native early depth write with alpha discard is unsupported");
    uint8_t zeroBias = 0;
    for (unsigned i = 0; i < 8; ++i)
        if (ActivePrepared().info.sampledTextures.test(i)) {
            (void)Texture(i);
            if (aurora::native_texture_shader_lod_bias(i) == 0)
                zeroBias |= 1u << i;
        }
    const auto portable = PortableSampling(ActivePrepared().info, zeroBias);
    const auto lookup = MaterialLookup(ActivePrepared(), zeroBias, portable.zeroBias);
    const auto& program = MaterialProgram(ActivePrepared(), lookup, [&] {
        // Always use the user-selected float native equations. The generator's
        // exact mode exists only for independent reference probes.
        auto description = aurora::gx::build_shader_description(ActivePrepared().config, true, zeroBias, true);
        description.origin = "runtime/src/native_material.cpp:SubmitNativeMaterialGeometry";
        if (portable.active) {
            const auto counterpart = aurora::gx::build_shader_description(ActivePrepared().config, true, portable.zeroBias, true);
            description.portableVertex = counterpart.vertex;
            description.portableFragment = counterpart.fragment;
            description.portableOrigin = PortableOrigin(portable, ActivePrepared().info);
        }
        return std::make_shared<aurora::gl::Program>(std::move(description));
    });
    AuroraNativeUniforms uniforms{};
    if (!aurora_allocate_native_uniforms(ActivePrepared().info.uniformSize, &uniforms))
        return;
    WriteUniforms(ActivePrepared(), uniforms);
    aurora_submit_native_material_geometry(&geometry, program->get(), &uniforms);
}
void SubmitNativeModelMesh(const mh3_resolved_mesh_submit& mesh) {
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
    if (ShadedPrograms()) {
        SubmitShadedModelMesh(mesh, resident);
        return;
    }
    EnsurePreparedMaterial();
    auto draw = mesh;
    draw.mesh = *resident.mesh;
    draw.material = &material;
    if (((draw.layout.flags >> MH3_LAYOUT_MATRIX_MASK_SHIFT) & 0x1ffu) != resident.layout->matrixIndexMask)
        throw std::runtime_error("Native model matrix selector layout mismatch");
    mh3_model_gpu_inputs inputs{};
    Check(mh3_model_gpu_prepare(&draw, uint8_t(ActivePrepared().info.sampledTexCoords.to_ulong()), ActivePrepared().info.lightingEnabled, GetNativeDrawState().viewport[4],
                                GetNativeDrawState().viewport[5], &inputs),
          "Native model GPU inputs");
    auto& modelState = ModelPreparation(inputs, *resident.layout);
    const auto& config = modelState.config;
    const auto& raster = GetNativeDrawState().raster;
    if (depthBeforeTexture && raster.depth_test && raster.depth_write && bool(config.alphaCompare))
        throw std::runtime_error("Native early depth write with alpha discard is unsupported");
    uint8_t zeroBias = 0;
    for (unsigned i = 0; i < 8; ++i)
        if (modelState.info.sampledTextures.test(i)) {
            (void)Texture(i);
            if (aurora::native_texture_shader_lod_bias(i) == 0)
                zeroBias |= 1u << i;
        }
    const auto portable = PortableSampling(modelState.info, zeroBias);
    const auto layout = ModelLayout(*resident.layout);
    const auto lookup = ModelLookup(modelState, zeroBias, portable.zeroBias, layout, inputs.nonzero_projective_mask, inputs.zero_projective_mask);
    const auto& program = ModelProgram(modelState, lookup, [&] {
        auto description = aurora::gx::build_shader_description(config, true, zeroBias, true, resident.layout, inputs.nonzero_projective_mask, inputs.zero_projective_mask);
        description.origin =
            "runtime/src/native_material.cpp:SubmitNativeModelMesh asset=" + std::to_string(mesh.asset_id) + " model=" + std::to_string(mesh.model_id) + " shape=" + std::to_string(mesh.shape_id);
        if (portable.active) {
            const auto counterpart = aurora::gx::build_shader_description(config, true, portable.zeroBias, true, resident.layout, inputs.nonzero_projective_mask, inputs.zero_projective_mask);
            description.portableVertex = counterpart.vertex;
            description.portableFragment = counterpart.fragment;
            description.portableOrigin = PortableOrigin(portable, modelState.info);
        }
        return std::make_shared<aurora::gl::Program>(std::move(description));
    });
    AuroraNativeUniforms uniforms{};
    if (!aurora_allocate_native_uniforms(modelState.info.uniformSize, &uniforms))
        return;
    WriteUniforms(modelState, uniforms, &inputs, mesh.texcoord_sizes);
    const auto programID = program->get();
    aurora::gl::draw_native_mesh(*resident.buffer, *resident.layout, resident.mesh->vertex_offset, resident.mesh->vertex_count, resident.mesh->index_offset + mesh.first_index * 2, mesh.index_count,
                                 programID, uniforms);
#ifdef MH3_DEVELOPMENT
    if (aurora::gfx::port_capture::enabled())
        aurora::gfx::port_capture::native_mesh_source({aurora::gfx::current_frame(), programID, mesh.asset_id, mesh.model_id, mesh.shape_id, mesh.mesh_index, mesh.first_index, mesh.index_count,
                                                       mesh.resident_generation, mesh.source_generation, mesh.pose_generation, material.generation, mesh.layout.flags});
#endif
}
void GetNativeMaterialPreparationStatistics(uint64_t statistics[12]) {
    Owner();
    if (!statistics)
        return;
    const std::array<uint64_t, 12> values{preparationCounters.needs,
                                          preparationCounters.dirtyLookups,
                                          preparationCounters.baseHits,
                                          preparationCounters.fullPreparations,
                                          preparationCounters.baseEvictions,
                                          preparationCounters.modelHits,
                                          preparationCounters.modelMisses,
                                          preparationCounters.configHashes,
                                          preparationCounters.suffixHashes,
                                          basePreparations.size(),
                                          modelVariants.size(),
                                          basePreparations.capacity() * sizeof(BasePreparation) + modelVariants.capacity() * sizeof(ModelVariant)};
    std::copy(values.begin(), values.end(), statistics);
}
void GetNativeMaterialProgramStatistics(uint64_t statistics[6]) {
    Owner();
    if (!statistics)
        return;
    const std::array<uint64_t, 6> values{programCounters.baseMemoHits, programCounters.modelMemoHits,     programCounters.baseLookups,
                                         programCounters.modelLookups, programCounters.configComparisons, programCounters.suffixReplacements};
    std::copy(values.begin(), values.end(), statistics);
}
void ClearNativeMaterialPrograms() {
    Owner();
    ClearMaterialProgramMemos();
    programs.clear();
    modelPrograms.clear();
    ClearNativeModelGPU();
    ClearMaterialPreparations();
    if (rasterObserverInstalled)
        SetNativeRasterObserver(nullptr);
    rasterObserverInstalled = false;
}
