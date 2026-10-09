// Startup conversion of the disc's models into native source packs: one
// M3MP/M3ID pair per FST file with models, and the M3IX source index the
// runtime admits them through (native-models/, see docs/native-model-assets.md).
//
// Model IDs follow the canonical recipe: standalone BRRES files by (file,
// model name), then the common and map effect archives by (file, member
// offset, model name). Texture IDs follow PrepareTextures. A source whose
// conversion fails (unsupported shapes, a texture bound outside its file) is
// left out and reported; its models then fail at admission if drawn.
#include "vita_launcher.h"
#include "disc_image.h"
#include "launcher_texture_inventory.h"
#include "launcher_convert.h"
#include "runtime/native/mh3_prepare_model.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <stdexcept>

namespace VitaLauncher {
namespace {
constexpr uint32_t Mdl0 = 0x4d444c30;
const char* const Ready = "native-models/ready.txt";

// The reference's component descriptors (they do not enable renderer sorting).
struct Selector {
    const char* file;
    const char* model;
    std::vector<uint16_t> shapes;
};

const Selector Selectors[] = {{"15/m02d/m02a06d/m02a06d.brres", "m02a06d", {43, 49}}};

struct Memory {
    std::vector<uint8_t> data;
    static int Write(void* context, uint64_t at, const uint8_t* bytes, size_t count) {
        auto& m = static_cast<Memory*>(context)->data;

        if (at > SIZE_MAX - count)
            return -1;

        if (m.size() < at + count)
            m.resize(at + count);

        std::memcpy(m.data() + at, bytes, count);
        return 0;
    }

    static int Truncate(void* context, uint64_t size) {
        static_cast<Memory*>(context)->data.resize(size);
        return 0;
    }

    static int Read(void* context, uint64_t at, uint8_t* bytes, size_t count) {
        const auto& m = static_cast<Memory*>(context)->data;

        if (at > m.size() || count > m.size() - at)
            return -1;

        std::memcpy(bytes, m.data() + at, count);
        return 0;
    }

    mh3_prepare_sink Sink() { return {this, Write, Truncate}; }
    mh3_prepare_input Input() { return {this, Read}; }
};

struct File {
    std::string path;
    uint64_t offset;
    std::vector<ModelSource> models;
};
} // namespace

// Workspace sized like the parity harness: per-source caps, one conversion at a time.
struct ModelConverter::Workspace {
    std::vector<uint8_t> metadata = std::vector<uint8_t>(MH3_PREPARE_MAX_METADATA), identities = std::vector<uint8_t>(MH3_PREPARE_MAX_IDENTITY);
    std::vector<uint32_t> scratch = std::vector<uint32_t>((8u << 20) / 4);
    std::vector<mh3_pack_layout> layouts = std::vector<mh3_pack_layout>(256);
    std::vector<mh3_prepare_string> strings = std::vector<mh3_prepare_string>(4096);
    std::vector<mh3_prepare_model_plan> plans = std::vector<mh3_prepare_model_plan>(4096);
    std::vector<uint8_t> vertices = std::vector<uint8_t>(65536 * 128);
    std::vector<mh3_corner_tree_node> tree = std::vector<mh3_corner_tree_node>(65536);
    std::vector<uint16_t> corners = std::vector<uint16_t>(65536);
    std::vector<mh3_pack_palette_batch> batches = std::vector<mh3_pack_palette_batch>(16384);
    Memory indices, gpu;
    mh3_prepare_model_workspace Get() {
        indices.data.clear();
        gpu.data.clear();
        mh3_prepare_model_workspace w{};
        w.metadata = metadata.data();
        w.metadata_bytes = metadata.size();
        w.identities = identities.data();
        w.identity_bytes = identities.size();
        w.scratch = reinterpret_cast<uint8_t*>(scratch.data());
        w.scratch_bytes = scratch.size() * 4;
        w.layouts = layouts.data();
        w.layout_count = uint32_t(layouts.size());
        w.strings = strings.data();
        w.string_count = uint32_t(strings.size());
        w.plans = plans.data();
        w.plan_count = uint32_t(plans.size());
        w.shape = {vertices.data(), vertices.size(), tree.data(), uint32_t(tree.size()), corners.data(), uint32_t(corners.size()), batches.data(), uint32_t(batches.size()), indices.Sink()};
        w.indices = indices.Input();
        w.gpu = gpu.Sink();
        return w;
    }
};

namespace {
std::string Hex16(uint64_t value) {
    char text[17];
    std::snprintf(text, sizeof text, "%016llx", static_cast<unsigned long long>(value));
    return text;
}

void Save(const std::filesystem::path& path, const uint8_t* bytes, size_t count) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes), std::streamsize(count));
    out.close();

    if (!out)
        throw std::runtime_error("Cannot write " + path.string() + " (not enough space?)");
}

// Every model and texture of the disc, with canonical IDs.
void Inventory(const DiscImage& disc, std::vector<File>& files, std::vector<TextureRow>& textures, const Progress& progress) {
    const auto names = TextureInventory::SourceNames(disc.FileNames());
    uint32_t nextTexture = 1, nextModel = 1;
    size_t done = 0;

    for (const auto& path : names) {
        progress("Indexing game models", done++, names.size());

        if (disc.FileSize(path) > TextureInventory::MaxSourceBytes)
            throw std::runtime_error("Oversized model source: " + path);

        const auto data = disc.ReadFile(path);
        const uint64_t offset = disc.FileOffset(path);
        std::vector<TextureInventory::Texture> parsed;
        CheckStatus(TextureInventory::Parse(data, offset, parsed, nextTexture));

        for (const auto& t : parsed)
            textures.push_back(TextureRow(offset, t));

        nextTexture = textures.empty() ? 1 : textures.back().id + 1;
        File file{path, offset, {}};
        CheckStatus(ListModels(data, file.models));

        for (auto& model : file.models)
            model.id = nextModel++;

        if (!file.models.empty())
            files.push_back(std::move(file));
    }

    if (nextModel > 65536)
        throw std::runtime_error("Model inventory exceeds the ID encoding");

    std::sort(textures.begin(), textures.end(), [](const TextureRow& a, const TextureRow& b) { return a.partition != b.partition ? a.partition < b.partition : a.name < b.name; });
}
} // namespace

bool ModelsReady(const std::filesystem::path& root) {
    // ready.txt lists every published file and its size; it is written last.
    std::ifstream list(root / Ready);

    if (!list)
        return false;

    std::string path;
    uint64_t bytes = 0;

    while (list >> bytes >> path) {
        std::error_code error;

        if (std::filesystem::file_size(root / "native-models" / path, error) != bytes || error)
            return false;
    }

    return list.eof(); // a disc without models has an empty list
}

Status ListModels(std::span<const uint8_t> data, std::vector<ModelSource>& models) {
    models.clear();
    std::vector<TextureInventory::Container> containers;
    std::vector<TextureInventory::detail::Resource> resources;
    MH3_RETURN_IF_ERROR(TextureInventory::Containers(data, containers));

    for (const auto& container : containers) {
        const TextureInventory::detail::Reader r{data, container.offset, size_t(container.offset) + container.bytes};
        MH3_RETURN_IF_ERROR(TextureInventory::detail::Resources(r, resources));

        for (const auto& resource : resources)
            if (resource.magic == Mdl0)
                models.push_back({container.offset, container.bytes, uint32_t(resource.offset - container.offset), 0, resource.name});
    }

    std::sort(models.begin(), models.end(), [](const ModelSource& a, const ModelSource& b) { return a.container != b.container ? a.container < b.container : a.name < b.name; });
    return {};
}

TextureRow::TextureRow(uint64_t fileOffset, const TextureInventory::Texture& t)
    : partition(fileOffset + t.containerOffset), name(t.name), container(t.containerBytes), offset(t.header - t.containerOffset), bytes(t.bytes + 64), id(t.id), width(t.width), height(t.height) {}
ModelConverter::ModelConverter() : work(std::make_unique<Workspace>()) {}
ModelConverter::~ModelConverter() = default;
bool ModelConverter::Convert(const std::string& path, uint64_t fileOffset, std::span<const uint8_t> data, const std::vector<ModelSource>& models, std::vector<TextureRow> textures,
                             ModelConversion& out) {
    // Materials bind textures of their own container, by (container, name).
    std::sort(textures.begin(), textures.end(), [](const TextureRow& a, const TextureRow& b) { return a.partition != b.partition ? a.partition < b.partition : a.name < b.name; });
    std::vector<mh3_prepare_texture> table;
    table.reserve(textures.size());

    for (const auto& t : textures)
        table.push_back({t.partition, t.name.c_str(), t.container, t.offset, t.bytes, t.id, t.width, t.height});

    std::vector<mh3_prepare_model_source> sources;

    for (const auto& m : models) {
        mh3_prepare_model_source s{};

        if (m.container > data.size() || m.bytes > data.size() - m.container || mh3_res_model_open({data.data() + m.container, m.bytes}, m.offset, &s.model)) {
            std::fprintf(stderr, "MH3 prepare: %s left out (model %s does not open)\n", path.c_str(), m.name.c_str());
            return false;
        }

        s.file = path.c_str();
        s.partition_offset = fileOffset + m.container;
        s.container_offset = m.container;
        s.asset_id = m.id;

        for (const auto& selector : Selectors)
            if (path == selector.file && m.name == selector.model) {
                s.component_shapes = selector.shapes.data();
                s.component_shape_count = uint32_t(selector.shapes.size());
            }
        sources.push_back(s);
    }

    auto workspace = work->Get();
    mh3_prepared_model_pack result{};
    mh3_prepare_model_diagnostic diagnostic{};
    const auto status = mh3_prepare_model_pack(sources.data(), uint32_t(sources.size()), table.data(), uint32_t(table.size()), &workspace, &result, &diagnostic);
    Memory pack;

    if (status == MH3_PREPARE_OK && mh3_prepare_emit(&result.mesh, work->gpu.Input(), pack.Sink(), scratch.data(), scratch.size()) == MH3_PREPARE_OK) {
        // Validate the emitted GPU indices and palettes before publication.
        payload.assign(work->gpu.data.size() + 16, 0);
        auto* gpu = reinterpret_cast<uint8_t*>((reinterpret_cast<uintptr_t>(payload.data()) + 15) & ~uintptr_t(15));
        std::memcpy(gpu, work->gpu.data.data(), work->gpu.data.size());

        if (mh3_mesh_pack_bind_gpu(&result.pack, {gpu, work->gpu.data.size()}) == MH3_PACK_OK && mh3_mesh_pack_validate_indices(&result.pack) == MH3_PACK_OK) {
            out.pack = std::move(pack.data);
            out.identity.assign(work->identities.data(), work->identities.data() + result.identity.size);
            out.metadataBytes = result.mesh.size;
            out.gpuBytes = result.mesh.gpu_bytes;
            return true;
        }
    }

    std::fprintf(stderr, "MH3 prepare: %s left out (status %d, model %u kind %u resource %u at %zx)\n", path.c_str(), int(status), diagnostic.model_id, diagnostic.kind, diagnostic.resource_id,
                 diagnostic.offset);
    return false;
}

void PrepareModels(const std::filesystem::path& root, const Progress& progress) {
    DiscImage disc(root, false);
    std::vector<File> files;
    std::vector<TextureRow> textures;
    Inventory(disc, files, textures, progress);
    std::sort(files.begin(), files.end(), [](const File& a, const File& b) { return a.offset < b.offset; });

    const auto output = root / "native-models", staging = root / "native-models.preparing";
    std::filesystem::remove_all(staging);
    std::filesystem::create_directories(staging / "packs");
    ModelConverter converter;
    std::vector<std::string> paths;
    std::vector<mh3_prepare_index_pack> packs;
    std::vector<mh3_prepare_index_source> sources;
    std::vector<std::pair<std::string, uint64_t>> published;
    std::vector<uint8_t> data;
    paths.reserve(files.size());
    unsigned skipped = 0;

    for (size_t f = 0; f < files.size(); ++f) {
        const auto& file = files[f];
        progress("Converting game models", f, files.size());
        data = disc.ReadFile(file.path);

        // Texture rows are sorted by partition offset: this file's containers form one run.
        const auto first = std::lower_bound(textures.begin(), textures.end(), file.offset, [](const TextureRow& t, uint64_t at) { return t.partition < at; });
        const auto last = std::lower_bound(first, textures.end(), file.offset + data.size(), [](const TextureRow& t, uint64_t at) { return t.partition < at; });
        ModelConversion result;

        if (!converter.Convert(file.path, file.offset, data, file.models, {first, last}, result)) {
            ++skipped;
            continue;
        }

        const auto name = "packs/" + Hex16(file.offset);
        Save(staging / (name + ".bin"), result.pack.data(), result.pack.size());
        Save(staging / (name + ".identity.bin"), result.identity.data(), result.identity.size());
        published.emplace_back(name + ".bin", result.pack.size());
        published.emplace_back(name + ".identity.bin", result.identity.size());
        paths.push_back(name + ".bin");
        packs.push_back({paths.back().c_str(), result.metadataBytes, uint32_t(result.identity.size()), result.gpuBytes, uint32_t(result.pack.size())});
        std::vector<std::pair<uint64_t, uint32_t>> members;

        for (const auto& m : file.models)
            members.emplace_back(file.offset + m.container, m.bytes);

        std::sort(members.begin(), members.end());
        members.erase(std::unique(members.begin(), members.end()), members.end());

        for (const auto& [at, bytes] : members)
            sources.push_back({at, bytes, uint32_t(packs.size() - 1)});
    }

    if (!packs.empty()) {
        std::vector<uint8_t> index(MH3_PREPARE_MAX_INDEX);
        size_t indexBytes = 0;

        if (mh3_prepare_index(index.data(), index.size(), packs.data(), uint32_t(packs.size()), sources.data(), uint32_t(sources.size()), &indexBytes) != MH3_PREPARE_OK)
            throw std::runtime_error("Native model index preparation failed");

        Save(staging / "models.index.bin", index.data(), indexBytes);
        published.emplace_back("models.index.bin", indexBytes);
    }

    std::fprintf(stderr, "MH3 prepare: %zu model source packs, %u sources left out\n", packs.size(), skipped);

    if (std::filesystem::exists(output))
        std::filesystem::remove_all(output);

    std::filesystem::rename(staging, output);
    std::string list;

    for (const auto& [path, bytes] : published)
        list += std::to_string(bytes) + " " + path + "\n";

    Save(root / Ready, reinterpret_cast<const uint8_t*>(list.data()), list.size());
}
} // namespace VitaLauncher
