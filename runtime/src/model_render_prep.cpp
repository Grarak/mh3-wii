// Deferred model preparation (model_render_prep.h). The executor follows the
// original 80076220 step order and the 800879DC/80087B34 shape setup exactly,
// against a host mirror of the G3DState caches and the given owner's state.
#include "model_render_prep.h"
#ifdef MH3_DEVELOPMENT
#include "dev_options.h"
#endif
#include "fatal.h"
#include <aurora/fast_map.h>
#include "memory.h"
#include "native_graphics.h"
#include "native_frame_packet.h"
#include "native_material_gpu.h"
#include "runtime/native/adapters/model_material_bridge.h"
#include "runtime/native/adapters/model_pose_bridge.h"
#include "runtime/native/adapters/model_state_bridge.h"
#include "runtime/native/mh3_model_material_prep.h"
#include "runtime/native/mh3_model_prepare_cache.h"
#include "runtime/native/mh3_shade.h"
#include <array>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <mutex>
#include <unordered_map>
#include <cstdio>

namespace {
constexpr uint32_t Genmode = 0x80685300, Coordinates = 0x8068530c, TextureCache = 0x80685380, PaletteCache = 0x806854a0, ShapeCache = 0x80685504, TextureSources = 0x80685510;
constexpr uint32_t Sync = 0x80796db0, CurrentValid = 0x80796db4, PostIdentity = 0x80796db8, Pipeline = 0x80796dbc, DepthCompare = 0x80796dc8;

// A failed step names its function (the call's text up to its arguments).
void CheckedStep(mh3_res_status status, const char* call) {
    if (status != MH3_RES_OK) [[unlikely]]
        MH3_THROW(std::runtime_error("Deferred model preparation failed in " + std::string(call, std::strcspn(call, "(")) + ": " + std::to_string(status)));
}

#define Checked(call) CheckedStep((call), #call)

uint32_t Word(const uint8_t* p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

void PutWord(uint8_t* p, uint32_t value) {
    p[0] = uint8_t(value >> 24);
    p[1] = uint8_t(value >> 16);
    p[2] = uint8_t(value >> 8);
    p[3] = uint8_t(value);
}

mh3_res_span Span(const uint8_t* data, size_t size) {
    return {data, size};
}

ModelPrepMirror renderMirror;

// Whether a preparation ran on the render mirror since it was last forgotten
// (only ExecuteModelPrep writes it): restores in a row forget it once.
bool renderMirrorUsed = true;
uint32_t pendingInvalidate = 0;

// Game thread: the shared records of captured material scopes, keyed by the
// material and its selected resource slots, with the live typed blocks they
// were read from. Node pointers stay valid while the live material revision is
// unchanged (in-place producer updates keep nodes); the pipeline record stays
// valid while its block keeps the frozen generation.
struct MaterialEntry {
    uint32_t material = 0;
    uint32_t slots[MH3_MAT_RESOURCE_COUNT]{};
    uint64_t live = 0, objects = 0;
    const NativeLiveMaterialBlock* blocks[MH3_MODEL_MATERIAL_BLOCK_COUNT]{};

    // The frozen generations of the record's pipeline, indirect and texgen blocks.
    uint64_t generations[MH3_MODEL_MATERIAL_BLOCK_COUNT]{};
    const ModelPrepMaterial* record = nullptr;
};

// The typed blocks a record freezes; pixel and colours are copied per draw.
constexpr bool Frozen(unsigned kind) {
    return kind == MH3_MODEL_MATERIAL_PIPELINE || kind == MH3_MODEL_MATERIAL_INDIRECT || kind == MH3_MODEL_MATERIAL_TEXGEN;
}

FastMap<uint64_t, MaterialEntry> materialEntries;
void ReleaseRecord(const ModelPrepMaterial* record) {
    if (!record)
        return;

    if (!NativeRenderThreadRunning()) {
        delete record;
        return;
    }

    QueueNativeGraphicsTask(NativeTaskOrder::AfterDraws, [record] { delete record; });
}

// Copies the presence word and the present 32/12-byte objects only: the
// cache producer reads nothing else.
void CopyObjects(uint8_t* out, const uint8_t* objects, uint32_t stride) {
    std::memcpy(out, objects, 4);
    const uint32_t present = Word(objects) & 255u;

    for (uint32_t i = 0; i < 8; i++)
        if (present & (1u << i))
            std::memcpy(out + 4 + i * stride, objects + 4 + i * stride, stride);
}

// One dispatch's execution context.
// Group publication sources of the Render and Shadow owners.
mh3_native_material_state groupStates[2];
#ifdef MH3_DEVELOPMENT
// A ResMat's name (+8, relative to the material).
std::array<char, 32> MaterialName(uint32_t material) {
    std::array<char, 32> name{};

    for (uint32_t i = 0, at = material + Memory::Read32(material + 8); i + 1 < name.size() && Memory::Read8(at + i); i++)
        name[i] = char(Memory::Read8(at + i));

    return name;
}

// MH3_TRACE=materials: each model material once, the first time it is
// prepared: its name, its TEV stages (colour and alpha a b c d op bias scale
// clamp out; texture map/coordinate, raster channel, konst selectors,
// indirect stage/format/bias/alpha/matrix), swap tables, colour and konst
// registers, indirect stages, the pixel block (alpha compare, depth, blend)
// and what the shader classifier makes of it (model, textures, quality:
// 1 exact, 2 swap, 4 folded, 8 fallback, 16 compare, 32 indirect ignored,
// 64 channel 1).
void TraceMaterial(uint32_t material, const NativeLiveMaterialBlock* const* blocks, const uint32_t* slots) {
    static std::mutex lock;
    static std::unordered_map<uint32_t, bool> seen;
    const std::lock_guard guard(lock);

    if (!seen.try_emplace(material, true).second)
        return;

    const auto* pipeline = blocks[MH3_MODEL_MATERIAL_PIPELINE];
    const auto* pixel = blocks[MH3_MODEL_MATERIAL_PIXEL];
    const auto* colors = blocks[MH3_MODEL_MATERIAL_COLORS];
    const auto* indirect = blocks[MH3_MODEL_MATERIAL_INDIRECT];

    if (!pipeline)
        return;

    mh3_native_material_state state = pipeline->fields;

    if (pixel)
        state.pixel = pixel->fields.pixel;

    if (colors) {
        std::memcpy(state.color, colors->fields.color, sizeof state.color);
        std::memcpy(state.konst, colors->fields.konst, sizeof state.konst);
    }

    if (indirect) {
        std::memcpy(state.indirect, indirect->fields.indirect, sizeof state.indirect);
        std::memcpy(state.indirect_matrix, indirect->fields.indirect_matrix, sizeof state.indirect_matrix);
    }

    mh3_shade_material shading{};
    const uint8_t quality = mh3_shade_classify(&state, &shading);
    std::fprintf(stderr, "MH3 material: %08x (%s) stages %u indirect %u textures %u model %u quality %02x alpha-test %u\n", material, MaterialName(material).data(),
                 state.tev_count, indirect ? indirect->fields.indirect_count : 0, shading.textures, shading.model, quality, shading.alpha_test);

    // What takes a light or camera: the light set, the indirect methods (NW4R:
    // 0 warp, 1 normal map, 2 normal map + specular, 3 fur) with their lights,
    // and each texture SRT slot's mapping mode with its camera and light.
    if (slots[MH3_MAT_MISC]) {
        mh3_model_material_misc misc{};
        Checked(mh3_model_material_misc_read({static_cast<const uint8_t*>(Memory::GetPointer(slots[MH3_MAT_MISC], 12)), 12}, &misc));
        std::fprintf(stderr, "MH3 material:   misc light set %d fog %d indirect methods %u %u %u normal-map lights %d %d %d\n", misc.light_reference, misc.fog_reference,
                     misc.indirect_method[0], misc.indirect_method[1], misc.indirect_method[2], misc.normal_map_light[0], misc.normal_map_light[1], misc.normal_map_light[2]);
    }

    if (slots[MH3_MAT_TEXTURE_SRT]) {
        mh3_native_texture_srt srt{};
        Checked(mh3_texture_srt_read_for_prepare({static_cast<const uint8_t*>(Memory::GetPointer(slots[MH3_MAT_TEXTURE_SRT], 0x248)), 0x248}, &srt));

        for (uint32_t i = 0; i < 8; i++)
            if (srt.slot[i].flags & 1)
                std::fprintf(stderr, "MH3 material:   srt slot %u map mode %u camera %d light %d\n", i, srt.slot[i].map_mode, srt.slot[i].camera_reference, srt.slot[i].light_reference);
    }

    for (uint32_t i = 0; i < state.tev_count && i < 16; i++) {
        const auto& t = state.tev[i];
        std::fprintf(stderr,
                     "MH3 material:   stage %u colour %u %u %u %u op %u bias %u scale %u clamp %u out %u | alpha %u %u %u %u op %u bias %u scale %u clamp %u out %u | map %u coord %u ras %u "
                     "swap %u/%u konst %u/%u | ind stage %u format %u bias %u alpha %u matrix %u wrap %u/%u add %u\n",
                     i, t.color.a, t.color.b, t.color.c, t.color.d, t.color.operation, t.color.bias, t.color.scale, t.color.clamp, t.color.output, t.alpha.a, t.alpha.b, t.alpha.c,
                     t.alpha.d, t.alpha.operation, t.alpha.bias, t.alpha.scale, t.alpha.clamp, t.alpha.output, t.texture_map, t.texture_coordinate, t.raster_channel, t.raster_swap,
                     t.texture_swap, t.konst_color, t.konst_alpha, t.indirect_stage, t.indirect_format, t.indirect_bias, t.indirect_alpha, t.indirect_matrix, t.indirect_wrap_s,
                     t.indirect_wrap_t, t.indirect_add_previous);
    }

    for (uint32_t i = 0; i < 4; i++)
        std::fprintf(stderr, "MH3 material:   swap %u %u%u%u%u colour %d %d %d %d konst %d %d %d %d indirect %u/%u scale %u/%u\n", i, state.swaps[i].rgba[0], state.swaps[i].rgba[1],
                     state.swaps[i].rgba[2], state.swaps[i].rgba[3], state.color[i].rgba[0], state.color[i].rgba[1], state.color[i].rgba[2], state.color[i].rgba[3], state.konst[i].rgba[0],
                     state.konst[i].rgba[1], state.konst[i].rgba[2], state.konst[i].rgba[3], state.indirect[i].coordinate, state.indirect[i].map, state.indirect[i].scale_s,
                     state.indirect[i].scale_t);

    for (uint32_t i = 0; i < 3; i++) {
        const auto& m = state.indirect_matrix[i];

        if (m.known)
            std::fprintf(stderr, "MH3 material:   indirect matrix %u %d %d %d / %d %d %d exponent %d\n", i, m.coefficients[0], m.coefficients[1], m.coefficients[2], m.coefficients[3],
                         m.coefficients[4], m.coefficients[5], m.scale_exponent);
    }

    // The raw colour channels (40 bytes) and the texture objects (GXTexObj:
    // the format in bits 20-23 of the third word).
    if (const uint32_t channels = slots[MH3_MAT_CHANNELS]) {
        std::fprintf(stderr, "MH3 material:   channels");

        for (uint32_t i = 0; i < 40; i += 4)
            std::fprintf(stderr, " %08x", Memory::Read32(channels + i));

        std::fprintf(stderr, "\n");
    }

    if (const uint32_t objects = slots[MH3_MAT_TEXTURE_OBJECTS])
        for (uint32_t i = 0; i < 8; i++)
            if (Memory::Read32(objects) & (1u << i)) {
                const uint32_t at = objects + 4 + i * 32;
                std::fprintf(stderr, "MH3 material:   texture %u format %u size %ux%u words", i, (Memory::Read32(at + 8) >> 20) & 15, (Memory::Read32(at + 8) & 1023) + 1,
                             ((Memory::Read32(at + 8) >> 10) & 1023) + 1);

                for (uint32_t k = 0; k < 32; k += 4)
                    std::fprintf(stderr, " %08x", Memory::Read32(at + k));

                std::fprintf(stderr, "\n");
            }

    std::fprintf(stderr, "MH3 material:   pixel");

    for (uint32_t i = 0; i < MH3_PIXEL_FIELD_COUNT; i++)
        std::fprintf(stderr, " %u", state.pixel.values[i]);

    std::fprintf(stderr, " (alpha compare %u %u %u %u op %u, depth %u %u write %u, blend %u logic %u subtract %u src %u dst %u, dst alpha %u %u)\n",
                 state.pixel.values[MH3_PIXEL_ALPHA_COMPARE0], state.pixel.values[MH3_PIXEL_ALPHA_REFERENCE0], state.pixel.values[MH3_PIXEL_ALPHA_COMPARE1],
                 state.pixel.values[MH3_PIXEL_ALPHA_REFERENCE1], state.pixel.values[MH3_PIXEL_ALPHA_OPERATION], state.pixel.values[MH3_PIXEL_DEPTH_TEST],
                 state.pixel.values[MH3_PIXEL_DEPTH_COMPARE], state.pixel.values[MH3_PIXEL_DEPTH_WRITE], state.pixel.values[MH3_PIXEL_BLEND_ENABLE],
                 state.pixel.values[MH3_PIXEL_LOGIC_ENABLE], state.pixel.values[MH3_PIXEL_BLEND_SUBTRACT], state.pixel.values[MH3_PIXEL_BLEND_SOURCE],
                 state.pixel.values[MH3_PIXEL_BLEND_DESTINATION], state.pixel.values[MH3_PIXEL_DESTINATION_ALPHA_ENABLE], state.pixel.values[MH3_PIXEL_DESTINATION_ALPHA]);
}

// MH3_TRACE=srt: a texture SRT resource (guest address) whose slots'
// translations or effect-matrix translations moved, the first time and every
// 60th move, with the step counter (system_w+0xC, in ticks): how fast each
// material's texture scrolls. The model prep may run off the game thread.
void TraceTextureSrt(uint32_t resource, uint32_t material, const mh3_native_texture_srt& srt) {
    struct Seen {
        float t[8][4];
        uint32_t moves;
    };

    static std::mutex lock;
    static std::unordered_map<uint32_t, Seen> seen;
    const std::lock_guard guard(lock);
    auto [it, fresh] = seen.try_emplace(resource);
    Seen& s = it->second;
    bool moved = false;

    for (uint32_t i = 0; i < 8; i++) {
        const auto& slot = srt.slot[i];
        const float now[4]{slot.translation[0], slot.translation[1], slot.effect_matrix[3], slot.effect_matrix[7]};
        moved |= !fresh && std::memcmp(now, s.t[i], sizeof now) != 0;
        std::memcpy(s.t[i], now, sizeof now);
    }

    if (!moved || s.moves++ % 60)
        return;

    const auto name = MaterialName(material);

    for (uint32_t i = 0; i < 8; i++)
        if (srt.slot[i].flags & 1)
            std::fprintf(stderr, "MH3 srt: resource %08x material %08x (%s) slot %u move %u translation %.5f %.5f effect %.5f %.5f tick %u\n", resource, material, name.data(), i,
                         s.moves - 1, s.t[i][0], s.t[i][1], s.t[i][2], s.t[i][3], Memory::Read32(0x8065AA6C));
}
#endif

struct Prep {
    NativeOwner owner;
    ModelPrepMirror& m;
    const ModelPrepInputs& in;
    const ModelPrepSrt* srt;
    uint8_t& sync() { return *m.At(Sync, 1).data; }
    mh3_model_genmode Mode() {
        mh3_model_genmode mode{};
        Checked(mh3_model_genmode_read({m.At(Genmode, 12).data, 12}, &mode));
        return mode;
    }

    void PublishGenmode(bool xf) {
        mh3_model_genmode value{};
        uint32_t top = 0;
        int apply = 0;
        Checked(mh3_model_genmode_publish_counts(m.At(Genmode, 12), xf, &value, &top, &apply));

        if (apply)
            OverlayNativeMaterialTop(top, value.texgens, value.channels, value.tevs, value.indirect, value.cull, owner);
    }

    // The captured pixel/colour group version as a material state; a group's
    // publication reads only its own fields (mh3_material_group_publish).
    const mh3_native_material_state& GroupState(mh3_model_material_block kind) {
        auto& state = groupStates[owner == NativeOwner::Render ? 0 : 1];

        if (kind == MH3_MODEL_MATERIAL_PIXEL)
            std::memcpy(&state.pixel, &in.pixel->pixel, sizeof state.pixel);
        else {
            std::memcpy(state.color, in.colors->color, sizeof state.color);
            std::memcpy(state.konst, in.colors->konst, sizeof state.konst);
        }

        return state;
    }

    void Publish(const mh3_native_material_state& fields, mh3_model_material_block kind, bool xf) {
        const auto mode = Mode();
        const uint32_t count = kind == MH3_MODEL_MATERIAL_INDIRECT ? mode.indirect : kind == MH3_MODEL_MATERIAL_TEXGEN ? mode.texgens : 1;

        if (!xf)
            PublishGenmode(false);

        sync() = 0;

        if (count)
            OverlayNativeModelMaterialGroup(fields, kind, count, owner);

        if (xf)
            PublishGenmode(true);
    }

    void Group(mh3_model_material_block kind) {
        if (kind == MH3_MODEL_MATERIAL_PIXEL ? in.pixel : in.colors)
            Publish(GroupState(kind), kind, false);
    }

    void Group(const NativeFrozenMaterialBlock* block, mh3_model_material_block kind, bool xf = false) {
        if (block)
            Publish(block->fields, kind, xf);
    }

    void CoordinateSizes(uint32_t count) {
        mh3_model_texcoord_sizes sizes{};
        uint8_t written = 0;
        int apply = 0;
        Checked(mh3_model_coordinate_prepare(m.At(Coordinates, 108), count, m.coordinate_map_known, &m.map_sizes, &m.coordinate_sizes, &sizes, &written, &apply));

        if (apply)
            m.coordinate_sizes = sizes;
    }

    void Pipeline() {
        if (!in.record || !in.record->pipeline)
            return;

        const uint32_t resource = in.record->pipeline_address;
        const auto& block = *in.record->pipeline;
        const uint64_t generation = block.fields.generation;

        if (Word(m.At(::Pipeline, 4).data) == resource && m.pipeline_address == resource && m.pipeline_revision == generation)
            return;

        Checked(mh3_model_coordinate_table(m.At(Coordinates, 108), block.texcoord_map));
        m.coordinate_map_known = block.texcoord_known;
        PublishGenmode(false);
        sync() = 0;
        OverlayNativeModelMaterialGroup(block.fields, MH3_MODEL_MATERIAL_PIPELINE, 1, owner);
        PutWord(m.At(::Pipeline, 4).data, resource);
        m.pipeline_address = resource;
        m.pipeline_revision = generation;
        CoordinateSizes(Mode().texgens);
    }

    void Indirect() {
        Group(in.indirect, MH3_MODEL_MATERIAL_INDIRECT);

        if (!in.indirect_operator)
            return;

        mh3_pack_indirect_matrix base[3]{}, matrices[3];

        if (in.indirect)
            std::memcpy(base, in.indirect->fields.indirect_matrix, sizeof base);

        Checked(mh3_model_indirect_matrices(base, &in.op, matrices));
        OverlayNativeMaterialIndirectMatrices(matrices, owner);
    }

    void Palettes() {
        const auto& record = *in.record;
        uint8_t changed = 0;
        Checked(mh3_resource_objects_cache(m.At(PaletteCache, 98), Span(record.palette_objects, 100), 1, &changed));
        const uint32_t present = Word(record.palette_objects);

        for (uint32_t i = 0; i < 8; i++)
            if (present & (1u << i))
                BindNativePalette(i, record.palettes[i], owner);
        Checked(mh3_texture_cache_palette_changed(m.At(TextureCache, 257), changed));
    }

    void Textures() {
        const auto& record = *in.record;
        uint8_t changed = 0;
        Checked(mh3_resource_objects_cache(m.At(TextureCache, 257), Span(record.texture_objects, 260), 0, &changed));
        const uint32_t present = Word(record.texture_objects);

        for (uint32_t i = 0; i < 8; i++) {
            if (!(present & (1u << i)))
                continue;

            auto binding = record.textures[i];
            binding.map_id = i;
            BindNativeTexture(binding, owner);

            if (changed & (1u << i)) {
                if (binding.source.width > UINT16_MAX || binding.source.height > UINT16_MAX)
                    MH3_THROW(std::runtime_error("Native G3D texture dimensions exceed coordinate cache"));

                Checked(mh3_texture_scale_size(m.At(Coordinates, 36), i, uint16_t(binding.source.width), uint16_t(binding.source.height)));

                if (!binding.source.width || !binding.source.height)
                    MH3_THROW(std::runtime_error("Native G3D map size is invalid"));

                m.map_sizes.width[i] = uint16_t(binding.source.width);
                m.map_sizes.height[i] = uint16_t(binding.source.height);
                m.map_sizes.known |= uint8_t(1u << i);
            }
        }
    }

    void Channels() {
        mh3_pack_channel channels[2];
        Checked(mh3_model_channels_prepare_channels(Span(in.channels, 40), Mode().channels, in.light_masks, in.ambient, (in.flags & 8) != 0, channels));
        OverlayNativeMaterialChannels(channels, owner);
        PublishGenmode(true);
    }

    void Srt() {
        if (!srt)
            MH3_THROW(std::runtime_error("Deferred model preparation has no texture SRT inputs"));

        mh3_texture_matrices matrices{};
        uint32_t next = 0;
        Checked(mh3_texture_matrices_prepare(&srt->srt, srt->maps, srt->map_sources, Word(m.At(PostIdentity, 4).data), &matrices, &next));

        for (uint32_t i = 0; i < 8; i++) {
            PutWord(m.At(TextureSources + i * 4, 4).data, matrices.source[i]);

            if (matrices.apply & (1u << i))
                SetNativeTextureMatrix(64 + i * 3, &matrices.matrix[i].v[0][0], true, owner);
        }

        PutWord(m.At(PostIdentity, 4).data, next);
        PublishNativeTextureSrt(srt->srt, matrices.apply, owner);
    }

    void Material() {
        const auto has = [&](mh3_material_resource_kind kind) { return (in.present & (1u << kind)) != 0; };

        // 80076220 order; fog (step 1) and the light set (step 10) ran on the game thread.
        if (has(MH3_MAT_MISC)) {
            int apply = 0;
            Checked(mh3_depth_compare_cache(m.At(DepthCompare, 5), m.At(Sync, 1), in.depth_before_texture, &apply));

            if (apply)
                SetNativeDepthCompareBeforeTexture(in.depth_before_texture != 0, owner);
        }

        if (has(MH3_MAT_PALETTE_OBJECTS))
            Palettes();

        if (has(MH3_MAT_TEXTURE_OBJECTS))
            Textures();

        if (has(MH3_MAT_GENERAL_MODE))
            Checked(mh3_model_genmode_select(m.At(Genmode, 12), Span(in.genmode, 8)));

        Pipeline();
        Group(MH3_MODEL_MATERIAL_PIXEL);
        Group(MH3_MODEL_MATERIAL_COLORS);
        Indirect();

        if (has(MH3_MAT_CHANNELS))
            Channels();

        Group(in.record ? in.record->texgen : nullptr, MH3_MODEL_MATERIAL_TEXGEN, true);

        if (has(MH3_MAT_TEXTURE_SRT))
            Srt();
    }

    void PublishRows(const uint32_t rows[8]) {
        mh3_model_matrix_select(&m.selectors, rows);
        SetNativeModelMatrixSelectors(m.selectors, owner);
    }

    void Shape(const mh3_matrix34* direct_position, const mh3_matrix34* direct_normal, ModelPrepShapeResult& out) {
        uint8_t& current_valid = *m.At(CurrentValid, 1).data;

        if (!(in.flags & 2)) {
            // 800879DC primitive setup; vertex sources were checked on the game thread.
            CoordinateSizes(Mode().texgens);
            PublishGenmode(true);
            Checked(mh3_model_shape_cache(m.At(ShapeCache, 12), Span(in.shape_cache, 12)));
            sync() = 0;
        }

        if (in.flags & 4) {
            // The original republishes the whole material with front culling.
            OverlayNativeMaterialTop(MH3_MATERIAL_TOP_CULL, 0, 0, 0, 0, 1, owner);
            std::memset(m.At(ShapeCache, 12).data, 0, 12);
            sync() = 1;
        }

        // 80087B34 matrix selection.
        mh3_model_shape_input_unchecked(in.matrix_id, in.has_normal, m.At(TextureSources, 32).data, current_valid != 0, in.has_binding, direct_position,
                                        direct_position ? direct_normal : nullptr, &out.input);
        CoordinateSizes(Mode().texgens);

        if (out.input.matrix_id >= 0) {
            uint32_t rows[8];
            const bool identity = mh3_model_shape_rows_unchecked(out.input.texture_source, rows);

            if (!identity || !current_valid)
                PublishRows(rows);

            current_valid = identity ? 1 : 0;
        } else if (!current_valid) {
            uint32_t rows[8];

            for (auto& row : rows)
                row = 60;

            PublishRows(rows);
            current_valid = 1;
        }

        PublishGenmode(true);
        sync() = 0;
        SetNativeModelMatrixSelectors(m.selectors, owner);

        if (out.input.matrix_id < 0)
            current_valid = 0;

        out.coordinate_sizes = m.coordinate_sizes;
    }
};
} // namespace

void ReleaseNativeFrozenMaterialBlock(const NativeFrozenMaterialBlock* block) {
    if (!NativeRenderThreadRunning()) {
        delete block;
        return;
    }

    QueueNativeGraphicsTask(NativeTaskOrder::AfterDraws, [block] { delete block; });
}

namespace {
// Group versions retired while one packet was recorded, deleted by one task
// after a later packet rendered (a version retires every time an animated
// group changes, so one task per version would cost more than the copy).
std::vector<const NativeMaterialGroupVersion*> retiredVersions;
uint64_t retiredSequence = 0;
} // namespace

void ReleaseNativeMaterialGroupVersion(const NativeMaterialGroupVersion* version) {
    if (!NativeRenderThreadRunning()) {
        delete version;
        return;
    }

    const uint64_t sequence = NativeFramePacketSequence();

    if (!retiredVersions.empty() && retiredSequence != sequence) {
        QueueNativeGraphicsTask(NativeTaskOrder::AfterDraws, [versions = std::move(retiredVersions)] {
            for (const auto* retired : versions)
                delete retired;
        });
        retiredVersions.clear();
    }

    retiredSequence = sequence;
    retiredVersions.push_back(version);
}

ModelPrepMode NativeModelPrepMode() {
#ifdef MH3_DEVELOPMENT
    static const ModelPrepMode mode = VerifyOptions().Has("prep") ? ModelPrepMode::Verify : ModelPrepMode::Deferred;
    return mode;
#else
    return ModelPrepMode::Deferred;
#endif
}

mh3_res_mut_span ModelPrepMirror::At(uint32_t address, size_t size) {
    if (address >= BaseA && address - BaseA + size <= SizeA)
        return {a + (address - BaseA), size};

    if (address >= BaseB && address - BaseB + size <= SizeB)
        return {b + (address - BaseB), size};

    MH3_THROW(std::runtime_error("Deferred model preparation has no mirror for " + std::to_string(address)));
}

void ModelPrepMirror::Invalidate(uint32_t fields) {
    // Fog, light and view-source caches belong to the game thread.
    const mh3_model_cache_storage storage{At(0x80685480, 1),  At(0x80685500, 2),  At(Coordinates, 4), At(Pipeline, 4), At(Genmode, 12),
                                          At(ShapeCache, 12), At(CurrentValid, 1), At(TextureSources, 32), At(PostIdentity, 4), At(DepthCompare, 4),
                                          {unused, 4},        {unused + 4, 28},  {unused + 32, 12},  At(Sync, 1)};
    Checked(mh3_model_cache_invalidate(storage, fields & ~(256u | 512u | 1024u)));
}

ModelPrepMirror& RenderModelPrepMirror() {
    return renderMirror;
}

void ForgetNativeModelPrepCaches() {
    if (!renderMirrorUsed)
        return;

    renderMirror.Invalidate(0x7ff);
    renderMirrorUsed = false;
}

uint32_t TakeNativeModelPrepInvalidate() {
    const uint32_t fields = pendingInvalidate;
    pendingInvalidate = 0;
    return fields;
}

void NoteNativeModelPrepInvalidate(uint32_t fields) {
    pendingInvalidate |= fields;
}

namespace {
// Aligned word compare (memcmp is a byte loop on the Vita).
bool SameWords(const void* a, const void* b, size_t bytes) {
    const auto* x = static_cast<const uint32_t*>(a);
    const auto* y = static_cast<const uint32_t*>(b);

    for (size_t i = 0; i < bytes / 4; i++)
        if (x[i] != y[i])
            return false;
    return true;
}

const uint8_t* Bytes(uint32_t address, size_t size) {
    return static_cast<const uint8_t*>(Memory::GetPointer(address, size));
}

// The typed block addresses of a material's slots, by mh3_model_material_block.
void BlockAddresses(const uint32_t* slots, uint32_t out[MH3_MODEL_MATERIAL_BLOCK_COUNT]) {
    if (slots[MH3_MAT_TEV] > UINT32_MAX - 32)
        MH3_THROW(std::runtime_error("Model pipeline offset overflows"));

    out[MH3_MODEL_MATERIAL_PIPELINE] = slots[MH3_MAT_TEV] ? slots[MH3_MAT_TEV] + 32 : 0;
    out[MH3_MODEL_MATERIAL_PIXEL] = slots[MH3_MAT_PIXEL_STATE];
    out[MH3_MODEL_MATERIAL_COLORS] = slots[MH3_MAT_TEV_COLORS];
    out[MH3_MODEL_MATERIAL_INDIRECT] = slots[MH3_MAT_INDIRECT_MATRIX_SCALE];
    out[MH3_MODEL_MATERIAL_TEXGEN] = slots[MH3_MAT_TEXCOORD_GENERATION];
}

// The object records' bytes and typed bindings.
void CaptureObjects(ModelPrepMaterial& record, const uint32_t* slots) {
    if (const uint32_t objects = slots[MH3_MAT_PALETTE_OBJECTS]) {
        CopyObjects(record.palette_objects, Bytes(objects, 100), 12);
        const uint32_t present = Word(record.palette_objects);

        for (uint32_t i = 0; i < 8; i++)
            if (present & (1u << i))
                record.palettes[i] = ReadNativePaletteObject(objects + 4 + i * 12);
    }

    if (const uint32_t objects = slots[MH3_MAT_TEXTURE_OBJECTS]) {
        CopyObjects(record.texture_objects, Bytes(objects, 260), 32);
        const uint32_t present = Word(record.texture_objects);

        for (uint32_t i = 0; i < 8; i++)
            if (present & (1u << i))
                record.textures[i] = ReadNativeTextureObject(objects + 4 + i * 32);
    }
}

// The record's object bytes still match guest memory (present objects only).
bool SameObjectBytes(const uint8_t* captured, uint32_t objects, uint32_t stride) {
    const uint8_t* guest = Bytes(objects, 4 + 8 * stride);

    if (!SameWords(captured, guest, 4))
        return false;

    const uint32_t present = Word(captured) & 255u;

    for (uint32_t i = 0; i < 8; i++)
        if ((present & (1u << i)) && !SameWords(captured + 4 + i * stride, guest + 4 + i * stride, stride))
            return false;
    return true;
}

// The record's typed bindings are still the objects' current ones.
// A missing object is a change too: the rebuild reports it.
bool SameBindings(const ModelPrepMaterial& record, const uint32_t* slots) {
    if (const uint32_t objects = slots[MH3_MAT_PALETTE_OBJECTS])
        if (!SameNativePaletteObjects(objects + 4, Word(record.palette_objects), record.palettes))
            return false;

    if (const uint32_t objects = slots[MH3_MAT_TEXTURE_OBJECTS])
        if (!SameNativeTextureObjects(objects + 4, Word(record.texture_objects), record.textures))
            return false;

    return true;
}
} // namespace

const NativeFrozenMaterialBlock* CaptureModelPrepBlock(uint32_t address, mh3_model_material_block kind) {
    return FreezeNativeLiveMaterialBlock(ReadNativeLiveMaterialBlockView(address, kind));
}

void CaptureModelPrepMaterial(ModelPrepInputs& in, const mh3_model_material_scope& scope, ModelPrepSrtList& srts) {
    const auto& slots = scope.selected.resource_slots;
    in.present = 0;

    for (unsigned i = 0; i < MH3_MAT_RESOURCE_COUNT; i++)
        if (slots[i])
            in.present |= uint16_t(1u << i);
    if (slots[MH3_MAT_MISC]) {
        mh3_model_material_misc misc{};
        Checked(mh3_model_material_misc_read({Bytes(slots[MH3_MAT_MISC], 12), 12}, &misc));
        in.depth_before_texture = misc.depth_before_texture;
    }

    if (slots[MH3_MAT_GENERAL_MODE])
        std::memcpy(in.genmode, Bytes(slots[MH3_MAT_GENERAL_MODE], 8), 8);

    if (slots[MH3_MAT_CHANNELS])
        std::memcpy(in.channels, Bytes(slots[MH3_MAT_CHANNELS], 40), 40);

    // The shared record: reused while the slots, the live block nodes, the
    // pipeline block's version, the typed texture/palette objects and the
    // object bytes are unchanged.
    uint32_t hash = scope.material_address;

    for (unsigned i = 0; i < MH3_MAT_RESOURCE_COUNT; i++)
        hash = (hash ^ slots[i]) * 0x9E3779B1u;

    if (materialEntries.size() >= 4096) {
        for (auto& item : materialEntries)
            ReleaseRecord(item.second.record);

        materialEntries.clear();
    }

    auto& entry = materialEntries[uint64_t(scope.material_address) << 32 | hash];
    const bool same = entry.record && entry.material == scope.material_address && SameWords(entry.slots, slots, sizeof entry.slots);

    if (!same || entry.live != ReadNativeLiveMaterialRevision()) {
        uint32_t addresses[MH3_MODEL_MATERIAL_BLOCK_COUNT];
        BlockAddresses(slots, addresses);

        for (unsigned kind = 0; kind < MH3_MODEL_MATERIAL_BLOCK_COUNT; kind++)
            entry.blocks[kind] = addresses[kind] ? &ReadNativeLiveMaterialBlockView(addresses[kind], mh3_model_material_block(kind)) : nullptr;
        // Reading may admit blocks; record the revision they were read at.
        entry.live = ReadNativeLiveMaterialRevision();
    }

    bool rebuild = !same;

    for (unsigned kind = 0; kind < MH3_MODEL_MATERIAL_BLOCK_COUNT && !rebuild; kind++)
        if (Frozen(kind) && (entry.blocks[kind] ? entry.blocks[kind]->fields.generation : 0) != entry.generations[kind])
            rebuild = true;
    if (!rebuild && slots[MH3_MAT_PALETTE_OBJECTS] && !SameObjectBytes(entry.record->palette_objects, slots[MH3_MAT_PALETTE_OBJECTS], 12))
        rebuild = true;

    if (!rebuild && slots[MH3_MAT_TEXTURE_OBJECTS] && !SameObjectBytes(entry.record->texture_objects, slots[MH3_MAT_TEXTURE_OBJECTS], 32))
        rebuild = true;

    if (!rebuild && entry.objects != ReadNativeTextureObjectRevision()) {
        // Some typed object changed: rebuild only if one of this material's did.
        rebuild = !SameBindings(*entry.record, slots);
        entry.objects = ReadNativeTextureObjectRevision();
    }

    if (rebuild) {
        auto record = std::make_unique<ModelPrepMaterial>();
        CaptureObjects(*record, slots);
        record->pipeline_address = slots[MH3_MAT_TEV];

        // A live block owns its frozen copy; a newer version replaces it only
        // after the generation check above made this record stale.
        const auto freeze = [&](mh3_model_material_block kind) {
            const auto* block = entry.blocks[kind];
            entry.generations[kind] = block ? block->fields.generation : 0;
            return block ? FreezeNativeLiveMaterialBlock(*block) : nullptr;
        };

        record->pipeline = freeze(MH3_MODEL_MATERIAL_PIPELINE);
        record->indirect = freeze(MH3_MODEL_MATERIAL_INDIRECT);
        record->texgen = freeze(MH3_MODEL_MATERIAL_TEXGEN);
        ReleaseRecord(entry.record);
        entry.record = record.release();
        entry.material = scope.material_address;
        std::memcpy(entry.slots, slots, sizeof entry.slots);
        entry.objects = ReadNativeTextureObjectRevision();
    }

#ifdef MH3_DEVELOPMENT
    static const bool tracedMaterials = TraceOptions().Has("materials");

    if (tracedMaterials) [[unlikely]]
        TraceMaterial(scope.material_address, entry.blocks, slots);
#endif

    in.record = entry.record;
    in.indirect = entry.record->indirect;

    // Animated group fields: the live blocks' (updated in place) current versions.
    const auto* pixel = entry.blocks[MH3_MODEL_MATERIAL_PIXEL];
    in.pixel = pixel ? VersionNativeLiveMaterialGroup(*pixel) : nullptr;
    const auto* colors = entry.blocks[MH3_MODEL_MATERIAL_COLORS];
    in.colors = colors ? VersionNativeLiveMaterialGroup(*colors) : nullptr;

    in.srt = -1;

    if (const uint32_t resource = slots[MH3_MAT_TEXTURE_SRT]) {
        auto& out = srts.Next(&in.srt);
        Checked(mh3_texture_srt_read_for_prepare({Bytes(resource, 0x248), 0x248}, &out.srt));
#ifdef MH3_DEVELOPMENT
        static const bool tracedSrt = TraceOptions().Has("srt");

        if (tracedSrt) [[unlikely]]
            TraceTextureSrt(resource, entry.material, out.srt);
#endif

        // The constructor defines these exact built-in mapping functions. Custom
        // slots may change their callback and type at runtime; reject those until
        // their own typed native producer is implemented.
        constexpr uint32_t functions[]{0x800850a0, 0x80084694, 0x80084a78, 0x80084694, 0x80084c00};
        constexpr uint32_t kinds[]{0, 1, 2, 1, 1};

        for (uint32_t i = 0; i < 8; i++) {
            out.map_sources[i] = 0;
            const auto& slot = out.srt.slot[i];

            if (!(slot.flags & 1) || !slot.map_mode)
                continue;

            const uint32_t mode = slot.map_mode, entry = 0x80685560 + mode * 8;
            const uint32_t builtin = mode < 5 ? mode : 0;

            if (Memory::Read32(entry) != functions[builtin] || Memory::Read32(entry + 4) != kinds[builtin])
                MH3_THROW(std::runtime_error("Custom model texture mapping has no native producer: " + std::to_string(mode)));

            if (builtin) {
                mh3_texture_mapping_input input{};
                Checked(mh3_texture_mapping_read({Memory::GetPointer(0x80688d94, 0x1a34), 0x1a34}, {Bytes(0x80686368, MH3_LIGHT_STATE_BYTES), MH3_LIGHT_STATE_BYTES}, builtin,
                                                 slot.camera_reference, slot.light_reference, &input));
                Checked(mh3_texture_mapping(builtin, &input, &out.maps[i]));
            } else
                Checked(mh3_texture_mapping(0, nullptr, &out.maps[i]));

            out.map_sources[i] = kinds[builtin];
        }
    }
}

void ExecuteModelPrep(NativeOwner owner, ModelPrepMirror& mirror, const ModelPrepInputs& in, const ModelPrepSrt* srt, const mh3_matrix34* direct_position,
                      const mh3_matrix34* direct_normal, ModelPrepShapeResult* shape) {
    if (&mirror == &renderMirror)
        renderMirrorUsed = true;

    if (in.invalidate)
        mirror.Invalidate(in.invalidate);

    if (in.selectors)
        mirror.selectors = in.selector_state;

    Prep prep{owner, mirror, in, srt};

    if (in.material)
        prep.Material();
    else if (in.indirect_operator)
        prep.Indirect();

    if (in.visible && shape)
        prep.Shape(direct_position, direct_normal, *shape);
}
