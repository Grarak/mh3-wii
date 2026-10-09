#pragma once
#include <cstdint>
#include <array>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

class DiscFile;
// Read-only decrypted partition view. Extracted mode preserves the original
// file checks; disc-layout.txt selects a single exact partition with sys metadata.
class DiscImage {
public:
    explicit DiscImage(const std::filesystem::path& root);
    ~DiscImage();
    std::vector<uint8_t> Read(uint64_t offset, uint32_t length) const;
    std::vector<uint8_t> ReadFile(const std::string& name) const;
private:
    struct Extent { uint64_t offset, size; std::filesystem::path path; };
    std::vector<Extent> extents;
    std::map<std::string, std::pair<uint64_t, uint32_t>> fileEntries;
    std::unique_ptr<DiscFile> partition;
    uint64_t partitionSize = 0;
    bool packed = false;
    struct NativeTexturePatch { uint64_t offset; std::array<uint8_t, 4> bytes; };
    std::vector<NativeTexturePatch> nativeTexturePatches;
    void ApplyNativeTexturePatches(uint64_t offset, std::vector<uint8_t>& data) const;
    void Add(uint64_t offset, const std::filesystem::path& path, uint64_t expected, bool verify = true);
};

// Shared by IOS disc requests and the development quest catalog.
DiscImage& GameDiscImage();
