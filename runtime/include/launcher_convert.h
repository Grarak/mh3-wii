#pragma once
// Per-file native texture and model conversion, shared by the whole-disc
// preparation (PrepareTextures/PrepareModels) and the load-time asset cache.
#include "launcher_texture_inventory.h"
#include "gfx/native_texture_pack.hpp"
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace VitaLauncher {
struct TextureConversion {
    aurora::gfx::NativeTextureRecord record; // offset is assigned by the pack writer
    std::vector<uint8_t> payload;
};

// One parsed texture of the source file at fileOffset (data is the whole file).
TextureConversion ConvertTexture(const TextureInventory::Texture& texture, std::span<const uint8_t> data, uint64_t fileOffset, const std::string& path);

// A version 2 native texture pack holding IDs firstId..firstId+count-1, added in order.
class TexturePackWriter {
  public:
    TexturePackWriter(const std::filesystem::path& path, uint32_t count, uint32_t firstId);
    void Add(TextureConversion&& texture);
    void Finish();

  private:
    std::fstream output;
    std::vector<aurora::gfx::NativeTextureRecord> records;
    uint32_t base, added = 0;
};

struct ModelSource {
    uint32_t container, bytes, offset, id;
    std::string name;
};

// Every MDL0 of a source file, by (container, name); IDs are left 0.
std::vector<ModelSource> ListModels(std::span<const uint8_t> data);
struct TextureRow {
    uint64_t partition;
    std::string name;
    uint32_t container, offset, bytes, id, width, height;
    TextureRow(uint64_t fileOffset, const TextureInventory::Texture& texture);
};

struct ModelConversion {
    std::vector<uint8_t> pack, identity;
    uint32_t metadataBytes = 0, gpuBytes = 0;
};

// Converts one source file's models into a native model pack. Holds a ~30 MiB
// workspace; false (logged) when the source cannot be converted.
class ModelConverter {
  public:
    ModelConverter();
    ~ModelConverter();
    bool Convert(const std::string& path, uint64_t fileOffset, std::span<const uint8_t> data, const std::vector<ModelSource>& models, std::vector<TextureRow> textures,
                 ModelConversion& out);

  private:
    struct Workspace;
    std::unique_ptr<Workspace> work;
    std::vector<uint8_t> scratch = std::vector<uint8_t>(4096), payload;
};
} // namespace VitaLauncher
