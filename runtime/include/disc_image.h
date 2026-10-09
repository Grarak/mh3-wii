#pragma once
#include "status.h"
#include <cstdint>
#include <array>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

class DiscFile;
class RomImage;

// Read-only decrypted partition view. Extracted mode preserves the original
// file checks; disc-layout.txt selects a single exact partition with sys
// metadata, read from partition.bin or in place from the disc image named by
// rom.txt.
class DiscImage {
  public:
    // Empty until Load; the constructor that takes a root stops the game when
    // the data does not load.
    DiscImage();
    explicit DiscImage(const std::filesystem::path& root, bool applyNativeTextures = true);
    ~DiscImage();
    Status Load(const std::filesystem::path& root, bool applyNativeTextures = true);
    Status TryRead(uint64_t offset, uint32_t length, std::vector<uint8_t>& result) const;
    std::vector<uint8_t> Read(uint64_t offset, uint32_t length) const;
    std::vector<uint8_t> ReadFile(const std::string& name) const;

    // Launcher quest overlays preserve lengths and apply to partial DVD reads.
    void SetFileOverride(const std::string& name, std::vector<uint8_t> bytes);
    std::vector<std::string> FileNames() const;
    uint64_t FileOffset(const std::string& name) const;
    uint32_t FileSize(const std::string& name) const;

    // Without a prepared native texture map, the game's image hands reads to
    // the load-time asset cache (asset_cache.h), which patches its own tags.
    bool HasNativeTextureMap() const { return hasTextureMap; }
    bool Packed() const { return packed; }
    using Reader = std::function<Status(uint64_t offset, uint8_t* output, size_t length)>;
    void SetReader(Reader reader) { this->reader = std::move(reader); }
    // Partition bytes without native texture tags or quest overrides.
    Status ReadRaw(uint64_t offset, uint8_t* output, size_t length) const;

  private:
    struct Extent {
        uint64_t offset, size;
        std::filesystem::path path;
    };

    std::vector<Extent> extents;
    std::map<std::string, std::pair<uint64_t, uint32_t>> fileEntries;
    std::unique_ptr<DiscFile> partition;
    std::unique_ptr<RomImage> rom;
    Reader reader;
    bool hasTextureMap = false;
    Status ReadPartition(uint64_t offset, uint8_t* output, size_t length) const;
    uint64_t partitionSize = 0;
    bool packed = false;
    std::map<uint64_t, std::vector<uint8_t>> fileOverrides;
    void ApplyFileOverrides(uint64_t offset, std::vector<uint8_t>& data) const;
    struct NativeTexturePatch {
        uint64_t offset;
        std::array<uint8_t, 4> bytes;
    };

    std::vector<NativeTexturePatch> nativeTexturePatches;
    void ApplyNativeTexturePatches(uint64_t offset, std::vector<uint8_t>& data) const;
    Status Add(uint64_t offset, const std::filesystem::path& path, uint64_t expected, bool verify = true);
};

// Shared by IOS disc requests and the development quest catalog.
DiscImage& GameDiscImage();
