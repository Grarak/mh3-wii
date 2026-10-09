// Native model material/mesh submission boundary. Portable C owns decoding,
// order, coordinate scales and matrix math; these adapters own CPU records and
// forward direct material/mesh publications. No guest/GX/FIFO calls.
#include "abi_bridge.h"
#include "memory.h"
#include "native_graphics.h"
#include "runtime/native/adapters/model_scope_bridge.h"
#include "runtime/native/adapters/model_catalog_bridge.h"
#include "runtime/native/adapters/model_material_bridge.h"
#include "runtime/native/adapters/model_pose_bridge.h"
#include "runtime/native/adapters/model_state_bridge.h"
#include "runtime/native/mh3_model_material_prep.h"
#include "frame_split.h"
#include "native_frame_packet.h"
#include <array>
#include <cstring>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

const mh3_res_memory& MH3NativeResourceMemory();
namespace {
constexpr uint32_t genmode = 0x80685300, coordinate_cache = 0x8068530c, sync_address = 0x80796db0;
constexpr mh3_matrix_runtime_locations locations{0x80685530, 0x80685510, 0x80796db4, 0x80688d94, 0x80686368};
mh3_model_texcoord_sizes map_sizes{}, coordinate_sizes{};
uint8_t coordinate_map_known = 0;
uint32_t pipeline_address = 0, operator_address = 0;
uint64_t pipeline_revision = 0;
mh3_indirect_output current_operator{};
void Checked(mh3_res_status status) {
    if (status != MH3_RES_OK)
        throw std::runtime_error("Native model scope failed: " + std::to_string(status));
}
void PackChecked(mh3_pack_status status) {
    if (status != MH3_PACK_OK)
        throw std::runtime_error("Native model resident mesh failed: " + std::to_string(status));
}
mh3_res_span Bytes(uint32_t address, size_t size) {
    return {Memory::GetPointer(address, size), size};
}
mh3_res_mut_span Mutable(uint32_t address, size_t size) {
    return {Memory::GetPointer(address, size), size};
}
uint32_t Resource(uint32_t reference, bool required = false) {
    uint32_t data = 0;
    Checked(mh3_res_runtime_reference(&MH3NativeResourceMemory(), reference, required, &data));
    return data;
}
uint32_t Canonical(uint32_t address) {
    return address ? (address & 0x3fffffffu) | 0x80000000u : 0;
}
void Publish(const mh3_native_material_state& patch, uint32_t top = 0) {
    OverlayNativeMaterial(patch, top);
}
mh3_model_genmode Mode() {
    mh3_model_genmode mode{};
    Checked(mh3_model_genmode_read(Bytes(genmode, 12), &mode));
    return mode;
}
void Genmode(bool xf) {
    mh3_model_genmode value{};
    uint32_t top = 0;
    int apply = 0;
    Checked(mh3_model_genmode_publish_counts(Mutable(genmode, 12), xf, &value, &top, &apply));
    if (apply)
        OverlayNativeMaterialTop(top, value.texgens, value.channels, value.tevs, value.indirect, value.cull);
}
void Group(uint32_t address, mh3_model_material_block kind, bool xf = false) {
    if (!address)
        return;
    const auto mode = Mode();
    const uint32_t count = kind == MH3_MODEL_MATERIAL_INDIRECT ? mode.indirect : kind == MH3_MODEL_MATERIAL_TEXGEN ? mode.texgens : 1;
    const auto* fields = count ? &ReadNativeLiveMaterialBlockView(address, kind).fields : nullptr;
    if (!xf)
        Genmode(false);
    Memory::GetPointer(sync_address, 1)[0] = 0;
    if (count)
        OverlayNativeModelMaterialGroup(*fields, kind, count);
    if (xf)
        Genmode(true);
}
mh3_model_material_scope Scope(uint32_t reference, uint32_t swaps) {
    const uint32_t material = Resource(reference);
    uint32_t version = 0;
    if (material) {
        uint32_t parent = 0;
        Checked(mh3_res_runtime_offset(&MH3NativeResourceMemory(), reference, int32_t(Memory::Read32(material + 4)), 4, &parent));
        const auto owner = ResolveNativeModelOwner(parent);
        (void)ResolveNativeModelResource(owner, material, true);
        mh3_catalog_model model{};
        PackChecked(mh3_model_catalog_model(owner.catalog, owner.catalog_model_index, &model));
        version = model.revision;
    }
    mh3_model_material_bindings replacements{};
    if (swaps)
        Checked(mh3_model_material_bindings_read(Bytes(swaps, 56), &replacements));
    mh3_model_material_scope scope{};
    Checked(mh3_model_material_scope_read(&MH3NativeResourceMemory(), material, version, &replacements, &scope));
    return scope;
}
struct PrepareContext {
    mh3_native_light_slots lights{};
};
void OperatorKnown(uint32_t address) {
    Checked(mh3_matrix_runtime_operator(&MH3NativeResourceMemory(), address, 0x80088a5c, 0x800888bc));
    const uint32_t table = Memory::Read32(address);
    if (Memory::Read32(table + 8) != 0x80088aac)
        throw std::runtime_error("Custom indirect apply callback has no native producer");
}
void Indirect(uint32_t resource, uint32_t op) {
    if (resource)
        Group(resource, MH3_MODEL_MATERIAL_INDIRECT);
    if (!op)
        return;
    if (op != operator_address || !current_operator.active)
        throw std::runtime_error("Indirect matrix operator has no completed native producer");
    OperatorKnown(op);
    mh3_pack_indirect_matrix base[3]{}, matrices[3];
    if (resource) {
        const auto& source = ReadNativeLiveMaterialBlockView(resource, MH3_MODEL_MATERIAL_INDIRECT);
        std::memcpy(base, source.fields.indirect_matrix, sizeof base);
    }
    Checked(mh3_model_indirect_matrices(base, &current_operator, matrices));
    OverlayNativeMaterialIndirectMatrices(matrices);
}
mh3_res_status PrepareStep(void* opaque, const mh3_model_material_step* step) {
    auto& context = *static_cast<PrepareContext*>(opaque);
    const uint32_t resource = step->resource_address;
    switch (step->action) {
    case MH3_MATERIAL_PREP_FOG: {
        mh3_model_material_misc misc{};
        Checked(mh3_model_material_misc_read(Bytes(resource, 12), &misc));
        mh3_native_fog fog{};
        int apply = 0;
        Checked(mh3_fog_state_select(Mutable(0x80685d60, 1544), uint32_t(int32_t(misc.fog_reference)), &fog, &apply));
        if (apply)
            SetNativeFog(fog);
        break;
    }
    case MH3_MATERIAL_PREP_MISC: {
        mh3_model_material_misc misc{};
        Checked(mh3_model_material_misc_read(Bytes(resource, 12), &misc));
        int apply = 0;
        Checked(mh3_depth_compare_cache(Mutable(0x80796dc8, 5), Mutable(sync_address, 1), misc.depth_before_texture, &apply));
        if (apply)
            SetNativeDepthCompareBeforeTexture(misc.depth_before_texture);
        break;
    }
    case MH3_MATERIAL_PREP_PALETTES:
        SelectNativeModelPaletteObjects(0x806854a0, resource);
        break;
    case MH3_MATERIAL_PREP_TEXTURES:
        SelectNativeModelTextureObjects(0x80685380, resource);
        break;
    case MH3_MATERIAL_PREP_GENMODE:
        Checked(mh3_model_genmode_select(Mutable(genmode, 12), Bytes(resource, 8)));
        break;
    case MH3_MATERIAL_PREP_PIPELINE:
        LoadNativeModelPipeline(resource);
        break;
    case MH3_MATERIAL_PREP_PIXEL:
        Group(resource, MH3_MODEL_MATERIAL_PIXEL);
        break;
    case MH3_MATERIAL_PREP_COLORS:
        Group(resource, MH3_MODEL_MATERIAL_COLORS);
        break;
    case MH3_MATERIAL_PREP_INDIRECT:
        Indirect(resource, step->indirect_operator);
        break;
    case MH3_MATERIAL_PREP_LIGHTS: {
        mh3_model_material_misc misc{};
        Checked(mh3_model_material_misc_read(Bytes(resource, 12), &misc));
        context.lights = SelectNativeModelLightSet(0x80686368, uint32_t(int32_t(misc.light_reference)));
        break;
    }
    case MH3_MATERIAL_PREP_CHANNELS: {
        const uint32_t masks[]{context.lights.diffuse_color, context.lights.diffuse_alpha, context.lights.specular_color, context.lights.specular_alpha};
        mh3_pack_channel channels[2];
        Checked(mh3_model_channels_prepare_channels(Bytes(resource, 40), Mode().channels, masks, reinterpret_cast<const uint8_t*>(&context.lights.ambient), step->force_light_off, channels));
        OverlayNativeMaterialChannels(channels);
        Genmode(true);
        break;
    }
    case MH3_MATERIAL_PREP_TEXGEN:
        Group(resource, MH3_MODEL_MATERIAL_TEXGEN, true);
        break;
    case MH3_MATERIAL_PREP_SRT:
        LoadNativeModelTextureSrt(resource);
        break;
    }
    return MH3_RES_OK;
}
struct ShapeSources {
    uint64_t source_generation, resident_generation;
    uintptr_t resident_handle;
    std::array<mh3_native_vertex_source, 12> source;
    uint16_t known;
};
std::unordered_map<uint32_t, ShapeSources> shape_sources;
const ShapeSources& Sources(const NativeModelOwner& owner, uint32_t reference, uint32_t shape) {
    const uint32_t key = Canonical(shape);
    const auto cached = shape_sources.find(key);
    if (cached != shape_sources.end() && cached->second.source_generation == owner.source_generation && cached->second.resident_handle == owner.resident_handle &&
        cached->second.resident_generation == owner.resident_generation)
        return cached->second;
    ShapeSources result{};
    result.source_generation = owner.source_generation;
    result.resident_handle = owner.resident_handle;
    result.resident_generation = owner.resident_generation;
    for (uint32_t i = 0; i < 12; i++) {
        uint32_t resource = 0;
        Checked(mh3_res_runtime_shape_array(&MH3NativeResourceMemory(), reference, i, &resource));
        if (!resource)
            continue;
        const auto kind = i == 0 ? MH3_VERTEX_POSITION : i == 1 ? MH3_VERTEX_NORMAL : i < 4 ? MH3_VERTEX_COLOR : MH3_VERTEX_UV;
        result.source[i] = ReadNativeVertexResourceData(resource, kind);
        result.known |= uint16_t(1u << i);
    }
    // Owned scalar metadata only. Bound the cache across recycled scene heaps;
    // evicting it requires fresh CPU header resolution, never GPU repacking.
    if (shape_sources.size() >= 8192)
        shape_sources.clear();
    shape_sources.insert_or_assign(key, result);
    return shape_sources.at(key);
}
NativeModelOwner ShapeOwner(uint32_t reference, uint32_t* shape) {
    *shape = Resource(reference, true);
    uint32_t parent = 0;
    Checked(mh3_res_runtime_shape_parent(&MH3NativeResourceMemory(), reference, &parent));
    auto owner = ResolveNativeModelOwner(parent);
    (void)ResolveNativeModelResource(owner, *shape, false);
    return owner;
}
// A shape's owner and authored vertex sources, resolved once per dispatch.
struct ShapeContext {
    uint32_t reference, shape;
    NativeModelOwner owner;
    const ShapeSources* sources;
};
ShapeContext ResolveShape(uint32_t reference) {
    ShapeContext context{reference, 0, {}, nullptr};
    context.owner = ShapeOwner(reference, &context.shape);
    context.sources = &Sources(context.owner, reference, context.shape);
    return context;
}
void PrePrimitive(const ShapeContext& context) {
    const uint32_t shape = context.shape;
    const auto& sources = *context.sources;
    PrepareNativeModelCoordinateSizes(Mode().texgens);
    Genmode(true);
    Checked(mh3_model_shape_cache(Mutable(0x80685504, 12), Bytes(shape + 12, 12)));
    Memory::GetPointer(sync_address, 1)[0] = 0;
    for (uint32_t i = 0; i < 12; i++)
        if (sources.known & (1u << i))
            StoreNativeVertexSource(i, sources.source[i]);
}
void PrePrimitive(uint32_t reference) {
    if (Resource(reference))
        PrePrimitive(ResolveShape(reference));
}
void Replacements(const ShapeContext& context, const mh3_model_material_scope& scope) {
    const uint32_t shape = context.shape;
    const auto& originals = *context.sources;
    for (unsigned i = 0; i < 4; i++) {
        const uint32_t table = scope.selected.vertex_tables[i < 2 ? i : 2];
        if (!table)
            continue;
        const int32_t id = int16_t(Memory::Read16(shape + 0x48 + i * 2));
        if (id < 0)
            continue;
        if (uint64_t(table) + uint32_t(id) * 4 > UINT32_MAX)
            throw std::runtime_error("Model replacement vertex table overflows");
        const uint32_t resource = Memory::Read32(table + uint32_t(id) * 4);
        const auto kind = i == 0 ? MH3_VERTEX_POSITION : i == 1 ? MH3_VERTEX_NORMAL : MH3_VERTEX_COLOR;
        const auto source = ReadNativeVertexResourceData(resource, kind);
        if (i == 0 && Canonical(source.payload_address) != Canonical(originals.source[0].payload_address))
            throw std::runtime_error("Position replacement differs from the shape's authored array");
        StoreNativeVertexSource(i, source);
    }
}
} // namespace

void PublishNativeModelMapSize(uint32_t map, uint16_t width, uint16_t height) {
    if (map >= 8 || !width || !height)
        throw std::runtime_error("Native G3D map size is invalid");
    map_sizes.width[map] = width;
    map_sizes.height[map] = height;
    map_sizes.known |= uint8_t(1u << map);
}
const mh3_model_texcoord_sizes& ReadNativeModelCoordinateSizes() {
    return coordinate_sizes;
}
void PrepareNativeModelCoordinateSizes(uint32_t count) {
    mh3_model_texcoord_sizes sizes{};
    uint8_t written = 0;
    int apply = 0;
    Checked(mh3_model_coordinate_prepare(Mutable(coordinate_cache, 108), count, coordinate_map_known, &map_sizes, &coordinate_sizes, &sizes, &written, &apply));
    if (apply)
        coordinate_sizes = sizes;
}
void LoadNativeModelPipeline(uint32_t resource) {
    if (!resource)
        return;
    if (resource > UINT32_MAX - 32)
        throw std::runtime_error("Model pipeline offset overflows");
    const auto& fields = ReadNativeLiveMaterialBlockView(resource + 32, MH3_MODEL_MATERIAL_PIPELINE).fields;
    const auto generation = fields.generation;
    if (Memory::Read32(0x80796dbc) == resource && pipeline_address == resource && pipeline_revision == generation)
        return;
    uint8_t table[8], known = 0;
    ReadNativeModelCoordinateMap(resource + 32, table, &known);
    Checked(mh3_model_coordinate_table(Mutable(coordinate_cache, 108), table));
    coordinate_map_known = known;
    Genmode(false);
    Memory::GetPointer(sync_address, 1)[0] = 0;
    OverlayNativeModelMaterialGroup(fields, MH3_MODEL_MATERIAL_PIPELINE);
    Memory::Write32(0x80796dbc, resource);
    pipeline_address = resource;
    pipeline_revision = generation;
    PrepareNativeModelCoordinateSizes(Mode().texgens);
}
void PrepareNativeModelMaterial(uint32_t reference, uint32_t control, uint32_t swaps, uint32_t op, bool ignore) {
    const auto scope = Scope(reference, swaps);
    PrepareContext context{};
    Checked(mh3_model_material_prepare_scope(&scope, control, op, ignore, PrepareStep, &context));
}
namespace {
// One model mesh draw split at the shading boundary. The game thread prepares
// it (material scope, shape matrix selection, row selectors) and records it in
// the frame packet with its shading state; the render thread rebuilds the
// matrix palette from the copied pose and submits.
struct ModelItem {
    NativeModelOwner owner;
    const mh3_prepared_mesh* prepared;
    mh3_matrix_runtime_storage storage;
    mh3_shape_matrix_input input;
    uint32_t pose;
    mh3_model_texcoord_sizes coordinate_sizes;
};
// Pose arrays as the draw found them: copied once per binding and pass.
struct ModelPose {
    uint32_t address[3];
    std::vector<uint8_t> bytes;
    size_t offset[3];
    mh3_matrix_sources layout;
};
// Items and poses of one frame packet, indexed by its exchange slot: the game
// thread fills the producer's store, the render thread reads the consumer's.
struct ModelStore {
    std::vector<ModelItem> items;
    std::vector<ModelPose> poses;
    // Poses are shared only within one model draw: a later view pass may
    // rewrite the same matrix arrays.
    size_t itemCount = 0, poseCount = 0, poseShared = 0;
    uint64_t sequence = UINT64_MAX;
};
std::array<ModelStore, 3> stores;
// Frames that are not recorded still prepare (guest/selector side effects).
ModelStore scratch;
ModelStore& Store() {
    if (!NativeGraphicsFrameShouldRender()) {
        scratch.itemCount = scratch.poseCount = scratch.poseShared = 0;
        return scratch;
    }
    auto& store = stores.at(NativeFramePacketSlot());
    if (store.sequence != NativeFramePacketSequence()) {
        store.sequence = NativeFramePacketSequence();
        store.itemCount = store.poseCount = store.poseShared = 0;
    }
    return store;
}
uint32_t CapturePose(ModelStore& capture, const uint32_t addresses[3], const mh3_matrix_sources& sources) {
    for (size_t i = capture.poseCount; i-- > capture.poseShared;)
        if (!std::memcmp(capture.poses[i].address, addresses, sizeof capture.poses[i].address))
            return uint32_t(i);
    if (capture.poseCount == capture.poses.size())
        capture.poses.emplace_back();
    auto& pose = capture.poses[capture.poseCount];
    std::memcpy(pose.address, addresses, sizeof pose.address);
    pose.layout = sources;
    const mh3_matrix_table* tables[3]{&sources.positions, &sources.normals, &sources.environment};
    size_t total = 0;
    for (unsigned i = 0; i < 3; ++i)
        total += tables[i]->bytes.data ? tables[i]->bytes.size : 0;
    pose.bytes.resize(total);
    size_t offset = 0;
    for (unsigned i = 0; i < 3; ++i) {
        pose.offset[i] = offset;
        if (!tables[i]->bytes.data)
            continue;
        std::memcpy(pose.bytes.data() + offset, tables[i]->bytes.data, tables[i]->bytes.size);
        offset += tables[i]->bytes.size;
    }
    return uint32_t(capture.poseCount++);
}
mh3_matrix_sources PoseSources(const ModelPose& pose) {
    auto sources = pose.layout;
    mh3_matrix_table* tables[3]{&sources.positions, &sources.normals, &sources.environment};
    for (unsigned i = 0; i < 3; ++i)
        if (tables[i]->bytes.data)
            tables[i]->bytes.data = pose.bytes.data() + pose.offset[i];
    return sources;
}
void PrepareModelItem(ModelStore& capture, ModelItem& item, const ShapeContext& context, uint32_t direct_position, uint32_t direct_normal) {
    const uint32_t reference = context.reference, shape = context.shape;
    item.owner = context.owner;
    const auto& prepared = ReadNativePreparedMesh(item.owner, shape);
    item.prepared = &prepared;
    const auto& originals = *context.sources;
    for (uint32_t i = 0; i < prepared.immutable.layout.count; i++) {
        const uint32_t semantic = prepared.immutable.layout.attributes[i].semantic;
        mh3_native_vertex_source selected{};
        // These are immutable authoritative primitive matrix indices packed by
        // the extractor, not replacements for one of the twelve array sources.
        if (semantic >= 12 && prepared.palette_batch_count)
            continue;
        if (!(originals.known & (1u << semantic)) || !ReadNativeVertexSource(semantic, &selected) || Canonical(selected.payload_address) != Canonical(originals.source[semantic].payload_address) ||
            selected.source_generation)
            throw std::runtime_error("Resident model vertex binding requires a converted authored source: " + std::to_string(semantic));
    }
    const auto binding = ReadNativeModelMatrixSources(locations.arrays);
    mh3_matrix_sources sources{};
    if (binding.address[0])
        sources = ReadNativeModelPose(binding.address, item.owner.model_address);
    else if (!direct_position)
        throw std::runtime_error("Indexed model matrix has no completed pose producer");
    mh3_matrix_sources ignored{};
    Checked(mh3_matrix_runtime_shape(&MH3NativeResourceMemory(), &locations, reference, direct_position, direct_normal, &item.storage, &item.input, &ignored));
    if (item.input.matrix_id < 0 && !prepared.palette_batch_count)
        throw std::runtime_error("Envelope model requires converted skinning palette records");
    item.pose = CapturePose(capture, binding.address, sources);
    PrepareNativeModelCoordinateSizes(Mode().texgens);
    if (item.input.matrix_id >= 0) {
        // Row selectors depend on the shape's matrix sources, not pose values.
        mh3_model_palette produced{};
        Checked(mh3_model_palette_rigid(&item.input, &sources, &produced));
        bool identity = true;
        for (uint32_t row : produced.texture_ids)
            if (row != 60)
                identity = false;
        if (!identity || !Memory::Read8(locations.current_valid))
            PublishNativeModelMatrixRows(produced.texture_ids);
        Memory::Write8(locations.current_valid, identity ? 1 : 0);
    } else if (!Memory::Read8(locations.current_valid)) {
        // Envelope setup selects identity CURRENT tex-matrix rows; it does not
        // load identity position/normal slots. Per-vertex indices select the
        // actual ordered loads/inherited values in the converted primitives.
        uint32_t rows[8];
        for (auto& row : rows)
            row = 60;
        PublishNativeModelMatrixRows(rows);
        Memory::Write8(locations.current_valid, 1);
    }
    Genmode(true);
    Memory::GetPointer(sync_address, 1)[0] = 0;
    // Publish the producer's row/dual fields once. Palette batches consume the
    // owner's material directly; they do not submit a full material copy.
    ApplyNativeModelSelectors();
    if (item.input.matrix_id < 0)
        Memory::Write8(locations.current_valid, 0);
}
void ExecuteModelItem(const ModelStore& capture, const ModelItem& item) {
    const auto& prepared = *item.prepared;
    const auto& owner = item.owner;
    const auto sources = PoseSources(capture.poses[item.pose]);
    auto input = item.input;
    if (input.direct_position)
        input.direct_position = &item.storage.direct_position;
    if (input.direct_normal)
        input.direct_normal = &item.storage.direct_normal;
    mh3_model_palette palette = ReadNativeModelPalette();
    if (input.matrix_id >= 0) {
        mh3_model_palette produced{};
        Checked(mh3_model_palette_rigid(&input, &sources, &produced));
        Checked(mh3_model_palette_overlay(&palette, &produced, &palette));
        PublishNativeModelPalette(palette);
        SetNativePositionMatrix(&produced.position[0].v[0][0]);
        if (produced.normal_valid & 1)
            SetNativeNormalMatrix(0, &produced.normal[0].v[0][0]);
        for (uint32_t i = 0; i < 10; i++)
            if (produced.texture_valid & (1u << i))
                SetNativeTextureMatrix(30 + i * 3, &produced.texture[i].v[0][0], true);
    }
    const auto& material = GetNativeMaterialState();
    float post[8][12]{};
    uint8_t postKnown = 0, post3x4 = 0;
    for (uint32_t i = 0; i < 8; i++) {
        bool matrix3x4 = false;
        if (GetNativeTextureMatrix(64 + i * 3, post[i], &matrix3x4)) {
            postKnown |= uint8_t(1u << i);
            post3x4 |= uint8_t(matrix3x4 ? 1u << i : 0);
        }
    }
    auto submit_one = [&](uint32_t batch_index) {
        mh3_resolved_mesh_submit submit{};
        if (prepared.palette_batch_count) {
            const auto& batch = prepared.palette_batches[batch_index];
            Checked(mh3_model_palette_apply_batch(&batch, &sources, &palette, &palette));
            PublishNativeModelPalette(palette);
            if (batch.position_known & 1)
                SetNativePositionMatrix(&palette.position[0].v[0][0]);
            for (uint32_t slot = 0; slot < 10; slot++) {
                if (batch.normal_known & (1u << slot))
                    SetNativeNormalMatrix(slot * 3, &palette.normal[slot].v[0][0]);
                if (batch.texture_known & (1u << slot))
                    SetNativeTextureMatrix(30 + slot * 3, &palette.texture[slot].v[0][0], true);
            }
            PackChecked(mh3_model_mesh_bind_batch(&prepared, batch_index, owner.resident_generation, owner.source_generation, ReadNativeModelPaletteRevision(), &palette, &submit));
        } else
            PackChecked(mh3_model_mesh_bind_pose(&prepared, owner.resident_generation, owner.source_generation, ReadNativeModelPaletteRevision(), &palette, &submit));
        submit.material = &material;
        submit.projection = GetNativeDrawState().projection;
        submit.texture_matrices = post;
        submit.texture_matrix_known = postKnown;
        submit.texture_matrix_3x4 = post3x4;
        submit.texcoord_sizes = &item.coordinate_sizes;
        SubmitNativeModelMesh(submit);
    };
    if (prepared.palette_batch_count)
        for (uint32_t i = 0; i < prepared.palette_batch_count; i++)
            submit_one(i);
    else
        submit_one(0);
}
} // namespace

namespace {
void SubmitShape(const ShapeContext& context, uint32_t direct_position, uint32_t direct_normal) {
    auto& capture = Store();
    if (capture.itemCount == capture.items.size())
        capture.items.emplace_back();
    auto& item = capture.items[capture.itemCount];
    {
        MH3_SPLIT_SCOPE(MH3_SPLIT_SHAPE_PREP);
        PrepareModelItem(capture, item, context, direct_position, direct_normal);
    }
    item.coordinate_sizes = coordinate_sizes;
    if (&capture == &scratch)
        return;
    MH3_SPLIT_SCOPE(MH3_SPLIT_MODEL_RECORD);
    RecordNativeModelDraw(uint32_t(capture.itemCount++));
}
} // namespace
void SubmitNativeModelShape(uint32_t reference, uint32_t direct_position, uint32_t direct_normal) {
    if (Resource(reference))
        SubmitShape(ResolveShape(reference), direct_position, direct_normal);
}
void BeginNativeModelDraw() {
    auto& capture = Store();
    capture.poseShared = capture.poseCount;
}
void ExecuteNativeModelDraw(uint32_t slot, uint32_t item) {
    const auto& capture = stores.at(slot);
    ExecuteModelItem(capture, capture.items.at(item));
}

extern "C" void MH3_ModelMaterialScope(CpuContext* cpu) {
    PrepareNativeModelMaterial(cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7] != 0);
}
REGISTER_NATIVE_FUNCTION_AS(0x80076220, MH3_ModelMaterialScope, "ModelMaterialScope");
extern "C" void MH3_ModelPipelineLoad(CpuContext* cpu) {
    LoadNativeModelPipeline(Resource(cpu->gpr[3]));
}
REGISTER_NATIVE_FUNCTION_AS(0x800865CC, MH3_ModelPipelineLoad, "ModelPipelineLoad");
extern "C" void MH3_ModelIndirectOperatorLoad(CpuContext* cpu) {
    const uint32_t resource = Resource(cpu->gpr[3]);
    if (resource)
        Indirect(resource, cpu->gpr[4]);
}
REGISTER_NATIVE_FUNCTION_AS(0x80086B0C, MH3_ModelIndirectOperatorLoad, "ModelIndirectOperatorLoad");
extern "C" void MH3_ModelShapePrePrimitive(CpuContext* cpu) {
    PrePrimitive(cpu->gpr[3]);
}
REGISTER_NATIVE_FUNCTION_AS(0x800879DC, MH3_ModelShapePrePrimitive, "ModelShapePrePrimitive");
extern "C" void MH3_ModelShapePrimitive(CpuContext* cpu) {
    SubmitNativeModelShape(cpu->gpr[3], cpu->gpr[4], cpu->gpr[5]);
}
REGISTER_NATIVE_FUNCTION_AS(0x80087B34, MH3_ModelShapePrimitive, "ModelShapePrimitive");
extern "C" void MH3_ModelIndirectMatrices(CpuContext* cpu) {
    current_operator = {};
    operator_address = 0;
    const uint32_t material = Resource(cpu->gpr[3]), shape = Resource(cpu->gpr[5]);
    if (material && shape && (Memory::Read8(material + 0x20) || Memory::Read8(material + 0x21) || Memory::Read8(material + 0x22))) {
        uint32_t parent = 0;
        Checked(mh3_res_runtime_shape_parent(&MH3NativeResourceMemory(), cpu->gpr[5], &parent));
        const auto owner = ResolveNativeModelOwner(parent);
        const auto binding = ReadNativeModelMatrixSources(locations.arrays);
        if (int32_t(Memory::Read32(shape + 8)) >= 0)
            (void)ReadNativeModelPose(binding.address, owner.model_address);
    }
    Checked(mh3_matrix_runtime_indirect(&MH3NativeResourceMemory(), &locations, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], &current_operator));
    if (!current_operator.active) {
        cpu->gpr[3] = 0;
        return;
    }
    operator_address = Resource(0x80793708, true);
    OperatorKnown(operator_address);
    Checked(mh3_matrix_store_indirect(Mutable(operator_address, 152), &current_operator));
    cpu->gpr[3] = operator_address;
}
REGISTER_NATIVE_FUNCTION_AS(0x80077814, MH3_ModelIndirectMatrices, "ModelIndirectMatrices");
extern "C" void MH3_ModelDispatch(CpuContext* cpu) {
    TraceNativeModelDispatch(cpu);
    mh3_model_material_scope scope;
    {
        MH3_SPLIT_SCOPE(MH3_SPLIT_MATERIAL_PREP);
        scope = Scope(cpu->gpr[3], cpu->gpr[8]);
        const bool ignore = !scope.material_address || (cpu->gpr[7] & 16);
        PrepareContext context{};
        Checked(mh3_model_material_prepare_scope(&scope, cpu->gpr[7], cpu->gpr[9], ignore, PrepareStep, &context));
    }
    uint32_t visible = 0;
    Checked(mh3_res_runtime_shape_visible(&MH3NativeResourceMemory(), cpu->gpr[4], &visible));
    if (!visible)
        return;
    if (scope.fur_address)
        throw std::runtime_error("Fur model needs native sampled-shell material preparation");
    // A dispatch resolves its shape's owner and sources once for the
    // primitive setup, replacements and submission. Without a shape only the
    // replacements (which require one) have an effect: they fail.
    if (!Resource(cpu->gpr[4])) {
        if (!(cpu->gpr[7] & 2))
            Replacements(ResolveShape(cpu->gpr[4]), scope);
        return;
    }
    const auto shape = [&] {
        MH3_SPLIT_SCOPE(MH3_SPLIT_SHAPE_PREP);
        return ResolveShape(cpu->gpr[4]);
    }();
    if (!(cpu->gpr[7] & 2)) {
        MH3_SPLIT_SCOPE(MH3_SPLIT_SHAPE_PREP);
        PrePrimitive(shape);
        Replacements(shape, scope);
    }
    if (cpu->gpr[7] & 4) {
        // The original republishes the whole material with front culling.
        OverlayNativeMaterialTop(MH3_MATERIAL_TOP_CULL, 0, 0, 0, 0, 1);
        std::memset(Memory::GetPointer(0x80685504, 12), 0, 12);
        Memory::Write8(sync_address, 1);
    }
    SubmitShape(shape, cpu->gpr[5], cpu->gpr[6]);
}
REGISTER_NATIVE_FUNCTION_AS(0x80076A58, MH3_ModelDispatch, "ModelDisplayListDispatch");
