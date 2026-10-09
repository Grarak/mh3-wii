// Deferred model preparation (model_render_prep.h). The executor follows the
// original 80076220 step order and the 800879DC/80087B34 shape setup exactly,
// against a host mirror of the G3DState caches and the given owner's state.
#include "model_render_prep.h"
#include "memory.h"
#include "native_graphics.h"
#include "native_frame_packet.h"
#include "runtime/native/adapters/model_material_bridge.h"
#include "runtime/native/adapters/model_pose_bridge.h"
#include "runtime/native/adapters/model_state_bridge.h"
#include "runtime/native/mh3_model_material_prep.h"
#include "runtime/native/mh3_model_prepare_cache.h"
#include <array>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>

namespace {
constexpr uint32_t Genmode = 0x80685300, Coordinates = 0x8068530c, TextureCache = 0x80685380, PaletteCache = 0x806854a0, ShapeCache = 0x80685504, TextureSources = 0x80685510;
constexpr uint32_t Sync = 0x80796db0, CurrentValid = 0x80796db4, PostIdentity = 0x80796db8, Pipeline = 0x80796dbc, DepthCompare = 0x80796dc8;
void Checked(mh3_res_status status) {
    if (status != MH3_RES_OK)
        throw std::runtime_error("Deferred model preparation failed: " + std::to_string(status));
}
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
uint32_t pendingInvalidate = 0;
// Game thread: the frozen typed blocks of recently captured material scopes.
// While the live material revision is unchanged, every live block keeps its
// version, so a scope with the same records reuses them.
struct FrozenSet {
    uint32_t material = 0;
    uint32_t slots[MH3_MAT_RESOURCE_COUNT]{};
    uint64_t live = UINT64_MAX;
    std::shared_ptr<const NativeFrozenMaterialBlock> pipeline, pixel, colors, indirect, texgen;
};
std::array<FrozenSet, 512> frozenSets;
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
struct Prep {
    NativeOwner owner;
    ModelPrepMirror& m;
    const ModelPrepInputs& in;
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
    void Group(const NativeFrozenMaterialBlock* block, mh3_model_material_block kind, bool xf = false) {
        if (!block)
            return;
        const auto mode = Mode();
        const uint32_t count = kind == MH3_MODEL_MATERIAL_INDIRECT ? mode.indirect : kind == MH3_MODEL_MATERIAL_TEXGEN ? mode.texgens : 1;
        if (!xf)
            PublishGenmode(false);
        sync() = 0;
        if (count)
            OverlayNativeModelMaterialGroup(block->fields, kind, count, owner);
        if (xf)
            PublishGenmode(true);
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
        const uint32_t resource = in.pipeline_address;
        if (!resource || !in.pipeline)
            return;
        const auto& block = *in.pipeline;
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
        if (in.indirect)
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
        uint8_t changed = 0;
        Checked(mh3_resource_objects_cache(m.At(PaletteCache, 98), Span(in.palette_objects, 100), 1, &changed));
        const uint32_t present = Word(in.palette_objects);
        for (uint32_t i = 0; i < 8; i++)
            if (present & (1u << i))
                BindNativePalette(i, in.palettes[i], owner);
        Checked(mh3_texture_cache_palette_changed(m.At(TextureCache, 257), changed));
    }
    void Textures() {
        uint8_t changed = 0;
        Checked(mh3_resource_objects_cache(m.At(TextureCache, 257), Span(in.texture_objects, 260), 0, &changed));
        const uint32_t present = Word(in.texture_objects);
        for (uint32_t i = 0; i < 8; i++) {
            if (!(present & (1u << i)))
                continue;
            auto binding = in.textures[i];
            binding.map_id = i;
            BindNativeTexture(binding, owner);
            if (changed & (1u << i)) {
                if (binding.source.width > UINT16_MAX || binding.source.height > UINT16_MAX)
                    throw std::runtime_error("Native G3D texture dimensions exceed coordinate cache");
                Checked(mh3_texture_scale_size(m.At(Coordinates, 36), i, uint16_t(binding.source.width), uint16_t(binding.source.height)));
                if (!binding.source.width || !binding.source.height)
                    throw std::runtime_error("Native G3D map size is invalid");
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
        mh3_texture_matrices matrices{};
        uint32_t next = 0;
        Checked(mh3_texture_matrices_prepare(&in.srt, in.maps, in.map_sources, Word(m.At(PostIdentity, 4).data), &matrices, &next));
        for (uint32_t i = 0; i < 8; i++) {
            PutWord(m.At(TextureSources + i * 4, 4).data, matrices.source[i]);
            if (matrices.apply & (1u << i))
                SetNativeTextureMatrix(64 + i * 3, &matrices.matrix[i].v[0][0], true, owner);
        }
        PutWord(m.At(PostIdentity, 4).data, next);
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
        Group(in.pixel, MH3_MODEL_MATERIAL_PIXEL);
        Group(in.colors, MH3_MODEL_MATERIAL_COLORS);
        Indirect();
        if (has(MH3_MAT_CHANNELS))
            Channels();
        Group(in.texgen, MH3_MODEL_MATERIAL_TEXGEN, true);
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

ModelPrepMode NativeModelPrepMode() {
    static const ModelPrepMode mode = [] {
        const char* value = std::getenv("MH3_RENDER_PREP");
        if (value && value[0] == '0')
            return ModelPrepMode::GameThread;
#ifdef MH3_DEVELOPMENT
        if (value && value[0] == '2')
            return ModelPrepMode::Verify;
#endif
        return ModelPrepMode::Deferred;
    }();
    return mode;
}

mh3_res_mut_span ModelPrepMirror::At(uint32_t address, size_t size) {
    if (address >= BaseA && address - BaseA + size <= SizeA)
        return {a + (address - BaseA), size};
    if (address >= BaseB && address - BaseB + size <= SizeB)
        return {b + (address - BaseB), size};
    throw std::runtime_error("Deferred model preparation has no mirror for " + std::to_string(address));
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
    renderMirror.Invalidate(0x7ff);
}
uint32_t TakeNativeModelPrepInvalidate() {
    const uint32_t fields = pendingInvalidate;
    pendingInvalidate = 0;
    return fields;
}
void NoteNativeModelPrepInvalidate(uint32_t fields) {
    pendingInvalidate |= fields;
}

void CaptureModelPrepMaterial(ModelPrepInputs& in, const mh3_model_material_scope& scope) {
    const auto& slots = scope.selected.resource_slots;
    in.present = 0;
    for (unsigned i = 0; i < MH3_MAT_RESOURCE_COUNT; i++)
        if (slots[i])
            in.present |= uint16_t(1u << i);
    const auto bytes = [](uint32_t address, size_t size) { return static_cast<const uint8_t*>(Memory::GetPointer(address, size)); };
    if (slots[MH3_MAT_MISC]) {
        mh3_model_material_misc misc{};
        Checked(mh3_model_material_misc_read({bytes(slots[MH3_MAT_MISC], 12), 12}, &misc));
        in.depth_before_texture = misc.depth_before_texture;
    }
    if (const uint32_t objects = slots[MH3_MAT_PALETTE_OBJECTS]) {
        CopyObjects(in.palette_objects, bytes(objects, 100), 12);
        const uint32_t present = Word(in.palette_objects);
        for (uint32_t i = 0; i < 8; i++)
            if (present & (1u << i))
                in.palettes[i] = ReadNativePaletteObject(objects + 4 + i * 12);
    }
    if (const uint32_t objects = slots[MH3_MAT_TEXTURE_OBJECTS]) {
        CopyObjects(in.texture_objects, bytes(objects, 260), 32);
        const uint32_t present = Word(in.texture_objects);
        for (uint32_t i = 0; i < 8; i++)
            if (present & (1u << i))
                in.textures[i] = ReadNativeTextureObject(objects + 4 + i * 32);
    }
    if (slots[MH3_MAT_GENERAL_MODE])
        std::memcpy(in.genmode, bytes(slots[MH3_MAT_GENERAL_MODE], 8), 8);
    in.pipeline_address = slots[MH3_MAT_TEV];
    if (in.pipeline_address > UINT32_MAX - 32)
        throw std::runtime_error("Model pipeline offset overflows");
    auto& set = frozenSets[((scope.material_address >> 4) * 2654435761u) >> 23];
    if (set.material == scope.material_address && set.live != ReadNativeLiveMaterialRevision() && !std::memcmp(set.slots, slots, sizeof set.slots)) {
        // Some live block changed (animated materials publish every frame):
        // refreeze only this material's blocks whose generation moved.
        const auto refresh = [](std::shared_ptr<const NativeFrozenMaterialBlock>& frozen, uint32_t address, mh3_model_material_block kind) {
            if (!address)
                return;
            const auto& live = ReadNativeLiveMaterialBlockView(address, kind);
            if (!frozen || frozen->fields.generation != live.fields.generation)
                frozen = FreezeNativeLiveMaterialBlock(live);
        };
        refresh(set.pipeline, in.pipeline_address ? in.pipeline_address + 32 : 0, MH3_MODEL_MATERIAL_PIPELINE);
        refresh(set.pixel, slots[MH3_MAT_PIXEL_STATE], MH3_MODEL_MATERIAL_PIXEL);
        refresh(set.colors, slots[MH3_MAT_TEV_COLORS], MH3_MODEL_MATERIAL_COLORS);
        refresh(set.indirect, slots[MH3_MAT_INDIRECT_MATRIX_SCALE], MH3_MODEL_MATERIAL_INDIRECT);
        refresh(set.texgen, slots[MH3_MAT_TEXCOORD_GENERATION], MH3_MODEL_MATERIAL_TEXGEN);
        set.live = ReadNativeLiveMaterialRevision();
    }
    if (set.material != scope.material_address || set.live != ReadNativeLiveMaterialRevision() || std::memcmp(set.slots, slots, sizeof set.slots)) {
        const auto freeze = [](uint32_t address, mh3_model_material_block kind) -> std::shared_ptr<const NativeFrozenMaterialBlock> {
            if (!address)
                return nullptr;
            return FreezeNativeLiveMaterialBlock(ReadNativeLiveMaterialBlockView(address, kind));
        };
        set.pipeline = freeze(in.pipeline_address ? in.pipeline_address + 32 : 0, MH3_MODEL_MATERIAL_PIPELINE);
        set.pixel = freeze(slots[MH3_MAT_PIXEL_STATE], MH3_MODEL_MATERIAL_PIXEL);
        set.colors = freeze(slots[MH3_MAT_TEV_COLORS], MH3_MODEL_MATERIAL_COLORS);
        set.indirect = freeze(slots[MH3_MAT_INDIRECT_MATRIX_SCALE], MH3_MODEL_MATERIAL_INDIRECT);
        set.texgen = freeze(slots[MH3_MAT_TEXCOORD_GENERATION], MH3_MODEL_MATERIAL_TEXGEN);
        // Freezing may admit blocks; record the revision they were read at.
        set.material = scope.material_address;
        std::memcpy(set.slots, slots, sizeof set.slots);
        set.live = ReadNativeLiveMaterialRevision();
    }
    in.pipeline = set.pipeline.get();
    in.pixel = set.pixel.get();
    in.colors = set.colors.get();
    in.indirect = set.indirect.get();
    in.texgen = set.texgen.get();
    if (slots[MH3_MAT_CHANNELS])
        std::memcpy(in.channels, bytes(slots[MH3_MAT_CHANNELS], 40), 40);
    if (const uint32_t resource = slots[MH3_MAT_TEXTURE_SRT]) {
        Checked(mh3_texture_srt_read_for_prepare({bytes(resource, 0x248), 0x248}, &in.srt));
        // The constructor defines these exact built-in mapping functions. Custom
        // slots may change their callback and type at runtime; reject those until
        // their own typed native producer is implemented.
        constexpr uint32_t functions[]{0x800850a0, 0x80084694, 0x80084a78, 0x80084694, 0x80084c00};
        constexpr uint32_t kinds[]{0, 1, 2, 1, 1};
        for (uint32_t i = 0; i < 8; i++) {
            in.map_sources[i] = 0;
            const auto& slot = in.srt.slot[i];
            if (!(slot.flags & 1) || !slot.map_mode)
                continue;
            const uint32_t mode = slot.map_mode, entry = 0x80685560 + mode * 8;
            const uint32_t builtin = mode < 5 ? mode : 0;
            if (Memory::Read32(entry) != functions[builtin] || Memory::Read32(entry + 4) != kinds[builtin])
                throw std::runtime_error("Custom model texture mapping has no native producer: " + std::to_string(mode));
            if (builtin) {
                mh3_texture_mapping_input input{};
                Checked(mh3_texture_mapping_read({Memory::GetPointer(0x80688d94, 0x1a34), 0x1a34}, {bytes(0x80686368, MH3_LIGHT_STATE_BYTES), MH3_LIGHT_STATE_BYTES}, builtin,
                                                 slot.camera_reference, slot.light_reference, &input));
                Checked(mh3_texture_mapping(builtin, &input, &in.maps[i]));
            } else
                Checked(mh3_texture_mapping(0, nullptr, &in.maps[i]));
            in.map_sources[i] = kinds[builtin];
        }
    }
}

void ExecuteModelPrep(NativeOwner owner, ModelPrepMirror& mirror, const ModelPrepInputs& in, const mh3_matrix34* direct_position, const mh3_matrix34* direct_normal,
                      ModelPrepShapeResult* shape) {
    if (in.invalidate)
        mirror.Invalidate(in.invalidate);
    if (in.selectors)
        mirror.selectors = in.selector_state;
    Prep prep{owner, mirror, in};
    if (in.material)
        prep.Material();
    else if (in.indirect_operator)
        prep.Indirect();
    if (in.visible && shape)
        prep.Shape(direct_position, direct_normal, *shape);
}
