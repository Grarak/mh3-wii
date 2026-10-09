// Native model material/mesh submission boundary. Portable C owns decoding,
// order, coordinate scales and matrix math; these adapters own CPU records and
// forward direct material/mesh publications. No guest/GX/FIFO calls.
#include "abi_bridge.h"
#include "fatal.h"
#include <aurora/fast_map.h>
#include "guest_span.h"
#include "guest_worker.h"
#include "memory.h"
#include "native_graphics.h"
#include "runtime/native/adapters/model_scope_bridge.h"
#include "runtime/native/adapters/model_catalog_bridge.h"
#include "runtime/native/adapters/model_material_bridge.h"
#include "runtime/native/adapters/model_pose_bridge.h"
#include "runtime/native/adapters/model_state_bridge.h"
#include "runtime/native/mh3_model_material_prep.h"
#include "runtime/native/mh3_model_prepare_cache.h"
#include "frame_split.h"
#ifdef MH3_DEVELOPMENT
#include "dev_options.h"
#endif
#include "native_frame_packet.h"
#include "native_shade_snapshot.h"
#include "model_render_prep.h"
#include "model_assets.h"
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

const mh3_res_memory& MH3NativeResourceMemory();
namespace {
constexpr uint32_t genmode = 0x80685300, coordinate_cache = 0x8068530c, sync_address = 0x80796db0;
constexpr mh3_matrix_runtime_locations locations{0x80685530, 0x80685510, 0x80796db4, 0x80688d94, 0x80686368};

// G3D cache mirrors (game thread draw state; recorded items carry their sizes).
constinit mh3_model_texcoord_sizes map_sizes{}, coordinate_sizes{};
constinit uint8_t coordinate_map_known = 0;
constinit uint32_t pipeline_address = 0, operator_address = 0;
constinit uint64_t pipeline_revision = 0;
constinit mh3_indirect_output current_operator{};
[[noreturn, gnu::cold, gnu::noinline]] void CheckedFailed(mh3_res_status status) {
    MH3_THROW(std::runtime_error("Native model scope failed: " + std::to_string(status)));
}

inline void Checked(mh3_res_status status) {
    if (status != MH3_RES_OK) [[unlikely]]
        CheckedFailed(status);
}

[[noreturn, gnu::cold, gnu::noinline]] void PackCheckedFailed(mh3_pack_status status) {
    MH3_THROW(std::runtime_error("Native model resident mesh failed: " + std::to_string(status)));
}

inline void PackChecked(mh3_pack_status status) {
    if (status != MH3_PACK_OK) [[unlikely]]
        PackCheckedFailed(status);
}

// Checked and PackChecked for a caller that recovers from the failure.
Status CheckedStatus(mh3_res_status status) {
    if (status != MH3_RES_OK) [[unlikely]]
        return Status::Error("Native model scope failed: " + std::to_string(status));

    return {};
}

Status PackCheckedStatus(mh3_pack_status status) {
    if (status != MH3_PACK_OK) [[unlikely]]
        return Status::Error("Native model resident mesh failed: " + std::to_string(status));

    return {};
}

// A guest word for a caller that recovers from an unmapped address.
Status ReadGuest32(uint32_t address, uint32_t& value) {
    if (!Memory::TryRead32(address, value))
        return Status::Error(Memory::UnmappedMessage(address));

    return {};
}

mh3_res_span Bytes(uint32_t address, size_t size) {
    return {GuestRange(address, size), size};
}

mh3_res_mut_span Mutable(uint32_t address, size_t size) {
    return {GuestRange(address, size), size};
}

uint32_t Resource(uint32_t reference, bool required = false) {
    uint32_t data = 0;
    Checked(mh3_res_runtime_reference(&MH3NativeResourceMemory(), reference, required, &data));
    return data;
}

uint32_t Canonical(uint32_t address) {
    return address ? (address & 0x3fffffffu) | 0x80000000u : 0;
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

    Memory::Write8(sync_address, 0);

    if (count)
        OverlayNativeModelMaterialGroup(*fields, kind, count);

    if (xf)
        Genmode(true);
}

template <class T> struct RecipeCache {
    uint64_t revision = UINT64_MAX;
    FastMap<uint32_t, T> entries;
};

template <class T> RecipeCache<T>& Recipes() {
    static RecipeCache<T> cache;
    const uint64_t revision = ReadNativeModelStructureRevision();

    if (cache.revision != revision) {
        cache.entries.clear();
        cache.revision = revision;
    }

    return cache;
}

mh3_model_material_scope Scope(uint32_t reference, uint32_t swaps) {
    const uint32_t material = Resource(reference);
    mh3_model_material_bindings replacements{};

    if (swaps)
        Checked(mh3_model_material_bindings_read(Bytes(swaps, 56), &replacements));

    auto& recipes = Recipes<mh3_model_scope_recipe>();
    const auto cached = recipes.entries.find(material);

    if (cached != recipes.entries.end() && mh3_model_scope_recipe_matches(&cached->second, recipes.revision, material, &replacements))
        return cached->second.scope;

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

    mh3_model_material_scope scope{};
    Checked(mh3_model_material_scope_read(&MH3NativeResourceMemory(), material, version, &replacements, &scope));

    // Admission may demand-load a pack and advance the structural token.
    auto& admitted = Recipes<mh3_model_scope_recipe>();

    if (admitted.entries.size() >= 8192)
        admitted.entries.clear();

    admitted.entries.insert_or_assign(material, mh3_model_scope_recipe{admitted.revision, replacements, scope});
    return scope;
}

// The selected light set's masks and ambient colour for the channel step
// (all zero when the material selects no set before it).
struct PrepareContext {
    uint32_t masks[4]{};
    uint8_t ambient[4]{};
};

void OperatorKnown(uint32_t address) {
    Checked(mh3_matrix_runtime_operator(&MH3NativeResourceMemory(), address, 0x80088a5c, 0x800888bc));
    const uint32_t table = Memory::Read32(address);

    if (Memory::Read32(table + 8) != 0x80088aac)
        MH3_THROW(std::runtime_error("Custom indirect apply callback has no native producer"));
}

void Indirect(uint32_t resource, uint32_t op) {
    if (resource)
        Group(resource, MH3_MODEL_MATERIAL_INDIRECT);

    if (!op)
        return;

    if (op != operator_address || !current_operator.active)
        MH3_THROW(std::runtime_error("Indirect matrix operator has no completed native producer"));

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
        const auto& lights = SelectNativeModelLightSet(0x80686368, uint32_t(int32_t(misc.light_reference)));
        context.masks[0] = lights.diffuse_color;
        context.masks[1] = lights.diffuse_alpha;
        context.masks[2] = lights.specular_color;
        context.masks[3] = lights.specular_alpha;
        std::memcpy(context.ambient, &lights.ambient, sizeof context.ambient);
        break;
    }
    case MH3_MATERIAL_PREP_CHANNELS: {
        mh3_pack_channel channels[2];
        Checked(mh3_model_channels_prepare_channels(Bytes(resource, 40), Mode().channels, context.masks, context.ambient, step->force_light_off, channels));
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
    NativeModelOwner owner;
    const mh3_prepared_mesh* prepared;
    int32_t matrix_id;
    bool has_normal;

    // The prepared mesh's vertex attributes all read authored sources the
    // shape binds (CaptureShape's converted-source check, given the bindings).
    bool covers_layout;
    uint32_t address; // the shape
    uint32_t mesh_index; // the prepared mesh's pack index (draw tables)
};

// The owner's prepared mesh and the authored vertex sources of the shape.
Status TryReadSources(const NativeModelOwner& owner, uint32_t shape, ShapeSources& result) {
    result = {};
    result.source_generation = owner.source_generation;
    result.resident_handle = owner.resident_handle;
    result.resident_generation = owner.resident_generation;
    result.owner = owner;
    result.address = shape;
    MH3_RETURN_IF_ERROR(ReadNativePreparedMesh(owner, shape, result.prepared));
    uint32_t word = 0;
    MH3_RETURN_IF_ERROR(ReadGuest32(shape + 8, word));
    result.matrix_id = int32_t(word);
    MH3_RETURN_IF_ERROR(ReadGuest32(shape + 0x30, word));
    result.has_normal = (word & (1u << 10)) != 0;

    for (uint32_t i = 0; i < 12; i++) {
        uint32_t resource = 0;
        MH3_RETURN_IF_ERROR(CheckedStatus(mh3_res_runtime_shape_array_at(&MH3NativeResourceMemory(), shape, i, &resource)));

        if (!resource)
            continue;

        const auto kind = i == 0 ? MH3_VERTEX_POSITION : i == 1 ? MH3_VERTEX_NORMAL : i < 4 ? MH3_VERTEX_COLOR : MH3_VERTEX_UV;
        MH3_RETURN_IF_ERROR(TryReadNativeVertexResourceData(resource, kind, result.source[i]));
        result.known |= uint16_t(1u << i);
    }

    const auto& layout = result.prepared->immutable.layout;
    result.covers_layout = true;

    for (uint32_t i = 0; i < layout.count; i++) {
        const uint32_t semantic = layout.attributes[i].semantic;

        if (semantic >= 12 && (result.prepared->palette_batch_count || result.prepared->skin_row_count))
            continue;

        if (semantic >= 12 || !(result.known & (1u << semantic)))
            result.covers_layout = false;
    }

    return {};
}

ShapeSources ReadSources(const NativeModelOwner& owner, uint32_t shape) {
    ShapeSources result;
    CheckStatus(TryReadSources(owner, shape, result));
    return result;
}

const ShapeSources& Sources(const NativeModelOwner& owner, uint32_t shape) {
    const uint32_t key = shape;
    auto& recipes = Recipes<ShapeSources>();
    auto& shape_sources = recipes.entries;
    const auto cached = shape_sources.find(key);

    if (cached != shape_sources.end() && cached->second.source_generation == owner.source_generation && cached->second.resident_handle == owner.resident_handle &&
        cached->second.resident_generation == owner.resident_generation)
        return cached->second;
    const ShapeSources result = ReadSources(owner, shape);

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
    context.shape = Resource(reference, true);
    auto& recipes = Recipes<ShapeSources>();
    const auto& entries = recipes.entries;
    const auto cached = entries.find(context.shape);

    if (cached != entries.end()) {
        context.owner = cached->second.owner;
        context.sources = &cached->second;
        return context;
    }

    context.owner = ShapeOwner(reference, &context.shape);
    context.sources = &Sources(context.owner, context.shape);
    return context;
}

// A dispatch's material scope and shape, resolved through its model's draw
// table instead of by address (DispatchNativeModelIndexed).
struct Resolved {
    mh3_model_material_scope scope;
    ShapeContext shape;
};
} // namespace

// One certified model's resolved draw records (model_catalog_bridge.h): what
// Scope and ResolveShape produce for its materials and shapes, by draw-stream
// ID. Built once per certified owner; replacements are applied per draw.
struct NativeModelDrawTable {
    uint32_t model = 0; // MDL0 address (the alias the addresses were made from)
    NativeModelOwner owner{};

    // Scope without replacements; material_address 0: no record (by address).
    std::vector<mh3_model_material_scope> materials;

    // prepared == nullptr: no record (by address).
    std::vector<ShapeSources> shapes;

    // The draw walk's material, shape and node entries by ID (offsets from
    // the MDL0, UINT32_MAX unknown) and the walk's view of them.
    std::vector<uint32_t> material_entries, shape_entries, node_entries;
    mh3_model_draw_entries entries{};

    // Null when a shape reads an envelope matrix (FindNativeModelNodeViews).
    std::shared_ptr<const NativeModelNodeViews> node_views;
    void ViewEntries() {
        entries = {material_entries.data(), shape_entries.data(), node_entries.data(), uint32_t(material_entries.size()), uint32_t(shape_entries.size()),
                   uint32_t(node_entries.size())};
    }

    NativeModelDrawTable() = default;
    NativeModelDrawTable(const NativeModelDrawTable&) = delete;

    // Shape sources may be bound in place (Dispatch).
    ~NativeModelDrawTable() { ReleaseNativeVertexSources(shapes.data(), shapes.data() + shapes.size()); }
};

namespace {
// Draw items resolve through their model's draw table (by address when it has
// none).
// Tables read from the cache file (the loading screen line).
uint64_t tablesCached = 0;

// Models whose table could not be built, with the structure revision then.
FastMap<uint32_t, uint64_t> tableFailures;

// ---- Draw table cache ----
// The address-independent part of the draw tables, in one file next to the
// packs (draw-tables.bin): per pack and catalog model, the model's records
// with every address relative to its MDL0. A pack's records are keyed by its
// file name and content: metadata and identity sizes, the identity's FNV-1a,
// mesh and model counts; a changed pack replaces them. The file is read once,
// when the first table is made; tables made from it are instantiated instead
// of resolved. Tables made otherwise are written back at the next loading
// screen (the whole file, under a temporary name first).
//
// u32 little-endian words: "M3DT", version, pack count; per pack the name's
// byte count, the name (zero-padded to words), the five key words, the model
// count and per model its record's byte count and the record (none: 0); then
// the FNV-1a of everything before it. A record: material, shape and node
// counts; the material, shape and node entry offsets (UINT32_MAX unknown);
// per material a presence mask (bit 0 record, 1 fur, 2..12 the eleven slots),
// its own, fur and slot offsets; per shape flags (bit 0 record, 1 normal,
// 2 covers layout), offset, mesh pack index, matrix ID, known semantics and
// per known semantic a vertex source: resource and payload offsets, source ID,
// resource and payload bytes, count, stride, components, format, fraction,
// component mode, sample index, count and stride, kind.
constexpr uint32_t TableMagic = 0x5444334d, TableVersion = 3, TableKeyWords = 5, SourceWords = 15;
struct CachedPack {
    uint32_t key[TableKeyWords]{};
    std::vector<std::vector<uint8_t>> records; // by catalog model index
};

struct TableCache {
    bool loaded = false, dirty = false;
    std::string path; // empty: no pack file to cache next to
    std::map<std::string, CachedPack> packs;
} tableCache;

// A resident pack's records in the cache (nodes of the map stay put).
struct PackTables {
    uintptr_t handle;
    uint64_t generation;
    CachedPack* pack;
};

std::vector<PackTables> packTables;
uint32_t Le32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

void Put32(std::vector<uint8_t>& out, uint32_t value) {
    for (unsigned i = 0; i < 4; i++)
        out.push_back(uint8_t(value >> (8 * i)));
}

uint32_t Fnv(const uint8_t* p, size_t n) {
    uint32_t hash = 2166136261u;

    for (size_t i = 0; i < n; i++)
        hash = (hash ^ p[i]) * 16777619u;

    return hash;
}

// Reads the table file; a damaged or other-version file reads as empty.
void LoadTableCache() {
    std::vector<uint8_t> bytes;

    if (std::FILE* file = std::fopen(tableCache.path.c_str(), "rb")) {
        if (!std::fseek(file, 0, SEEK_END)) {
            const long size = std::ftell(file);

            if (size > 0 && size < (64l << 20) && !std::fseek(file, 0, SEEK_SET)) {
                bytes.resize(size_t(size));

                if (std::fread(bytes.data(), 1, bytes.size(), file) != bytes.size())
                    bytes.clear();
            }
        }

        std::fclose(file);
    }

    if (bytes.size() < 16 || Le32(bytes.data()) != TableMagic || Le32(bytes.data() + 4) != TableVersion || Fnv(bytes.data(), bytes.size() - 4) != Le32(bytes.data() + bytes.size() - 4))
        return;

    const uint8_t* p = bytes.data() + 12;
    size_t left = bytes.size() - 16;
    const auto word = [&](uint32_t& value) {
        if (left < 4)
            return false;

        value = Le32(p);
        p += 4;
        left -= 4;
        return true;
    };

    std::map<std::string, CachedPack> packs;

    for (uint32_t n = Le32(bytes.data() + 8); n--;) {
        uint32_t length, models;

        if (!word(length) || length > 255 || left < (length + 3u) / 4 * 4)
            return;

        std::string name(reinterpret_cast<const char*>(p), length);
        p += (length + 3) / 4 * 4;
        left -= (length + 3) / 4 * 4;
        CachedPack pack;

        for (auto& key : pack.key)
            if (!word(key))
                return;
        if (!word(models) || models > 65536)
            return;

        pack.records.resize(models);

        for (auto& record : pack.records) {
            uint32_t size;

            if (!word(size) || size % 4 || size > left)
                return;

            record.assign(p, p + size);
            p += size;
            left -= size;
        }

        packs[name] = std::move(pack);
    }

    if (!left)
        tableCache.packs = std::move(packs);
}

// The cache of the owner's pack, the file read at the first use; null when
// the pack has no file.
CachedPack* TablesOf(const NativeModelOwner& owner) {
    for (const auto& tables : packTables)
        if (tables.handle == owner.resident_handle && tables.generation == owner.resident_generation)
            return tables.pack;
    NativeModelPackFile file;
    CachedPack* pack = nullptr;

    if (ReadNativeModelPackFile(owner.resident_handle, owner.resident_generation, &file)) {
        const auto slash = file.path.find_last_of("/:");
        const std::string directory = slash == std::string::npos ? std::string() : file.path.substr(0, slash + 1);
        const std::string name = file.path.substr(directory.size());

        if (!tableCache.loaded) {
            tableCache.loaded = true;
            tableCache.path = directory + "draw-tables.bin";
            LoadTableCache();
        }

        const uint32_t key[TableKeyWords]{file.metadata_bytes, file.identity_bytes, file.identity_hash, owner.pack->counts[MH3_PACK_MESHES], owner.catalog->model_count};
        pack = &tableCache.packs[name];

        if (std::memcmp(pack->key, key, sizeof key) || pack->records.size() != owner.catalog->model_count) {
            // A new or changed pack: its old records are void.
            std::memcpy(pack->key, key, sizeof key);
            pack->records.assign(owner.catalog->model_count, {});
        }
    }

    // Handles may be reused by later packs; the generation tells them apart.
    if (packTables.size() >= 1024)
        packTables.erase(packTables.begin());

    packTables.push_back({owner.resident_handle, owner.resident_generation, pack});
    return pack;
}

// The table's address-independent record.
std::vector<uint8_t> EncodeTable(const NativeModelDrawTable& table) {
    const uint32_t model = table.model;
    std::vector<uint8_t> out;
    out.reserve(12 + table.materials.size() * 60 + table.shapes.size() * 144 + table.node_entries.size() * 4);
    Put32(out, uint32_t(table.materials.size()));
    Put32(out, uint32_t(table.shapes.size()));
    Put32(out, uint32_t(table.node_entries.size()));

    for (const auto* entries : {&table.material_entries, &table.shape_entries, &table.node_entries})
        for (const uint32_t offset : *entries)
            Put32(out, offset);
    for (const auto& scope : table.materials) {
        uint32_t present = scope.material_address ? 1u : 0u;

        if (present && scope.fur_address)
            present |= 2;

        for (unsigned i = 0; present && i < MH3_MAT_RESOURCE_COUNT; i++)
            if (scope.selected.resource_slots[i])
                present |= 4u << i;
        Put32(out, present);
        Put32(out, present ? scope.material_address - model : 0);
        Put32(out, present & 2 ? scope.fur_address - model : 0);

        for (unsigned i = 0; i < MH3_MAT_RESOURCE_COUNT; i++)
            Put32(out, present & (4u << i) ? scope.selected.resource_slots[i] - model : 0);
    }

    for (const auto& shape : table.shapes) {
        if (!shape.prepared) {
            for (unsigned i = 0; i < 5; i++)
                Put32(out, 0);

            continue;
        }

        Put32(out, 1u | (shape.has_normal ? 2u : 0u) | (shape.covers_layout ? 4u : 0u));
        Put32(out, shape.address - model);
        Put32(out, shape.mesh_index);
        Put32(out, uint32_t(shape.matrix_id));
        Put32(out, shape.known);

        for (uint32_t i = 0; i < 12; i++) {
            if (!(shape.known & (1u << i)))
                continue;

            const auto& v = shape.source[i];
            const uint32_t words[SourceWords]{v.resource_address - model, v.payload_address - model, v.source_id, v.resource_bytes, v.payload_bytes, v.count, v.stride,
                                              v.components, v.format, v.fraction, v.component_mode, v.sample_index, v.sample_count, v.sample_stride, uint32_t(v.kind)};
            for (const uint32_t word : words)
                Put32(out, word);
        }
    }

    return out;
}

// Fills the table from a record made for the same pack content and model;
// false (table untouched) when the record does not fit the live model.
bool DecodeTable(const std::vector<uint8_t>& record, uint32_t materials, uint32_t shapes, uint32_t nodes, NativeModelDrawTable& table) {
    const uint8_t* p = record.data();
    size_t left = record.size();
    const auto word = [&](uint32_t& value) {
        if (left < 4)
            return false;

        value = Le32(p);
        p += 4;
        left -= 4;
        return true;
    };

    uint32_t count[3];

    if (!word(count[0]) || !word(count[1]) || !word(count[2]) || count[0] != materials || count[1] != shapes || count[2] != nodes)
        return false;

    std::vector<uint32_t> entries[3];

    for (unsigned kind = 0; kind < 3; kind++) {
        entries[kind].resize(count[kind]);

        for (auto& offset : entries[kind])
            if (!word(offset))
                return false;
    }

    const auto& owner = table.owner;
    const uint32_t model = table.model;
    std::vector<mh3_model_material_scope> scopes(materials);

    for (auto& scope : scopes) {
        uint32_t present, material, fur, slots[MH3_MAT_RESOURCE_COUNT];

        if (!word(present) || !word(material) || !word(fur))
            return false;

        for (auto& slot : slots)
            if (!word(slot))
                return false;
        if (!(present & 1))
            continue;

        scope.material_address = model + material;
        scope.fur_address = present & 2 ? model + fur : 0;

        for (unsigned i = 0; i < MH3_MAT_RESOURCE_COUNT; i++)
            scope.selected.resource_slots[i] = present & (4u << i) ? model + slots[i] : 0;
    }

    std::vector<ShapeSources> sources(shapes);

    for (auto& shape : sources) {
        uint32_t flags, offset, mesh, matrix, known;

        if (!word(flags) || !word(offset) || !word(mesh) || !word(matrix) || !word(known) || known >> 12)
            return false;

        if (!(flags & 1))
            continue;

        shape.source_generation = owner.source_generation;
        shape.resident_handle = owner.resident_handle;
        shape.resident_generation = owner.resident_generation;
        shape.owner = owner;
        shape.address = model + offset;
        shape.mesh_index = mesh;
        shape.prepared = ReadNativePreparedMeshAt(owner, mesh);

        if (!shape.prepared)
            return false;

        shape.matrix_id = int32_t(matrix);
        shape.has_normal = (flags & 2) != 0;
        shape.covers_layout = (flags & 4) != 0;
        shape.known = uint16_t(known);

        for (uint32_t i = 0; i < 12; i++) {
            if (!(known & (1u << i)))
                continue;

            uint32_t w[SourceWords];

            for (auto& value : w)
                if (!word(value))
                    return false;
            auto& v = shape.source[i];
            v.resource_address = model + w[0];
            v.payload_address = model + w[1];
            v.source_id = w[2];
            v.resource_bytes = w[3];
            v.payload_bytes = w[4];
            v.count = w[5];
            v.stride = w[6];
            v.components = w[7];
            v.format = w[8];
            v.fraction = w[9];
            v.component_mode = w[10];
            v.sample_index = w[11];
            v.sample_count = w[12];
            v.sample_stride = w[13];
            v.kind = mh3_vertex_kind(w[14]);
        }
    }

    if (left)
        return false;

    table.materials = std::move(scopes);
    table.shapes = std::move(sources);
    table.material_entries = std::move(entries[0]);
    table.shape_entries = std::move(entries[1]);
    table.node_entries = std::move(entries[2]);
    return true;
}

// Writes the table file when tables were added (loading screens).
void FlushTables() {
    if (!tableCache.dirty || tableCache.path.empty())
        return;

    tableCache.dirty = false;
    std::vector<uint8_t> out;
    Put32(out, TableMagic);
    Put32(out, TableVersion);
    Put32(out, uint32_t(tableCache.packs.size()));

    for (const auto& [name, pack] : tableCache.packs) {
        Put32(out, uint32_t(name.size()));
        out.insert(out.end(), name.begin(), name.end());
        out.resize((out.size() + 3) / 4 * 4, 0);

        for (const uint32_t key : pack.key)
            Put32(out, key);

        Put32(out, uint32_t(pack.records.size()));

        for (const auto& record : pack.records) {
            Put32(out, uint32_t(record.size()));
            out.insert(out.end(), record.begin(), record.end());
        }
    }

    Put32(out, Fnv(out.data(), out.size()));

    // A file under its final name is complete.
    const std::string temporary = tableCache.path + ".tmp";
    std::FILE* file = std::fopen(temporary.c_str(), "wb");
    bool written = file && std::fwrite(out.data(), 1, out.size(), file) == out.size();

    if (file && std::fclose(file))
        written = false;

    std::remove(tableCache.path.c_str());

    if (!written || std::rename(temporary.c_str(), tableCache.path.c_str())) {
        std::remove(temporary.c_str());
#ifdef MH3_DEVELOPMENT
        std::fprintf(stderr, "MH3 draw table: cannot write %s\n", tableCache.path.c_str());
#endif
        return;
    }
}

// The matrix IDs below a model's view count that its draws read, when no
// shape reads envelopes per vertex: every shape resolved, without palette
// batches, and skinned or rigid. Node matrices and the envelopes of rigid
// shapes (the matrix-to-node map at MDL0 info + 0x24, -1 for an envelope; the
// view count at MDL0 + 0x68). Null otherwise.
#ifdef MH3_DEVELOPMENT
// MH3 node views line (split detail): FindNativeModelNodeViews results by
// reason: found, off the game thread, beside a draw, no table, envelopes read.
uint32_t nodeViewLookups[5];
#endif

std::shared_ptr<const NativeModelNodeViews> NodeViews(uint32_t model, const std::vector<ShapeSources>& shapes) {
    const uint32_t info = model + 0x4c;
    uint32_t map = 0, count = 0, views = 0;

    if (!ReadGuest32(info + 0x24, map).ok() || !ReadGuest32(info + map, count).ok() || !ReadGuest32(model + 0x68, views).ok() || views > count || views > 65535)
        return nullptr;

    const auto node = [&](uint32_t id) {
        uint32_t word = UINT32_MAX;
        return id < count && ReadGuest32(info + map + 4 + 4 * id, word).ok() && int32_t(word) >= 0;
    };

    for (size_t i = 0; i < shapes.size(); i++) {
        const auto* prepared = shapes[i].prepared;
        const int32_t matrix = shapes[i].matrix_id;

        if (!prepared || prepared->palette_batch_count || (!prepared->skin_row_count && (matrix < 0 || uint32_t(matrix) >= views))) {
#ifdef MH3_DEVELOPMENT
            if (mh3SplitDetail)
                std::printf("MH3 node views: model %08x reads envelopes: shape %lu %s\n", model, static_cast<unsigned long>(i),
                            !prepared ? "unresolved" : prepared->palette_batch_count ? "has palette batches" : "rigid without a view matrix");
#endif
            return nullptr;
        }
    }

    auto result = std::make_shared<NativeModelNodeViews>();

    for (const auto& shape : shapes) {
        if (shape.prepared->skin_row_count || node(uint32_t(shape.matrix_id)))
            continue;

        result->envelopes.resize(count);
        result->envelopes[uint32_t(shape.matrix_id)] = 1;
    }

    for (uint32_t id = 0; id < views; id++) {
        if (!node(id) && (result->envelopes.empty() || !result->envelopes[id]))
            continue;

        if (!result->runs.empty() && result->runs.back().second == id)
            result->runs.back().second = uint16_t(id + 1);
        else
            result->runs.emplace_back(uint16_t(id), uint16_t(id + 1));
    }

    return result;
}

// Resolves every material and shape of the certified model at `model`, or
// takes them from its pack's table cache. A record that does not resolve
// stays empty: its draws take the address path, which fails as before if it
// is ever drawn.
Status BuildDrawTable(uint32_t model, std::shared_ptr<NativeModelDrawTable>& result) {
    const auto& memory = MH3NativeResourceMemory();
    auto table = std::make_shared<NativeModelDrawTable>();
    table->model = model;
    MH3_RETURN_IF_ERROR(TryResolveNativeModelOwner(model, table->owner));
    const auto& owner = table->owner;
    mh3_catalog_model catalog{};
    MH3_RETURN_IF_ERROR(PackCheckedStatus(mh3_model_catalog_model(owner.catalog, owner.catalog_model_index, &catalog)));
    mh3_res_memory_view view{};
    MH3_RETURN_IF_ERROR(CheckedStatus(mh3_res_runtime_map(&memory, model, 16, &view)));
    mh3_res_model resource{};
    MH3_RETURN_IF_ERROR(CheckedStatus(mh3_res_model_open(view.span, view.offset, &resource)));
    const auto address = [&](size_t offset, uint32_t& guest) {
        if (offset > UINT32_MAX - view.guest_base)
            return Status::Error("Model resource address overflow");

        guest = view.guest_base + uint32_t(offset);
        return Status{};
    };

    size_t dictionary = 0;
    uint32_t materials = 0, shapes = 0, nodes = 0;
    MH3_RETURN_IF_ERROR(CheckedStatus(mh3_res_model_dictionary(&resource, MH3_RES_MATERIALS, &dictionary, &materials)));
    MH3_RETURN_IF_ERROR(CheckedStatus(mh3_res_model_dictionary(&resource, MH3_RES_SHAPES, &dictionary, &shapes)));
    MH3_RETURN_IF_ERROR(CheckedStatus(mh3_res_model_dictionary(&resource, MH3_RES_NODES, &dictionary, &nodes)));
    auto* cache = TablesOf(owner);
    const uint32_t index = owner.catalog_model_index;
    const bool cached = cache && index < cache->records.size() && !cache->records[index].empty() && DecodeTable(cache->records[index], materials, shapes, nodes, *table);

    if (!cached) {
        const auto entry = [&](mh3_res_kind kind, uint32_t id) {
            mh3_res_entry value{};
            return mh3_res_model_entry(&resource, kind, id, &value) == MH3_RES_OK && value.offset >= resource.offset && value.offset - resource.offset < UINT32_MAX
                       ? uint32_t(value.offset - resource.offset)
                       : UINT32_MAX;
        };

        table->material_entries.resize(materials);
        table->shape_entries.resize(shapes);
        table->node_entries.resize(nodes);

        for (uint32_t id = 0; id < materials; id++)
            table->material_entries[id] = entry(MH3_RES_MATERIALS, id);

        for (uint32_t id = 0; id < shapes; id++)
            table->shape_entries[id] = entry(MH3_RES_SHAPES, id);

        for (uint32_t id = 0; id < nodes; id++)
            table->node_entries[id] = entry(MH3_RES_NODES, id);

        table->materials.resize(materials);
        const mh3_model_material_bindings none{};
        const auto readMaterial = [&](uint32_t id, mh3_model_material_scope& scope) {
            mh3_res_entry entry{};
            uint32_t material = 0, word = 0;
            MH3_RETURN_IF_ERROR(CheckedStatus(mh3_res_model_entry(&resource, MH3_RES_MATERIALS, id, &entry)));
            MH3_RETURN_IF_ERROR(address(entry.offset, material));

            // Scope's admission: the material's parent is this model.
            MH3_RETURN_IF_ERROR(ReadGuest32(material + 4, word));
            const int32_t delta = int32_t(word);

            if (!delta || int64_t(material) + delta < 0 || int64_t(material) + delta > UINT32_MAX || Canonical(uint32_t(int64_t(material) + delta)) != Canonical(model))
                return Status::Error("Material belongs to another model");

            mh3_catalog_resource admitted{};
            MH3_RETURN_IF_ERROR(TryResolveNativeModelResource(owner, material, true, admitted));
            return CheckedStatus(mh3_model_material_scope_read(&memory, material, catalog.revision, &none, &scope));
        };

        for (uint32_t id = 0; id < materials; id++) {
            mh3_model_material_scope scope{};
            const Status read = readMaterial(id, scope);

            if (read.ok()) {
                table->materials[id] = scope;
                continue;
            }

            table->materials[id] = {};
        }

        table->shapes.resize(shapes);
        const auto readShape = [&](uint32_t id, ShapeSources& sources) {
            mh3_res_entry entry{};
            uint32_t shape = 0, word = 0;
            MH3_RETURN_IF_ERROR(CheckedStatus(mh3_res_model_entry(&resource, MH3_RES_SHAPES, id, &entry)));
            MH3_RETURN_IF_ERROR(address(entry.offset, shape));

            // ShapeOwner's admission: the shape's parent is this model.
            MH3_RETURN_IF_ERROR(ReadGuest32(shape + 4, word));
            const int32_t delta = int32_t(word);

            if (!delta || int64_t(shape) + delta < 0 || int64_t(shape) + delta > UINT32_MAX || Canonical(uint32_t(int64_t(shape) + delta)) != Canonical(model))
                return Status::Error("Shape belongs to another model");

            mh3_catalog_resource admitted{};
            MH3_RETURN_IF_ERROR(TryResolveNativeModelResource(owner, shape, false, admitted));
            MH3_RETURN_IF_ERROR(TryReadSources(owner, shape, sources));
            sources.mesh_index = admitted.pack_index;
            return Status{};
        };

        for (uint32_t id = 0; id < shapes; id++) {
            const Status read = readShape(id, table->shapes[id]);

            if (read.ok())
                continue;

            table->shapes[id] = {};
        }

        if (cache && index < cache->records.size()) {
            cache->records[index] = EncodeTable(*table);
            tableCache.dirty = true;
        }
    }

    table->ViewEntries();
    table->node_views = NodeViews(model, table->shapes);
    MH3_RETURN_IF_ERROR(AttachNativeModelDrawTable(owner, table));
    tablesCached += cached;
    result = std::move(table);
    return {};
}
} // namespace

std::shared_ptr<const NativeModelNodeViews> FindNativeModelNodeViews(uint32_t model) {
    // The game thread's passes while no draw runs beside them (the overlapped
    // draw's traverse follows its wait, game_frame.cpp), which may attach
    // tables, and the draw thread's while it walks the scene graph beside the
    // game thread's layers (game_frame.cpp LaunchScenePrepare); the guest
    // worker's run beside the game thread's catalog changes.
    const unsigned thread = PpcThread::CurrentThread();
    const bool game = thread == PpcThread::Game;

    if (game ? DrawThread::Thread().Pending() : !(thread == PpcThread::Draw && NativeFramePacketPrepareOnDraw())) {
#ifdef MH3_DEVELOPMENT
        ++nodeViewLookups[game ? 2 : 1];
#endif
        return nullptr;
    }

    const auto table = FindNativeModelDrawTable(model);
#ifdef MH3_DEVELOPMENT
    ++nodeViewLookups[!table ? 3 : !table->node_views ? 4 : 0];
#endif
    return table ? table->node_views : nullptr;
}
#ifdef MH3_DEVELOPMENT
void MH3PrintNodeViewSplit(unsigned frames) {
    const auto f = [frames](unsigned i) { return frames ? double(nodeViewLookups[i]) / frames : 0.0; };
    std::printf("MH3 node views: lookups/frame found %.1f, off the game thread %.1f, beside a draw %.1f, no table %.1f, envelopes read %.1f\n", f(0), f(1), f(2), f(3), f(4));
    std::memset(nodeViewLookups, 0, sizeof nodeViewLookups);
}
#endif

NativeModelDrawTable* NativeModelDrawTableFor(uint32_t model, std::shared_ptr<NativeModelDrawTable>& hold) {
    hold.reset();

    if (!model)
        return nullptr;

    hold = FindNativeModelDrawTable(model);

    if (!hold) {
        const uint64_t revision = ReadNativeModelStructureRevision();
        const auto failed = tableFailures.find(model);

        if (failed != tableFailures.end() && failed->second == revision)
            return nullptr;

        if (const Status built = BuildDrawTable(model, hold); !built.ok()) {
            // The address path resolves (and reports) whatever this model draws.
            if (tableFailures.size() >= 4096)
                tableFailures.clear();

            tableFailures.insert_or_assign(model, ReadNativeModelStructureRevision());
            return nullptr;
        }
    }

    if (hold->model != model)
        return nullptr;

    return hold.get();
}

namespace {
// One loading screen: the span of its PrepareNativeModelDrawTables calls (one
// per loading-screen frame) and the time those calls took.
struct LoadingScreen {
    bool open = false;
    std::chrono::steady_clock::time_point first, last;
    uint32_t frames = 0, quiet = 0;
    uint64_t certified = 0, tables = 0, cached = 0, admissions = 0;
    double prepare_ms = 0, longest_ms = 0; // longest frame: span between two calls
    double certify_ms = 0, open_ms = 0;    // of prepare_ms: certification, and the packs it opened
    double cache_ms = 0, take_ms = 0;      // of certify_ms: waiting for the asset cache and the read-ahead
    double ensure_ms = 0;                  // of certify_ms: the packs' admission in all (cache_ms and take_ms are of it)
    uint64_t ready = 0, waited = 0, own = 0; // the read-ahead's packs: read already, waited for, read here
} loading;

void ReportLoadingScreen() {
#ifdef MH3_DEVELOPMENT
    if (!loading.open)
        return;

    loading.open = false;
    std::fprintf(stderr,
                 "MH3 loading screen: %.3f s, %u frames, longest %.0f ms; draw table preparation %.2f ms (%llu models certified in %.2f ms, %llu packs opened in %.2f ms, %llu tables, %llu "
                 "from the cache); certification's admissions took %.2f ms, of it %.2f ms waiting for the asset cache and %.2f ms for read-ahead packs (%llu read ahead, %llu waited "
                 "for, %llu read here)\n",
                 std::chrono::duration<double>(loading.last - loading.first).count(), loading.frames, loading.longest_ms, loading.prepare_ms, static_cast<unsigned long long>(loading.certified),
                 loading.certify_ms, static_cast<unsigned long long>(loading.admissions), loading.open_ms, static_cast<unsigned long long>(loading.tables),
                 static_cast<unsigned long long>(loading.cached), loading.ensure_ms, loading.cache_ms, loading.take_ms, static_cast<unsigned long long>(loading.ready),
                 static_cast<unsigned long long>(loading.waited), static_cast<unsigned long long>(loading.own));
#endif
}
} // namespace

const mh3_model_draw_entries* NativeModelDrawEntries(const NativeModelDrawTable& table) {
    return &table.entries;
}

void PrepareNativeModelDrawTables() {
    const auto start = std::chrono::steady_clock::now();

    if (loading.open && start - loading.last > std::chrono::seconds(1))
        ReportLoadingScreen();

    if (!loading.open) {
        loading = {};
        loading.open = true;
        loading.first = start;
    } else
        loading.longest_ms = std::max(loading.longest_ms, std::chrono::duration<double, std::milli>(start - loading.last).count());

    const uint64_t cachedBefore = tablesCached;
#ifdef MH3_DEVELOPMENT
    const NativeModelPackOpenings before = ReadNativeModelPackOpenings();
#endif
    const size_t certified = CertifyLoadedNativeModels();
#ifdef MH3_DEVELOPMENT
    const NativeModelPackOpenings after = ReadNativeModelPackOpenings();
    loading.certify_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    loading.admissions += after.admissions - before.admissions;
    loading.open_ms += after.open_ms - before.open_ms;
    loading.cache_ms += after.cache_ms - before.cache_ms;
    loading.take_ms += after.take_ms - before.take_ms;
    loading.ensure_ms += after.ensure_ms - before.ensure_ms;
    loading.ready += after.ready - before.ready;
    loading.waited += after.waited - before.waited;
    loading.own += after.own - before.own;
#endif
    std::vector<NativeModelOwner> owners;
    ListNativeModelOwnersWithoutDrawTable(owners);
    size_t built = 0;

    for (const auto& owner : owners) {
        if (FindNativeModelDrawTable(owner.model_address))
            continue; // a sibling's certification may have replaced the list
        std::shared_ptr<NativeModelDrawTable> table;

        if (const Status status = BuildDrawTable(owner.model_address, table); !status.ok())
            continue;

        ++built;
    }

    // Tables made since the file was read join it once the loading screen has
    // made none for a few frames: one write per screen, not one per file loaded.
    loading.quiet = built ? 0 : loading.quiet + 1;

    if (loading.quiet >= 8)
        FlushTables();

    const auto end = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(end - start).count();
    loading.last = end;
    ++loading.frames;
    loading.certified += certified;
    loading.tables += built;
    loading.cached += tablesCached - cachedBefore;
    loading.prepare_ms += ms;
}

void ReportNativeModelDrawTables() {
    if (loading.open && std::chrono::steady_clock::now() - loading.last > std::chrono::seconds(1))
        ReportLoadingScreen();
}

namespace {
void PrePrimitive(const ShapeContext& context) {
    const uint32_t shape = context.shape;
    const auto& sources = *context.sources;
    PrepareNativeModelCoordinateSizes(Mode().texgens);
    Genmode(true);
    Checked(mh3_model_shape_cache(Mutable(0x80685504, 12), Bytes(shape + 12, 12)));
    Memory::Write8(sync_address, 0);

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
            MH3_THROW(std::runtime_error("Model replacement vertex table overflows"));

        const uint32_t resource = Memory::Read32(table + uint32_t(id) * 4);
        const auto kind = i == 0 ? MH3_VERTEX_POSITION : i == 1 ? MH3_VERTEX_NORMAL : MH3_VERTEX_COLOR;
        const auto source = ReadNativeVertexResourceData(resource, kind);

        if (i == 0 && Canonical(source.payload_address) != Canonical(originals.source[0].payload_address))
            MH3_THROW(std::runtime_error("Position replacement differs from the shape's authored array"));

        StoreNativeVertexSource(i, source);
    }
}
} // namespace

void PublishNativeModelMapSize(uint32_t map, uint16_t width, uint16_t height) {
    if (map >= 8 || !width || !height)
        MH3_THROW(std::runtime_error("Native G3D map size is invalid"));

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
    const mh3_res_status status = mh3_model_coordinate_prepare(Mutable(coordinate_cache, 108), count, coordinate_map_known, &map_sizes, &coordinate_sizes, &sizes, &written, &apply);

    // A coordinate reading a map no draw bound yet keeps its size, as on the
    // render thread (model_render_prep.cpp).
    if (status == MH3_RES_ABSENT)
        return;

    Checked(status);

    if (apply)
        coordinate_sizes = sizes;
}

void LoadNativeModelPipeline(uint32_t resource) {
    if (!resource)
        return;

    if (resource > UINT32_MAX - 32)
        MH3_THROW(std::runtime_error("Model pipeline offset overflows"));

    const auto& fields = ReadNativeLiveMaterialBlockView(resource + 32, MH3_MODEL_MATERIAL_PIPELINE).fields;
    const auto generation = fields.generation;

    if (Memory::Read32(0x80796dbc) == resource && pipeline_address == resource && pipeline_revision == generation)
        return;

    uint8_t table[8], known = 0;
    ReadNativeModelCoordinateMap(resource + 32, table, &known);
    Checked(mh3_model_coordinate_table(Mutable(coordinate_cache, 108), table));
    coordinate_map_known = known;
    Genmode(false);
    Memory::Write8(sync_address, 0);
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
// matrix palette from the pose version and submits.
struct ModelItem {
    NativeModelOwner owner;
    const mh3_prepared_mesh* prepared;
    mh3_matrix_runtime_storage storage;
    mh3_shape_matrix_input input;
    const NativePoseVersion* pose;
    mh3_model_texcoord_sizes coordinate_sizes;

    // Deferred items carry the dispatch's preparation inputs instead of its
    // prepared input/coordinate sizes; the render thread prepares them.
    bool deferred = false, direct_position = false, direct_normal = false;
    ModelPrepInputs prep;
};

// Items of one frame packet, indexed by its ring slot: the game thread fills
// the recording packet's store, the render thread reads the one it renders.
struct ModelStore {
    std::vector<ModelItem> items;
    ModelPrepSrtList srts;
    size_t itemCount = 0;
    uint64_t sequence = UINT64_MAX;
};

std::array<ModelStore, kNativeFramePacketSlots> stores;

// Frames that are not recorded still prepare (guest/selector side effects).
ModelStore scratch;
ModelStore& Store() {
    if (!NativeGraphicsFrameShouldRender()) {
        scratch.itemCount = 0;
        scratch.srts.count = 0;
        return scratch;
    }

    auto& store = stores.at(NativeFramePacketSlot());

    if (store.sequence != NativeFramePacketSequence()) {
        store.sequence = NativeFramePacketSequence();
        store.itemCount = 0;
        store.srts.count = 0;
    }

    return store;
}

// The shapes of one model draw share its binding: the version found for the
// first serves the rest without reading the pose again. A later model draw,
// even of the same arrays, asks for the version again (a view pass between
// them may have rewritten the arrays), and so does every shape drawn outside
// a model draw.
struct DrawPose {
    uint32_t address[3]{};
    uint64_t sequence = UINT64_MAX;
    const NativePoseVersion* version = nullptr;
    bool active = false; // between BeginNativeModelDraw and EndNativeModelDraw
} drawPose;

// The version of the bound pose arrays (an empty one without arrays).
const NativePoseVersion* Pose(const uint32_t addresses[3], uint32_t model) {
    const uint64_t sequence = NativeFramePacketSequence();

    if (drawPose.active && drawPose.version && drawPose.sequence == sequence && !std::memcmp(drawPose.address, addresses, sizeof drawPose.address))
        return drawPose.version;

    const auto live = addresses[0] ? ReadNativeModelPose(addresses, model) : mh3_matrix_sources{};
    drawPose.version = VersionNativeModelPose(addresses, live);
    drawPose.sequence = sequence;
    std::memcpy(drawPose.address, addresses, sizeof drawPose.address);
    return drawPose.version;
}

void CapturePose(ModelItem& item, const uint32_t addresses[3]) {
    item.pose = Pose(addresses, item.owner.model_address);
}

// The bound array of `semantic` is the shape's authored source, unwritten
// since conversion (no producer generation): the converted mesh matches it.
bool BoundOriginal(const ShapeSources& originals, uint32_t semantic) {
    if (!(originals.known & (1u << semantic)))
        return false;

    const auto* selected = PeekNativeVertexSource(semantic);

    if (!selected || selected->source_generation)
        return false;

    const uint32_t bound = selected->payload_address, original = originals.source[semantic].payload_address;
    return bound == original || Canonical(bound) == Canonical(original);
}

void PrepareModelItem(ModelItem& item, const ShapeContext& context, uint32_t direct_position, uint32_t direct_normal) {
    item.owner = context.owner;
    const auto& prepared = *context.sources->prepared;
    item.prepared = &prepared;
    const auto& originals = *context.sources;

    for (uint32_t i = 0; i < prepared.immutable.layout.count; i++) {
        const uint32_t semantic = prepared.immutable.layout.attributes[i].semantic;

        // These are immutable authoritative primitive matrix indices packed by
        // the extractor, not replacements for one of the twelve array sources.
        if (semantic >= 12 && (prepared.palette_batch_count || prepared.skin_row_count))
            continue;

        if (!BoundOriginal(originals, semantic))
            MH3_THROW(std::runtime_error("Resident model vertex binding requires a converted authored source: " + std::to_string(semantic)));
    }

    const auto binding = ReadNativeModelMatrixSources(locations.arrays);

    if (!binding.address[0] && !direct_position)
        MH3_THROW(std::runtime_error("Indexed model matrix has no completed pose producer"));

    CapturePose(item, binding.address);
    item.storage = {};

    if (direct_position) {
        mh3_matrix_read34_unchecked({Bytes(direct_position, 48), 1, 48, 1}, 0, &item.storage.direct_position);

        if (direct_normal)
            mh3_matrix_read34_unchecked({Bytes(direct_normal, 48), 1, 48, 1}, 0, &item.storage.direct_normal);
    }

    mh3_model_shape_input_unchecked(originals.matrix_id, originals.has_normal, Bytes(locations.texture_sources, 32).data,
        Memory::Read8(locations.current_valid) != 0, binding.address[0] != 0,
        direct_position ? &item.storage.direct_position : nullptr, direct_position && direct_normal ? &item.storage.direct_normal : nullptr, &item.input);
    if (item.input.matrix_id < 0 && !prepared.palette_batch_count && !prepared.skin_row_count)
        MH3_THROW(std::runtime_error("Envelope model requires converted skinning palette records"));

    PrepareNativeModelCoordinateSizes(Mode().texgens);

    if (item.input.matrix_id >= 0) {
        // Row selectors depend on the shape's matrix sources, not pose values.
        uint32_t rows[8];
        const bool identity = mh3_model_shape_rows_unchecked(item.input.texture_source, rows);

        if (!identity || !Memory::Read8(locations.current_valid))
            PublishNativeModelMatrixRows(rows);

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
    Memory::Write8(sync_address, 0);

    // Publish the producer's row/dual fields once. Palette batches consume the
    // owner's material directly; they do not submit a full material copy.
    ApplyNativeModelSelectors();

    if (item.input.matrix_id < 0)
        Memory::Write8(locations.current_valid, 0);
}

// a * b for 3x4 affine matrices (implicit last row 0 0 0 1).
void Concat34(const float* a, const float* b, float* out) {
    for (unsigned r = 0; r < 3; r++) {
        for (unsigned c = 0; c < 4; c++)
            out[r * 4 + c] = a[r * 4] * b[c] + a[r * 4 + 1] * b[4 + c] + a[r * 4 + 2] * b[8 + c];

        out[r * 4 + 3] += a[r * 4 + 3];
    }
}

// A skinned mesh's palette (docs/optimizations/rendering/gpu-skinning.md) from its pose version: a
// rigid row is the view-space draw matrix itself, a skin bone row its view
// matrix (the camera times its world matrix past the view matrices) times the
// bone's inverse bind matrix. Render thread only.
const float (*SkinPalette(const mh3_prepared_mesh& prepared, const NativePoseVersion& pose))[12] {
    static float rows[MH3_PACK_SKIN_MAX_ROWS][12];

    for (uint32_t r = 0; r < prepared.skin_row_count; r++) {
        const auto& row = prepared.skin_rows[r];
        mh3_matrix34 matrix;

        if (row.matrix < pose.sources.positions.count)
            PackChecked(mh3_matrix_read34(pose.sources.positions, row.matrix, &matrix) == MH3_RES_OK ? MH3_PACK_OK : MH3_PACK_RANGE);
        else if (row.kind && row.matrix - pose.bone_first < pose.bone_count)
            Concat34(pose.camera, pose.bones[row.matrix - pose.bone_first], &matrix.v[0][0]);
        else
            PackChecked(MH3_PACK_RANGE);

        if (row.kind)
            Concat34(&matrix.v[0][0], row.inverse_bind, rows[r]);
        else
            std::memcpy(rows[r], &matrix, sizeof rows[r]);
    }

    return rows;
}

// Render thread: replays one recorded item against the Render owner's state.
void ExecuteModelItem(const ModelItem& item, mh3_shape_matrix_input input, const mh3_model_texcoord_sizes& coordinate_sizes) {
    const auto& prepared = *item.prepared;
    const auto& owner = item.owner;
    const auto& sources = item.pose->sources;

    if (input.direct_position)
        input.direct_position = &item.storage.direct_position;

    if (input.direct_normal)
        input.direct_normal = &item.storage.direct_normal;
    // The Render owner's palette is rebuilt in place: the loads write its
    // slots, and the setters below find their matrices already there.
    auto& palette = EditNativeModelPalette(NativeOwner::Render);

    if (input.matrix_id >= 0) {
        const uint32_t produced = mh3_model_palette_rigid_into(&input, &sources, &palette);
        SetNativePositionMatrix(&palette.position[0].v[0][0], NativeOwner::Render);

        if (produced & MH3_PALETTE_PRODUCED_NORMAL)
            SetNativeNormalMatrix(0, &palette.normal[0].v[0][0], NativeOwner::Render);

        for (uint32_t i = 0; i < 8; i++)
            if (produced & MH3_PALETTE_PRODUCED_TEXTURE(i))
                SetNativeTextureMatrix(30 + i * 3, &palette.texture[i].v[0][0], true, NativeOwner::Render);
    }

    const auto& material = GetNativeMaterialState(NativeOwner::Render);
    float post[8][12]{};
    uint8_t postKnown = 0, post3x4 = 0;

    for (uint32_t i = 0; i < 8; i++) {
        bool matrix3x4 = false;

        if (GetNativeTextureMatrix(64 + i * 3, post[i], &matrix3x4, NativeOwner::Render)) {
            postKnown |= uint8_t(1u << i);
            post3x4 |= uint8_t(matrix3x4 ? 1u << i : 0);
        }
    }

    auto submit_one = [&](uint32_t batch_index) {
        // Every field is written by the bind (or the draw is abandoned).
        mh3_resolved_mesh_submit submit;

        if (prepared.skin_row_count)
            PackChecked(mh3_model_mesh_bind_skin(&prepared, owner.resident_generation, owner.source_generation, ReadNativeModelPaletteRevision(NativeOwner::Render),
                                                 SkinPalette(prepared, *item.pose), &submit));
        else if (prepared.palette_batch_count) {
            const auto& batch = prepared.palette_batches[batch_index];
            mh3_model_palette_apply_batch(&batch, &sources, &palette, &palette);
            EditNativeModelPalette(NativeOwner::Render);

            if (batch.position_known & 1)
                SetNativePositionMatrix(&palette.position[0].v[0][0], NativeOwner::Render);

            for (uint32_t slot = 0; slot < 10; slot++) {
                if (batch.normal_known & (1u << slot))
                    SetNativeNormalMatrix(slot * 3, &palette.normal[slot].v[0][0], NativeOwner::Render);

                if (batch.texture_known & (1u << slot))
                    SetNativeTextureMatrix(30 + slot * 3, &palette.texture[slot].v[0][0], true, NativeOwner::Render);
            }

            PackChecked(mh3_model_mesh_bind_batch(&prepared, batch_index, owner.resident_generation, owner.source_generation, ReadNativeModelPaletteRevision(NativeOwner::Render), &palette, &submit));
        } else
            PackChecked(mh3_model_mesh_bind_pose(&prepared, owner.resident_generation, owner.source_generation, ReadNativeModelPaletteRevision(NativeOwner::Render), &palette, &submit));

        submit.material = &material;
        submit.projection = GetNativeDrawState(NativeOwner::Render).projection;
        submit.texture_matrices = post;
        submit.texture_matrix_known = postKnown;
        submit.texture_matrix_3x4 = post3x4;
        submit.texcoord_sizes = &coordinate_sizes;
        SubmitNativeModelMesh(submit);
    };

#ifdef MH3_DEVELOPMENT
    if (const uint32_t skip = NativeSkippedMeshes()) {
        const auto& pixel = material.pixel.values;
        const uint32_t operation = pixel[MH3_PIXEL_ALPHA_OPERATION], c0 = pixel[MH3_PIXEL_ALPHA_COMPARE0], c1 = pixel[MH3_PIXEL_ALPHA_COMPARE1];
        const bool alwaysPasses = operation == 0 ? c0 == 7 && c1 == 7 : operation == 1 && (c0 == 7 || c1 == 7);

        if (((skip & NATIVE_SKIP_SKINNED) && prepared.skin_row_count) || ((skip & NATIVE_SKIP_BLENDED) && pixel[MH3_PIXEL_BLEND_ENABLE]) ||
            ((skip & NATIVE_SKIP_DISCARD) && !alwaysPasses))
            return;
    }
#endif
    if (prepared.palette_batch_count && !prepared.skin_row_count)
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
    item.deferred = false;
    {
        MH3_SPLIT_DETAIL_SCOPE(MH3_SPLIT_SHAPE_PREP);
        PrepareModelItem(item, context, direct_position, direct_normal);
    }

    item.coordinate_sizes = coordinate_sizes;

    if (&capture == &scratch)
        return;

    MH3_SPLIT_DETAIL_SCOPE(MH3_SPLIT_MODEL_RECORD);
    RecordNativeModelDraw(uint32_t(capture.itemCount++));
}
} // namespace

void SubmitNativeModelShape(uint32_t reference, uint32_t direct_position, uint32_t direct_normal) {
    if (Resource(reference))
        SubmitShape(ResolveShape(reference), direct_position, direct_normal);
}

void BeginNativeModelDraw() {
    drawPose.version = nullptr;
    drawPose.active = true;
}

void EndNativeModelDraw() {
    drawPose.version = nullptr;
    drawPose.active = false;
}

void ExecuteNativeModelDraw(uint32_t slot, uint32_t index) {
    const auto& capture = stores.at(slot);
    const auto& item = capture.items.at(index);

    if (!item.deferred) {
#ifdef MH3_DEVELOPMENT
        MarkNativeRenderModelDraw();
#endif
        ExecuteModelItem(item, item.input, item.coordinate_sizes);
        return;
    }

    ModelPrepShapeResult shape;
    ExecuteModelPrep(NativeOwner::Render, RenderModelPrepMirror(), item.prep, item.prep.srt >= 0 ? &capture.srts.items[item.prep.srt] : nullptr,
                     item.direct_position ? &item.storage.direct_position : nullptr, item.direct_normal ? &item.storage.direct_normal : nullptr, &shape);
    if (!item.prep.visible)
        return;
#ifdef MH3_DEVELOPMENT
    MarkNativeRenderModelDraw();
#endif
    ApplyNativeRenderRaster();
    ExecuteModelItem(item, shape.input, shape.coordinate_sizes);
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
uint32_t NativeModelIndirectMatrices(uint32_t material_reference, uint32_t node_reference, uint32_t shape_reference) {
    return NativeModelIndirectMatricesAt(Resource(material_reference), Resource(shape_reference), material_reference, node_reference, shape_reference);
}

uint32_t NativeModelIndirectMatricesAt(uint32_t material, uint32_t shape, uint32_t material_reference, uint32_t node_reference, uint32_t shape_reference) {
    current_operator = {};
    operator_address = 0;

    // Without a material, a shape or an indirect method the operator stays
    // inactive (mh3_matrix_runtime_indirect would only clear its records).
    if (!material || !shape || !(Memory::Read8(material + 0x20) || Memory::Read8(material + 0x21) || Memory::Read8(material + 0x22)))
        return 0;

    {
        uint32_t parent = 0;
        Checked(mh3_res_runtime_shape_parent(&MH3NativeResourceMemory(), shape_reference, &parent));
        const auto owner = ResolveNativeModelOwner(parent);
        const auto binding = ReadNativeModelMatrixSources(locations.arrays);

        if (int32_t(Memory::Read32(shape + 8)) >= 0)
            (void)ReadNativeModelPose(binding.address, owner.model_address);
    }

    Checked(mh3_matrix_runtime_indirect(&MH3NativeResourceMemory(), &locations, material_reference, node_reference, shape_reference, &current_operator));

    if (!current_operator.active)
        return 0;

    operator_address = Resource(0x80793708, true);
    OperatorKnown(operator_address);
    Checked(mh3_matrix_store_indirect(Mutable(operator_address, 152), &current_operator));
    return operator_address;
}

extern "C" void MH3_ModelIndirectMatrices(CpuContext* cpu) {
    cpu->gpr[3] = NativeModelIndirectMatrices(cpu->gpr[3], cpu->gpr[4], cpu->gpr[5]);
}

// Only integer guest arguments/results reach the native matrix producers;
// no guest call or CpuContext is passed to their descendants.
REGISTER_NATIVE_FUNCTION_AS(0x80077814, MH3_ModelIndirectMatrices, "ModelIndirectMatrices");
namespace {
// The dispatch's shape visibility: its resolved shape's flag, or by reference.
bool ShapeVisible(const NativeModelDispatch& d, const Resolved* r) {
    if (r)
        return !(Memory::Read32(r->shape.shape + 0x34) & 2);

    uint32_t visible = 0;
    Checked(mh3_res_runtime_shape_visible(&MH3NativeResourceMemory(), d.shape, &visible));
    return visible != 0;
}

// ---- Deferred preparation (model_render_prep.h) ----
// 80076220 steps 1 (fog) and 10 (light set) stay on the game thread: they
// publish the lighting snapshot and keep the guest fog/light caches.
void GameLightingSteps(const mh3_model_material_scope& scope) {
    mh3_model_material_misc misc{};
    Checked(mh3_model_material_misc_read(Bytes(scope.selected.resource_slots[MH3_MAT_MISC], 12), &misc));
    mh3_native_fog fog{};
    int apply = 0;
    Checked(mh3_fog_state_select(Mutable(0x80685d60, 1544), uint32_t(int32_t(misc.fog_reference)), &fog, &apply));

    if (apply)
        SetNativeFog(fog);

    (void)SelectNativeModelLightSet(0x80686368, uint32_t(int32_t(misc.light_reference)));
}

// 800879DC's vertex source bindings: the game thread keeps them to check that
// the converted mesh matches the bound arrays.
void PrePrimitiveSources(const ShapeContext& context) {
    const auto& sources = *context.sources;

    for (uint32_t i = 0; i < 12; i++)
        if (sources.known & (1u << i))
            StoreNativeVertexSource(i, sources.source[i]);
}

uint64_t capturedSelectors = UINT64_MAX, capturedSequence = UINT64_MAX;
void CaptureInputs(ModelPrepInputs& in, const NativeModelDispatch& d, const mh3_model_material_scope& scope, bool ignore, ModelPrepSrtList& srts) {
    if (!ignore) {
        CaptureModelPrepMaterial(in, scope, srts);
        const auto& lights = GetNativeModelLights();
        in.light_masks[0] = lights.diffuse_color;
        in.light_masks[1] = lights.diffuse_alpha;
        in.light_masks[2] = lights.specular_color;
        in.light_masks[3] = lights.specular_alpha;
        std::memcpy(in.ambient, &lights.ambient, 4);
    } else {
        in.present = 0;
        in.record = nullptr;
        in.pixel = in.colors = nullptr;
        in.srt = -1;

        // An ignored material still applies a replaced indirect matrix block.
        const uint32_t indirect = (scope.replacement_mask & (1u << MH3_MAT_INDIRECT_MATRIX_SCALE)) ? scope.selected.resource_slots[MH3_MAT_INDIRECT_MATRIX_SCALE] : 0;
        in.indirect = indirect ? CaptureModelPrepBlock(indirect, MH3_MODEL_MATERIAL_INDIRECT) : nullptr;
    }

    in.indirect_operator = d.descriptor != 0;

    if (in.indirect_operator) {
        if (d.descriptor != operator_address || !current_operator.active)
            MH3_THROW(std::runtime_error("Indirect matrix operator has no completed native producer"));

        OperatorKnown(d.descriptor);
        in.op = current_operator;
    }
}

// PrepareModelItem's game-thread part: the converted-source check, the pose
// version and the direct matrices. The rest runs with the deferred preparation.
void CaptureShape(ModelItem& item, const ShapeContext& context, uint32_t direct_position, uint32_t direct_normal) {
    item.owner = context.owner;
    const auto& prepared = *context.sources->prepared;
    item.prepared = &prepared;
    const auto& originals = *context.sources;

    // Sources bound in place since are the shape's own, unproduced ones: the
    // check holds when they cover the converted layout.
    const bool bound = originals.covers_layout && NativeVertexSourcesBoundInPlace(originals.source.data()) && NativeVertexSourcesUnproduced();

    for (uint32_t i = 0; i < (bound ? 0 : prepared.immutable.layout.count); i++) {
        const uint32_t semantic = prepared.immutable.layout.attributes[i].semantic;

        if (semantic >= 12 && (prepared.palette_batch_count || prepared.skin_row_count))
            continue;

        if (!BoundOriginal(originals, semantic))
            MH3_THROW(std::runtime_error("Resident model vertex binding requires a converted authored source: " + std::to_string(semantic)));
    }

    const auto binding = ReadNativeModelMatrixSources(locations.arrays);

    if (!binding.address[0] && !direct_position)
        MH3_THROW(std::runtime_error("Indexed model matrix has no completed pose producer"));

    CapturePose(item, binding.address);
    item.storage = {};
    item.direct_position = direct_position != 0;
    item.direct_normal = direct_position && direct_normal;

    if (direct_position) {
        mh3_matrix_read34_unchecked({Bytes(direct_position, 48), 1, 48, 1}, 0, &item.storage.direct_position);

        if (direct_normal)
            mh3_matrix_read34_unchecked({Bytes(direct_normal, 48), 1, 48, 1}, 0, &item.storage.direct_normal);
    }

    if (originals.matrix_id < 0 && !prepared.palette_batch_count && !prepared.skin_row_count)
        MH3_THROW(std::runtime_error("Envelope model requires converted skinning palette records"));

    item.prep.matrix_id = originals.matrix_id;
    item.prep.has_normal = originals.has_normal;
    item.prep.has_binding = binding.address[0] != 0;
}

void Dispatch(const NativeModelDispatch& d, const Resolved* r) {
    TraceNativeModelDispatch(d);

    // Skipped frames record nothing (the game-side steps still run); the
    // render thread forgets its caches at the next recorded frame.
    ModelStore* capture = NativeGraphicsFrameShouldRender() ? &Store() : nullptr;
    ModelItem* item = nullptr;

    if (capture) {
        if (capture->itemCount == capture->items.size())
            capture->items.emplace_back();

        item = &capture->items[capture->itemCount];
        item->deferred = true;
    }

    mh3_model_material_scope scope;
    bool ignore;
    {
        MH3_SPLIT_DETAIL_SCOPE(MH3_SPLIT_MATERIAL_PREP);
        scope = r ? r->scope : Scope(d.material, d.bindings);
        ignore = !scope.material_address || (d.flags & 16);

        if (!ignore)
            GameLightingSteps(scope);

        if (item) {
            auto& in = item->prep;
            in.flags = d.flags;
            in.invalidate = TakeNativeModelPrepInvalidate();
            in.material = !ignore;

            // A newer packet can replace this one before it renders: every
            // packet's first item carries the selectors again.
            const uint64_t selectors = ReadNativeModelSelectorsRevision();
            in.selectors = selectors != capturedSelectors || capturedSequence != NativeFramePacketSequence();

            if (in.selectors) {
                in.selector_state = ReadNativeModelSelectors();
                capturedSelectors = selectors;
                capturedSequence = NativeFramePacketSequence();
            }

            CaptureInputs(in, d, scope, ignore, capture->srts);
        }
    }

    bool draws = ShapeVisible(d, r);

    if (draws) {
        if (scope.fur_address)
            MH3_THROW(std::runtime_error("Fur model needs native sampled-shell material preparation"));

        if (!r && !Resource(d.shape)) {
            if (!(d.flags & 2))
                Replacements(ResolveShape(d.shape), scope);

            draws = false;
        } else {
            MH3_SPLIT_DETAIL_SCOPE(MH3_SPLIT_SHAPE_PREP);
            const auto shape = r ? r->shape : ResolveShape(d.shape);

            if (!(d.flags & 2)) {
                // A table's sources stay put while its owner lives: bind them
                // in place instead of copying twelve records per draw.
                if (r)
                    BindNativeVertexSourcesInPlace(shape.sources->source.data(), shape.sources->known);
                else
                    PrePrimitiveSources(shape);

                Replacements(shape, scope);

                if (item)
                    std::memcpy(item->prep.shape_cache, Bytes(shape.shape + 12, 12).data, 12);
            }

            if (item)
                CaptureShape(*item, shape, d.position, d.normal);
        }
    }

    if (!item)
        return;

    item->prep.visible = draws;
    MH3_SPLIT_DETAIL_SCOPE(MH3_SPLIT_MODEL_RECORD);
    RecordNativeModelDraw(uint32_t(capture->itemCount++), true);
}

} // namespace

void DispatchNativeModel(const NativeModelDispatch& d) {
    Dispatch(d, nullptr);
}

bool DispatchNativeModelIndexed(const NativeModelDrawTable& table, uint32_t material_id, uint32_t shape_id, const mh3_model_material_bindings* replacements, const NativeModelDispatch& d) {
    if (material_id >= table.materials.size() || shape_id >= table.shapes.size()) {
        return false;
    }

    const auto& material = table.materials[material_id];
    const auto& sources = table.shapes[shape_id];

    if (!material.material_address || !sources.prepared) {
        return false;
    }

    Resolved r;

    if (replacements)
        Checked(mh3_model_material_scope_overlay(&MH3NativeResourceMemory(), &material, replacements, &r.scope));
    else
        r.scope = material;

    r.shape = {d.shape, sources.address, sources.owner, &sources};
    Dispatch(d, &r);
    return true;
}

extern "C" void MH3_ModelDispatch(CpuContext* cpu) {
    DispatchNativeModel({cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7], cpu->gpr[8], cpu->gpr[9]});
}

// Material/shape preparation and packet recording preserve every guest FPR.
REGISTER_NATIVE_FUNCTION_AS(0x80076A58, MH3_ModelDispatch, "ModelDisplayListDispatch");
